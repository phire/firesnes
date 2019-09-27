#include "m65816.h"
#include "literalfn.h"
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

template<bool (*fn)(m65816, int)>
// template<std::function<bool (m65816 cpu, int cycle)>>
//template<LiteralFn<bool(m65816, int), 128> &fn>
static void Execute(m65816 &cpu) {
    int cycle = 2;
    while (!fn(cpu, cycle))
        cycle++;
}

static void StepIRLoad(m65816 &cpu);

template<LiteralFn<bool(m65816, int), 128> &fn>
static void Step(m65816 &cpu) {
    bool end = fn(cpu, cpu.state.cycle);
    if (end)
        cpu.state.stepfn = StepIRLoad;
}


//template<bool fn(m65816 &cpu, int cycle)>
//constexpr static int cycles(LiteralFn<bool(m65816, int), 128> &fn) {
constexpr static int cycles(bool (*fn)(m65816, int)) {
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
    void (*execute)(m65816 &cpu); // Function that executes full instruciton
    //void (*step)(m65816 &cpu); // Fuction that only executes a single cycle
    //std::function<void (m65816 &cpu)> execute;
    //LiteralFn<void (m65816 &cpu)> execute;
    int cycle_count;

    constexpr Instruction() : name(""), cycle_count(0), execute(nullptr) {};

    //Instruction(char *name, LiteralFn<bool(m65816, int), 128> &fn)
    template<bool (*fn)(m65816, int)>
    constexpr Instruction(char *name)
    : name(name),
      cycle_count(cycles(fn)) {
        //void (*e_fn)(m65816& cpu) = (void (*)(m65816 &cpu))
        execute = Execute<fn>;
        //step = Step<fn>;

      }
};

using ReadFn = uint8_t(*)();
using WriteFn = void(*)(uint8_t);

template<bool (*fn)(m65816, int)>
bool AbsoluteA(m65816, int) {

}

void

const std::array<Instruction, 256> table = []() constexpr -> std::array<Instruction, 256>  {
    struct InstructionDef {
        const char* name;
        uint8_t op_base;
        //LiteralFn<void (State, ReadFn, WriteFn), 128> fn;
        void (*fn)(State, ReadFn, WriteFn);
    };

    std::array<InstructionDef, 8> universal = {
        { "ORA", 0x00, [] (State state, uint8_t &a, ReadFn readfn, WriteFn writefn) { a |= readfn(); } },
        { "AND", 0x20, [] (State state, uint8_t &a, ReadFn readfn, WriteFn writefn) { a &= readfn(); } },
        { "EOR", 0x40, [] (State state, uint8_t &a, ReadFn readfn, WriteFn writefn) { a ^= readfn(); } },
        { "ADC", 0x60, [] (State state, uint8_t &a, ReadFn readfn, WriteFn writefn) { uint16_t sum = a + readfn() + state.p.c;
                                                                         a = sum & 0xff; state.p.c = !!(sum & 0x100); } },
        { "STA", 0x80, [] (State state, uint8_t &a, ReadFn readfn, WriteFn writefn) { writefn(a); } },
        { "LDA", 0xa0, [] (State state, uint8_t &a, ReadFn readfn, WriteFn writefn) { a = readfn(); } },
        //{ "CMP", 0xc0, [] (State state, uint8_t &a, ReadFn readfn, WriteFn writefn) { compare(state.p, a, readfn()); } },
        //{ "SBC", 0xe0, [] (State state, uint8_t &a, ReadFn readfn, WriteFn writefn) { substract_a(state, readfn()); },
    };

    std::array<Instruction, 256> T = {};
    for (auto &def : universal )
    {
        bool (*aa)(m65816, int) = AbsoluteA<def.fn>;
        T[def.op_base + 0x0d] = Instruction<aa>(def.name);        // a
        // T[def.op_base + 0x1d] = Instruction(def.name, AbsoluteA_X<def.fn>);      // a,x
        // T[def.op_base + 0x19] = Instruction(def.name, AbsoluteA_Y<def.fn>);      // a, y
        // T[def.op_base + 0x0f] = Instruction(def.name, Absolute_LongA<def.fn>);   // al
        // T[def.op_base + 0x1f] = Instruction(def.name, Absolute_LongA_X<def.fn>); // al,x
        // T[def.op_base + 0x05] = Instruction(def.name, Direct<def.fn>);           // d
        // T[def.op_base + 0x03] = Instruction(def.name, StackRelative<def.fn>);    // d,s
        // T[def.op_base + 0x15] = Instruction(def.name, DirectIndexedY<def.fn>);   // d,x
        // T[def.op_base + 0x12] = Instruction(def.name, DirectIndirect<def.fn>);     // (d)
        // T[def.op_base + 0x07] = Instruction(def.name, DirectIndirectLong<def.fn>); // [d]
        // T[def.op_base + 0x13] = Instruction(def.name, StackRelativeIndirectIndexed<def.fn>); // (d,s),y
        // T[def.op_base + 0x01] = Instruction(def.name, DirectIndexedIndirect<def.fn>);     // (d,x)
        // T[def.op_base + 0x11] = Instruction(def.name, DirectIndirectIndexed<def.fn>);     // (d),y
        // T[def.op_base + 0x17] = Instruction(def.name, DirectIndirectLongIndexed<def.fn>); // [d],y
        // if (def.op_base != 0x80) {
        //     T[def.op_base + 0x09] = Instruction(def.name, Immediate<def.fn>);    // #
        // }
    }

    return T;
}();




void m65816::run_for(int cycles) {
    printf("test\n");
    printf("%s, %i\n", table[0x0d].name, table[0x0d].cycle_count);
}