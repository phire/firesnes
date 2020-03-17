
#include <functional>

class Memory {
    // Memory that exists
    // Either ram or rom
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


};

class Bus {
    // Combines multiple BusDevice onto a single bus
};