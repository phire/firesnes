#include "m65816_emitter.h"
#include "m65816_utils.h"

#include <cassert>

namespace m65816 {

// Given an address, applies a read or write operation.
// Applies the operation twice when M = 0
void ApplyMemoryOperation(Emitter& e, inner_fn operation, ssa address) {
    operation(e, e.state[A], address);
    e.IncCycle();

    e.If(e.Not(e.state[Flag_M]), [&] () {
        ssa address2 = e.Add(address, 1);
        operation(e, e.state[B], address2);
        e.IncCycle();
    });
}

// Applies an operation with an immediate argument
// Handles 16bit mode
void ApplyImmediate(Emitter& e, inner_fn operation) {
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

// Applies an operation directly to the Accumulator (A/B)
// Handles 16bit mode
void ApplyAcc(Emitter& e, rmw_fn operation) {
    e.IncCycle();

    // 8 bit version
    e.If(e.state[Flag_M], [&] () {
        e.state[A] = operation(e, e.state[A], 8);
    });

    // 16 bit version
    e.If(e.Not(e.state[Flag_M]), [&] () {
        ssa result = operation(e, e.Cat(e.state[B], e.state[A]), 16);
        e.state[A] = e.Extract(result, 0, 8);
        e.state[B] = e.Extract(result, 8, 8);
        e.IncCycle();
    });
}

// Applies a Read-Write-Modify operation
// Handles 16bit mode
void ApplyModify(Emitter& e, rmw_fn operation, ssa address) {
    ssa low = e.Read(address);
    e.IncCycle();

    // 8 bit version
    e.If(e.state[Flag_M], [&] () {
        ssa result = operation(e, low, 8);
        // TODO: Dummy read to same address as previous=
        e.IncCycle();

        e.Write(address, result);
        e.IncCycle();
    });

    // 16 bit version
    e.If(e.Not(e.state[Flag_M]), [&] () {
        ssa high_address = e.Add(address, e.Const<24>(1));
        ssa high = e.Read(high_address);
        ssa value = e.Cat(high, low);
        e.IncCycle();

        ssa result = operation(e, value, 16);
        // TODO: Dummy read to same address as previous=
        e.IncCycle();

        e.Write(high_address, e.Extract(result, 8, 8));
        e.IncCycle();

        e.Write(address, e.Extract(result, 0, 8));
        e.IncCycle();
    });
}

// Calculates zero flag of an 8bit result.
// Chains to 16bits.
// For 16bit chaining, calculate the flags for the low 8 bits first
// then the upper 8 bits
void zero_flag(Emitter &e, ssa result) {
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

// Calculates the Negative and Zero flags for logic operations
// Chains to 16 bits.
// For 16bit chaining, calculate the flags for the low 8 bits first
// then the upper 8 bits
void nz_flags(Emitter &e, ssa result) {
    e.state[Flag_N] = e.Extract(result, 7, 1);
    zero_flag(e, result);
}

// 16 bit calculation of Negative and Zero flags
void nz_flags16(Emitter &e, ssa result) {
    e.state[Flag_N] = e.Extract(result, 15, 1);
    e.state[Flag_Z] = e.Eq(result, e.Const<16>(0));
}

// Calculates the Negative and Zero flags for logic operations
// Chains to 16 bits.
// For 16bit chaining, calculate the flags for the low 8 bits first
// then the upper 8 bits
void nvz_flags(Emitter &e, ssa result) {
    e.state[Flag_N] = e.Extract(result, 7, 1);
    e.state[Flag_V] = e.Extract(result, 6, 1);
    zero_flag(e, result);
}

// 8 bit add. Matches the ADC instruction. Handles carry and overflow flag
// When doing a 16 bit ADC it chains thorugh Flag C
void add_carry_overflow(Emitter& e, ssa& dst, ssa val) {
    ssa sign_a = e.Extract(dst, 7, 1);
    ssa sign_b = e.Extract(val, 7, 1);

    // TODO: Decimal mode
    ssa result = e.Add(e.Zext<9>(dst), e.Add(e.Zext<9>(val), e.Zext<9>(e.state[Flag_C])));
    e.state[Flag_C] = e.Extract(result, 8, 1);
    dst = e.Extract(result, 0, 8);

    ssa sign_out = e.Extract(dst, 7, 1);
    // Overflow when both input sign bits are diffrent from the output sign bit
    e.state[Flag_V] = e.And(e.Xor(sign_a, sign_out), e.Xor(sign_b, sign_out));
}

// 8 bit subtract. Matches the SBC instructions. Handles carry and overflow flag
// When doing a 16 bit SBC it chains thorugh Flag C
void subtract_borrow(Emitter& e, ssa& dst, ssa val) {
    // TODO: Decimal mode
    // Simply invert one of the arguments and use ADC.
    add_carry_overflow(e, dst, e.Xor(e.Const<8>(0xff), val));
}

// 8bit compare.
// Like subtract, but forces carry to 1 and doesn't set result
void compare(Emitter& e, ssa dst, ssa val) {
    ssa inverted = e.Xor(e.Const<8>(0xff), val);
    ssa result = e.Add(e.Zext<9>(dst), e.Add(e.Zext<9>(inverted), e.Const<9>(1)));
    e.state[Flag_C] = e.Extract(result, 8, 1);
    nz_flags(e, e.Extract(result, 0, 8));
}

// Increments or Decrements the stack pointer
// Takes into account emulated mode
ssa modifyStack(Emitter& e, int dir) {
    ssa native_stack = e.Add(e.state[S], e.Const<16>(u32(dir) & 0xffff));

    // The emulated stack is forced into the 0x0100 to 0x01ff range on E bit toggle
    // and kept in that range after any stack update during emulated mode
    ssa emulated_stack = e.Cat(e.Const<8>(0x01), e.Extract(native_stack, 0, 8));
    e.state[S] = e.Ternary(e.state[Flag_E], emulated_stack, native_stack);
    return e.state[S];
}

// Loads a 16 bit value from a regsiter.
// Helper function for instructions that don't need to split 16 bit operations into
// two 8 bit memory operations.
// Handles any complexities with the M and X flags.
ssa loadReg16(Emitter &e, Reg reg, bool force16) {
    switch (reg) {
    case A:
        // It appears the full 16bits of A and B are always placed on the internal bus independent of M.
        // Most of the time it doesn't matter, as the memory subsystem will only write 8 bits.
        // but B will end up in the upper bits of a destination register during some transfer operations
        return e.Cat(e.state[B], e.state[A]);
    case X:
    case Y:
        if (force16)
            return e.state[reg];
        // The upper bits are forced to zero when Flag_X are set to
        return e.Ternary(e.state[Flag_X], e.Cat(e.Const<8>(0), e.Extract(e.state[reg], 0, 8)), e.state[reg]);
    case PBR:
    case DBR: {
        // These registers are always 8bit
        return e.Cat(e.Const<8>(0), e.state[reg]);
    }
    case S:
    case D:
        // These registers are always 16bit
        return e.state[reg];
    default:
        assert(false);
    }
}

// Stores a 16 bit value to a regsiter.
// Helper function for instructions that don't need to split 16 bit operations into
// two 8 bit memory operations.
// Handles any complexities with the M and X flags.
void storeReg16(Emitter &e, Reg reg, ssa value, bool force16) {
    switch (reg) {
    case A: {
        ssa low = e.Extract(value, 0, 8);
        ssa high = e.Extract(value, 8, 8);

        // A is always modified
        e.state[A] = low;

        if (force16) { // When an operation is forced to 16bits, B is always modified
            e.state[B] = high;
            nz_flags16(e, value); // All writes to A/B modify flags
            return;
        }
        nz_flags(e, low); // All writes to A/B modify flags
        // otherwise B is only modified when M is 0;
        e.If(e.Not(e.state[Flag_M]), [&] () {
            e.state[B] = high;
            nz_flags(e, high);
        });
        return;
    }
    case X:
    case Y: {
        ssa old_upper = e.Extract(e.state[reg], 8, 8);

        // Do the 16bit write first
        e.state[reg] = value;
        nz_flags16(e, value); // All writes to A/B modify flags
        if (force16)
            return;

        // Fall back to an 8bit write when Flag_X is 1
        e.If(e.state[Flag_X], [&] () {
            ssa low = e.Extract(value, 0, 8);
            e.state[reg] = e.Cat(old_upper, low);
            nz_flags(e, low);
        });
        return;
    }
    case PBR:
    case DBR: {
        // These registers are always 8bit
        ssa low = e.Extract(value, 0, 8);
        e.state[reg] = low;
        nz_flags(e, low);
        return;
    }
    case S: {
        // Updates to S don't update flags
        // In emulation mode, the upper bits are forced to 0x0100
        ssa low = e.Extract(value, 0, 8);
        e.state[reg] = e.Ternary(e.state[Flag_E], e.Cat(e.Const<8>(0x01), low), value);
        return;
    }
    case D:
        e.state[reg] = value; // D is always 16 bit
        nz_flags16(e, value);
        return;
    default:
        assert(false);
    }
}

// Takes the current flags and packs them into a 8bit value
ssa pack_flags(Emitter& e) {
    ssa n = e.ShiftLeft(e.state[Flag_N], 7);
    ssa v = e.Zext<8>(e.ShiftLeft(e.state[Flag_V], 6));
    ssa m = e.Zext<8>(e.ShiftLeft(e.Ternary(e.state[Flag_E], e.Const<1>(1), e.state[Flag_M]), 5));
    ssa x = e.Zext<8>(e.ShiftLeft(e.Ternary(e.state[Flag_E], e.Const<1>(1), e.state[Flag_X]), 4));
    ssa d = e.Zext<8>(e.ShiftLeft(e.state[Flag_D], 3));
    ssa i = e.Zext<8>(e.ShiftLeft(e.state[Flag_I], 2));
    ssa z = e.Zext<8>(e.ShiftLeft(e.state[Flag_Z], 1));
    ssa c = e.Zext<8>(e.state[Flag_C]);

    // Zip all the flags together
    return e.Or(e.Or(e.Or(n, v), e.Or(m, x)), e.Or(e.Or(d, i), e.Or(z, c)));
}

// Unpack flags from a value
void unpack_flags(Emitter& e, ssa val) {
    e.state[Flag_N] = e.Extract(val, 7, 1);
    e.state[Flag_V] = e.Extract(val, 6, 1);
    e.state[Flag_M] = e.Ternary(e.state[Flag_E], e.state[Flag_M], e.Extract(val, 5, 1));
    e.state[Flag_X] = e.Ternary(e.state[Flag_E], e.state[Flag_X], e.Extract(val, 4, 1));
    e.state[Flag_D] = e.Extract(val, 3, 1);
    e.state[Flag_I] = e.Extract(val, 2, 1);
    e.state[Flag_Z] = e.Extract(val, 1, 1);
    e.state[Flag_C] = e.Extract(val, 0, 1);
}

}