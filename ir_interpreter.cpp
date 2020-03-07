#include "ir_base.h"

#include <vector>
#include <cassert>
#include <array>

std::array<u64, 32> registers;
std::array<u8, 0xffff> memory;

// Allows us to interpte an incomplete IR list, continuing it as it is built.
void partial_interpret(std::vector<IR_Base> irlist, std::vector<u64> &ssalist, std::vector<u8> &ssatype, int offset) {
    ssalist.resize(irlist.size());
    ssatype.resize(irlist.size());

    for (int i=offset; i < irlist.size(); i++) {



        auto write = [&] (u64 value, u8 type) {
            ssalist[i] = value;
            ssatype[i] = type;
        };


        auto ir = irlist[i];
        u8 width = ssatype[ir.arg_1];

        auto mem_address = [&] () {
            // TODO: This only works with raw memory
            //       How are we going to get this to work with MMIO?

            u64 offset =  ssalist[ir.arg_2];

            auto mem_ir = irlist[ir.arg_1];
            int mem_type = ssalist[mem_ir.arg_1];

            if (mem_type == 0) {
                return (void *)(&registers[offset]);
            }
            if (mem_type == 1) {
                assert(offset <= 0xffff);
                u64 address = ssalist[ir.arg_1];
                return (void*)(&memory[offset]);
            }

            assert(false);
        };

        auto printarg = [&] (u16 arg) {
            // exclude null args
            if (arg == 0xffff) return;

            auto ir = irlist[arg];

            if (ir.is<IR_Const32>()) {
                auto value = ir.cast<IR_Const32>();
                printf(" ssa%i-%c%i(%i)", arg, value->is_signed ? 's' : 'u', value->num_bits, value->arg_32);
            }
            else {
                printf(" ssa%i", arg);
            }
        };

        if (ir.id < 0x8000) {
            printf("% 5i: %s", i, OpcodeName(ir.id));
            printarg(ir.arg_1);
            printarg(ir.arg_2);
            printarg(ir.arg_3);
            //printf("\n");
        } else if (ir.id == 0x8000) {
            printf("const48 %x\n", ir.arg_48);
        } else if (ir.id == Const) {
            // Don't print consts, because printarg inlines them
            printf("% 5i: const%i %x", i, ir.num_bits, ir.arg_32);
        }

        switch(ir.id) {
        case Not: { // ~A
            u64 mask = 0xffffffffffffffff >> (64 - width);
            write((~ssalist[ir.arg_1]) & mask, width);
            break;
        }
        case Add: { // A + B
            //assert(width == ssatype[ir.arg_2]);
            u64 mask =  0xffffffffffffffff >> (64 - width);
            write((ssalist[ir.arg_1] + ssalist[ir.arg_2]) & mask, width);
            break;
        }
        case Sub: { // A - B
            assert(width == ssatype[ir.arg_2]);
            u64 mask =  0xffffffffffffffff >> (64 - width);
            u64 value = ssalist[ir.arg_1] + ssalist[ir.arg_2];
            write(value & mask, width + 1);
            break;
        }
        case And: {// A & B
            assert(width == ssatype[ir.arg_2]);
            write(ssalist[ir.arg_1] & ssalist[ir.arg_2], width);
            break;
        }
        case Or: { // A | B
            assert(width == ssatype[ir.arg_2]);
            write(ssalist[ir.arg_1] & ssalist[ir.arg_2], width);
            break;
        }
        case Xor: {// A ^ B
            assert(width == ssatype[ir.arg_2]);
            write(ssalist[ir.arg_1] ^ ssalist[ir.arg_2], width);
            break;
        }
        case ShiftLeft: { // A << b OR A >> -b
            int shift = ssalist[ir.arg_2];
            write(ssalist[ir.arg_2] << shift, width + shift);
            break;
        }
        case ShiftRight: { // A << b OR A >> -b
            int shift = ssalist[ir.arg_2];
            write(ssalist[ir.arg_2] >> shift, width - shift);
            break;
        }
        case Cat: { // A << sizeof(B) | B
            int width2 = ssatype[ir.arg_2];
            u64 a = ssalist[ir.arg_1];
            u64 b = ssalist[ir.arg_2];
            write(b | (a << width2), width + width2);
            break;
        }
        case Extract: { // (A >> B) & mask(C)
            int shift = ssalist[ir.arg_2];
            int out_width = ssalist[ir.arg_3];
            assert(width >= out_width + shift);
            u64 mask = 0xffffffffffffffff >> (64 - width);
            u64 a = ssalist[ir.arg_1];
            write((a >> shift) & mask, out_width);
            break;
        }
        case Eq: { // A == B
            assert(width == ssatype[ir.arg_2]);
            write(ssalist[ir.arg_1] == ssalist[ir.arg_2], 1);
            break;
        }
        case Neq: { // A != B
            assert(width == ssatype[ir.arg_2]);
            write(ssalist[ir.arg_1] != ssalist[ir.arg_2], 1);
            break;
        }
        case Ternary: { // condition, true, false
            if (ssalist[ir.arg_1] != 0) {
                write(ssalist[ir.arg_2], ssatype[ir.arg_2]);
            } else {
                write(ssalist[ir.arg_3], ssatype[ir.arg_3]);
            }
            break;
        }
        case Assert:
            // Not needed during interpretation.
            break;
        case Const: // 8: num_bits, 8: is_signed, 32: data
            write(ir.arg_32, ir.num_bits);
            break;

        case memState:
            // Handled in mem_address
            break;

        case load8: { // mem, offset
            u64 value = *(u8*)(mem_address());
            write(value, 8);
            break;
        }
        case load16: { // mem, offset
            u64 value = *(u16*)(mem_address());
            write(value, 16);
            break;
        }
        case load32: { // mem, offset
            u64 value = *(u32*)(mem_address());
            write(value, 32);
            break;
        }
        case load64: { // mem, offset
            u64 value = *(u64*)(mem_address());
            write(value, 64);
            break;
        }
        case store8: { // mem, offset, data
            assert(ssatype[ir.arg_3] == 8);
            *(u8*)(mem_address())  = ssalist[ir.arg_3];
            write(ssalist[ir.arg_3], 8); // for debugging only
            break;
        }
        case store16: { // mem, offset, data
            assert(ssatype[ir.arg_3] == 16);
            *(u16*)(mem_address()) = ssalist[ir.arg_3];
            write(ssalist[ir.arg_3], 16); // for debugging only
            break;
        }
        case store32: { // mem, offset, data
            assert(ssatype[ir.arg_3] == 32);
            *(u32*)(mem_address()) = ssalist[ir.arg_3];
            write(ssalist[ir.arg_3], 32); // for debugging only
            break;
        }
        case store64: { // mem, offset, data
            //assert(ssatype[ir.arg_3] == 64);
            *(u64*)(mem_address()) = ssalist[ir.arg_3];
            write(ssalist[ir.arg_3], 64); // for debugging only
            break;
        }

        default:
            assert(false); // Not implemented
        }

        printf(" = %x:%i\n", ssalist[i], ssatype[i]);
    }
}

void interpret(std::vector<IR_Base> ir) {
    std::vector<u64> ssalist;
    std::vector<u8> ssatype;

    partial_interpret(ir, ssalist, ssatype, 0);
}