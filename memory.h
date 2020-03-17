
#include "types.h"
#include "ir_emitter.h"


#include <functional>
#include <vector>

class MemoryView;

class Memory {
    // Memory that exists
    // Either ram or rom

    std::vector<u8> data;
    bool readwrite;

public:
    Memory(size_t size, bool readwrite) : data(size), readwrite(readwrite) {  }

    MemoryView* view(std::function<ssa(BaseEmitter&, ssa)> select) {
        return new MemoryView(this, select);
    }
};

class BusDevice {
    // Generic device on bus for things too complicated to be
    // represented by MemoryView
    // Includes MMIO objects
};

class MemoryView : public BusDevice {
    // Maps a bus address to a Memory object

    // Handles common memory mapping cases, such as bank
    // switching and mirroring

    Memory *mem;
    std::function<ssa(BaseEmitter&, ssa)> select;

public:
    MemoryView(Memory* mem, std::function<ssa(BaseEmitter&, ssa)> select) : mem(mem), select(select) {}

};

class Bus {
    std::vector<BusDevice *> devices;
public:
    void Attach(BusDevice *);
    // Combines multiple BusDevice onto a single bus
};