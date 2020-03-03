
#include "m65816_emitter.h"

namespace m65816 {

ssa ReadPc(Emitter& e) {
    ssa data = e.Read(e.Cat(e.state[PBR], e.state[PC]));
    e.IncPC();
    e.IncCycle();
    return data;
}


ssa ReadPc16(Emitter& e) {
    ssa data_low = ReadPc(e);
    ssa data_high = ReadPc(e);
    ssa data = e.Cat(data_high, data_low);
    return data;
}


ssa Absolute(Emitter& e) {
    return e.Cat(e.state[DBR], ReadPc16(e));
}

ssa AbsoluteLong(Emitter& e) {
    ssa low = ReadPc16(e);
    ssa high = ReadPc(e);
    return e.Cat(high, low);
}


// Adds one of the index registers (X or Y) to the address.
// handles adds extra cycles when required by page cross or the X flag.
static ssa AddIndexReg(Emitter& e, Reg reg, ssa address) {
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


template<Reg indexreg>
ssa AbsoluteIndex(Emitter& e) {
    return e.Cat(e.state[DBR], AddIndexReg(e, indexreg, ReadPc16(e)));
}

template ssa AbsoluteIndex<X>(Emitter& e);
template ssa AbsoluteIndex<Y>(Emitter& e);

ssa AbsoluteLongX(Emitter& e) {
    return e.Add(AbsoluteLong(e), e.Cat(e.Const<8>(0), e.state[X]));
}

ssa Direct(Emitter& e) {
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
ssa DirectIndex(Emitter& e) {
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

template ssa DirectIndex<X>(Emitter& e);
template ssa DirectIndex<Y>(Emitter& e);

ssa IndirectDirect(Emitter& e) {
    ssa location = Direct(e);
    e.IncCycle();

    ssa address_low = e.Read(location);
    ssa location_next = e.Add(location, 1);

    e.IncCycle();

    ssa address_high = e.Read(location_next);

    return e.Cat(e.state[DBR], e.Cat(address_high, address_low));
}

ssa IndirectDirectLong(Emitter& e) {
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

ssa StackRelative(Emitter& e) {
    ssa offset = ReadPc16(e);

    // TODO: Dummy read to PBR,PC+1
    e.IncCycle(); // Internal cycle to do add

    return e.Add(e.state[S], offset);
}


}