#include <stdio.h>
#include <array>
#include <functional>
#include <vector>
#include <cassert>

#include "m65816_emitter.h"
#include "m65816.h"
#include "ir_base.h"

namespace m65816 {

Emitter::Emitter(u32 pc) {
    ssa null = Const<32>(0);
    ssa one  = Const<32>(1);
    regs = push(IR_MemState(null, null, one));
    auto reg8 =  [&] (Reg r) { return push(IR_Load8( regs, Const<32>(r))); };
    auto reg16 = [&] (Reg r) { return push(IR_Load16(regs, Const<32>(r))); };
    auto reg64 = [&] (Reg r) { return push(IR_Load64(regs, Const<32>(r))); };
    auto flag =  [&] (Reg r) { return Extract(push(IR_Load64(regs, Const<32>(r))), 0, 1); };

    // Create SSA nodes for all state regs
    state[A]      = reg8(A);
    state[B]      = reg8(B);
    state[D]      = reg16(D);
    state[X]      = reg16(X);
    state[Y]      = reg16(Y);
    state[S]      = reg16(S);
    state[PC]     = Const<16>(pc & 0xffff); // PC is baked in to blocks
    state[DBR]    = reg8(DBR);
    state[PBR]    = Const<8>((pc >> 16) & 0xff);
    state[Flag_N] = flag(Flag_N);
    state[Flag_V] = flag(Flag_V);
    state[Flag_M] = flag(Flag_M);
    state[Flag_X] = flag(Flag_X);
    state[Flag_D] = flag(Flag_D);
    state[Flag_I] = flag(Flag_I);
    state[Flag_Z] = flag(Flag_Z);
    state[Flag_C] = flag(Flag_C);
    state[Flag_E] = flag(Flag_E);
    state[CYCLE]  = reg64(CYCLE);

    //
    initializer_end_marker = buffer.size();

    bus_a = one;
    memory_conditional = one;
}

template<u8 bits> void Emitter::finaliseReg(Reg reg) {
    // We only want to write regs which have changed
    if (state[reg].offset >= initializer_end_marker) {
        ssa offset = Const<32>(reg);

        if constexpr(bits ==  8) push( IR_Store8(regs, offset, state[reg]));
        if constexpr(bits == 16) push(IR_Store16(regs, offset, state[reg]));
        if constexpr(bits == 64) push(IR_Store64(regs, offset, state[reg]));
    }
}

void Emitter::Finalize() {
    finaliseReg<8>(A);
    finaliseReg<8>(B);
    finaliseReg<16>(D);
    finaliseReg<16>(X);
    finaliseReg<16>(Y);
    finaliseReg<16>(S);
    finaliseReg<16>(PC);
    finaliseReg<8>(DBR);
    finaliseReg<8>(PBR);
    finaliseReg<64>(Flag_N);
    finaliseReg<64>(Flag_V);
    finaliseReg<64>(Flag_M);
    finaliseReg<64>(Flag_X);
    finaliseReg<64>(Flag_D);
    finaliseReg<64>(Flag_I);
    finaliseReg<64>(Flag_Z);
    finaliseReg<64>(Flag_C);
    finaliseReg<64>(Flag_E);
    finaliseReg<64>(CYCLE);
}

// Reads 8 or 16 bytes from PC depending on the Reg register
static ssa ReadPc(Emitter& e, Reg reg) {
    ssa low = ReadPc(e);

    ssa wide = e.Not(e.state[reg]);

    ssa high;
    e.If(wide, [&] () {
        high = ReadPc(e);
    });

    return e.Cat(e.Ternary(wide, high, e.Const<8>(0)), low);
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
    ssa immediate_address = e.Cat(e.state[PBR], e.state[PC]);
    e.IncPC();
    e.IncCycle();

    operation(e, e.state[A], immediate_address);

    e.If(e.Not(e.state[Flag_M]), [&] () {
        immediate_address = e.Add(immediate_address, e.Const<24>(1));
        e.IncPC();
        e.IncCycle();
        operation(e, e.state[B], immediate_address);
    });
}


using rmw_fn = std::function<ssa(Emitter&, ssa)>;

static void ApplyAcc(Emitter& e, rmw_fn operation) {
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
    ssa value = e.Cat(e.Ternary(word, high, e.Const<8>(0)), low);

    // Internal operation
    // TODO: Dummy read to same address as previous
    ssa result = operation(e, value);
    e.IncCycle();

    // Writeback High
    e.If(word, [&] () {
        e.Write(high_address, e.ShiftRight(result, 8));
        e.IncCycle();
    });

    // Writeback Low
    e.Write(address, e.And(result, e.Const<16>(255)));
    e.IncCycle();
}

// Calculates zero flag of an 8bit result. Chains to 16bits
static void zero_flag(Emitter &e, ssa result) {
    ssa zero = e.Eq(result, e.Const<8>(0));

    // As a hack we stash the result of the previous zero flag calculation in emitter
    // It gets reset before every instruction.
    // If it's present, then this must be the upper 8 bits
    if (e.zero_lower) {
        // And we And the result of the upper and lower bits to get our 16bit zero flag
        e.state[Flag_Z] = e.And(zero, *e.zero_lower);
    } else {
        // Otherwise, stash this result away.
        e.zero_lower = zero;
        e.state[Flag_Z] = zero;
    }
}

static void nz_flags(Emitter &e, ssa result) {
    e.state[Flag_N] = e.Extract(result, 7, 1);
    zero_flag(e, result);
}

static void nvz_flags(Emitter &e, ssa result) {
    e.state[Flag_N] = e.Extract(result, 7, 1);
    e.state[Flag_V] = e.Extract(result, 6, 1);
    zero_flag(e, result);
}

static void add_carry(Emitter& e, ssa& dst, ssa val) {
    ssa result = e.Add(e.Extract(dst, 0, 9), e.Add(e.Extract(val, 0, 9), e.state[Flag_C]));
    e.state[Flag_C] = e.ShiftLeft(result, 8);
    dst = e.Extract(result, 0, 8);
}

static ssa modifyStack(Emitter& e, int dir) {
    ssa native_stack = e.Add(e.state[S], e.Const<16>(u32(dir) & 0xffff));

    // The emulated stack is forced into the 0x0100 to 0x01ff range on E bit toggle
    // and kept in that range after any stack update during emulated mode
    ssa emulated_stack = e.Cat(e.Const<8>(0x01), e.Extract(native_stack, 0, 8));
    e.state[S] = e.Ternary(e.state[Flag_E], emulated_stack, native_stack);
    return e.state[S];
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
            insert(op_base | sub_op, name, [fn, address_fn] (Emitter& e) {ApplyMemoryOperation(e, fn, address_fn(e)); });
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
            insert(op_base + 0x09, name, [fn] (Emitter& e) { ApplyImmediate(e, fn); });
        }
    };

    universal("ORA", 0x00, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.Or(reg, e.Read(addr)); nz_flags(e, reg); });
    universal("AND", 0x20, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.And(reg, e.Read(addr)); nz_flags(e, reg); });
    universal("EOR", 0x40, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.Xor(reg, e.Read(addr)); nz_flags(e, reg); });
    universal("ADC", 0x60, [] (Emitter& e, ssa& reg, ssa addr) { add_carry(e, reg, e.Read(addr)); nvz_flags(e, reg); });
    universal("STA", 0x80, [] (Emitter& e, ssa& reg, ssa addr) { e.Write(addr, reg); }); // Doesn't modify flags
    universal("LDA", 0xa0, [] (Emitter& e, ssa& reg, ssa addr) { reg = e.Read(addr); nz_flags(e, reg); });
    universal("CMP", 0xc0, [] (Emitter& e, ssa& reg, ssa addr) { ssa temp; add_carry(e, temp, e.Read(addr)); nz_flags(e, temp); });
    // TODO: Check correctness of SBC
    universal("SBC", 0xe0, [] (Emitter& e, ssa& reg, ssa addr) { add_carry(e, reg, e.Xor(e.Read(addr), e.Const<8>(255))); nz_flags(e, reg); });

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
            insert(op_base + sub_op, name, [fn, address_fn] (Emitter& e) { ApplyModify(e, fn, address_fn(e)); });
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
    rwm("ASL", 0x00, [] (Emitter& e, ssa val) { return e.ShiftLeft(val, e.Const<32>(1));  });
    rwm("LSR", 0x40, [] (Emitter& e, ssa val) { return e.ShiftRight(val, e.Const<32>(1)); });
    rwm("ROL", 0x20, [] (Emitter& e, ssa val) { return e.ShiftLeft(val, e.Const<32>(1));  });
    rwm("ROR", 0x60, [] (Emitter& e, ssa val) { return e.ShiftRight(val, e.Const<32>(1)); });
    rwm("INC", 0xe0, [] (Emitter& e, ssa val) { return e.Add(val,   e.Const<16>(1)); });
    rwm("DEC", 0xc0, [] (Emitter& e, ssa val) { return e.Sub(val,   e.Const<16>(1)); });

    // Bit instructions:
    //      dir   abs     dir,x   abs,x  !imm!
    // TRB  14    1c
    // TSB  04    0c
    // BIT  24    2c      34      3c     <89>

    auto bit_rmw = [&] (const char* name, size_t op_base, rmw_fn fn) {
        auto apply = [&] (size_t sub_op, std::function<ssa(Emitter&)> address_fn) {
            insert(op_base + sub_op, name, [fn, address_fn] (Emitter& e) { ApplyModify(e, fn, address_fn(e)); });
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
    // FIXME: wrong
    //bit_rmw("TRB", 0x10, [] (Emitter& e, ssa val) { return e.And(val, e.Xor(e.Const<8>(255), e.state[A])); });
    //bit_rmw("TSB", 0x00, [] (Emitter& e, ssa val) { return e.Or(val, e.state[A]); });

    {
        auto apply_bit = [&] (size_t sub_op, std::function<ssa(Emitter&)> address_fn) {
            insert(0x20 | sub_op, "BIT", [address_fn] (Emitter& e) {
                auto fn = [] (Emitter &e, ssa &dst, ssa address) {
                    ssa val =  e.Read(address);
                    e.state[Flag_N] = e.Extract(val, 7, 1);
                    e.state[Flag_V] = e.Extract(val, 6, 1);
                    ssa result = e.And(dst, val);
                    zero_flag(e, result);
                };
                ApplyMemoryOperation(e, fn, address_fn(e));
            });
        };

        apply_bit(0x04, Direct);
        apply_bit(0x0c, Absolute);
        apply_bit(0x14, DirectIndex<X>);
        apply_bit(0x1c, AbsoluteIndex<X>);
        insert(0x89, "BIT", [] (Emitter &e) {
            ssa value = ReadPcM(e);
            ssa acc_high = e.Ternary(e.state[Flag_M], e.Const<8>(0), e.state[B]);
            ssa acc = e.Cat(acc_high, e.state[A]);

            ssa result = e.And(value, acc);

            // Only sets Z
            e.state[Flag_Z] = e.Eq(result, e.Const<16>(0));
        });
    }

    // Index<-->Memory instructions:
    //      dir     abs     dir.X/Y  abs.X/Y   imm
    // STY  84      8c      94       --        --
    // STX  86      8e      96       --        --
    // LDY  a4      ac      b4 (X)   bc (X)    a0
    // LDX  a6      ae      b6 (Y)   be (Y)    a2
    // CPY  c4      cc      --       --        c0
    // CPX  e4      ec      --       --        e0

    // NOTE: Index registers are swapped.

    {
        enum idxmem_type {
            STORE,
            LOAD,
            CMP,
        };

        auto idxmem = [&] (const char* name, size_t op_base, idxmem_type type, Reg reg) {
            auto apply = [&] (size_t sub_op, std::function<ssa(Emitter&)> address_fn) {
                insert(op_base + sub_op, name, [type, reg, address_fn] (Emitter& e) {
                    ssa addr = address_fn(e);
                    ssa val_low;
                    if (type == STORE) {
                         e.Write(addr, e.Extract(e.state[reg], 0, 8));
                    } else {
                            val_low = e.Read(addr);
                            if (type == LOAD) {
                                e.state[reg] = e.Cat(e.Const<8>(0), val_low);
                            }
                            nz_flags(e, val_low);
                    }
                    e.IncCycle();

                    e.If(e.Not(e.state[Flag_X]), [&] () {
                        ssa addr2 = e.Add(addr, 1);
                        if (type == STORE) {
                            e.Write(addr2, e.Extract(e.state[reg], 8, 8));
                        } else {
                            ssa val_high = e.Read(addr);
                            if (type == LOAD) {
                                e.state[reg] = e.Cat(val_high, val_low);
                            }
                            nz_flags(e, val_high);
                        }
                        e.IncCycle();
                    });
                });
            };

            apply(0x04, Direct);
            apply(0x0c, Absolute);

            if (type != CMP)
                apply(0x14, reg == X ? DirectIndex<Y> : DirectIndex<X>);
            if (type == LOAD) {
                apply(0x1c, reg == X ? AbsoluteIndex<Y> : AbsoluteIndex<X>);

                insert(op_base + 0x00, name, [reg] (Emitter& e) {
                    ssa low = ReadPc(e);
                    nz_flags(e, low);

                    ssa wide = e.Not(e.state[Flag_X]);

                    ssa high;
                    e.If(wide, [&] () {
                        high = ReadPc(e);
                        nz_flags(e, high);
                    });

                    e.state[reg] = e.Cat(e.Ternary(wide, high, e.Const<8>(0)), low);
                });
            }
            if (type == CMP) {
                insert(op_base + 0x00, name, [reg] (Emitter& e) {
                    ssa result = e.state[reg];
                    add_carry(e, result, ReadPcX(e));
                    nz_flags(e, result);
                });
            }
        };

        idxmem("STY", 0x80, STORE, Y);
        idxmem("STX", 0x82, STORE, X);
        idxmem("LDY", 0xa0, LOAD,  Y);
        idxmem("LDX", 0xa2, LOAD,  X);
        idxmem("CPY", 0xc0, CMP,   Y);
        idxmem("CPX", 0xe0, CMP,   X);
    }

    // STZ  dir     abs     dir,X    abs,X
    //      64      9c      74       9e

    // Store Zero kind of fits into the above Index<-->Memory pattern if you squint.
    // But its cleanly been stuffed into free slots.

    {
        auto stz = [&] (size_t opcode, std::function<ssa(Emitter&)> address_fn) {
            insert(opcode, "STZ", [address_fn] (Emitter& e) { e.Write(address_fn(e), e.Const<8>(0)); e.IncCycle();});
        };
        stz(0x64, Direct);
        stz(0x9c, Absolute);
        stz(0x74, DirectIndex<X>);
        stz(0x9e, AbsoluteIndex<X>);
    }

    // Implied Operations on Index:
    // DEY  88
    // INY  c8
    // DEX  ca
    // INX  e8

    auto inc = [&] (const char* name, size_t opcode, Reg index, int dir) {
        insert(opcode, name, [&] (Emitter& e) {
            ssa mask = e.Const<32>(0xffff); // Always 16bit
            e.state[X] = e.And(e.Add(e.state[X], e.Const<32>(dir)), mask);

            // TODO: Dummy read to PC + 1
            e.IncCycle(); // Internal operation;
        } );
    };

    inc("DEY", 0x88, Y, -1);
    inc("INY", 0xc8, Y,  1);
    inc("DEX", 0xca, X, -1);
    inc("INX", 0xe8, X,  1);

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
        insert(opcode, name, [src, dst] (Emitter& e) {
            ssa val = e.state[src];
            ssa truncate = e.state[Flag_M]; // Truncate to 8 bits when M is set
            e.state[dst] = e.Ternary(truncate, e.And(val, e.Const<32>(0xff)), val);
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
        insert(opcode, name, [dst] (Emitter& e) { e.state[dst] = e.Cat(e.state[B], e.state[A]); e.IncCycle(); });
    };
    auto movetoC = [&] (const char* name, size_t opcode, Reg src) {
        // Always transfer as 16bits
        insert(opcode, name, [src] (Emitter& e) {
            e.state[A] = e.And(e.state[src], e.Const<32>(0xff));
            e.state[B] = e.ShiftLeft(e.state[src], 8);
            e.IncCycle();
         });
    };

    movefromC("TCD", 0x5b, D);
    movefromC("TCS", 0x1b, S);
    movetoC(  "TDC", 0x7b, D);
    movetoC(  "TSC", 0x3b, S);

    auto swap = [&] (const char* name, size_t opcode, Reg a, Reg b) {
        insert(opcode, name, [a, b] (Emitter& e) {
            ssa c = e.state[a];
            e.state[a] = e.state[b];
            e.state[b] = c;
            e.IncCycle();
         });
    };

    // XBA -- swap B and A
    swap("XBA", 0xeb, B, A); // TODO: flags????!!!
    // XCE -- swap carry and emu
    swap("XCE", 0xfb, Flag_E, Flag_C);

    // Flag Modification Instructions:

    auto flag = [&] (const char* name, size_t opcode, Reg flag, int value) {
        insert(opcode, name, [flag, value] (Emitter& e) {
            e.state[flag] = e.Const<1>(value);
            // TODO: Dummy read to PC+1
            e.IncCycle();
        });
    };

    flag("CLC", 0x18, Flag_C, 0);
    flag("SEC", 0x38, Flag_C, 1);
    flag("CLI", 0x58, Flag_I, 0);
    flag("SEI", 0x78, Flag_I, 1);
    flag("CLV", 0xb8, Flag_V, 0);
    flag("CLD", 0xd8, Flag_D, 0);
    flag("SED", 0xf8, Flag_D, 1);

    // Stack operations

    // Unconditional Jump Instructions:
    //       a    al   (a)   (a,x)
    // JMP   4c   5c   6c    7c
    // JML             dc
    // JSR   20              fc
    // JSL        22

    // No real pattern to extract here.

    auto jump = [&] (const char* name, size_t opcode, std::function<ssa(Emitter&)> address_fn, bool subroutine) {
        insert(opcode, name, [address_fn, subroutine] (Emitter& e) {
            ssa long_address = address_fn(e);
            if (subroutine) {
                // TODO: Dummy Read to PBR,PC+2
                e.IncCycle(); // Internal operation

                ssa low =  e.Extract(e.state[PC], 0, 8);
                ssa high =  e.Extract(e.state[PC], 8, 8);
                e.Write(e.Cat(e.Const<8>(0), e.state[S]), low);
                e.IncCycle();

                modifyStack(e, -1);
                e.Write(e.Cat(e.Const<8>(0), e.state[S]), high);
                e.IncCycle();

                modifyStack(e, -1);
            }
            e.state[PC] = e.Extract(long_address, 0, 16);
            e.state[PBR] = e.Extract(long_address, 16, 8);
            e.MarkBlockEnd();
        });
    };

    jump("JMP", 0x4c, Absolute, false);
    jump("JMP", 0x5c, AbsoluteLong, false);
    //jump("JMP", 0x6c, AbsoluteIndirect, false);
    //jump("JMP", 0x7c, AbsoluteIndexedXIndirect, false);
    //jump("JML", 0x5c, AbsoluteIndirectLong, false);
    jump("JSR", 0x20, Absolute, true);
    //jump("JSR", 0xfc, AbsoluteIndexedXIndirect, true);
    //jump("JSL", 0x22, AbsoluteIndirectLong, true);

    insert(0x60, "RTS", [] (Emitter& e) {
        // TODO: Dummy Read to PBR,PC+1
        e.IncCycle(); // Internal operation

        modifyStack(e, +1);

        // TODO: Dummy Read to PBR,PC+1
        e.IncCycle(); // Internal operation

        ssa high = e.Read(e.Cat(e.Const<8>(0), e.state[S]));
        modifyStack(e, +1);
        e.IncCycle();

        ssa low  = e.Read(e.Cat(e.Const<8>(0), e.state[S]));
        e.IncCycle();

        e.state[PC] = e.Cat(high, low);
        e.MarkBlockEnd();
        // TODO: Dummy Read to S
        e.IncCycle(); // Internal operation
    });

    // Conditional Branch Instructions:

    auto branch = [&] (const char* name, size_t opcode, std::function<ssa(Emitter&)> condition_fn) {
        insert(opcode, name, [condition_fn] (Emitter& e) {
            ssa cond = condition_fn(e);
            ssa offset = ReadPc(e);
            e.If(cond, [&] () {
                ssa old_pc = e.state[PC];
                e.state[PC] = e.Add(e.state[PC], e.Cat(e.Const<8>(0), offset));
                e.IncCycle(); // Extra cycle when branch taken
                e.If(e.state[Flag_E], [&] () {
                    // In emulation mode, an extra cycle is taken when a branch crosses a page boundary
                    e.If(e.Neq(e.Extract(old_pc, 8, 8), e.Extract(e.state[PC], 8, 8)), [&] () {
                        e.state[CYCLE] = e.Add(e.state[CYCLE], e.Const<64>(1));
                    });
                });
            });

            e.MarkBlockEnd();
        });
    };

    branch("BPL", 0x10, [] (Emitter& e) { return e.Not(e.state[Flag_N]); });
    branch("BMI", 0x30, [] (Emitter& e) { return e.state[Flag_N]; });
    branch("BCV", 0x50, [] (Emitter& e) { return e.Not(e.state[Flag_V]); });
    branch("BSV", 0x70, [] (Emitter& e) { return e.state[Flag_V]; });
    branch("BRA", 0x80, [] (Emitter& e) { return e.Const<1>(1); });
    branch("BCC", 0x90, [] (Emitter& e) { return e.Not(e.state[Flag_C]); });
    branch("BCS", 0xB0, [] (Emitter& e) { return e.state[Flag_C]; });
    branch("BNE", 0xD0, [] (Emitter& e) { return e.Not(e.state[Flag_Z]); });
    branch("BEQ", 0xF0, [] (Emitter& e) { return e.state[Flag_Z]; });

    // Nop Instruction:
    insert(0xea, "NOP", [] (Emitter& e) {
        // TODO: Dummy read to PBR,PC+1
        e.IncCycle();
    });
}


void emit(Emitter& e, u8 opcode) {
    // The opcode always gets baked into the IR trace, so we need emit code to check it hasn't changed
    ssa runtime_opcode = ReadPc(e);
    e.Assert(runtime_opcode, e.Const<8>(opcode));

    e.zero_lower.reset();

    gen_table[opcode](e);
}

}

void interpeter_loop() {

    // Initial register state
    registers[m65816::Flag_M] = 1;
    registers[m65816::Flag_X] = 1;
    registers[m65816::Flag_E] = 1;
    registers[m65816::Flag_I] = 1;
    registers[m65816::S] = 0x01fd;

    u32 pc = 0xc000;
    m65816::Emitter e(pc);

    std::vector<u64> ssalist;
    std::vector<u8> ssatype;
    int offset = 0;

    u8 a = 0;
    u16 x = 0;
    u16 y = 0;
    u8  p = 0x24;
    u8 sp = 0xfd;
    u64 cycle = 0;


    int count = 300;

    while (count-- > 0) {


        u8 opcode = memory[pc];

        u64 nes_cycle    = (cycle * 3) % 341;
        u64 nes_scanline = (341 * 241 + (cycle * 3)) / 341;
        printf("%X  %X A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%3i SL:%3i\n", pc, opcode, a, x, y, p, sp, nes_cycle, nes_scanline);

        m65816::emit(e, opcode);
        partial_interpret(e.buffer, ssalist, ssatype, offset);
        offset = e.buffer.size();

        // Extract PC so we know the next instruction
        pc = ssalist[e.state[m65816::PC].offset];

        // Extact other registers for debugging:
        a  = ssalist[e.state[m65816::A].offset];
        x  = ssalist[e.state[m65816::X].offset];
        y  = ssalist[e.state[m65816::Y].offset];
        sp = ssalist[e.state[m65816::S].offset] & 0xFF;
        cycle = ssalist[e.state[m65816::CYCLE].offset];
        p = ssalist[e.state[m65816::Flag_N].offset] << 7
          | ssalist[e.state[m65816::Flag_V].offset] << 6
          | 1 << 5
          | 0 << 4
          | ssalist[e.state[m65816::Flag_D].offset] << 3
          | ssalist[e.state[m65816::Flag_I].offset] << 2
          | ssalist[e.state[m65816::Flag_Z].offset] << 1
          | ssalist[e.state[m65816::Flag_C].offset] << 0;

        if (e.ending) {
            printf("End of block\n");
            e.Finalize();
            partial_interpret(e.buffer, ssalist, ssatype, offset);
            e = m65816::Emitter(pc);
            offset = 0;

            ssalist.resize(0);
            ssatype.resize(0);
        }
    }

    e.Finalize();
    partial_interpret(e.buffer, ssalist, ssatype, offset);

}

void load_nestest() {
    FILE *f = fopen("nestest.nes", "rb");
    assert(f > 0);

    fseek(f, 16, SEEK_SET); // Skip header

    // Read 16 kilobytes to memory[0xc000]
    for (int i = 0; i < 0x4000; i++) {
        char byte;
        fread(&byte, 1, 1, f);
        memory[0xc000 + i] = byte;
        memory[0x8000 + i] = byte; // And it's mirror
    }

    fclose(f);
}

int main(int, char**) {
    printf("test\n");

    m65816::populate_tables();

    int count = 255;


    printf("     ");
    for(int i = 0; i<16; i++) {
        printf("  0x%x ", i);
    }

    m65816::Emitter e(0);

    for(int i = 0; i < 16; i++) {
        printf ("\n0x%x  ", i);
        for(int j = 0; j < 16; j++) {
            int op = i << 4 | j;
            printf("%5s ", m65816::name_table[op].c_str());

            if(m65816::name_table[op] == "") {
                count--;
            } else {
                //m65816::emit(e, op);
            }
        }
    }

    printf("\n\n\t\t%i/255\n", count);

    load_nestest();

    interpeter_loop();
    return 0;

    m65816::emit(e,  0xe9);

    e.Finalize();

    auto code = e.buffer;

    auto printarg = [&] (u16 arg) {
        // exclude null args
        if (arg == 0xffff) return;

        IR_Base ir = code[arg];

        if (ir.is<IR_Const32>()) {
            auto value = ir.cast<IR_Const32>();
            printf(" %c%i(%i)", value->is_signed ? 's' : 'u', value->num_bits, value->arg_32);
        }
        else {
            printf(" ssa%i", arg);
        }
    };

    for (int i = 0; i < code.size(); i++) {
        IR_Base ir = code[i];
        if (ir.id < 0x8000) {
            printf("% 3i: %s", i, OpcodeName(ir.id));
            printarg(ir.arg_1);
            printarg(ir.arg_2);
            printarg(ir.arg_3);
            printf("\n");
        } else if (ir.id == 0x8000) {
            printf("const48 %x\n", ir.arg_48);
        } else if (ir.id == Const) {
            // Don't print consts, because printarg inlines them
            //printf("const%i %x\n", ir.num_bits, ir.arg_32);
        }
    }

}