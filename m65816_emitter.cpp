

#include "m65816_emitter.h"

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

}