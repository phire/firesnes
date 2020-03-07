#include <stdio.h>
#include <array>
#include <functional>
#include <vector>

#include "m65816_emitter.h"
#include "m65816.h"
#include "ir_base.h"

namespace m65816 {

Emitter::Emitter(u32 pc) {
    ssa null = Const<32>(0);
    regs = push(IR_MemState(null, null, null));
    auto reg8 =  [&] (Reg r) { return push(IR_Load8( regs, Const<32>(r))); };
    auto reg16 = [&] (Reg r) { return push(IR_Load16(regs, Const<32>(r))); };
    auto reg64 = [&] (Reg r) { return push(IR_Load64(regs, Const<32>(r))); };

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
    state[Flag_N] = reg64(Flag_N);
    state[Flag_V] = reg64(Flag_V);
    state[Flag_M] = reg64(Flag_M);
    state[Flag_X] = reg64(Flag_X);
    state[Flag_D] = reg64(Flag_D);
    state[Flag_I] = reg64(Flag_I);
    state[Flag_Z] = reg64(Flag_Z);
    state[Flag_C] = reg64(Flag_C);
    state[Flag_E] = reg64(Flag_E);
    state[Flag_B] = reg64(Flag_B);
    state[CYCLE]  = reg64(CYCLE);
    state[ALIVE]  = null; // Never actually read.

    //
    initializer_end_marker = buffer.size();

    bus_a = Const<32>(1);
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
    finaliseReg<64>(Flag_B);
    finaliseReg<64>(CYCLE);

    // Todo: how to handle alive?
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
    ssa immediate_address = e.Cat(e.state[PBR], e.state[PC]);
    e.IncPC();
    e.IncCycle();

    operation(e, e.state[A], immediate_address);
    e.IncCycle();

    immediate_address = e.Add(immediate_address, e.Const<24>(1));
    e.IncPC();
    e.IncCycle();

    e.If(e.Not(e.state[Flag_M]), [&] () {
        operation(e, e.state[B], immediate_address);
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
        e.Write(high_address, e.ShiftRight(result, 8));
        e.IncCycle();
    });

    // Writeback Low
    e.Write(address, e.And(result, e.Const<16>(255)));
    e.IncCycle();
}

static void add_carry(Emitter e, ssa& dst, ssa val) {
    ssa result = e.Add(e.Zext32(dst), e.Add(e.Zext32(val), e.state[Flag_C]));
    e.state[Flag_C] = e.ShiftLeft(result, 8);
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
    // TODO: These won't work to well in 16bit mode
    rwm("ASL", 0x00, [] (Emitter& e, ssa val) { return e.ShiftLeft(val, e.Const<32>(1));  });
    rwm("LSR", 0x40, [] (Emitter& e, ssa val) { return e.ShiftRight(val, e.Const<32>(1)); });
    rwm("ROL", 0x20, [] (Emitter& e, ssa val) { return e.ShiftLeft(val, e.Const<32>(1));  });
    rwm("ROR", 0x60, [] (Emitter& e, ssa val) { return e.ShiftRight(val, e.Const<32>(1)); });
    rwm("INC", 0xe0, [] (Emitter& e, ssa val) { return e.Add(val,   e.Const<8>(1));   });
    rwm("DEC", 0xc0, [] (Emitter& e, ssa val) { return e.Add(val,   e.Const<8>(0xff));});

    // Bit instructions:
    //      dir   abs     dir,x   abs,x  !imm!
    // TRB  14    1c
    // TSB  04    0c
    // BIT  24    2c      34      3c     <89>

    auto bit = [&] (const char* name, size_t op_base, rmw_fn fn) {
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
    bit("TRB", 0x10, [] (Emitter& e, ssa val) { return e.And(val, e.Xor(e.Const<8>(255), e.state[A])); });
    bit("TSB", 0x00, [] (Emitter& e, ssa val) { return e.Or(val, e.state[A]); });
    bit("BIT", 0x20, [] (Emitter& e, ssa val) { /* do flags; */ return val; });

    // Bit #imm is diffrent. Doesn't fit with the above encoding and does flags differently.
    insert(0x89, "BIT", [] (Emitter e) {
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
                    switch(type) {
                        case STORE: e.Write(addr, e.Extract(e.state[reg], 0, 8)); break;
                        case LOAD:  e.state[reg] = e.Cat(e.Extract(e.state[reg], 8, 8), e.Read(addr)); break;
                        case CMP:   /* flags */ break;
                    }
                    e.IncCycle();

                    e.If(e.Not(e.state[Flag_X]), [&] () {
                        ssa addr2 = e.Add(addr, 1);
                        switch(type) {
                            case STORE: e.Write(addr2, e.Extract(e.state[reg], 8, 8)); break;
                            case LOAD:  e.state[reg] = e.Cat(e.Read(addr2), e.Extract(e.state[reg], 0, 8)); break;
                            case CMP:   /* flags */ break;
                        }
                        e.IncCycle();
                    });
                });
            };

            apply(0x04, Direct);
            apply(0x0c, Absolute);

            if (type != CMP)
                apply(0x14, reg == X ? DirectIndex<Y> : DirectIndex<X>);
            if (type == LOAD)
                apply(0x1c, reg == X ? AbsoluteIndex<Y> : AbsoluteIndex<X>);
            if (type == LOAD)
                insert(op_base + 0x00, name, [reg] (Emitter& e) { e.state[reg] = ReadPcX(e);  });
            if (type == CMP)
                insert(op_base + 0x00, name, [] (Emitter& e) { ReadPcX(e); /* flags */ });
        };

