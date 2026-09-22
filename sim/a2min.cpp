// a2min -- a minimal 65816 machine wired directly to the Verilated a2nix card.
//
// WHY THIS EXISTS
// ---------------
// M2 is a claim about the CARD: that real 65816 machine code, executing real bus
// cycles, drives the real RTL to a store path identical to upstream nix. It is not a
// claim about Apple's firmware.
//
// MAME can only start apple2gs if it has all four ROMs, and one of them
// (344s0047.bin, region "gfx1") is the character-generator font -- irrelevant to a
// headless run that reads its answer out of memory, but still mandatory. Those ROMs
// are copyrighted and not redistributable, so a MAME-only M2 could never run in CI.
//
// So this harness supplies the smallest thing that can execute the real binary: RAM,
// a slot decoder, and a 65816 interpreter. No ROM, no OS, no copyrighted material --
// and it runs the *same* .bin and the *same* RTL.
//
// This is a TEST HARNESS, not an emulator. It is not an Apple IIgs and does not
// pretend to be: no video, no ROM, no soft switches, no interrupts. MAME remains the
// higher-fidelity confirmation for when a full romset is available (real slot timing,
// real GS/OS, the genuine FPI clock stretch).
//
// NATIVE MODE
// -----------
// Both modes are modelled. Emulation mode (E=1) is the 6502-compatible 8-bit machine
// the early demos use. Native mode (E=0) is what the L4 bytecode VM needs: the M and
// X flags select 16-bit accumulator and index widths, the stack pointer and direct
// page register are 16 bits, and the data/program bank registers give a 24-bit
// address space with the long addressing modes. A 64-bit-int lazy evaluator in 8-bit
// mode would be a dishonest baseline; the 16-bit registers are the reason to be on a
// 65816 at all.
//
// Unknown opcodes ABORT rather than being skipped. A silently-ignored instruction
// would produce a wrong digest that still looks like a hash, which is the failure
// mode this whole project is most careful about. Decimal mode aborts for the same
// reason, and so does anything interrupt-related: none of that is modelled, and a
// program that reaches it is outside what this harness can vouch for.
//
// CYCLE COUNTS are the WDC data-sheet base costs: 2 per instruction, plus the
// addressing-mode cost, plus one per extra byte moved when a register is 16 bits
// wide, plus one for direct-page modes when the D register's low byte is non-zero.
// Not modelled: the page-crossing penalty on indexed reads, and the 6502
// direct-page wrap in emulation mode. They exist so the CARD:CPU ratio -- which is
// what the verb-economics gate measures -- is right to within a few percent, not so
// that the harness could pass as a cycle-exact emulator.

#include "Va2nix_card.h"
#include "verilated.h"

#include "cardbus.h"      // tag constants, for verifying the promoted graph
#include "heapmem.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// --- Timing ---------------------------------------------------------------------
// The card runs on its own 50 MHz oscillator; the IIgs CPU runs at 2.8 MHz, and any
// $Cxxx access is stretched to 1.024 MHz by the FPI. So:
constexpr int CARD_CYCLES_PER_CPU_CYCLE = 18;  // 50 MHz / 2.8 MHz
constexpr int CARD_CYCLES_PER_SLOT_ACCESS = 48; // 50 MHz / 1.024 MHz

constexpr uint32_t RAM_BYTES   = 0x100000;      // 1 MB: banks $00-$0F
constexpr uint16_t LOAD_ADDR   = 0x2000;
constexpr uint16_t HALT_ADDR   = 0xFFFE;        // RTS from the entry point lands here
constexpr uint16_t RESULT      = 0x0300;
constexpr uint16_t RESULT_STAT = 0x0320;
constexpr uint16_t SELFTEST_RESULT = 0x0400;    // the native self-test's word table

int card_slot = 4;

Va2nix_card *card = nullptr;
HeapMem<Va2nix_card> heap;          // the card's off-chip heap, distinct from CPU RAM
std::vector<uint8_t> ram(RAM_BYTES);// the 65816's own address space

uint64_t card_cycles = 0;
uint64_t cpu_cycles = 0;
uint64_t slot_accesses = 0;

// See the port contract in sim/heapmem.h for why settle/commit straddle the edge.
void tick() {
    heap.settle(card);
    card->clk = 0;
    card->eval();
    card->clk = 1;
    card->eval();
    heap.commit(card);
    ++card_cycles;
}

void advance(int n) {
    while (n-- > 0) tick();
}

[[noreturn]] void die(const char *what, uint32_t where) {
    std::fprintf(stderr, "a2min: %s at $%06X\n", what, where);
    std::exit(3);
}

// --- Slot I/O (bank 0 only) --------------------------------------------------------
bool is_slot_io(uint32_t a) { return a >= 0xC080 && a <= 0xC0FF; }
int  slot_of(uint32_t a) { return static_cast<int>((a & 0xF0) >> 4) - 8; }

uint8_t card_read(uint8_t off) {
    card->c0n_addr  = off;
    card->c0n_read  = 1;
    card->c0n_write = 0;
    card->eval();
    const uint8_t v = card->c0n_rdata;
    tick();                               // commit the auto-increment
    card->c0n_read = 0;
    advance(CARD_CYCLES_PER_SLOT_ACCESS - 1);
    ++slot_accesses;
    return v;
}

void card_write(uint8_t off, uint8_t v) {
    card->c0n_addr  = off;
    card->c0n_wdata = v;
    card->c0n_write = 1;
    card->c0n_read  = 0;
    tick();
    card->c0n_write = 0;
    advance(CARD_CYCLES_PER_SLOT_ACCESS - 1);
    ++slot_accesses;
}

// --- 24-bit memory ---------------------------------------------------------------
uint8_t rd8(uint32_t a) {
    a &= 0xFFFFFF;
    if (is_slot_io(a)) {
        if (slot_of(a) == card_slot) return card_read(a & 0x0F);
        return 0x00;                      // empty slot: the scan must see nothing
    }
    if (a >= RAM_BYTES) die("read outside modelled RAM", a);
    return ram[a];
}

void wr8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (is_slot_io(a)) {
        if (slot_of(a) == card_slot) card_write(a & 0x0F, v);
        return;
    }
    if (a >= RAM_BYTES) die("write outside modelled RAM", a);
    ram[a] = v;
}

uint16_t rd16(uint32_t a) { return static_cast<uint16_t>(rd8(a) | (rd8(a + 1) << 8)); }
void     wr16(uint32_t a, uint16_t v) { wr8(a, v & 0xFF); wr8(a + 1, v >> 8); }
uint32_t rd24(uint32_t a) { return rd16(a) | (static_cast<uint32_t>(rd8(a + 2)) << 16); }

