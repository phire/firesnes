#pragma once

#include "ir_base.h"

#include <map>

class BaseEmitter {
protected:
    ssa push(IR_Base&& ir) {
        buffer.push_back(std::move(ir));
        return { u16(buffer.size() - 1) };
    }

    std::map<u64, ssa> consts_cache;

public:
    std::vector<IR_Base> buffer;
    bool ending = false;

    std::optional<ssa> zero_lower; // Bit of a hack to make emitting 16bit zero flag checks easier

    ssa Const(u32 a, int bits) {
         // Cache constants to make our IR smaller
        u64 index = a | u64(bits) << 32;

        if (consts_cache.find(index) != consts_cache.end()) {
            return consts_cache[index];
        }
        ssa constant = push(IR_Const<false>(a, bits));
        consts_cache[index] = constant;
        return constant;
    }

    template<u8 bits>
    ssa Const(u32 a) {
       return Const(a, bits);
    }

    ssa ShiftLeft(ssa a, ssa b) {
        return push(IR_ShiftLeft(a, b));
    }
    ssa ShiftLeft(ssa a, int b) {
        return ShiftLeft(a, Const<32>(b));
    }
    ssa ShiftRight(ssa a, ssa b) {
        return push(IR_ShiftRight(a, b));
    }
    ssa ShiftRight(ssa a, int b) {
        return ShiftRight(a, Const<32>(b));
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
    ssa Extract(ssa a, ssa shift, int width) {
        return push(IR_Extract(a, shift, Const<32>(width)));
    }
    ssa Extract(ssa a, int shift, int width) {
        return push(IR_Extract(a, Const<32>(shift), Const<32>(width)));
    }

    template <u8 bits>
    ssa Zext(ssa a) {
         return push(IR_Zext(a, Const<32>(bits)));
    }

    void Assert(ssa a, ssa b) {
        push(IR_Assert(a, b));
    }

    ssa Add(ssa a, ssa b) {
        return push(IR_Add(a, b));
    }
    ssa Add(ssa a, int b) {
        return Add(a, Const<32>(b));
    }
    ssa Sub(ssa a, ssa b) {
        return push(IR_Sub(a, b));
    }

    template <u8 bits>
    ssa StateRead(size_t offset) {
        return push(IR_StateRead(Const<32>(offset), Const<8>(bits)));
    }

    template <u8 bits>
    void StateWrite(size_t offset, ssa value) {
        push(IR_StateWrite(Const<32>(offset), Const<8>(bits), value));
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
};