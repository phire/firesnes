
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


ssa Absolute(Emitter& e, bool is_store) {
    return e.Cat(e.state[DBR], ReadPc16(e));
}

ssa AbsoluteLong(Emitter& e, bool is_store) {
    ssa low = ReadPc16(e);
    ssa high = ReadPc(e);
    return e.Cat(high, low);
}

// Adds two addresses, wrapping on the page boundary depending on flag
static ssa AddWrapped(Emitter&e, ssa a, ssa b, Reg flag) {
    ssa wrap = e.state[flag];

    ssa address = e.Add(a, b);
    return e.Ternary(wrap, e.Cat(e.Extract(a, 8, 8), e.Extract(address, 0, 8)), address);
}

// Adds one of the index registers (X or Y) to the address.
// handles adds extra cycles when required by page cross or the X flag.
static ssa AddIndexReg(Emitter& e, Reg reg, ssa address, bool is_store) {
    ssa index = e.state[reg];
    ssa new_address = e.Add(address, index);

    // See if the upper bits change
    ssa mask = e.Const<16>(0xff00);
    ssa page_cross = e.Neq(e.And(new_address, mask), e.And(address,mask));

    // Always takes an extra cycle on store
    if (is_store) {
        e.IncCycle();
    } else {
        // Takes an extra cycle when index is 16bit or an 8bit index crosses a page boundary
        e.If(e.Or(page_cross, e.Not(e.state[Flag_X])), [&] {
            // TODO: Dummy read to DBR,AAH,AAL+XL
            e.IncCycle();
        });
    }

    return new_address;
}

template<Reg indexreg>
ssa AbsoluteIndex(Emitter& e, bool is_store) {
    return e.Cat(e.state[DBR], AddIndexReg(e, indexreg, ReadPc16(e), is_store));
}

template ssa AbsoluteIndex<X>(Emitter& e, bool is_store);
template ssa AbsoluteIndex<Y>(Emitter& e, bool is_store );

ssa AbsoluteLongX(Emitter& e, bool is_store) {
    return e.Add(AbsoluteLong(e), e.Cat(e.Const<8>(0), e.state[X]));
}

static ssa Direct16(Emitter & e) {
    ssa offset = ReadPc(e);
    ssa DL_cycle = e.Neq(e.Const<8>(0), e.Extract(e.state[D], 0, 8));

    e.If(DL_cycle, [&] {
        // TODO: Dummy read to PBR,PC+1
        e.IncCycle();
    });

    return e.Add(e.state[D], offset);
}

ssa Direct(Emitter& e, bool is_store) {
    return e.Cat(e.Const<8>(0), Direct16(e));
}

template<Reg indexreg>
ssa DirectIndex(Emitter& e, bool is_store) {
    ssa direct = Direct16(e);
    ssa address = AddWrapped(e, direct, e.state[indexreg], Flag_X);
    e.IncCycle();

    return e.Cat(e.Const<8>(0), address);
}

template ssa DirectIndex<X>(Emitter& e, bool is_store = false);
template ssa DirectIndex<Y>(Emitter& e, bool is_store = false);

ssa IndirectDirect(Emitter& e, bool is_store) {
    ssa location = Direct(e);
    e.IncCycle();

    ssa address_low = e.Read(location);
    ssa location_next = e.Add(location, 1);

    e.IncCycle();

    ssa address_high = e.Read(location_next);

    return e.Cat(e.state[DBR], e.Cat(address_high, address_low));
}

ssa IndirectDirectLong(Emitter& e, bool is_store) {
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

ssa IndirectDirectIndexX(Emitter& e, bool is_store) {
    ssa location = DirectIndex<X>(e);
    e.IncCycle();

    ssa address_low = e.Read(location);
    ssa location_next = AddWrapped(e, location, e.Const<24>(1), Flag_E);
    e.IncCycle();

    ssa address_high = e.Read(location_next);

    return e.Cat(e.state[DBR], e.Cat(address_high, address_low));
}

ssa IndexYIndirectDirect(Emitter& e, bool is_store) {
    ssa location = Direct(e);
    e.IncCycle();

    ssa address_low = e.Read(location);
    ssa location_next = AddWrapped(e, location, e.Const<24>(1), Flag_E);
    e.IncCycle();

    ssa address_high = e.Read(location_next);
    ssa address = e.Cat(address_high, address_low);

    ssa indexed_address = e.Add(address, e.state[Y]);
    // TODO: Dummy Read to AAH,AAL+YL

    ssa overflow = e.Neq(address_high, e.Extract(indexed_address, 8, 8));
    if (is_store) {
        e.IncCycle();
    } else {
        e.If(overflow, [&] {
            e.IncCycle();
        });
    }

    return e.Cat(e.state[DBR], indexed_address);
}


ssa IndirectAbsolute(Emitter& e, bool is_store) {
    ssa location = Absolute(e);
    e.IncCycle();

    ssa address_low = e.Read(location);
    ssa location_next = AddWrapped(e, location, e.Const<24>(1), Flag_E);

    e.IncCycle();

    ssa address_high = e.Read(location_next);

    return e.Cat(e.state[DBR], e.Cat(address_high, address_low));
}

ssa StackRelative(Emitter& e, bool is_store ) {
    ssa offset = ReadPc16(e);

    // TODO: Dummy read to PBR,PC+1
    e.IncCycle(); // Internal cycle to do add

    return e.Cat(e.Const<8>(0), e.Add(e.state[S], offset));
}

}