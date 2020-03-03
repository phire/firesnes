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
    S,
    PC,
    PBR,
    DBR,
    Flag_N, // Negative
    Flag_V, // Overflow
    Flag_M, // Accumulator register size (0 == 16 bit)
    Flag_X, // Index register size       (1 ==  8 bit)
    Flag_D, // Decimal
    Flag_I, // IRQ disable
    Flag_Z, // Zero
    Flag_C, // Carry
    Flag_E, // Emulation mode
    Flag_B, // Break
    CYCLE, // Not a register, but lets pretend.
    ALIVE, // also not a register. But this has to be represented somehow
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

// Reads 8 or 16 bytes from PC depending on the Reg register
static ssa ReadPc(Emitter& e, Reg reg) {
    ssa low = ReadPc(e);
    e.IncCycle();

    ssa high = e.Const<8>(0);

    e.If(e.Not(e.state[reg]), [&] () {
        high = ReadPc(e);
        e.IncCycle();
    });

    return e.Cat(high, low);
}

// Reads 8 or 16 bytes from PC depending on the M register
static ssa ReadPcM(Emitter& e) {
    return ReadPc(e, Flag_M);
}

// Reads 8 or 16 bytes from PC depending on the X register
static ssa ReadPcX(Emitter& e) {
    return ReadPc(e, Flag_X);
}

using inner_fn = std::function<void(Emitter&, ssa&, ssa)>;

// Given an address, applies a read or write operation.
// Applis the operation twice when M = 0
static void ApplyMemoryOperation(Emitter& e, inner_fn operation, ssa address) {
    operation(e, e.state[A], address);
    e.IncCycle();

    e.If(e.Not(e.state[Flag_M]), [&] () {
        ssa address2 = e.Add(address, 1);
        operation(e, e.state[B], address2);
        e.IncCycle();
    });
}

// Given an address, applies a read or write operation.
// Applis the operation twice when X = 0
static void ApplyIndexOperation(Emitter& e, inner_fn operation, ssa address) {
    operation(e, e.state[A], address);
    e.IncCycle();

    e.If(e.Not(e.state[Flag_X]), [&] () {
        ssa address2 = e.Add(address, 1);
        operation(e, e.state[B], address2);
        e.IncCycle();
    });
}

static void ApplyImmediate(Emitter& e, inner_fn operation) {
    operation(e, e.state[A], ReadPc(e));
    e.IncCycle();

    e.If(e.Not(e.state[Flag_M]), [&] () {
        operation(e, e.state[B], ReadPc(e));
        e.IncCycle();
    });
}


using rmw_fn = std::function<ssa(Emitter&, ssa)>;

static void ApplyAcc(Emitter e, rmw_fn operation) {
    e.state[A] = operation(e, e.state[A]);
    e.IncCycle();

    e.If(e.Not(e.state[Flag_M]), [&] () {
        e.state[B] = operation(e, e.state[B]);
        e.IncCycle();
    });
}