        idxmem("STY", 0x80, STORE, Y);
        idxmem("STX", 0x82, STORE, X);
        idxmem("LTY", 0xa0, LOAD,  Y);
        idxmem("LTX", 0xa2, LOAD,  X);
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

    // Stack operations


}


void emit(Emitter& e, u8 opcode) {
    // The opcode always gets baked into the IR trace, so we need emit code to check it hasn't changed
    ssa runtime_opcode = ReadPc(e);
    e.Assert(runtime_opcode, e.Const<8>(opcode));

    gen_table[opcode](e);
}

}

void interpeter_loop() {
    memory[0xc000] = 0xa2;
    memory[0xc001] = 0x00;
    memory[0xc002] = 0x86;
    memory[0xc003] = 0x00;
    memory[0xc004] = 0x86;
    memory[0xc005] = 0x10;
    memory[0xc006] = 0x87;
    memory[0xc007] = 0x11;
    memory[0xc008] = 0xea;
    memory[0xc008] = 0xea;

    // Initial register state
    registers[m65816::Flag_M] = 1;
    registers[m65816::Flag_X] = 1;

    u32 pc = 0xc000;
    m65816::Emitter e(pc);

    std::vector<u64> ssalist;
    std::vector<u8> ssatype;
    int offset = 0;

    u8 a = 0;
    u8 x = 0;
    u8 y = 0;
    u64 cycle = 0;

    int count = 4;

    while (count-- > 0) {
        u8 opcode = memory[pc];
        printf("%X %X A:%02X X:%02X Y:%02X CYC: %i\n", pc, opcode, a, x, y, cycle);

        m65816::emit(e, opcode);
        partial_interpret(e.buffer, ssalist, ssatype, offset);
        offset = e.buffer.size();

        pc = ssalist[e.state[m65816::PC].offset];
        a  = ssalist[e.state[m65816::A].offset];
        x  = ssalist[e.state[m65816::X].offset];
        y  = ssalist[e.state[m65816::Y].offset];
        cycle = ssalist[e.state[m65816::CYCLE].offset];
    }

    e.Finalize();
    partial_interpret(e.buffer, ssalist, ssatype, offset);

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