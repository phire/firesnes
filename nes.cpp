
#include "ir_emitter.h"

#include <functional>
#include <map>

#include "memory.h"

std::function<ssa(BaseEmitter&, ssa)> simple_selecter(size_t mask, size_t value) {
     return [mask, value] (BaseEmitter& e, ssa bus_address) {
        return e.Eq(e.And(e.Const<16>(mask), bus_address), e.Const<16>(value));
    };
}


std::function<ssa(BaseEmitter&, ssa)> reg_selecter(size_t addr) {
    return [addr] (BaseEmitter& e, ssa bus_address) {
        return e.Eq(bus_address, e.Const<16>(addr));
    };
}

struct NesState {
    u8 ppulatch;
    u8 ppuctrl;
    u8 ppumask;
    bool spriteOverflow;
    bool spriteZeroHit;
    bool vsync;

    u8 oamaddr;

    bool ppu_w; // first/second write toggle
    u16  ppu_t; // temporary VRAM address
    u16  ppu_v; // Current VRAM address
    u8   ppu_x; // Fine X scrolling
};

class PPUWriteFnReg : public IRDevice {
public:
    PPUWriteFnReg(size_t addr, DeviceWriteFn writefn) :
        IRDevice(simple_selecter(0xe007, 0x2000 | addr),
            [] (BaseEmitter& e, ssa bus_address) {
                return e.StateRead<8>(offsetof(NesState, ppulatch)); // read return value in latch
            }, writefn) {}
};

class PPUWriteReg : public PPUWriteFnReg {
public:
    PPUWriteReg(size_t addr, size_t stateOffset) :
        PPUWriteFnReg(addr,
            [stateOffset] (BaseEmitter& e, ssa bus_address, ssa value) {
                e.StateWrite<8>(stateOffset, value);
                e.StateWrite<8>(offsetof(NesState, ppulatch), value); // latch keeps the last value written
            }) {}
};

