#include <stdint.h>



struct State {
    uint16_t x;
    uint16_t y;
    uint16_t s; // Stack pointer
    struct {
        uint16_t c;

        uint8_t a() { return c & 0xff; };
        void a(uint8_t a) { c = (c & 0xff00) | a; };
        uint8_t b() { return (c & 0xff00) >> 8; };
        void b(uint8_t b) { c = (c & 0x00ff) | (b << 8); };
    } acc;
    uint16_t pc;
    uint16_t d;   // Direct
    uint16_t pbr; // program bank
    uint16_t dbr; // data bank

    uint16_t aa; // Address latch;

    uint8_t ir;

    union {
        uint8_t hex;

        struct
        {
            uint8_t c: 1; // swaps with emulation
            uint8_t z: 1;
            uint8_t i: 1;
            uint8_t d: 1;
            uint8_t x: 1; // also brk in 6502 mode
            uint8_t m: 1;
            uint8_t v: 1;
            uint8_t n: 1;
        };
    } p;

    // Extra flags
    bool emulation;
};

class Bus {
public:
    uint8_t read(uint32_t address) {
        return 0;
    }

    void write(uint32_t address, uint8_t data) {
        // pass
    }
};

class m65816 {
public:
    void run_for(int cycles);

    uint8_t read_p(uint16_t address) {
        uint32_t full_address = (state.pbr << 16) | address;
        return a_bus->read(full_address);
    }
    uint8_t read_d(uint16_t address) {
        uint32_t full_address = (state.dbr << 16) | address;
        return a_bus->read(full_address);
    }

    void write_z(uint16_t address, uint8_t data) {
        uint32_t full_address = address; // write to zero bank

    }

    State state;
    Bus *a_bus;
};