// --- CPU ------------------------------------------------------------------------
struct Cpu {
    uint16_t a = 0;         // C. In 8-bit mode the high byte (B) is preserved.
    uint16_t x = 0, y = 0;
    uint16_t sp = 0x01FF;
    uint16_t dp = 0;        // direct page register D
    uint8_t  dbr = 0, pbr = 0;
    uint16_t pc = LOAD_ADDR;
    bool c = false, z = false, n = false, v = false;
    bool dec = false, irq = false;
    bool m = true, xf = true;   // M and X width flags: 1 = 8-bit
    bool e = true;              // emulation mode
    bool halted = false;
    uint64_t instructions = 0;
} cpu;

bool m8() { return cpu.e || cpu.m; }    // accumulator is 8 bits wide
bool x8() { return cpu.e || cpu.xf; }   // index registers are 8 bits wide

// Cycle-cost modifiers: one extra cycle per extra byte moved, and one for a direct
// page that is not page-aligned.
int wA() { return m8() ? 0 : 1; }
int wX() { return x8() ? 0 : 1; }
int dl() { return (cpu.dp & 0xFF) ? 1 : 0; }

uint32_t pcaddr() { return (static_cast<uint32_t>(cpu.pbr) << 16) | cpu.pc; }
uint8_t  fetch8() { const uint8_t v = rd8(pcaddr()); ++cpu.pc; return v; }
uint16_t fetch16() { const uint8_t lo = fetch8(); const uint8_t hi = fetch8(); return static_cast<uint16_t>(lo | (hi << 8)); }
uint32_t fetch24() { const uint16_t lo = fetch16(); const uint8_t hi = fetch8(); return lo | (static_cast<uint32_t>(hi) << 16); }
uint16_t fetchA() { return m8() ? fetch8() : fetch16(); }
uint16_t fetchX() { return x8() ? fetch8() : fetch16(); }

// Width-aware register access.
uint16_t getA() { return m8() ? (cpu.a & 0xFF) : cpu.a; }
void setA(uint16_t v) { if (m8()) cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00) | (v & 0xFF)); else cpu.a = v; }
uint16_t maskA() { return m8() ? 0xFF : 0xFFFF; }
uint16_t signA() { return m8() ? 0x80 : 0x8000; }
uint16_t maskX() { return x8() ? 0xFF : 0xFFFF; }
uint16_t signX() { return x8() ? 0x80 : 0x8000; }
void setX(uint16_t v) { cpu.x = v & maskX(); }
void setY(uint16_t v) { cpu.y = v & maskX(); }

void nz_w(uint16_t v, uint16_t mask, uint16_t sign) { v &= mask; cpu.z = (v == 0); cpu.n = (v & sign) != 0; }
void nzA(uint16_t v) { nz_w(v, maskA(), signA()); }
void nzX(uint16_t v) { nz_w(v, maskX(), signX()); }

uint16_t rdA(uint32_t ea) { return m8() ? rd8(ea) : rd16(ea); }
void     wrA(uint32_t ea, uint16_t v) { if (m8()) wr8(ea, v & 0xFF); else wr16(ea, v); }
uint16_t rdX(uint32_t ea) { return x8() ? rd8(ea) : rd16(ea); }
void     wrX(uint32_t ea, uint16_t v) { if (x8()) wr8(ea, v & 0xFF); else wr16(ea, v); }

// Stack, always bank 0. In emulation mode the pointer lives in page 1.
void fix_sp() { if (cpu.e) cpu.sp = static_cast<uint16_t>(0x0100 | (cpu.sp & 0xFF)); }
void push8(uint8_t v) { wr8(cpu.sp, v); --cpu.sp; fix_sp(); }
uint8_t pop8() { ++cpu.sp; fix_sp(); return rd8(cpu.sp); }
void push16(uint16_t v) { push8(v >> 8); push8(v & 0xFF); }
uint16_t pop16() { const uint8_t lo = pop8(); const uint8_t hi = pop8(); return static_cast<uint16_t>(lo | (hi << 8)); }

// Processor status. Native: N V M X D I Z C. Emulation: bits 5 and 4 read as 1.
uint8_t packP() {
    return static_cast<uint8_t>((cpu.n << 7) | (cpu.v << 6) |
                                ((cpu.e ? 1 : cpu.m) << 5) | ((cpu.e ? 1 : cpu.xf) << 4) |
                                (cpu.dec << 3) | (cpu.irq << 2) | (cpu.z << 1) | (cpu.c ? 1 : 0));
}
void unpackP(uint8_t p) {
    cpu.n = p & 0x80; cpu.v = p & 0x40;
    if (!cpu.e) {
        cpu.m = p & 0x20;
        cpu.xf = p & 0x10;
        if (cpu.xf) { cpu.x &= 0xFF; cpu.y &= 0xFF; }   // narrowing clears the high bytes
    }
    cpu.dec = p & 0x08; cpu.irq = p & 0x04; cpu.z = p & 0x02; cpu.c = p & 0x01;
}

// --- Effective addresses (24-bit) ---------------------------------------------------
uint32_t bank(uint8_t b, uint16_t off) { return (static_cast<uint32_t>(b) << 16) | off; }
uint32_t dbr_(uint16_t off) { return bank(cpu.dbr, off); }

uint32_t ea_dp()        { return static_cast<uint16_t>(cpu.dp + fetch8()); }
uint32_t ea_dp_x()      { return static_cast<uint16_t>(cpu.dp + fetch8() + cpu.x); }
uint32_t ea_dp_y()      { return static_cast<uint16_t>(cpu.dp + fetch8() + cpu.y); }
uint32_t ea_dp_ind()    { return dbr_(rd16(static_cast<uint16_t>(cpu.dp + fetch8()))); }
uint32_t ea_dp_x_ind()  { return dbr_(rd16(static_cast<uint16_t>(cpu.dp + fetch8() + cpu.x))); }
uint32_t ea_dp_ind_y()  { return (dbr_(rd16(static_cast<uint16_t>(cpu.dp + fetch8()))) + cpu.y) & 0xFFFFFF; }
uint32_t ea_dp_long()   { return rd24(static_cast<uint16_t>(cpu.dp + fetch8())); }
uint32_t ea_dp_long_y() { return (rd24(static_cast<uint16_t>(cpu.dp + fetch8())) + cpu.y) & 0xFFFFFF; }
uint32_t ea_abs()       { return dbr_(fetch16()); }
uint32_t ea_abs_x()     { return (dbr_(fetch16()) + cpu.x) & 0xFFFFFF; }
uint32_t ea_abs_y()     { return (dbr_(fetch16()) + cpu.y) & 0xFFFFFF; }
uint32_t ea_long()      { return fetch24(); }
uint32_t ea_long_x()    { return (fetch24() + cpu.x) & 0xFFFFFF; }
uint32_t ea_sr()        { return static_cast<uint16_t>(cpu.sp + fetch8()); }
uint32_t ea_sr_ind_y()  { return (dbr_(rd16(static_cast<uint16_t>(cpu.sp + fetch8()))) + cpu.y) & 0xFFFFFF; }

