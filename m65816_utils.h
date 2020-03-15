#pragma once

#include <functional>

namespace m65816 {
class Emitter;

using inner_fn = std::function<void(Emitter&, ssa&, ssa)>;
using rmw_fn = std::function<ssa(Emitter&, ssa, int width)>;

// Given an address, applies a read or write operation.
// Applies the operation twice when M = 0
void ApplyMemoryOperation(Emitter& e, inner_fn operation, ssa address);

// Applies an operation with an immediate argument
// Handles 16bit mode
void ApplyImmediate(Emitter& e, inner_fn operation);

// Applies an operation directly to the Accumulator (A/B)
// Handles 16bit mode
void ApplyAcc(Emitter& e, rmw_fn operation);

// Applies a Read-Write-Modify operation
// Handles 16bit mode
void ApplyModify(Emitter& e, rmw_fn operation, ssa address);

// Calculates zero flag of an 8bit result.
// Chains to 16bits.
// For 16bit chaining, calculate the flags for the low 8 bits first
// then the upper 8 bits
void zero_flag(Emitter &e, ssa result);

// Calculates the Negative and Zero flags for logic operations
// Chains to 16 bits.
// For 16bit chaining, calculate the flags for the low 8 bits first
// then the upper 8 bits
void nz_flags(Emitter &e, ssa result);

// 16 bit calculation of Negative and Zero flags
void nz_flags16(Emitter &e, ssa result);

// Calculates the Negative and Zero flags for logic operations
// Chains to 16 bits.
// For 16bit chaining, calculate the flags for the low 8 bits first
// then the upper 8 bits
void nvz_flags(Emitter &e, ssa result);

// 8 bit add. Matches the ADC instruction. Handles carry and overflow flag
// When doing a 16 bit ADC it chains thorugh Flag C
void add_carry_overflow(Emitter& e, ssa& dst, ssa val);

// 8 bit subtract. Matches the SBC instructions. Handles carry and overflow flag
// When doing a 16 bit SBC it chains thorugh Flag C
void subtract_borrow(Emitter& e, ssa& dst, ssa val);

// 8bit compare.
// Like subtract, but forces carry to 1 and doesn't set result
void compare(Emitter& e, ssa dst, ssa val);

// Increments or Decrements the stack pointer
// Takes into account emulated mode
ssa modifyStack(Emitter& e, int dir);

// Loads a 16 bit value from a regsiter.
// Helper function for instructions that don't need to split 16 bit operations into
// two 8 bit memory operations.
// Handles any complexities with the M and X flags.
ssa loadReg16(Emitter &e, Reg reg, bool force16 = false);

// Stores a 16 bit value to a regsiter.
// Helper function for instructions that don't need to split 16 bit operations into
// two 8 bit memory operations.
// Handles any complexities with the M and X flags.
void storeReg16(Emitter &e, Reg reg, ssa value, bool force16 = false);

// Takes the current flags and packs them into a 8bit value
ssa pack_flags(Emitter& e);

// Unpack flags from a value
void unpack_flags(Emitter& e, ssa val);

}