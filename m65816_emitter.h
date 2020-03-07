#pragma once

#include "m65816.h"
#include "ir_base.h"

#include <vector>
#include <map>
#include <functional>

namespace m65816 {

class Emitter {
    ssa push(IR_Base&& ir) {
        buffer.push_back(std::move(ir));
        return { u16(buffer.size() - 1) };
    }
    ssa bus_a;
    ssa regs;

    size_t initializer_end_marker;

    template<u8 bits>
    void finaliseReg(Reg r);

public:
    std::vector<IR_Base> buffer;

    Emitter(u32 pc);
    void Finalize();

    std::map<Reg, ssa> state;

    template<u8 bits>
    ssa Const(u32 a) {
        return push(IR_Const<bits, false>(a));
    }

    ssa IncPC() {
        return state[PC] = Add(state[PC], Const<16>(1));
    }
    ssa IncCycle() {
        return state[CYCLE] = Add(state[CYCLE], 1);
    }
    ssa ShiftLeft(ssa a, ssa b) {
        return push(IR_ShiftLeft(a, b));
    }
    ssa ShiftLeft(ssa a, int b) {
        return ShiftLeft(a, push(IR_Const32(b)));
    }
    ssa ShiftRight(ssa a, ssa b) {
        return push(IR_ShiftRight(a, b));
    }
    ssa ShiftRight(ssa a, int b) {
        return ShiftRight(a, push(IR_Const32(b)));
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
    ssa Extract(ssa a, int shift, int width) {
        return push(IR_Extract(a, Const<32>(shift), Const<32>(width)));
    }
    ssa Zext16(ssa a) { return a; } // FIXME
    ssa Zext32(ssa a) { return a; } // FIXME

    ssa memState(ssa bus) {
        // state[ALIVE] allows us to disable memory operations when this codepath is dead.
        return push(IR_MemState(bus, state[CYCLE], state[ALIVE]));
    }

    ssa Read(ssa addr) {
        return push(IR_Load8(memState(bus_a), addr));
    }
    void Write(ssa addr, ssa value) {
        push(IR_Store8(memState(bus_a), addr, value));
    }

    void Assert(ssa a, ssa b) {
        push(IR_Assert(a, b));
    }

    ssa Add(ssa a, ssa b) {
        return push(IR_Add(a, b));
    }
    ssa Add(ssa a, int b) {
        return Add(a, push(IR_Const32(b)));
    }
    ssa Sub(ssa a, ssa b) {
        return push(IR_Sub(a, b));
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
                new_val = Ternary(cond, new_val, old_state[key]);
            }
        }
    }
};

};