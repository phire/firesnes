
#include "types.h"
#include "ir_emitter.h"


#include <functional>
#include <vector>

using SelectorFn = std::function<ssa(BaseEmitter&, ssa)>; // IsSelected(address) -> bool

 // address will contain the pre-selector address, useful if a device covers multiple addresses
using DeviceReadFn = std::function<ssa(BaseEmitter&, ssa)>; // Read(address) -> ssa
using DeviceWriteFn = std::function<void(BaseEmitter&, ssa, ssa)>; // Write(address, function)

class BusDevice {
    // Generic device on bus for things too complicated to be
    // represented by MemoryView
    // Includes MMIO objects

    SelectorFn select;

public:
    BusDevice(SelectorFn select) : select(select) {}
};

class Memory;

class MemoryView : public BusDevice {
    // Maps a bus address to a Memory object

    // Handles common memory mapping cases, such as bank
    // switching and mirroring

    Memory *mem;

public:
    MemoryView(Memory* mem, SelectorFn select) : BusDevice(select), mem(mem) {}

};

class Memory {
    // Memory that exists
    // Either ram or rom

    std::vector<u8> data;
    bool readwrite;

public:
    Memory(size_t size, bool readwrite) : data(size), readwrite(readwrite) {  }

    MemoryView* view(SelectorFn select) {
        return new MemoryView(this, select);
    }
};

template<typename T>
class StateDevice : public BusDevice {
    // Simplistic device that updates some internal state
    // Can be read-back. Masking prevents some values from

    u32 stateOff;
    T default_value;
    T read_mask;

public:
    StateDevice(SelectorFn selecter, size_t stateOff, T default_value = 0) :
        BusDevice(selecter), stateOff(stateOff), default_value(default_value), read_mask() {
            read_mask = std::numeric_limits<T>::max(); // All bits
        }

    StateDevice(SelectorFn selecter, size_t stateOff, T default_value, T read_mask) :
        BusDevice(selecter), stateOff(stateOff), default_value(default_value), read_mask(read_mask) {}
};

class IRDevice : public BusDevice {
    // Device that can do anything

    DeviceReadFn readFn;
    DeviceWriteFn writeFn;
public:
    IRDevice(SelectorFn selecter, DeviceReadFn read, DeviceWriteFn write) :
        BusDevice(selecter), readFn(read), writeFn(write) {}
};

class TransparentDevice : public BusDevice {
    // can monitor writes, selector can overlap other devices. Can't be read

    DeviceWriteFn writeFn;
public:
    TransparentDevice(SelectorFn selecter, DeviceWriteFn write) :
        BusDevice(selecter), writeFn(write) {}
};

class Bus {
    std::vector<BusDevice *> devices;
public:
    void Attach(BusDevice *);
    void Attach(BusDevice& device) { Attach(&device); }
    // Combines multiple BusDevice onto a single bus
};