// --- ALU helpers, width-generic ------------------------------------------------------
void do_adc(uint16_t mval) {
    if (cpu.dec) die("decimal-mode ADC is not modelled", pcaddr());
    const uint32_t a = getA(), s = a + mval + (cpu.c ? 1u : 0u);
    cpu.v = (~(a ^ mval) & (a ^ s) & signA()) != 0;
    cpu.c = s > maskA();
    setA(static_cast<uint16_t>(s & maskA()));
    nzA(static_cast<uint16_t>(s));
}

void do_sbc(uint16_t mval) {
    if (cpu.dec) die("decimal-mode SBC is not modelled", pcaddr());
    const uint32_t a = getA(), s = a - mval - (cpu.c ? 0u : 1u);
    cpu.v = ((a ^ mval) & (a ^ s) & signA()) != 0;
    cpu.c = !(s & (static_cast<uint32_t>(maskA()) + 1));
    setA(static_cast<uint16_t>(s & maskA()));
    nzA(static_cast<uint16_t>(s));
}

void do_cmp(uint16_t r, uint16_t mval, uint16_t mask, uint16_t sign) {
    r &= mask; mval &= mask;
    cpu.c = r >= mval;
    nz_w(static_cast<uint16_t>(r - mval), mask, sign);
}

// Shifts and rotates on an A-width value.
uint16_t sh_asl(uint16_t v) { cpu.c = (v & signA()) != 0; const uint16_t r = static_cast<uint16_t>((v << 1) & maskA()); nzA(r); return r; }
uint16_t sh_lsr(uint16_t v) { v &= maskA(); cpu.c = v & 1; const uint16_t r = v >> 1; nzA(r); return r; }
uint16_t sh_rol(uint16_t v) { const bool cin = cpu.c; cpu.c = (v & signA()) != 0; const uint16_t r = static_cast<uint16_t>(((v << 1) | (cin ? 1 : 0)) & maskA()); nzA(r); return r; }
uint16_t sh_ror(uint16_t v) { v &= maskA(); const bool cin = cpu.c; cpu.c = v & 1; const uint16_t r = static_cast<uint16_t>((v >> 1) | (cin ? signA() : 0)); nzA(r); return r; }
uint16_t sh_inc(uint16_t v) { const uint16_t r = static_cast<uint16_t>((v + 1) & maskA()); nzA(r); return r; }
uint16_t sh_dec(uint16_t v) { const uint16_t r = static_cast<uint16_t>((v - 1) & maskA()); nzA(r); return r; }

void rmw(uint32_t ea, uint16_t (*f)(uint16_t)) { wrA(ea, f(rdA(ea))); }

int branch(bool take) {
    const int8_t off = static_cast<int8_t>(fetch8());
    if (take) { cpu.pc = static_cast<uint16_t>(cpu.pc + off); return 3; }
    return 2;
}

// The eight "group 1" operations share fifteen addressing modes selected by the low
// five opcode bits. Decoding them in one place covers 119 opcodes without 119 cases.
// Returns the addressing-mode cycle cost (data-sheet cost minus the 2-cycle base), or
// -1 if the low bits are not a group-1 mode. `imm` is set for immediate mode.
int group1_ea(uint8_t op, uint32_t &ea, bool &imm) {
    imm = false;
    switch (op & 0x1F) {
        case 0x01: ea = ea_dp_x_ind();  return 4 + dl();
        case 0x03: ea = ea_sr();        return 2;
        case 0x05: ea = ea_dp();        return 1 + dl();
        case 0x07: ea = ea_dp_long();   return 4 + dl();
        case 0x09: imm = true;          return 0;
        case 0x0D: ea = ea_abs();       return 2;
        case 0x0F: ea = ea_long();      return 3;
        case 0x11: ea = ea_dp_ind_y();  return 3 + dl();
        case 0x12: ea = ea_dp_ind();    return 3 + dl();
        case 0x13: ea = ea_sr_ind_y();  return 5;
        case 0x15: ea = ea_dp_x();      return 2 + dl();
        case 0x17: ea = ea_dp_long_y(); return 4 + dl();
        case 0x19: ea = ea_abs_y();     return 2;
        case 0x1D: ea = ea_abs_x();     return 2;
        case 0x1F: ea = ea_long_x();    return 3;
        default: return -1;
    }
}

void group1_op(uint8_t op, uint32_t ea, bool imm) {
    const uint8_t which = op >> 5;      // ORA AND EOR ADC STA LDA CMP SBC
    if (which == 4) {                   // STA
        wrA(ea, getA());
        return;
    }
    const uint16_t mval = imm ? fetchA() : rdA(ea);
    switch (which) {
        case 0: setA(getA() | mval); nzA(getA()); break;
        case 1: setA(getA() & mval); nzA(getA()); break;
        case 2: setA(getA() ^ mval); nzA(getA()); break;
        case 3: do_adc(mval); break;
        case 5: setA(mval); nzA(mval); break;
        case 6: do_cmp(getA(), mval, maskA(), signA()); break;
        case 7: do_sbc(mval); break;
        default: break;
    }
}

