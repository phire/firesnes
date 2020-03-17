#pragma once

#include "m65816.h"
#include "ir_base.h"

#include <vector>
#include <map>
#include <functional>
#include <utility>

#include "ir_emitter.h"


namespace m65816 {

class Emitter : public BaseEmitter {
    ssa bus_a;
    ssa regs;

    ssa memory_conditional;

    size_t initializer_end_marker; // TODO: This is wrong. Doesn't handle moves/swaps.

    template<u8 bits>
    void finaliseReg(Reg r);


public:
    bool ending = false;

    std::optional<ssa> zero_lower; // Bit of a hack to make emitting 16bit zero flag checks easier

    void MarkBlockEnd() {
        ending = true;
    }

    Emitter(u32 pc);
    void Finalize();

    std::map<Reg, ssa> state;

    ssa IncPC() {
        return state[PC] = Add(state[PC], Const<16>(1));
    }
    ssa IncCycle() {
        return state[CYCLE] = Add(state[CYCLE], 1);
    }

    ssa memState(ssa bus) {
        // state[ALIVE] allows us to disable memory operations when this codepath is dead.
        return push(IR_MemState(bus, state[CYCLE], memory_conditional));
    }

    ssa Read(ssa addr) {
        return push(IR_Load8(memState(bus_a), addr));
    }
    void Write(ssa addr, ssa value) {
        push(IR_Store8(memState(bus_a), addr, value));
    }

    // Magic for conditional modification of state. Takes a lambda
    // Can be nested.
    // Unfortunately, only state changes and memory operations are conditional.
    // Any SSA varables that excape the lambda by reference won't be conditional.
    void If(ssa cond, std::function<void()> then) {
        std::map<Reg, ssa> old_state = state; // Push a copy of state
        ssa old_mem_conditional = memory_conditional; // Push the memory conditional
        memory_conditional = cond;

        then();

        // Scan for differences in old and new state
        for(auto & [key, new_val]: state) {
            auto &old_val = old_state[key];
            if (old_val.offset != new_val.offset) {
                // Insert a ternary operation where the state differs.
                new_val = Ternary(cond, new_val, old_state[key]);
            }
        }

        // restore memory conditional
        memory_conditional = old_mem_conditional;
    }
};

};