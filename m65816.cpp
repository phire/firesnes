#include "m65816.h"
#include "literalfn.h"
#include <stdio.h>
#include <array>
#include <functional>
#include <map>
#include <vector>

#include "ir_base.h"

enum Reg {
    A,
    B,
    D,
    X,
    Y,
    PC,
    PBR,
    DBR,
    Flag_M,
    Flag_X,
    Flag_E,
    CYCLE, // Not a register, but lets pretend.
};

class Emitter {
    std::vector<IR_Base> buffer;

    ssa push(IR_Base&& ir) {
        buffer.push_back(std::move(ir));
        return { u16(buffer.size() - 1) };
    }
    ssa bus_a;

public:
    std::map<Reg, ssa> state;

    template<u8 bits>
    ssa Const(u32 a) {
        return push(IR_Const<bits, false>(a));
    }

    ssa IncPC() {
        return state[PC] = Add(state[PC], 1);
    }
    ssa IncCycle() {
        return state[CYCLE] = Add(state[CYCLE], 1);
    }
    ssa Shift(ssa a, ssa b) {
        return push(IR_Shift(a, b));
    }
    ssa Shift(ssa a, int b) {
        return Shift(a, push(IR_Const32(b)));
    }
    ssa Not(ssa a) {
        return push(IR_Not(a));
    }
    ssa And(ssa a, ssa b) {
        return push(IR_And(a, b));
    }
    ssa Or(ssa a, ssa b) {
        return push(IR_Or(a, b));
    }
    ssa Xor(ssa a, ssa b) {
        return push(IR_Xor(a, b));
    }
    ssa Cat(ssa a, ssa b) {
        return push(IR_Cat(a, b));
    }
    ssa Zext16(ssa a) { return a; } // FIXME
    ssa Zext32(ssa a) { return a; } // FIXME
    ssa Read(ssa addr) {
        return push(IR_Load8(bus_a, addr, state[CYCLE]));
    }
    ssa Add(ssa a, ssa b) {
        return push(IR_Add(a, b));
    }
    ssa Add(ssa a, int b) {
        return Add(a, push(IR_Const32(b)));
    }
    ssa Ternary(ssa cond, ssa a, ssa b) {
        return push(IR_Ternary(cond, a, b));
    }
    ssa Neq(ssa a, ssa b) {
        return push(IR_Neq(a, b));
    }
    ssa Eq(ssa a, ssa b) {
        return push(IR_Eq(a, b));
    }
    void If(ssa cond, std::function<void()> then) {
        std::map<Reg, ssa> old_state = state; // Copy state

        then();

        // Scan for differences in old and new state
        for(auto & [key, new_val]: state) {
            auto &old_val = old_state[key];
            if (old_val.offset != new_val.offset) {
                // Insert a ternary operation where the state differs.
                new_val = Ternary(cond, old_state[key], new_val);
            }
        }
    }
};

static ssa ReadPc(Emitter& e) {
    ssa data = e.Read(e.Cat(e.state[PBR], e.state[PC]));
    e.IncPC();
    e.IncCycle();
    return data;
}

static ssa ReadPc16(Emitter& e) {
    ssa data_low = ReadPc(e);
    ssa data_high = ReadPc(e);
    ssa data = e.Cat(data_high, data_low);
    return data;
}

using inner_fn = std::function<void(Emitter&, ssa&, ssa)>;

// Given an address, applies a read or write operation.
// Applis the operaiton twice when M = 0
static void ApplyMemoryOperation(Emitter& e, inner_fn operation, ssa address) {
    operation(e, e.state[A], address);
    e.IncCycle();

    e.If(e.Not(e.state[Flag_M]), [&] () {
        ssa address2 = e.Add(address, 1);
        operation(e, e.state[B], address2);
        e.IncCycle();
    });
}

static void ApplyModify(Emitter& e, inner_fn operation, ssa address) {
    operation(e, e.state[A], address);
    e.IncCycle();

    e.If(e.Not(e.state[Flag_M]), [&] () {
        ssa address2 = e.Add(address, 1);
        operation(e, e.state[B], address2);
        e.IncCycle();
    });
}

static void ApplyAcc(Emitter e, inner_fn operation) {

}