void step() {
    if (cpu.pbr == 0 && (cpu.pc == HALT_ADDR || cpu.pc == HALT_ADDR + 1)) { cpu.halted = true; return; }

    const uint32_t op_pc = pcaddr();
    const uint8_t op = fetch8();
    int cyc = 2;
    ++cpu.instructions;

    switch (op) {
    // --- loads and stores outside group 1 ---
    case 0xA2: setX(fetchX());              nzX(cpu.x); cyc += wX(); break;                // LDX #
    case 0xA6: setX(rdX(ea_dp()));          nzX(cpu.x); cyc += 1 + dl() + wX(); break;     // LDX dp
    case 0xB6: setX(rdX(ea_dp_y()));        nzX(cpu.x); cyc += 2 + dl() + wX(); break;     // LDX dp,Y
    case 0xAE: setX(rdX(ea_abs()));         nzX(cpu.x); cyc += 2 + wX(); break;            // LDX abs
    case 0xBE: setX(rdX(ea_abs_y()));       nzX(cpu.x); cyc += 2 + wX(); break;            // LDX abs,Y
    case 0xA0: setY(fetchX());              nzX(cpu.y); cyc += wX(); break;                // LDY #
    case 0xA4: setY(rdX(ea_dp()));          nzX(cpu.y); cyc += 1 + dl() + wX(); break;     // LDY dp
    case 0xB4: setY(rdX(ea_dp_x()));        nzX(cpu.y); cyc += 2 + dl() + wX(); break;     // LDY dp,X
    case 0xAC: setY(rdX(ea_abs()));         nzX(cpu.y); cyc += 2 + wX(); break;            // LDY abs
    case 0xBC: setY(rdX(ea_abs_x()));       nzX(cpu.y); cyc += 2 + wX(); break;            // LDY abs,X
    case 0x86: wrX(ea_dp(),    cpu.x); cyc += 1 + dl() + wX(); break;                      // STX dp
    case 0x96: wrX(ea_dp_y(),  cpu.x); cyc += 2 + dl() + wX(); break;                      // STX dp,Y
    case 0x8E: wrX(ea_abs(),   cpu.x); cyc += 2 + wX(); break;                             // STX abs
    case 0x84: wrX(ea_dp(),    cpu.y); cyc += 1 + dl() + wX(); break;                      // STY dp
    case 0x94: wrX(ea_dp_x(),  cpu.y); cyc += 2 + dl() + wX(); break;                      // STY dp,X
    case 0x8C: wrX(ea_abs(),   cpu.y); cyc += 2 + wX(); break;                             // STY abs
    case 0x64: wrA(ea_dp(),    0); cyc += 1 + dl() + wA(); break;                          // STZ dp
    case 0x74: wrA(ea_dp_x(),  0); cyc += 2 + dl() + wA(); break;                          // STZ dp,X
    case 0x9C: wrA(ea_abs(),   0); cyc += 2 + wA(); break;                                 // STZ abs
    case 0x9E: wrA(ea_abs_x(), 0); cyc += 3 + wA(); break;                                 // STZ abs,X

    // --- BIT ---
    case 0x89: { const uint16_t mval = fetchA(); cpu.z = ((getA() & mval) & maskA()) == 0; cyc += wA(); } break;  // BIT # (Z only)
    case 0x24: case 0x34: case 0x2C: case 0x3C: {
        const uint32_t ea = (op == 0x24) ? ea_dp() : (op == 0x34) ? ea_dp_x() : (op == 0x2C) ? ea_abs() : ea_abs_x();
        const uint16_t mval = rdA(ea);
        cpu.z = ((getA() & mval) & maskA()) == 0;
        cpu.n = (mval & signA()) != 0;
        cpu.v = (mval & (signA() >> 1)) != 0;
        cyc += ((op == 0x24) ? 1 + dl() : (op == 0x34) ? 2 + dl() : 2) + wA();
    } break;

    // --- read-modify-write on memory: dp 5, dp,X 6, abs 6, abs,X 7; +2 when 16-bit ---
    case 0x06: rmw(ea_dp(),    sh_asl); cyc += 3 + dl() + 2 * wA(); break;
    case 0x16: rmw(ea_dp_x(),  sh_asl); cyc += 4 + dl() + 2 * wA(); break;
    case 0x0E: rmw(ea_abs(),   sh_asl); cyc += 4 + 2 * wA(); break;
    case 0x1E: rmw(ea_abs_x(), sh_asl); cyc += 5 + 2 * wA(); break;
    case 0x26: rmw(ea_dp(),    sh_rol); cyc += 3 + dl() + 2 * wA(); break;
    case 0x36: rmw(ea_dp_x(),  sh_rol); cyc += 4 + dl() + 2 * wA(); break;
    case 0x2E: rmw(ea_abs(),   sh_rol); cyc += 4 + 2 * wA(); break;
    case 0x3E: rmw(ea_abs_x(), sh_rol); cyc += 5 + 2 * wA(); break;
    case 0x46: rmw(ea_dp(),    sh_lsr); cyc += 3 + dl() + 2 * wA(); break;
    case 0x56: rmw(ea_dp_x(),  sh_lsr); cyc += 4 + dl() + 2 * wA(); break;
    case 0x4E: rmw(ea_abs(),   sh_lsr); cyc += 4 + 2 * wA(); break;
    case 0x5E: rmw(ea_abs_x(), sh_lsr); cyc += 5 + 2 * wA(); break;
    case 0x66: rmw(ea_dp(),    sh_ror); cyc += 3 + dl() + 2 * wA(); break;
    case 0x76: rmw(ea_dp_x(),  sh_ror); cyc += 4 + dl() + 2 * wA(); break;
    case 0x6E: rmw(ea_abs(),   sh_ror); cyc += 4 + 2 * wA(); break;
    case 0x7E: rmw(ea_abs_x(), sh_ror); cyc += 5 + 2 * wA(); break;
    case 0xE6: rmw(ea_dp(),    sh_inc); cyc += 3 + dl() + 2 * wA(); break;
    case 0xF6: rmw(ea_dp_x(),  sh_inc); cyc += 4 + dl() + 2 * wA(); break;
    case 0xEE: rmw(ea_abs(),   sh_inc); cyc += 4 + 2 * wA(); break;
    case 0xFE: rmw(ea_abs_x(), sh_inc); cyc += 5 + 2 * wA(); break;
    case 0xC6: rmw(ea_dp(),    sh_dec); cyc += 3 + dl() + 2 * wA(); break;
    case 0xD6: rmw(ea_dp_x(),  sh_dec); cyc += 4 + dl() + 2 * wA(); break;
    case 0xCE: rmw(ea_abs(),   sh_dec); cyc += 4 + 2 * wA(); break;
    case 0xDE: rmw(ea_abs_x(), sh_dec); cyc += 5 + 2 * wA(); break;

    // --- accumulator shifts ---
    case 0x0A: setA(sh_asl(getA())); break;
    case 0x2A: setA(sh_rol(getA())); break;
    case 0x4A: setA(sh_lsr(getA())); break;
    case 0x6A: setA(sh_ror(getA())); break;
    case 0x1A: setA(sh_inc(getA())); break;                                       // INC A
    case 0x3A: setA(sh_dec(getA())); break;                                       // DEC A

    // --- index arithmetic and compares ---
    case 0xE8: setX(cpu.x + 1); nzX(cpu.x); break;                                // INX
    case 0xCA: setX(cpu.x - 1); nzX(cpu.x); break;                                // DEX
    case 0xC8: setY(cpu.y + 1); nzX(cpu.y); break;                                // INY
    case 0x88: setY(cpu.y - 1); nzX(cpu.y); break;                                // DEY
    case 0xE0: do_cmp(cpu.x, fetchX(),        maskX(), signX()); cyc += wX(); break;             // CPX #
    case 0xE4: do_cmp(cpu.x, rdX(ea_dp()),    maskX(), signX()); cyc += 1 + dl() + wX(); break;  // CPX dp
    case 0xEC: do_cmp(cpu.x, rdX(ea_abs()),   maskX(), signX()); cyc += 2 + wX(); break;         // CPX abs
    case 0xC0: do_cmp(cpu.y, fetchX(),        maskX(), signX()); cyc += wX(); break;             // CPY #
    case 0xC4: do_cmp(cpu.y, rdX(ea_dp()),    maskX(), signX()); cyc += 1 + dl() + wX(); break;  // CPY dp
    case 0xCC: do_cmp(cpu.y, rdX(ea_abs()),   maskX(), signX()); cyc += 2 + wX(); break;         // CPY abs

    // --- transfers ---
    case 0xAA: setX(getA());  nzX(cpu.x); break;                                  // TAX
    case 0xA8: setY(getA());  nzX(cpu.y); break;                                  // TAY
    case 0x8A: setA(cpu.x);   nzA(getA()); break;                                 // TXA
    case 0x98: setA(cpu.y);   nzA(getA()); break;                                 // TYA
    case 0x9B: setY(cpu.x);   nzX(cpu.y); break;                                  // TXY
    case 0xBB: setX(cpu.y);   nzX(cpu.x); break;                                  // TYX
    case 0xBA: setX(cpu.sp);  nzX(cpu.x); break;                                  // TSX
    case 0x9A: cpu.sp = x8() ? static_cast<uint16_t>(cpu.x & 0xFF) : cpu.x; fix_sp(); break;  // TXS
    case 0x5B: cpu.dp = cpu.a; nz_w(cpu.a, 0xFFFF, 0x8000); break;                // TCD (always 16)
    case 0x7B: cpu.a  = cpu.dp; nz_w(cpu.a, 0xFFFF, 0x8000); break;               // TDC
    case 0x1B: cpu.sp = cpu.a; fix_sp(); break;                                   // TCS
    case 0x3B: cpu.a  = cpu.sp; nz_w(cpu.a, 0xFFFF, 0x8000); break;               // TSC
    case 0xEB: cpu.a  = static_cast<uint16_t>((cpu.a << 8) | (cpu.a >> 8));       // XBA
               nz_w(cpu.a, 0xFF, 0x80); cyc = 3; break;

    // --- stack ---
    case 0x48: if (m8()) push8(getA() & 0xFF); else push16(cpu.a); cyc += 1 + wA(); break;               // PHA
    case 0x68: { const uint16_t v = m8() ? pop8() : pop16(); setA(v); nzA(v); cyc += 2 + wA(); } break;  // PLA
    case 0xDA: if (x8()) push8(cpu.x & 0xFF); else push16(cpu.x); cyc += 1 + wX(); break;                // PHX
    case 0xFA: { const uint16_t v = x8() ? pop8() : pop16(); setX(v); nzX(v); cyc += 2 + wX(); } break;  // PLX
    case 0x5A: if (x8()) push8(cpu.y & 0xFF); else push16(cpu.y); cyc += 1 + wX(); break;                // PHY
    case 0x7A: { const uint16_t v = x8() ? pop8() : pop16(); setY(v); nzX(v); cyc += 2 + wX(); } break;  // PLY
    case 0x08: push8(packP()); cyc += 1; break;                                   // PHP
    case 0x28: unpackP(pop8()); cyc += 2; break;                                  // PLP
    case 0x8B: push8(cpu.dbr); cyc += 1; break;                                   // PHB
    case 0xAB: cpu.dbr = pop8(); nz_w(cpu.dbr, 0xFF, 0x80); cyc += 2; break;      // PLB
    case 0x0B: push16(cpu.dp); cyc += 2; break;                                   // PHD
    case 0x2B: cpu.dp = pop16(); nz_w(cpu.dp, 0xFFFF, 0x8000); cyc += 3; break;   // PLD
    case 0x4B: push8(cpu.pbr); cyc += 1; break;                                   // PHK
    case 0xF4: push16(fetch16()); cyc = 5; break;                                 // PEA
    case 0xD4: push16(rd16(static_cast<uint16_t>(cpu.dp + fetch8()))); cyc = 6 + dl(); break;  // PEI
    case 0x62: { const int16_t rel = static_cast<int16_t>(fetch16());             // PER
                 push16(static_cast<uint16_t>(cpu.pc + rel)); cyc = 6; } break;

    // --- flags ---
    case 0x18: cpu.c = false; break;                                              // CLC
    case 0x38: cpu.c = true;  break;                                              // SEC
    case 0x58: cpu.irq = false; break;                                            // CLI
    case 0x78: cpu.irq = true;  break;                                            // SEI
    case 0xD8: cpu.dec = false; break;                                            // CLD
    case 0xF8: cpu.dec = true;  break;                                            // SED
    case 0xB8: cpu.v = false; break;                                              // CLV
    case 0xC2: unpackP(static_cast<uint8_t>(packP() & ~fetch8())); cyc = 3; break;// REP #
    case 0xE2: unpackP(static_cast<uint8_t>(packP() |  fetch8())); cyc = 3; break;// SEP #
    case 0xFB: {                                                                  // XCE
        const bool old_e = cpu.e;
        cpu.e = cpu.c;
        cpu.c = old_e;
        // Either direction lands in 8-bit widths; entering emulation also narrows
        // the index registers and parks the stack in page 1.
        cpu.m = true; cpu.xf = true;
        if (cpu.e) { cpu.x &= 0xFF; cpu.y &= 0xFF; fix_sp(); }
    } break;

    // --- branches ---
    case 0x90: cyc = branch(!cpu.c); break;  case 0xB0: cyc = branch(cpu.c);  break;
    case 0xF0: cyc = branch(cpu.z);  break;  case 0xD0: cyc = branch(!cpu.z); break;
    case 0x10: cyc = branch(!cpu.n); break;  case 0x30: cyc = branch(cpu.n);  break;
    case 0x50: cyc = branch(!cpu.v); break;  case 0x70: cyc = branch(cpu.v);  break;
    case 0x80: cyc = branch(true);   break;                                       // BRA
    case 0x82: { const int16_t rel = static_cast<int16_t>(fetch16());             // BRL
                 cpu.pc = static_cast<uint16_t>(cpu.pc + rel); cyc = 4; } break;

    // --- jumps and calls ---
    case 0x4C: cpu.pc = fetch16(); cyc = 3; break;                                // JMP abs
    case 0x6C: cpu.pc = rd16(fetch16()); cyc = 5; break;                          // JMP (abs)   bank 0 pointer
    case 0x7C: cpu.pc = rd16(bank(cpu.pbr, static_cast<uint16_t>(fetch16() + cpu.x))); cyc = 6; break; // JMP (abs,X)
    case 0x5C: { const uint32_t t = fetch24(); cpu.pbr = t >> 16; cpu.pc = t & 0xFFFF; cyc = 4; } break; // JML long
    case 0xDC: { const uint32_t t = rd24(fetch16()); cpu.pbr = t >> 16; cpu.pc = t & 0xFFFF; cyc = 6; } break; // JML [abs]
    case 0x20: { const uint16_t t = fetch16(); push16(static_cast<uint16_t>(cpu.pc - 1)); cpu.pc = t; cyc = 6; } break; // JSR abs
    case 0xFC: { const uint16_t t = rd16(bank(cpu.pbr, static_cast<uint16_t>(fetch16() + cpu.x)));   // JSR (abs,X)
                 push16(static_cast<uint16_t>(cpu.pc - 1)); cpu.pc = t; cyc = 8; } break;
    case 0x22: { const uint32_t t = fetch24();                                    // JSL long
                 push8(cpu.pbr); push16(static_cast<uint16_t>(cpu.pc - 1));
                 cpu.pbr = t >> 16; cpu.pc = t & 0xFFFF; cyc = 8; } break;
    case 0x60: cpu.pc = static_cast<uint16_t>(pop16() + 1); cyc = 6; break;      // RTS
    case 0x6B: cpu.pc = static_cast<uint16_t>(pop16() + 1); cpu.pbr = pop8(); cyc = 6; break; // RTL

    // --- block moves: MVN increments, MVP decrements; A+1 bytes; A ends at $FFFF ---
    case 0x54: case 0x44: {
        const uint8_t dst = fetch8(), src = fetch8();   // encoded destination first
        cpu.dbr = dst;
        const int dir = (op == 0x54) ? 1 : -1;
        // With 16-bit index registers this copies up to 64K; with 8-bit ones the
        // pointers wrap within 256 bytes, as on the real chip.
        for (;;) {
            wr8(bank(dst, cpu.y), rd8(bank(src, cpu.x)));
            setX(static_cast<uint16_t>(cpu.x + dir));
            setY(static_cast<uint16_t>(cpu.y + dir));
            cpu_cycles += 7;
            if (cpu.a == 0) { cpu.a = 0xFFFF; break; }
            --cpu.a;
        }
        cyc = 0;
    } break;

    case 0xEA: break;                                                             // NOP

    // --- explicitly refused: none of this is modelled, and vouching for a program
    // that reaches it would be dishonest ---
    case 0x00: die("BRK (no interrupt model)", op_pc);
    case 0x02: die("COP (no interrupt model)", op_pc);
    case 0x40: die("RTI (no interrupt model)", op_pc);
    case 0x42: die("WDM", op_pc);
    case 0xCB: die("WAI (no interrupt model)", op_pc);
    case 0xDB: die("STP", op_pc);

    default: {
        uint32_t ea = 0; bool imm = false;
        const int mode_cost = group1_ea(op, ea, imm);
        if (mode_cost >= 0) {
            group1_op(op, ea, imm);
            cyc += mode_cost + wA();
        } else {
            std::fprintf(stderr,
                "a2min: unimplemented opcode $%02X at $%06X after %llu instructions.\n"
                "Add it rather than ignoring it -- a skipped instruction yields a\n"
                "wrong-but-plausible digest.\n",
                op, op_pc, static_cast<unsigned long long>(cpu.instructions));
            std::exit(3);
        }
    }
    }

    cpu_cycles += cyc;
    advance(cyc * CARD_CYCLES_PER_CPU_CYCLE);
}