static void ApplyModify(Emitter& e, rmw_fn operation, ssa address) {
    ssa low = e.Read(address);
        e.IncCycle();

    ssa word = e.Not(e.state[Flag_M]);
    ssa high_address = e.Add(address, e.Const<24>(1));

    // Read High
    ssa high;
    e.If(word, [&] () {
        high = e.Read(high_address);
        e.IncCycle();
    });
    ssa value = e.Ternary(word, e.Cat(high, low), low);

    // Internal operation
    // TODO: Dummy read to same address as previous
    ssa result = operation(e, value);
    e.IncCycle();

    // Writeback High
    e.If(word, [&] () {
        e.Write(high_address, e.Shift(result, -8));
        e.IncCycle();
    });

    // Writeback Low
    e.Write(address, e.And(result, e.Const<16>(255)));
    e.IncCycle();
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

    ssa wrapped = e.Or(e.And(e.state[D], e.Const<16>(0xff00)), e.And(e.Const<16>(0x00ff), e.Add(e.state[indexreg], offset)));
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

static ssa IndirectDirect(Emitter e) {
    ssa location = Direct(e);
    e.IncCycle();

    ssa address_low = e.Read(location);
    ssa location_next = e.Add(location, 1);

    e.IncCycle();

    ssa address_high = e.Read(location_next);

    return e.Cat(e.state[DBR], e.Cat(address_high, address_low));
}

static ssa IndirectDirectLong(Emitter e) {
    ssa location = Direct(e);
    e.IncCycle();

    ssa address_low = e.Read(location);
    ssa location_next = e.Add(location, 1);
    ssa location_next_next = e.Add(location, 2);

    e.IncCycle();
    ssa address_high = e.Read(location_next);

    e.IncCycle();

    ssa address_highest = e.Read(location_next_next);

    return e.Cat(address_highest, e.Cat(address_high, address_low));
}

static ssa StackRelative(Emitter e) {
    ssa offset = ReadPc16(e);

    // TODO: Dummy read to PBR,PC+1
    e.IncCycle(); // Internal cycle to do add

    return e.Add(e.state[S], offset);
}

static void add_carry(Emitter e, ssa& dst, ssa val) {
    ssa result = e.Add(e.Zext32(dst), e.Add(e.Zext32(val), e.state[Flag_C]));
    e.state[Flag_C] = e.Shift(result, 8);
    dst = e.And(result, e.Const<8>(0xff));
}

std::array<std::function<void(Emitter&)>, 256> gen_table;
std::array<std::string, 256> name_table;

void populate_tables() {
    auto insert = [&] (size_t opcode, std::string name, std::function<void(Emitter&)>&& fn) {
        gen_table[opcode] = std::move(fn);
        if (name_table[opcode] != "") {
            printf("Overwriting %s at %x with %s\n", name_table[opcode].c_str(), opcode, name.c_str());
        }
        name_table[opcode] = name;
    };

    // Universal Instructions:
    //      a     a,x   a,y   al    al,x  d     d,s   d,x   (d)   [d]   (d,s),y  (d,x)  (d),y  [d],y  #
    // ORA  0d    1d    19    0f    1f    05    03    15    12    07     13      01     11     17     09
    // AND  2d    3d    39    2f    3f    25    23    35    32    27     33      21     31     37     29
    // EOR  4d    5d    59    4f    5f    45    43    55    52    47     53      41     51     57     49
    // ADC  6d    7d    79    6f    7f    65    63    75    72    67     73      61     71     77     69
    // STA  8d    9d    99    8f    9f    85    83    95    92    87     93      81     91     97     --
    // LDA  ad    bd    b9    af    bf    a5    a3    b5    b2    a7     b3      a1     b1     b7     a9
    // CMP  cd    dd    d9    cf    df    c5    c3    d5    d2    c7     d3      c1     d1     d7     c9
    // SBC  ed    fd    f9    ef    ff    e5    e3    f5    f2    e7     f3      e1     f1     f7     e9

    // These are universal instructions that do A <--> Memory operations with almost every addressing mode

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
        apply(0x03, StackRelative);         // d,s
        apply(0x15, DirectIndex<X>);        // d,x
        apply(0x12, IndirectDirect);        // (d)
        apply(0x07, IndirectDirectLong);    // [d]
        // T[def.op_base + 0x13] = Instruction(def.name, StackRelativeIndirectIndexed<def.fn>); // (d,s),y
        // T[def.op_base + 0x01] = Instruction(def.name, DirectIndexedIndirect<def.fn>);     // (d,x)
        // T[def.op_base + 0x11] = Instruction(def.name, DirectIndirectIndexed<def.fn>);     // (d),y
        // T[def.op_base + 0x17] = Instruction(def.name, DirectIndirectLongIndexed<def.fn>); // [d],y
        if (std::string(name) != "STA") { // Can't store to an immidate
            insert(op_base + 0x09, name, [&] (Emitter& e) { ApplyImmediate(e, fn); });
        }
    };

    // TODO: All the flags

    universal("ORA", 0x00, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.Or(reg, e.Read(addr)); });
    universal("AND", 0x20, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.And(reg, e.Read(addr)); });
    universal("EOR", 0x40, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.Xor(reg, e.Read(addr)); });
    universal("ADC", 0x60, [] (Emitter& e, ssa& reg, ssa addr) { add_carry(e, reg, e.Read(addr)); });
    universal("STA", 0x80, [] (Emitter& e, ssa& reg, ssa addr) { e.Write(addr, reg); });
    universal("LDA", 0xa0, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.Read(addr); });
    //universal("CMP", 0xc0, [] (Emitter& e, ssa& reg, ssa addr) { compare(state.p, a, readfn()); } },
    // TODO: Check correctness of SBC
    universal("SBC", 0xe0, [] (Emitter& e, ssa& reg, ssa addr) { add_carry(e, reg, e.Xor(e.Read(addr), e.Const<8>(255))); });

    // General Read-Modify-Write instructions:
    //      dir     abs     dir,x   abs,x   acc
    // ASL  06      0e      16      1e      0a
    // LSR  46      4e      56      5e      4a
    // ROL  26      2e      36      3e      2a
    // ROR  66      6e      76      7e      6a
    // INC  e6      ee      f6      fe     <1a>
    // DEC  c6      ce      d6      de     <3a>

    // These do shifts and increments with a few addressing modes.
    // Doesn't include the bit RWM instructions below

    auto rwm = [&] (const char* name, size_t op_base, rmw_fn fn) {
        auto apply = [&] (size_t sub_op, std::function<ssa(Emitter&)> address_fn) {
            insert(op_base + sub_op, name, [&] (Emitter& e) { ApplyModify(e, fn, address_fn(e)); });
        };

        apply(0x06, Direct);
        apply(0x0e, Absolute);
        apply(0x16, DirectIndex<X>);
        apply(0x1e, AbsoluteIndex<X>);
        if (op_base > 0x80) {
            // the INC A and DEC A instructions were new to the 65816 and have a weird encoding.
            op_base = (op_base & 0x30) ^ 0x30; // Uh... What were you thinking 65816 designers?
            insert(op_base | 0x0a, name, std::bind(ApplyAcc, std::placeholders::_1, fn));
        } else {
            insert(op_base | 0x0a, name, std::bind(ApplyAcc, std::placeholders::_1, fn));
        }
    };

    // TODO: Flags!
    // TODO: These won't work to well in 16bit mode
    rwm("ASL", 0x00, [] (Emitter& e, ssa val) { return e.Shift(val, e.Const<32>(1));  });
    rwm("LSR", 0x40, [] (Emitter& e, ssa val) { return e.Shift(val, e.Const<32>(-1)); });
    rwm("ROL", 0x20, [] (Emitter& e, ssa val) { return e.Shift(val, e.Const<32>(1));  });
    rwm("ROR", 0x60, [] (Emitter& e, ssa val) { return e.Shift(val, e.Const<32>(-1)); });
    rwm("INC", 0xe0, [] (Emitter& e, ssa val) { return e.Add(val,   e.Const<8>(1));   });
    rwm("DEC", 0xc0, [] (Emitter& e, ssa val) { return e.Add(val,   e.Const<8>(0xff));});

    // Bit instructions:
    //      dir   abs     dir,x   abs,x  !imm!
    // TRB  14    1c
    // TSB  04    0c
    // BIT  24    2c      34      3c     <89>

    auto bit = [&] (const char* name, size_t op_base, rmw_fn fn) {
        auto apply = [&] (size_t sub_op, std::function<ssa(Emitter&)> address_fn) {
            insert(op_base + sub_op, name, [&] (Emitter& e) { ApplyModify(e, fn, address_fn(e)); });
        };

        apply(0x04, Direct);
        apply(0x0c, Absolute);

        if (std::string(name) == "BIT") {
            apply(0x14, DirectIndex<X>);
            apply(0x1c, AbsoluteIndex<X>);
            // BIT #imm is very much a diffrent instruction that we will handle below
        }
    };

    // TODO: Flags!
    bit("TRB", 0x10, [] (Emitter& e, ssa val) { return e.And(val, e.Xor(e.Const<8>(255), e.state[A])); });
    bit("TSB", 0x00, [] (Emitter& e, ssa val) { return e.Or(val, e.state[A]); });
    bit("BIT", 0x20, [] (Emitter& e, ssa val) { /* do flags; */ return val; });

    // Bit #imm is diffrent. Doesn't fit with the above encoding and does flags differently.
    insert(0x89, "BIT", [&] (Emitter e) {
        auto value = ReadPcM(e);
        auto acc_high = e.Ternary(e.state[Flag_M], e.Const<8>(0), e.state[B]);
        auto acc = e.Cat(acc_high, e.state[A]);

        auto result = e.And(value, acc);

        // Only sets Z
        e.state[Flag_Z] = e.Eq(result, e.Const<16>(0));

    });

    // Index<-->Memory instructions:
    //      dir     abs     dir.X/Y  abs.X/Y   imm
    // STY  84      8c      94       --        --
    // STX  86      8e      96       --        --
    // LDY  a4      ac      b4 (X)   bc (X)    a0
    // LDX  a6      ae      b6 (Y)   be (Y)    a2
    // CPY  c4      cc      --       --        c0
    // CPX  e4      ec      --       --        e0

    {
        enum idxmem_type {
            STORE,
            LOAD,
            CMP,
        };

        auto idxmem = [&] (const char* name, size_t op_base, idxmem_type type, Reg reg) {
            inner_fn fn;
            switch (type) {
                case STORE: fn = [&] (Emitter& e, ssa&, ssa addr) { e.Write(addr, e.state[reg]); }; break;
                case LOAD:  fn = [&] (Emitter& e, ssa&, ssa addr) { e.state[reg] = e.Read(addr); }; break;
                case CMP:   fn = [&] (Emitter& e, ssa&, ssa addr) { /* flags */ }; break;
            }

            auto apply = [&] (size_t sub_op, std::function<ssa(Emitter&)> address_fn) {
                insert(op_base + sub_op, name, [&] (Emitter& e) { ApplyMemoryOperation(e, fn, address_fn(e)); });
            };

            apply(0x04, Direct);
            apply(0x0c, Absolute);

            if (type != CMP)
                apply(0x14, DirectIndex<X>);
            if (type == LOAD)
                apply(0x1c, AbsoluteIndex<X>);
            if (type == LOAD)
                insert(op_base + 0x00, name, [&] (Emitter& e) { e.state[reg] = ReadPcX(e);  });
            if (type == CMP)
                insert(op_base + 0x00, name, [&] (Emitter& e) { ReadPcX(e); /* flags */ });
        };

        idxmem("STY", 0x80, STORE, Y);
        idxmem("STX", 0x82, STORE, X);
        idxmem("LTY", 0xa0, LOAD,  Y);
        idxmem("LTX", 0xa2, LOAD,  X);
        idxmem("CPY", 0xc0, CMP,   Y);
        idxmem("CPX", 0xe0, CMP,   X);
    }


    // Store Zero kind of fits into the above pattern if you squint.
    // But its cleanly been stuffed into free slots.

    // STZ  dir     abs     dir,X    abs,X
    //      64      9c      74       9e

    {
        auto stz = [&] (size_t opcode, std::function<ssa(Emitter&)> address_fn) {
            insert(opcode, "STZ", [&] (Emitter& e) { e.Write(address_fn(e), e.Const<8>(0)); e.IncCycle();});
        };
        stz(0x64, Direct);
        stz(0x9c, Absolute);
        stz(0x74, DirectIndex<X>);
        stz(0x9e, AbsoluteIndex<X>);
    }

    // Operations on Index
    // INX  e8
    // INY  c8
    // DEX  ca
    // DEY  88

    // Transfer operations:
    // TXA  8a
    // TYA  98
    // TXS  9a -- special. Doesn't effect flags
    // TXY  9b
    // TAY  a8
    // TAX  aa
    // TSX  ba
    // TYX  bb

    // TCD  5b
    // TCS  1b
    // TDC  7b
    // TSC  3B

    auto move = [&] (const char* name, size_t opcode, Reg src, Reg dst, bool flags = true) {
        // TODO: Flags!
        insert(opcode, name, [&] (Emitter& e) {
            ssa val = e.state[src];
            ssa truncate = e.state[Flag_M]; // Truncate to 8 bits when M is set
            e.state[dst] = e.Ternary(truncate, e.And(val, e.Const<32>(0xff)), val);
            e.IncCycle();
        });
    };

    auto swap = [&] (const char* name, size_t opcode, Reg a, Reg b) {
        insert(opcode, name, [&] (Emitter& e) {
            ssa c = e.state[a];
            e.state[a] = e.state[b];
            e.state[b] = c;
            e.IncCycle();
         });
    };

    move("TXA", 0x8a, X, A);
    move("TYA", 0x98, Y, A);
    move("TXS", 0x9a, X, S, false); // Only one which doesn't touch flags
    move("TXY", 0x9b, X, Y);
    move("TAX", 0xa8, A, Y);
    move("TAX", 0xaa, A, X);
    move("TSX", 0xba, S, X);
    move("TYX", 0xbb, Y, X);

    auto movefromC = [&] (const char* name, size_t opcode, Reg dst) {
        // Always transfer as 16bits
        insert(opcode, name, [&] (Emitter& e) { e.state[dst] = e.Cat(e.state[B], e.state[A]); e.IncCycle(); });
    };
    auto movetoC = [&] (const char* name, size_t opcode, Reg src) {
        // Always transfer as 16bits
        insert(opcode, name, [&] (Emitter& e) {
            e.state[A] = e.And(e.state[src], e.Const<32>(0xff));
            e.state[B] = e.Shift(e.state[src], 8);
            e.IncCycle();
         });
    };

    movefromC("TCD", 0x5b, D);
    movefromC("TCS", 0x1b, S);
    movetoC(  "TDC", 0x7b, D);
    movetoC(  "TSC", 0x3b, S);

    // XBA -- swap B and A
    swap("XBA", 0xeb, B, A); // TODO: flags????!!!
    // XCE -- swap carry and emu
    swap("XCE", 0xfb, Flag_E, Flag_C);

    // Stack operations


}





void m65816::run_for(int cycles) {
    printf("test\n");

    populate_tables();

    int count = 255;


    printf("     ");
    for(int i = 0; i<16; i++) {
        printf("  0x%x ", i);
    }

    for(int i = 0; i < 16; i++) {
        printf ("\n0x%x  ", i);
        for(int j = 0; j < 16; j++) {
            int op = i << 4 | j;
            printf("%5s ", name_table[op].c_str());

            if(name_table[op] == "") {
                count--;
            }
        }
    }

    printf("\n\n\t\t%i/255\n", count);
}