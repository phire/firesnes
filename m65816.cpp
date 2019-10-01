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
    C,
    X,
    Y,
    PC,
    PBR,
    DBR,
    Flag_M,
    Flag_X,
    CYCLE, // Not a register, but lets pretend.
};

class Emitter {
    std::vector<IR_Base> buffer;
    //ssa tail() { return { u16(buffer.size() - 1)}; }

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

static void ApplyMemoryOperation(Emitter& e, inner_fn operation, ssa address) {
    operation(e, e.state[A], address);
    e.IncCycle();

    e.If(e.state[Flag_M], [&] () {
        ssa address2 = e.Add(address, 1);
        operation(e, e.state[B], address2);
        e.IncCycle();
    });
}

static void AbsoluteA(Emitter& e, inner_fn operation) {
    ssa address = e.Cat(e.state[DBR], ReadPc16(e));
    ApplyMemoryOperation(e, operation, address);

}

ssa GetIndexReg(Emitter& e, Reg reg) {
    ssa index = e.state[reg];
    ssa masked_index;
    e.If(e.state[Flag_X], [&] {
        masked_index = e.And(index, e.Const<16>(0x00FF));
        e.IncCycle(); // Takes an extra cycle when index is 16bit
    });

    return e.Ternary(e.state[Flag_X], masked_index, index);
}

void AbsoluteA_X(Emitter& e, inner_fn operation) {
    ssa address = e.Cat(e.state[DBR], ReadPc16(e));
    ssa indexed_addr = e.Add(address, GetIndexReg(e, X));
    ApplyMemoryOperation(e, operation, indexed_addr);
}

std::array<std::function<void(Emitter&)>, 256> gen_table;
std::array<std::string, 256> name_table;

void populate_tables() {
    auto insert = [&] (size_t opcode, std::string name, std::function<void(Emitter&)>&& fn) {
        gen_table[opcode] = std::move(fn);
        name_table[opcode] = name;
    };

    auto universal = [&] (const char* name, size_t op_base, inner_fn fn) {
        auto apply = [&] (size_t subop, std::function<void(Emitter& , inner_fn)> address_fn) {
            insert(op_base + subop, name, std::bind(address_fn, std::placeholders::_1, fn));
        };

        apply(0x0d, AbsoluteA);        // a
        apply(0x1d, AbsoluteA_X);      // a,x
        // T[def.op_base + 0x19] = Instruction(def.name, AbsoluteA_Y<def.fn>);      // a, y
        // T[def.op_base + 0x0f] = Instruction(def.name, Absolute_LongA<def.fn>);   // al
        // T[def.op_base + 0x1f] = Instruction(def.name, Absolute_LongA_X<def.fn>); // al,x
        // T[def.op_base + 0x05] = Instruction(def.name, Direct<def.fn>);           // d
        // T[def.op_base + 0x03] = Instruction(def.name, StackRelative<def.fn>);    // d,s
        // T[def.op_base + 0x15] = Instruction(def.name, DirectIndexedY<def.fn>);   // d,x
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
    //};

    //for (auto &def : universal )
    {

       // apply(0x1d, AbsoluteA_X);      // a,x
        // T[def.op_base + 0x19] = Instruction(def.name, AbsoluteA_Y<def.fn>);      // a, y
        // T[def.op_base + 0x0f] = Instruction(def.name, Absolute_LongA<def.fn>);   // al
        // T[def.op_base + 0x1f] = Instruction(def.name, Absolute_LongA_X<def.fn>); // al,x
        // T[def.op_base + 0x05] = Instruction(def.name, Direct<def.fn>);           // d
        // T[def.op_base + 0x03] = Instruction(def.name, StackRelative<def.fn>);    // d,s
        // T[def.op_base + 0x15] = Instruction(def.name, DirectIndexedY<def.fn>);   // d,x
        // T[def.op_base + 0x12] = Instruction(def.name, DirectIndirect<def.fn>);     // (d)
        // T[def.op_base + 0x07] = Instruction(def.name, DirectIndirectLong<def.fn>); // [d]
        // T[def.op_base + 0x13] = Instruction(def.name, StackRelativeIndirectIndexed<def.fn>); // (d,s),y
        // T[def.op_base + 0x01] = Instruction(def.name, DirectIndexedIndirect<def.fn>);     // (d,x)
        // T[def.op_base + 0x11] = Instruction(def.name, DirectIndirectIndexed<def.fn>);     // (d),y
        // T[def.op_base + 0x17] = Instruction(def.name, DirectIndirectLongIndexed<def.fn>); // [d],y
        // if (def.op_base != 0x80) {
        //     T[def.op_base + 0x09] = Instruction(def.name, Immediate<def.fn>);    // #
        // }
    }
}





void m65816::run_for(int cycles) {
    printf("test\n");
}