// --- Harness plumbing ----------------------------------------------------------------
bool load_image(const char *bin_path, size_t *size_out = nullptr) {
    std::ifstream bin(bin_path, std::ios::binary);
    if (!bin) { std::fprintf(stderr, "cannot open %s\n", bin_path); return false; }
    std::fill(ram.begin(), ram.end(), 0);
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(bin)),
                                     std::istreambuf_iterator<char>());
    if (image.empty()) { std::fprintf(stderr, "%s is empty\n", bin_path); return false; }
    for (size_t i = 0; i < image.size(); ++i) ram[LOAD_ADDR + i] = image[i];
    if (size_out) *size_out = image.size();
    return true;
}

void bring_up_card() {
    card = new Va2nix_card;
    card->clk = 0; card->rst_n = 0;
    card->c0n_addr = 0; card->c0n_read = 0; card->c0n_write = 0; card->c0n_wdata = 0;
    card->c8_addr = 0; card->mem_rdata = 0;
    for (int i = 0; i < 8; ++i) tick();
    card->rst_n = 1;
    tick();
}

// RTS from the entry point returns to HALT_ADDR, so seed the stack accordingly, then
// run to the halt or the budget.
bool run_to_halt(uint64_t budget = 50'000'000) {
    cpu = Cpu{};
    const uint16_t seed = HALT_ADDR - 1;
    push16(seed);
    while (!cpu.halted && cpu_cycles < budget) step();
    return cpu.halted;
}