// Adds one of the index registers (X or Y) to the address.
// handles adds extra cycles when required by page cross or the X flag.
ssa AddIndexReg(Emitter& e, Reg reg, ssa address) {
    ssa index = e.state[reg];
    ssa new_address = e.Add(address, index);

    // See if the upper bits change
    ssa mask = e.Const<16>(0xff00);
    ssa page_cross = e.Neq(e.And(new_address, mask), e.And(address,mask));

     // Takes an extra cycle when index is 16bit or an 8bit index crosses a page boundary
     e.If(e.Or(page_cross, e.Not(e.state[Flag_X])), [&] {
         // TODO: Dummy read to DBR,AAH,AAL+XL
         e.IncCycle();
     });

    return new_address;
}

static ssa Absolute(Emitter& e) {
    return e.Cat(e.state[DBR], ReadPc16(e));
}

static ssa AbsoluteLong(Emitter& e) {
    ssa low = ReadPc16(e);
    ssa high = ReadPc(e);
    return e.Cat(high, low);
}

template<Reg indexreg>
static ssa AbsoluteIndex(Emitter& e) {
    return e.Cat(e.state[DBR], AddIndexReg(e, indexreg, ReadPc16(e)));
}

static ssa AbsoluteLongX(Emitter& e) {
    return e.Add(AbsoluteLong(e), e.Cat(e.Const<8>(0), e.state[X]));
}

static ssa Direct(Emitter& e) {
    ssa offset = ReadPc(e);
    ssa overflow = e.Neq(e.Const<16>(0x0000), e.And(e.state[D], e.Const<16>(0x00ff)));

    // FIXME: Docs seem to conflict about if this overflow cycle penalty goes away in 16bit mode too
    e.If(overflow, [&] {
        // TODO: Dummy read to PBR,PC+1
        e.IncCycle();
    });

    return e.Cat(e.Const<8>(0), e.Add(e.state[D], offset));
}

