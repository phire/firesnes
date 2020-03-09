#pragma once

#include "ir_base.h"
//#include "m65816_emitter.h"

namespace m65816 {

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
    CYCLE, // Not a register, but lets pretend.
    NUM_REGS
};

class Emitter;

// Address Modes

ssa ReadPc(Emitter& e);

ssa ReadPc16(Emitter& e);

ssa Absolute(Emitter& e);

ssa AbsoluteLong(Emitter& e);

template<Reg indexreg>
ssa AbsoluteIndex(Emitter& e);

ssa AbsoluteLongX(Emitter& e);

ssa Direct(Emitter& e);

template<Reg indexreg>
ssa DirectIndex(Emitter& e);

ssa IndirectDirect(Emitter& e);

ssa IndirectDirectLong(Emitter& e);

ssa StackRelative(Emitter& e);


}