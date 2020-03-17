
#include "ir_emitter.h"

#include <functional>
#include <map>

#include "memory.h"

std::function<ssa(BaseEmitter&, ssa)> simple_selecter(size_t mask, size_t value) {
     return [mask, value] (BaseEmitter& e, ssa bus_address) {
        return e.Eq(e.And(e.Const<16>(mask), bus_address), e.Const<16>(value));
    };
}

void func() {
    auto view = [&] (const char*, std::function<ssa(BaseEmitter&, ssa)> enable) {

    };

    Bus cpu_bus;

    Memory main_memory(0x800, true);
    cpu_bus.Attach(main_memory.view(simple_selecter(0xe000, 0x0000)));

    view("PPU Registers", [] (BaseEmitter& e, ssa bus_address) {
        return e.Eq(e.And(e.Const<16>(0xe000), bus_address), e.Const<16>(0x2000));
    });
    view("CPU Registers", [] (BaseEmitter& e, ssa bus_address) {
        return e.Eq(e.And(e.Const<16>(0xe000), bus_address), e.Const<16>(0x4000));
    });

    Memory pgr_rom(0x8000, false);
    cpu_bus.Attach(pgr_rom.view(simple_selecter(0xf000, 0x8000)));
    // Mapper zero
}