uint32_t r16at(uint32_t base, int off) { return static_cast<uint32_t>(ram[base + off]) | (static_cast<uint32_t>(ram[base + off + 1]) << 8); }
uint32_t r16(int off) { return r16at(RESULT, off); }
uint32_t r32(int off) { return r16(off) | (r16(off + 2) << 16); }

// --- Native-mode self-test -------------------------------------------------------------
//
//   a2min --selftest <bin>
//
// Runs target/tests/native_test.s, which exercises each native-mode feature and writes
// one 16-bit word per test into SELFTEST_RESULT. The EXPECTED values live here,
// derived by hand from the 65816's documented semantics, so a wrong interpreter and a
// wrong test cannot agree by construction: the assembly says what to do, this table
// says what the chip would have produced.
int selftest(const char *bin_path) {
    if (!load_image(bin_path)) return 2;
    bring_up_card();
    if (!run_to_halt()) { std::fprintf(stderr, "selftest: did not halt\n"); return 1; }

    struct Case { const char *name; uint16_t want; };
    const Case cases[] = {
        /*  0 */ { "LDA #imm is 16 bits wide",                 0xBEEF },
        /*  1 */ { "ADC wraps at 16 bits",                     0x0000 },  // $FFFF + 1
        /*  2 */ { "ADC carry-out is bit 16",                  0x0001 },
        /*  3 */ { "SEP #$20 keeps B; XBA swaps",              0x5612 },  // $1234 -> lda #$56 -> xba
        /*  4 */ { "TCD moves the direct page",                0xA5A5 },  // written at D+$10, read at $0D10
        /*  5 */ { "TDC reads D back",                         0x0D00 },
        /*  6 */ { "LDA [dp] follows a 24-bit pointer",        0x0BAD },  // bank 1
        /*  7 */ { "LDA [dp],Y indexes a 24-bit pointer",      0xC0DE },
        /*  8 */ { "STA long,X / LDA long across a bank",      0xF00D },  // $01FFFC + 4 = $020000
        /*  9 */ { "JSL/RTL into bank 1 and back",             0x0042 },
        /* 10 */ { "PLB: absolute addressing uses DBR",        0xD00D },
        /* 11 */ { "PEA then LDA sr,S",                        0x1234 },
        /* 12 */ { "LDA (sr,S),Y",                             0x5A5A },
        /* 13 */ { "LDX #imm / STX are 16 bits wide",          0xABCD },
        /* 14 */ { "MVN copies 16 bytes into bank 1",          0x0F0E },  // bytes 14,15 of 0..15
        /* 15 */ { "MVP copies 16 bytes downward",             0x0100 },  // bytes 0,1
        /* 16 */ { "16-bit RMW: INC ASL ROL DEC LSR ROR",      0x8001 },  // ROR must land in bit 15
        /* 17 */ { "16-bit CMP compares the whole word",       0x0001 },  // $0100 >= $00FF
        /* 18 */ { "PHD/PLD round-trip",                       0x0500 },
        /* 19 */ { "TCS/TSC round-trip",                       0x01F0 },
        /* 20 */ { "PEI pushes the word at dp",                0x7777 },
        /* 21 */ { "PER pushes a PC-relative address",         0x0001 },  // per L; pla; sbc #L; inc
        /* 22 */ { "BRL skips the poison",                     0x600D },
        /* 23 */ { "JML into bank 1 and JML back",             0x1111 },
        /* 24 */ { "TXY/TYX",                                  0x1357 },
        /* 25 */ { "XCE to emulation narrows A",               0x0034 },  // only the low byte is stored
        /* 26 */ { "XCE to emulation narrows X",               0x0078 },
        /* 27 */ { "B survives the emulation round-trip",      0x1234 },
        /* 28 */ { "abs,X with a 16-bit X",                    0x2468 },  // $0800 + $0300
        /* 29 */ { "(dp),Y with a 16-bit Y",                   0x9999 },
        /* 30 */ { "(dp,X)",                                   0x2468 },
        /* 31 */ { "dp,X with a 16-bit X",                     0x4242 },  // $00 + $0C00
        /* 32 */ { "abs,Y with a 16-bit Y",                    0x4343 },
        /* 33 */ { "16-bit SBC borrows across the word",       0xFFFF },  // 0 - 1
        /* 34 */ { "SBC leaves carry clear on borrow",         0x0000 },
        /* 35 */ { "16-bit PHA/PLA",                           0xCAFE },
        /* 36 */ { "16-bit PHX/PLY crosses registers",         0xF00F },
        /* 37 */ { "X narrowed by the emulation round-trip",   0x0078 },  // high byte gone after REP
        /* 38 */ { "ADC carry is bit 16, not bit 8",           0x0000 },  // $00FF + 1
    };

    int fails = 0;
    std::printf("native-mode self-test: %s\n", bin_path);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const uint16_t got = static_cast<uint16_t>(r16at(SELFTEST_RESULT, static_cast<int>(2 * i)));
        const bool ok = (got == cases[i].want);
        std::printf("  %2zu %-42s %s  (want %04X got %04X)\n", i, cases[i].name,
                    ok ? "ok  " : "FAIL", cases[i].want, got);
        if (!ok) ++fails;
    }
    std::printf("  status byte: $%02X, %llu instructions, %llu cycles\n", ram[RESULT_STAT],
                static_cast<unsigned long long>(cpu.instructions),
                static_cast<unsigned long long>(cpu_cycles));

    card->final();
    delete card;

    if (ram[RESULT_STAT] != 0x01) { std::fprintf(stderr, "selftest: status $%02X\n", ram[RESULT_STAT]); ++fails; }
    if (fails) { std::fprintf(stderr, "SELFTEST FAIL: %d\n", fails); return 1; }
    std::printf("SELFTEST OK: native mode\n");
    return 0;
}

