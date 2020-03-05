#pragma once


#include "types.h"
#include <optional>
#include <type_traits>


enum Opcode {
    Not, // ~A
    Add, // A + B
    Sub, // A - B
    And, // A & B
    Or,  // A | B
    Xor, // A ^ B
    Shift, // A << b OR A >> -b

    Cat, // A << sizeof(B) | B

    Eq,  // A == B
    Neq, // A != B

    memState, // base, cycle, validness (if this SSA node is dead, then the memory operation doesn't exist)
    load64, // mem, offset
    load32, // mem, offset
    load16, // mem, offset
    load8,  // mem, offset
    store64, // mem, offset, data
    store32, // mem, offset, data
    store16, // mem, offset, data
    store8,  // mem, offset, data



    ternary, // condition, true, false


    assert, // value, expected

    Const48 = 0x8000,
    Const,
};

inline const char* OpcodeName(u16 op) {
    switch (op) {
    case Not: return "Not";
    case Add: return "Add";
    case Sub: return "Sub";
    case And: return "And";
    case Or: return "Or";
    case Xor: return "Xor";
    case Shift: return "Shift";
    case Cat: return "Cat";
    case Eq: return "Eq";
    case Neq: return "Neq";
    case memState: return "memState";
    case load64: return "load64";
    case load32: return "load32";
    case load16: return "load16";
    case load8: return "load8";
    case store64: return "store64";
    case store32: return "store32";
    case store16: return "store16";
    case store8: return "store8";
    case ternary: return "ternary";
    case assert: return "assert";
    case Const48: return "Const48";
    case Const: return "Const";
    default: return "<error>";
    }
}

struct ssa {
    u16 offset;
};

struct IR_Base {
    union {
        struct {
            u64 id : 16;
            u64 arg_1 : 16;
            u64 arg_2 : 16;
            u64 arg_3 : 16;
        };
        struct {
            u64 _pad1     : 16;
            u64 num_bits  : 8;
            u64 is_signed : 8; // bool
            u64 arg_32    : 32;
        };
        struct
        {
            u64 _pad3  : 16;
            u64 arg_48 : 48;
        };

        u64 hex;
    };

    IR_Base(u16 id) : id(id) { arg_48 = 0xffffffffffff; }
    IR_Base(u16 id, ssa a) : IR_Base(id) { arg_1 = a.offset; }
    IR_Base(u16 id, ssa a, ssa b) : IR_Base(id, a) { arg_2 = b.offset; }
    IR_Base(u16 id, ssa a, ssa b, ssa c) : IR_Base(id, a, b) { arg_3 = c.offset; }
    IR_Base(u16 id, u8 bits, bool _is_signed, u32 a) : IR_Base(id) { arg_32 = a; num_bits = bits; is_signed = (u8)_is_signed; }
    IR_Base(u16 id, u64 a) : IR_Base(id) { arg_48 = a; }

    template<typename T>
    bool is() const {
        const T *ret = reinterpret_cast<const T*>(this);
        return ret->is();
    }

    template<typename T>
    std::optional<T> cast() {
        T *ret = reinterpret_cast<T*>(this);
        if (ret->is()) {
            return { *ret };
        }
        return {};
    }
};

static_assert(sizeof(IR_Base) == sizeof(u64));

template<Opcode op>
struct IR0 : public IR_Base {
    IR0() : IR_Base(op) {}
    bool is() const { return id == op; }
};

template<Opcode op>
struct IR1 : public IR_Base {
    IR1(ssa a) : IR_Base(op, a) {}
    bool is() const { return id == op; }
};

template<Opcode op>
struct IR2 : public IR_Base {
    IR2(ssa a, ssa b) : IR_Base(op, a, b) {}
    bool is() const { return id == op; }
};

template<Opcode op>
struct IR3 : public IR_Base {
    IR3(ssa a, ssa b, ssa c) : IR_Base(op, a, b, c) {}
    bool is() const { return id == op; }
};

template<u8 bits, bool _signed>
struct IR_Const : public IR_Base {
    //IR_Const(const IR_Base *base) { hex = base->hex; }
    template<typename T>
    IR_Const(T i) : IR_Base(Opcode::Const, bits, _signed, u32(i)) { static_assert(sizeof(T) <= sizeof(u32)); }
    bool is() const { return id == Opcode::Const; }
};

using IR_Not = IR1<Opcode::Not>;
using IR_Add = IR2<Opcode::Add>;
using IR_Sub = IR2<Opcode::Sub>;
using IR_And = IR2<Opcode::And>;
using IR_Or  = IR2<Opcode::Or>;
using IR_Xor = IR2<Opcode::Xor>;
using IR_Cat = IR2<Opcode::Cat>;
using IR_Ternary = IR3<Opcode::ternary>;
using IR_Shift   = IR2<Opcode::Shift>;
using IR_Const8  = IR_Const<8,  false>;
using IR_Const16 = IR_Const<16, false>;
using IR_Const24 = IR_Const<24, false>;
using IR_Const32 = IR_Const<32, false>;
using IR_MemState = IR3<Opcode::memState>;
using IR_Load8 = IR2<Opcode::load8>;
using IR_Load16 = IR2<Opcode::load16>;
using IR_Load32 = IR2<Opcode::load32>;
using IR_Load64 = IR2<Opcode::load64>;
using IR_Store8 = IR3<Opcode::store8>;
using IR_Store16 = IR3<Opcode::store16>;
using IR_Store32 = IR3<Opcode::store32>;
using IR_Store64 = IR3<Opcode::store64>;
using IR_Assert = IR2<Opcode::assert>;
using IR_Neq = IR2<Opcode::Neq>;
using IR_Eq  = IR2<Opcode::Eq>;


static_assert(sizeof(IR_Add) == sizeof(IR_Base));
static_assert(sizeof(IR_Const32) == sizeof(IR_Base));

/*struct IR_Add : public IR_Base {
    const Opcode op = Opcode::Add;

    IR_Add(u16 a, u16 b) : IR_Base(op, a, b) {}

    bool is() { return id == op; }
};*/