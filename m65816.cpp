#include "m65816.h"
#include <stdio.h>
#include <array>
#include <functional>
#include <vector>


typedef bool (*instruction_function)(m65816 &cpu, int cycle);

static void load_pc_16(m65816 &cpu, int cycle, uint16_t &dest) {
    // Reads
    switch (cycle) {
    case 0:
        dest = cpu.read_p(cpu.state.pc++);
        return;
    case 1:
        dest |= cpu.read_p(cpu.state.pc++) << 8;
        return;
    }
}

// template <int start_cycle>
// static void write_16(m65816 &cpu, int cycleuint16_t &src) {
//     // Reads
//     switch (cycle) {
//     case start_cycle:
//         cpu.write_z(cpu.state.pc++);
//         return;
//     case start_cycle + 1:
//         cpu.write_z(cpu.state.pc++) << 8;
//         return;
//     }
// }


// Absolute ADC, AND, BIT, CMP, CPX, CPY, EOR, LDA, LDX, LDY, ORA, SBC, STA, STX, STZ, STZ
template<instruction_function fn>
static bool timing_1a(m65816 &cpu, int cycle) {
    switch (cycle)
    {
    case 2:
    case 3:
        load_pc_16(cpu, cycle-2, cpu.state.aa);
        return false;
        //return cpu.state.p.x | cpu.state.p.m; // Finish early if in 8bit mode
    default:
        return fn(cpu, cycle - 4);
    }
}

// Absolute JMP (4C)
static bool timing_1b(m65816 &cpu, int cycle) {
    switch (cycle)
    {
    case 2:
    case 3:
        load_pc_16(cpu, cycle-2, cpu.state.aa);
        return false;
    case 4:
        cpu.state.pc = cpu.state.aa;
        return true;
    }
}

// Absolute JSR
static bool timing_1c(m65816 &cpu, int cycle) {
    switch (cycle)
    {
    case 2:
    case 3:
        load_pc_16(cpu, cycle-2, cpu.state.aa);
        return false;
    case 4:
        // internal operation
        cpu.read_d(cpu.state.pc - 1); // dummy read
        return false;
    case 5:
        cpu.write_z(cpu.state.s--, cpu.state.pc >> 8);
        return false;
    case 6:
        cpu.write_z(cpu.state.s--, cpu.state.pc & 0xff);
        cpu.state.pc = cpu.state.aa;
        return true;
    }
}

// Absolute Read-Modify-write
// ASL, DEC, INC, LSR, ROL, ROR, TRB, TSB
static bool timing_1d(m65816 &cpu, int cycle) {
    switch (cycle)
    {
    case 2:
    case 3:
        load_pc_16(cpu, cycle - 2, cpu.state.aa);
        return false;
    case 4:
        return true;
    }
}

template<bool fn(m65816 &cpu, int cycle)>
// template<std::function<bool (m65816 cpu, int cycle)>>
static void execute(m65816 &cpu) {
    int cycle = 2;
    while (!fn(cpu, cycle))
        cycle++;
}


//template<bool fn(m65816 &cpu, int cycle)>
constexpr static int cycles(instruction_function fn) {
    int cycle = 2;
    m65816 cpu = {};
    cpu.state.p.x = 0;
    cpu.state.p.m = 0;

    while (!fn(cpu, cycle))
        cycle++;

    return cycle;
}

static bool ora_fn(m65816 &cpu, int cycle) {
    if (cycle == 0) {
        cpu.state.acc.a(cpu.read_d(cpu.state.aa++)) ;
        return cpu.state.p.x | cpu.state.p.m;
        cpu.state.acc.b(cpu.read_d(cpu.state.aa) | cpu.state.acc.a()) ;
    }
    return true;
}

//template bool timing_1a<ora_fn>(m65816 &cpu, int cycle);



struct Instruction {
    const char* name;
    void (*execute)(m65816 &cpu);
    int cycles;
};

const std::array<Instruction, 256> table = []() constexpr -> std::array<Instruction, 256>  {

    std::array<Instruction, 256> T = {};
    struct InstructionDef {
        const char* name;
        uint8_t op_base;
        instruction_function operation;
    };

    bool (*fn)(m65816&, int) = timing_1a<ora_fn>;
    void (*e_fn)(m65816& cpu) = execute<timing_1a<ora_fn>>;

    //InstructionDef ora = { "ORA", template , 0x0 };
    std::array<InstructionDef, 1> universal = {
        { "ORA", 0x0, ora_fn },
       // { "AND", 0x2, (instruction_function) +[] (m65816 cpu, int cycle) constexpr -> bool { return true; } },
        // { "EOR", 0x4, [] (m65816 cpu, int cycle) -> bool { return true; } },
        // { "ADC", 0x6, [] (m65816 cpu, int cycle) -> bool { return true; } },
        // { "STA", 0x8, [] (m65816 cpu, int cycle) -> bool { return true; } },
        // { "LDA", 0xa, [] (m65816 cpu, int cycle) -> bool { return true; } },
        // { "CMP", 0xc, [] (m65816 cpu, int cycle) -> bool { return true; } },
        // { "SBC", 0xe, [] (m65816 cpu, int cycle) -> bool { return true; } },
    };

    for (auto &def : universal )
    {
        T[def.op_base + 0x0d] = {def.name, e_fn, cycles((instruction_function) fn ) };
    }

    return T;
}();




void m65816::run_for(int cycles) {
    printf("test\n");
    printf("%s, %i\n", table[0x0d].name, table[0x0d].cycles);
}