// --- Benchmark mode for the D5 verb-economics gate ---
//
//   a2min --bench-mul64 <bin> <a> <b>
//
// Runs a 65816 software 64x64 multiply with the operands poked into $0340/$0348,
// checks the product at $0300 against the host's own int64 multiply, and reports CPU
// cycles. That number is what ALU64's bus cost is judged against.
int bench_mul64(const char *bin_path, uint64_t a, uint64_t b, const char *label) {
    if (!load_image(bin_path)) return 2;
    for (int i = 0; i < 8; ++i) {
        ram[0x0340 + i] = static_cast<uint8_t>(a >> (8 * i));
        ram[0x0348 + i] = static_cast<uint8_t>(b >> (8 * i));
    }
    bring_up_card();
    if (!run_to_halt()) { std::fprintf(stderr, "bench: did not halt\n"); return 1; }

    uint64_t got = 0;
    for (int i = 0; i < 8; ++i) got |= static_cast<uint64_t>(ram[RESULT + i]) << (8 * i);
    const uint64_t want = a * b;    // wrapping, the same signed or unsigned

    std::printf("bench mul64 (%s): %llu x %llu\n", label,
                static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    std::printf("  status       : $%02X\n", ram[RESULT_STAT]);
    std::printf("  instructions : %llu\n", static_cast<unsigned long long>(cpu.instructions));
    std::printf("  CPU cycles   : %llu   <-- the 65816's cost\n",
                static_cast<unsigned long long>(cpu_cycles));
    std::printf("  want         : %016llx\n", static_cast<unsigned long long>(want));
    std::printf("  got          : %016llx\n", static_cast<unsigned long long>(got));

    card->final();
    delete card;

    if (ram[RESULT_STAT] != 0x01 || got != want) {
        std::fprintf(stderr, "BENCH FAIL: software multiply is wrong\n");
        return 1;
    }
    std::printf("BENCH OK\n");
    return 0;
}

// --- The D6 measurement: what fraction of allocated bytes crosses the bus? ---
//
//   a2min --bench-nursery <bin>
//
// Runs target/tests/nursery_bench.s, which allocates a workload in CPU RAM, keeps a
// little of it live, collects and promotes. The program reports allocated and promoted
// bytes; this harness counts slot accesses independently and then walks the promoted
// block on the card to verify the graph arrived intact with every young pointer
// translated -- a promoter that streamed the right byte COUNT but the wrong pointers
// would look perfect by the counts alone.
int bench_nursery(const char *bin_path) {
    if (!load_image(bin_path)) return 2;
    bring_up_card();
    if (!run_to_halt()) { std::fprintf(stderr, "bench: did not halt\n"); return 1; }

    const uint32_t allocd = r16(0), promoted = r16(2);
    const uint32_t nobj = r16(4), nprom = r16(6);
    const uint32_t base = r32(8), root0 = r32(12);

    std::printf("bench nursery (D6: the generational boundary is the slot connector)\n");
    std::printf("  status            : $%02X\n", ram[RESULT_STAT]);
    std::printf("  CPU cycles        : %llu\n", static_cast<unsigned long long>(cpu_cycles));
    std::printf("  allocated         : %u bytes in %u objects   (nursery, fast RAM)\n", allocd, nobj);
    std::printf("  promoted          : %u bytes in %u objects   (crossed the bus)\n", promoted, nprom);
    std::printf("  bus accesses      : %llu total\n", static_cast<unsigned long long>(slot_accesses));
    if (allocd)
        std::printf("  crossed the bus   : %.1f%% of allocated bytes\n",
                    100.0 * static_cast<double>(promoted) / static_cast<double>(allocd));
    std::printf("  card block base   : %08x, root0 -> %08x\n", base, root0);

    // --- verify the promoted graph on the card ---
    // Expected: 2 LISTs of 2, and the INTs they reference. The two root lists
    // (indices 3 and 7) reference ints[3],ints[0] and ints[7&3],ints[8&3] -- the
    // SAME two INTs -- so exactly 2 distinct INTs survive and each must appear on
    // the card once: 2x12 + 2x12 = 48 bytes. Sharing across separate roots is the
    // more valuable case to have exercised.
    bool ok = (ram[RESULT_STAT] == 0x01);
    int lists = 0, ints = 0, bad_ptr = 0, bad_tag = 0;
    for (uint32_t off = 0; off < promoted;) {
        const uint32_t at = base + off;
        const uint8_t tag = heap.peek(at);
        const uint32_t aux = heap.peek32(at) >> 8;
        uint32_t size = 8;
        if (tag == TAG_INT) { size = 12; ++ints; }
        else if (tag == TAG_LIST) {
            size = 4 + 4 * aux; ++lists;
            for (uint32_t k = 0; k < aux; ++k) {
                const uint32_t p = heap.peek32(at + 4 + 4 * k);
                if ((p & 0x80000000u) || p < base || p >= base + promoted) ++bad_ptr;
                else if (heap.peek(p) != TAG_INT) ++bad_ptr;
            }
        } else { ++bad_tag; }
        off += size;
    }
    std::printf("  on the card       : %d lists, %d ints, %d bad pointers, %d bad tags\n",
                lists, ints, bad_ptr, bad_tag);
    ok &= (lists == 2 && ints == 2 && bad_ptr == 0 && bad_tag == 0);
    ok &= (root0 >= base && root0 < base + promoted && heap.peek(root0) == TAG_LIST);
    ok &= (promoted == 48 && allocd == 600 && nprom == 4 && nobj == 50);
    // The bus must have carried each promoted byte exactly once, plus a small fixed
    // overhead (card scan, RESERVE, two address-register loads).
    ok &= (slot_accesses >= promoted && slot_accesses <= promoted + 32);

    card->final();
    delete card;

    if (!ok) { std::fprintf(stderr, "BENCH FAIL: promoted graph is not what was expected\n"); return 1; }
    std::printf("BENCH OK: %u of %u bytes crossed the bus; the rest died in the nursery\n",
                promoted, allocd);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    if (argc >= 3 && std::string(argv[1]) == "--selftest")
        return selftest(argv[2]);

    if (argc >= 3 && std::string(argv[1]) == "--bench-nursery")
        return bench_nursery(argv[2]);

    if (argc >= 5 && (std::string(argv[1]) == "--bench-mul64" || std::string(argv[1]) == "--bench-mul64-native"))
        return bench_mul64(argv[2],
                           std::strtoull(argv[3], nullptr, 0),
                           std::strtoull(argv[4], nullptr, 0),
                           std::string(argv[1]) == "--bench-mul64" ? "8-bit emulation mode"
                                                                    : "16-bit native mode");

    // --- M2: the store-path demo against the golden vector ---
    const char *bin_path = (argc > 1) ? argv[1] : "target/build/storepath_demo_headless.bin";
    const char *vec_path = (argc > 2) ? argv[2] : "tests/vectors/l1.txt";
    if (argc > 3) card_slot = std::atoi(argv[3]);

    size_t image_size = 0;
    if (!load_image(bin_path, &image_size)) {
        std::fprintf(stderr, "build it with: just asm-headless\n");
        return 2;
    }

    // --- expected answer, from the golden vectors (which came from real nix) ---
    std::string expect;
    {
        std::ifstream vec(vec_path);
        std::string line;
        while (std::getline(vec, line)) {
            if (line.rfind("SP ", 0) != 0) continue;
            if (line.find("nix-on-an-apple-iigs") == std::string::npos) continue;
            std::istringstream ls(line);
            std::string k, fp;
            ls >> k >> fp >> expect;
            break;
        }
    }
    if (expect.empty()) {
        std::fprintf(stderr, "no nix-on-an-apple-iigs vector in %s\n", vec_path);
        return 2;
    }

    bring_up_card();

    std::printf("a2min: %s (%zu bytes) at $%04X, card in slot %d\n",
                bin_path, image_size, LOAD_ADDR, card_slot);

    if (!run_to_halt()) {
        std::fprintf(stderr, "a2min: ran %llu CPU cycles without halting\n",
                     static_cast<unsigned long long>(cpu_cycles));
        return 1;
    }

    // --- check ---
    const uint8_t status = ram[RESULT_STAT];
    std::string got(reinterpret_cast<char *>(&ram[RESULT]), 32);

    std::printf("  instructions : %llu\n", static_cast<unsigned long long>(cpu.instructions));
    std::printf("  CPU cycles   : %llu\n", static_cast<unsigned long long>(cpu_cycles));
    std::printf("  slot accesses: %llu\n", static_cast<unsigned long long>(slot_accesses));
    std::printf("  status byte  : $%02X\n", status);
    std::printf("  expected     : %s\n", expect.c_str());
    std::printf("  got          : %s\n", got.c_str());

    card->final();
    delete card;

    if (status != 0x01) {
        std::fprintf(stderr, "M2 FAIL: demo reported status $%02X, expected $01\n", status);
        return 1;
    }
    if (got != expect) {
        std::fprintf(stderr, "M2 FAIL: store path mismatch\n");
        return 1;
    }

    std::printf("\nM2 PASS: 65816 machine code drove the RTL to a store path "
                "identical to upstream nix\n");
    return 0;
}