template<Reg indexreg>
static ssa DirectIndex(Emitter e) {
    ssa offset = ReadPc(e);
    ssa overflow = e.Neq(e.Const<16>(0x0000), e.And(e.state[D], e.Const<16>(0x00ff)));
    ssa wrap = e.And(e.Not(overflow), e.state[Flag_E]);

    ssa wrapped = e.Or(e.And(e.state[D], e.Const<16>(0xff00) e.And(e.Const<16>(0x00ff), e.Add(e.state[indexreg], offset)));
    ssa overflowed = e.Add(e.state[indexreg], offset);
    ssa address = e.Ternary(wrap, wrapped, overflowed);

    // TODO: Dummy read to PBR,PC+1
    e.IncCycle(); // Cycle to do add

    e.If(overflow, [&] {
        // TODO: Dummy read to PBR,PC+1
        e.IncCycle(); // Cycle to continue add
    });

    return e.Cat(e.Const<8>(0), e.Add(e.state[D], address));
}


std::array<std::function<void(Emitter&)>, 256> gen_table;
std::array<std::string, 256> name_table;

void populate_tables() {
    auto insert = [&] (size_t opcode, std::string name, std::function<void(Emitter&)>&& fn) {
        gen_table[opcode] = std::move(fn);
        name_table[opcode] = name;
    };

    // These are universal opcodes that do A <--> Memory operations with almost every addressing mode
    auto universal = [&] (const char* name, size_t op_base, inner_fn fn) {
        auto apply = [&] (size_t sub_op, std::function<ssa(Emitter&)> address_fn) {
            insert(op_base | sub_op, name, [&] (Emitter& e) { ApplyMemoryOperation(e, fn, address_fn(e)); });
        };

        apply(0x0d, Absolute);              // a
        apply(0x1d, AbsoluteIndex<X>);      // a,x
        apply(0x19, AbsoluteIndex<Y>);      // a, y
        apply(0x0f, AbsoluteLong);          // al
        apply(0x1f, AbsoluteLongX);         // al,x
        apply(0x05, Direct);                // d
        // T[def.op_base + 0x03] = Instruction(def.name, StackRelative<def.fn>);    // d,s
        apply(0x15, DirectIndex<X>);        // d,x
        // T[def.op_base + 0x12] = Instruction(def.name, DirectIndirect<def.fn>);     // (d)
        // T[def.op_base + 0x07] = Instruction(def.name, DirectIndirectLong<def.fn>); // [d]
        // T[def.op_base + 0x13] = Instruction(def.name, StackRelativeIndirectIndexed<def.fn>); // (d,s),y
        // T[def.op_base + 0x01] = Instruction(def.name, DirectIndexedIndirect<def.fn>);     // (d,x)
        // T[def.op_base + 0x11] = Instruction(def.name, DirectIndirectIndexed<def.fn>);     // (d),y
        // T[def.op_base + 0x17] = Instruction(def.name, DirectIndirectLongIndexed<def.fn>); // [d],y
        // if (def.op_base != 0x80) {
        //     T[def.op_base + 0x09] = Instruction(def.name, Immediate<def.fn>);    // #
        // }
    };

    universal("ORA", 0x00, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.Or(reg, e.Read(addr)); });
    universal("AND", 0x20, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.And(reg, e.Read(addr)); });
    universal("EOR", 0x40, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.Xor(reg, e.Read(addr)); });
    //universal("ADC", 0x60, [] (Emitter& e, ssa& reg, ssa addr) { uint16_t sum = a + readfn() + state.p.c;
    //                                                                   a = sum & 0xff; state.p.c = !!(sum & 0x100); } },
    //universal("STA", 0x80, [] (Emitter& e, ssa& reg, ssa addr) { writefn(a); } },
    universal("LDA", 0xa0, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.Read(addr); });
    //universal("CMP", 0xc0, [] (Emitter& e, ssa& reg, ssa addr) { compare(state.p, a, readfn()); } },
    //universal("SBC", 0xe0, [] (Emitter& e, ssa& reg, ssa addr) { substract_a(state, readfn()); },

    // Read-Modify-Write operations
    auto rwm = [&] (const char* name, size_t op_base, inner_fn fn) {
        auto apply = [&] (size_t sub_op, std::function<ssa(Emitter&)> address_fn) {
            insert(op_base + sub_op, name, [&] (Emitter& e) { ApplyModify(e, fn, address_fn(e)); });
        };

        apply(0x06, Direct);
        apply(0x0e, Absolute);
        apply(0x16, DirectIndex<X>);
        apply(0x1e, DirectIndex<X>);
        if (op_base > 0x80) {
            // the INC A and DEC A instructions were new to the 65816 and have a weird encoding.
            op_base = (op_base & 0x30) ^ 0x20; // Uh... What were you thinking 65816 designers?
            insert(op_base | 0x0a, name, std::bind(ApplyAcc, std::placeholders::_1, fn));
        } else {
            insert(op_base | 0x0a, name, std::bind(ApplyAcc, std::placeholders::_1, fn));
        }
    };

    // ASL  dir     abs     dir,x   abs,x   acc
    //      06      0e      16      1e      0a
    // LSR  dir     abs     dir,x   abs,x   acc
    //      46      4e      56      5e      4a
    // ROL  dir     abs     dir,x   abs,x   acc
    //      26      2e      36      3e      2a
    // ROR  dir     abs     dir,x   abs,x   acc
    //      66      6e      76      7e      6a
    // INC  dir     abs     dir,x   abs,x         acc
    //      e6      ee      f6      fe            1a
    // DEC  dir     abs     dir,x   abs,x         acc
    //      c6      ce      d6      de            3a

    // TRB  dir     abs
    //      14      1c
    // TSB  dir     abs
    //      04      0c

    // Fill in the rest of opeartions that touch A

    // BIT  dir     abs     dir,x   abs,x  !imm!
    //      24      2c      34      3c     89

    // Operations on Index
    // INX
    // INY
    // DEX
    // DEY
    // CPX  dir     abs     imm
    //      e4      ec      e0
    // CPY  dir     abs     imm
    //      c4      cc      c0
    // STX  dir     abs     dir,Y
    //      86      8e      96
    // STY  dir     abs     dir,X
    //      84      8c      94
    // STZ  dir     abs     dir,X   abs,X
    //      64      9c      74      9e

    // Transfer operations

    // TAX
    // TAY
    // TSX
    // TXA
    // TXS -- special
    // TXY
    // TYA
    // TYX

    // TCD
    // TCS
    // TDC
    // TSC

    // XBA -- swap
    // XCE -- swap carry and emu

    // Stack operations
}





void m65816::run_for(int cycles) {
    printf("test\n");
}