void func() {
    Bus ppu_bus;

    Bus cpu_bus;

    Memory main_memory(0x800, true);
    cpu_bus.Attach(main_memory.view(simple_selecter(0xe000, 0x0000)));



    TransparentDevice ppuLatch( // Record all writes in latch
        simple_selecter(0xe007, 0x2000),
        [] (BaseEmitter& e, ssa bus_address, ssa value) {
            e.StateWrite<8>(offsetof(NesState, ppulatch), value);
        }
    );
    cpu_bus.Attach(ppuLatch);


    PPUWriteFnReg ppuAddr(0x2000,
        [] (BaseEmitter& e, ssa bus_address, ssa value) {
            // update t with nametable
            ssa current_t = e.StateRead<16>(offsetof(NesState, ppu_t));
            ssa lower_2_bits = e.And(value, e.Const<8>(0x03));
            ssa new_t = e.Or(e.And(current_t, e.Const<16>(0x73ff)), e.ShiftLeft(lower_2_bits, 10));

            e.StateWrite<16>(offsetof(NesState, ppu_t), new_t);

            // write remaning
            e.StateWrite<8>(offsetof(NesState, ppuctrl), value));
        });

    PPUWriteReg ppuControl(0x2000, offsetof(NesState, ppuctrl));
    cpu_bus.Attach(ppuControl);

    PPUWriteReg ppuMask(0x2001, offsetof(NesState, ppumask));
    cpu_bus.Attach(ppuMask);

    IRDevice ppuStatus(
        simple_selecter(0xe007, 0x2002),
        [] (BaseEmitter& e, ssa bus_address) {
            ssa V = e.ShiftLeft(e.StateRead<8>(offsetof(NesState, vsync)), 7);
            ssa S = e.ShiftLeft(e.StateRead<8>(offsetof(NesState, spriteZeroHit)), 6);
            ssa O = e.ShiftLeft(e.StateRead<8>(offsetof(NesState, spriteOverflow)), 5);
            ssa latch = e.And(e.StateRead<8>(offsetof(NesState, ppulatch)), e.Const<8>(0x1F));

            e.StateWrite<8>(offsetof(NesState, ppu_w), e.Const<1>(0));

            ssa combined = e.Or(V, e.Or(S, e.Or(O, latch)));

            e.StateWrite<8>(offsetof(NesState, ppulatch), combined);

            return combined;
        },
        [] (BaseEmitter& e, ssa bus_address, ssa value) {}
    );
    cpu_bus.Attach(ppuStatus);


    PPUWriteReg oamAddr(0x2003, offsetof(NesState, oamaddr));
    cpu_bus.Attach(ppuMask);

    //IRDevice oamData(
    //    simple_selecter(0xe007, 0x2004),
    //
    //);

    PPUWriteFnReg ppuScroll(0x2005,
        [] (BaseEmitter& e, ssa bus_address, ssa value) {
            ssa w = e.Eq(e.StateRead<8>(offsetof(NesState, ppu_w)), e.Const<8>(1));

            // toggle w
            e.StateWrite<8>(offsetof(NesState, ppu_w), e.Ternary(w, e.Const<8>(0), e.Const<8>(1)));

            // calculate fine x scrolling
            ssa current_finex = e.StateRead<8>(offsetof(NesState, ppu_x));
            ssa new_finex = e.And(value, e.Const<8>(0x7));
            e.StateWrite<8>(offsetof(NesState, ppu_w), e.Ternary(w, current_finex, new_finex));

            // calculate t
            ssa current_t = e.StateRead<16>(offsetof(NesState, ppu_t));
            ssa upper_5_bits = e.ShiftRight(value, 3);
            ssa first_t = e.Or(e.And(current_t, e.Const<16>(0x7fe0)), upper_5_bits);
            ssa second_t = e.Or(e.And(current_t, e.Const<16>(0x181f)), e.Or(e.ShiftLeft(upper_5_bits, 5), e.ShiftLeft(new_finex, 12)));

            e.StateWrite<16>(offsetof(NesState, ppu_t), e.Ternary(w, second_t, first_t));
        }
    );
    cpu_bus.Attach(ppuScroll);

    PPUWriteFnReg ppuAddr(0x2006,
        [] (BaseEmitter& e, ssa bus_address, ssa value) {
            ssa w = e.Eq(e.StateRead<8>(offsetof(NesState, ppu_w)), e.Const<8>(1));

            // toggle w
            e.StateWrite<8>(offsetof(NesState, ppu_w), e.Ternary(w, e.Const<8>(0), e.Const<8>(1)));

            // calculate t
            ssa current_t = e.StateRead<16>(offsetof(NesState, ppu_t));
            ssa lower_6_bits = e.And(value, e.Const<8>(0x3f));
            ssa first_t = e.Or(e.And(current_t, e.Const<16>(0x00ff)), e.ShiftLeft(lower_6_bits, 8)); // NOTE: 15th bit is cleared
            ssa second_t = e.Or(e.And(current_t, e.Const<16>(0x7f00)), value);

            e.StateWrite<16>(offsetof(NesState, ppu_t), e.Ternary(w, second_t, first_t));

            // write v
            ssa current_v = e.StateRead<16>(offsetof(NesState, ppu_v));
            e.StateWrite<16>(offsetof(NesState, ppu_t), e.Ternary(w, second_t, current_v));
        }
    );
    cpu_bus.Attach(ppuAddr);

    IRDevice ppuData(
        simple_selecter(0xe007, 0x2007),
        [] (BaseEmitter& e, ssa bus_address) {
            // TODO: Ok... now we need to somehow access the ppu Bus, addressed by ppu_v
            return e.Const<8>(0xcc); // placeholder
        },
        [] (BaseEmitter& e, ssa bus_address, ssa value) {
            // Here too
        }
    );
    cpu_bus.Attach(ppuData);


    // Mapper zero
    Memory pgr_rom(0x8000, false);
    cpu_bus.Attach(pgr_rom.view(simple_selecter(0xf000, 0x8000)));
}