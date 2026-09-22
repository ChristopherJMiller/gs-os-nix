// a2min -- a minimal 65C02 machine wired directly to the Verilated a2nix card.
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
// So this harness supplies the smallest thing that can execute the real binary: 64K of
// RAM, a slot decoder, and a 65C02-subset interpreter. No ROM, no OS, no copyrighted
// material -- and it runs the *same* .bin and the *same* RTL.
//
// This is a TEST HARNESS, not an emulator. It is not an Apple IIgs and does not
// pretend to be: no video, no ROM, no soft switches, emulation mode only. MAME
// remains the higher-fidelity confirmation for when a full romset is available
// (real slot timing, real GS/OS, the genuine FPI clock stretch).
//
// Unknown opcodes ABORT rather than being skipped. A silently-ignored instruction
// would produce a wrong digest that still looks like a hash, which is the failure
// mode this whole project is most careful about.

#include "Va2nix_card.h"
#include "verilated.h"

#include "heapmem.h"

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

constexpr uint16_t LOAD_ADDR = 0x2000;
constexpr uint16_t HALT_ADDR = 0xFFFE;   // RTS from the entry point lands here
constexpr uint16_t RESULT = 0x0300;
constexpr uint16_t RESULT_STAT = 0x0320;

int card_slot = 4;

Va2nix_card *card = nullptr;
HeapMem<Va2nix_card> heap;          // the card's off-chip heap, distinct from CPU RAM
uint8_t ram[0x10000];               // the 65816's own address space

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

// --- Slot I/O -------------------------------------------------------------------
bool is_slot_io(uint16_t a) { return a >= 0xC080 && a <= 0xC0FF; }
int  slot_of(uint16_t a) { return static_cast<int>((a & 0xF0) >> 4) - 8; }

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

uint8_t rd(uint16_t a) {
    if (is_slot_io(a)) {
        if (slot_of(a) == card_slot) return card_read(a & 0x0F);
        return 0x00;                      // empty slot: the scan must see nothing
    }
    return ram[a];
}

void wr(uint16_t a, uint8_t v) {
    if (is_slot_io(a)) {
        if (slot_of(a) == card_slot) card_write(a & 0x0F, v);
        return;
    }
    ram[a] = v;
}

// --- CPU ------------------------------------------------------------------------
struct Cpu {
    uint8_t  a = 0, x = 0, y = 0, sp = 0xFF;
    uint16_t pc = LOAD_ADDR;
    bool c = false, z = false, n = false, v = false, d = false;
    bool halted = false;
    uint64_t instructions = 0;
} cpu;

uint8_t fetch() { return rd(cpu.pc++); }

uint16_t fetch16() {
    const uint8_t lo = fetch();
    const uint8_t hi = fetch();
    return static_cast<uint16_t>(lo | (hi << 8));
}

void set_nz(uint8_t r) {
    cpu.z = (r == 0);
    cpu.n = (r & 0x80) != 0;
}

void push(uint8_t v) { ram[0x0100 + cpu.sp--] = v; }
uint8_t pop() { return ram[0x0100 + ++cpu.sp]; }

void do_adc(uint8_t m) {
    // Binary mode only. The program executes CLD, and decimal mode would be a
    // silent correctness trap, so refuse rather than guess.
    if (cpu.d) { std::fprintf(stderr, "a2min: decimal-mode ADC is not implemented\n"); std::exit(3); }
    const unsigned s = cpu.a + m + (cpu.c ? 1u : 0u);
    cpu.v = (~(cpu.a ^ m) & (cpu.a ^ s) & 0x80) != 0;
    cpu.c = s > 0xFF;
    cpu.a = static_cast<uint8_t>(s);
    set_nz(cpu.a);
}

void do_sbc(uint8_t m) {
    if (cpu.d) { std::fprintf(stderr, "a2min: decimal-mode SBC is not implemented\n"); std::exit(3); }
    const unsigned s = cpu.a - m - (cpu.c ? 0u : 1u);
    cpu.v = ((cpu.a ^ m) & (cpu.a ^ s) & 0x80) != 0;
    cpu.c = !(s & 0x100);
    cpu.a = static_cast<uint8_t>(s);
    set_nz(cpu.a);
}

void do_cmp(uint8_t reg, uint8_t m) {
    const unsigned s = reg - m;
    cpu.c = reg >= m;
    set_nz(static_cast<uint8_t>(s));
}

void branch(bool take) {
    const int8_t off = static_cast<int8_t>(fetch());
    if (take) {
        cpu.pc = static_cast<uint16_t>(cpu.pc + off);
        cpu_cycles += 1;
    }
}

// Addressing helpers
uint16_t addr_zp()    { return fetch(); }
uint16_t addr_abs()   { return fetch16(); }
uint16_t addr_abs_x() { return static_cast<uint16_t>(fetch16() + cpu.x); }
uint16_t addr_abs_y() { return static_cast<uint16_t>(fetch16() + cpu.y); }
uint16_t addr_zp_ind_y() {
    const uint8_t zp = fetch();
    const uint16_t base = static_cast<uint16_t>(ram[zp] | (ram[(uint8_t)(zp + 1)] << 8));
    return static_cast<uint16_t>(base + cpu.y);
}

void step() {
    if (cpu.pc == HALT_ADDR || cpu.pc == HALT_ADDR + 1) { cpu.halted = true; return; }

    const uint16_t op_pc = cpu.pc;
    const uint8_t op = fetch();
    int cyc = 2;
    ++cpu.instructions;

    switch (op) {
    // --- loads ---
    case 0xA9: cpu.a = fetch();            set_nz(cpu.a); break;              // LDA #
    case 0xA5: cpu.a = rd(addr_zp());      set_nz(cpu.a); cyc = 3; break;     // LDA zp
    case 0xAD: cpu.a = rd(addr_abs());     set_nz(cpu.a); cyc = 4; break;     // LDA abs
    case 0xBD: cpu.a = rd(addr_abs_x());   set_nz(cpu.a); cyc = 4; break;     // LDA abs,x
    case 0xB9: cpu.a = rd(addr_abs_y());   set_nz(cpu.a); cyc = 4; break;     // LDA abs,y
    case 0xB1: cpu.a = rd(addr_zp_ind_y());set_nz(cpu.a); cyc = 5; break;     // LDA (zp),y
    case 0xA2: cpu.x = fetch();            set_nz(cpu.x); break;              // LDX #
    case 0xA6: cpu.x = rd(addr_zp());      set_nz(cpu.x); cyc = 3; break;     // LDX zp
    case 0xA0: cpu.y = fetch();            set_nz(cpu.y); break;              // LDY #
    case 0xA4: cpu.y = rd(addr_zp());      set_nz(cpu.y); cyc = 3; break;     // LDY zp

    // --- stores ---
    case 0x85: wr(addr_zp(),     cpu.a); cyc = 3; break;                      // STA zp
    case 0x8D: wr(addr_abs(),    cpu.a); cyc = 4; break;                      // STA abs
    case 0x9D: wr(addr_abs_x(),  cpu.a); cyc = 5; break;                      // STA abs,x
    case 0x99: wr(addr_abs_y(),  cpu.a); cyc = 5; break;                      // STA abs,y
    case 0x91: wr(addr_zp_ind_y(), cpu.a); cyc = 6; break;                    // STA (zp),y
    case 0x86: wr(addr_zp(),     cpu.x); cyc = 3; break;                      // STX zp
    case 0x84: wr(addr_zp(),     cpu.y); cyc = 3; break;                      // STY zp

    // --- transfers ---
    case 0xAA: cpu.x = cpu.a; set_nz(cpu.x); break;                           // TAX
    case 0x8A: cpu.a = cpu.x; set_nz(cpu.a); break;                           // TXA
    case 0xA8: cpu.y = cpu.a; set_nz(cpu.y); break;                           // TAY
    case 0x98: cpu.a = cpu.y; set_nz(cpu.a); break;                           // TYA

    // --- stack ---
    case 0x48: push(cpu.a); cyc = 3; break;                                   // PHA
    case 0x68: cpu.a = pop(); set_nz(cpu.a); cyc = 4; break;                  // PLA
    case 0x5A: push(cpu.y); cyc = 3; break;                                   // PHY (65C02)
    case 0x7A: cpu.y = pop(); set_nz(cpu.y); cyc = 4; break;                  // PLY (65C02)
    case 0xDA: push(cpu.x); cyc = 3; break;                                   // PHX
    case 0xFA: cpu.x = pop(); set_nz(cpu.x); cyc = 4; break;                  // PLX

    // --- arithmetic / logic ---
    case 0x69: do_adc(fetch());        break;                                 // ADC #
    case 0x65: do_adc(rd(addr_zp()));  cyc = 3; break;                        // ADC zp
    case 0xE9: do_sbc(fetch());        break;                                 // SBC #
    case 0xE5: do_sbc(rd(addr_zp()));  cyc = 3; break;                        // SBC zp
    case 0x29: cpu.a &= fetch(); set_nz(cpu.a); break;                        // AND #
    case 0x25: cpu.a &= rd(addr_zp()); set_nz(cpu.a); cyc = 3; break;         // AND zp
    case 0x09: cpu.a |= fetch(); set_nz(cpu.a); break;                        // ORA #
    case 0x05: cpu.a |= rd(addr_zp()); set_nz(cpu.a); cyc = 3; break;         // ORA zp
    case 0x49: cpu.a ^= fetch(); set_nz(cpu.a); break;                        // EOR #
    case 0x4A: cpu.c = cpu.a & 1; cpu.a >>= 1; set_nz(cpu.a); break;          // LSR A
    case 0x0A: cpu.c = (cpu.a & 0x80) != 0; cpu.a <<= 1; set_nz(cpu.a); break;// ASL A

    // Read-modify-write shifts on memory: the multi-byte shifts a 64-bit software
    // multiply is made of. Carry chains between bytes, so it must be sampled BEFORE
    // it is overwritten.
    case 0x0E: { const uint16_t ea = addr_abs(); uint8_t v = rd(ea);             // ASL abs
                 cpu.c = (v & 0x80) != 0; v <<= 1; wr(ea, v); set_nz(v); cyc = 6; } break;
    case 0x2E: { const uint16_t ea = addr_abs(); uint8_t v = rd(ea);             // ROL abs
                 const bool cin = cpu.c; cpu.c = (v & 0x80) != 0;
                 v = static_cast<uint8_t>((v << 1) | (cin ? 1 : 0));
                 wr(ea, v); set_nz(v); cyc = 6; } break;
    case 0x4E: { const uint16_t ea = addr_abs(); uint8_t v = rd(ea);             // LSR abs
                 cpu.c = v & 1; v >>= 1; wr(ea, v); set_nz(v); cyc = 6; } break;
    case 0x6E: { const uint16_t ea = addr_abs(); uint8_t v = rd(ea);             // ROR abs
                 const bool cin = cpu.c; cpu.c = v & 1;
                 v = static_cast<uint8_t>((v >> 1) | (cin ? 0x80 : 0));
                 wr(ea, v); set_nz(v); cyc = 6; } break;
    case 0x7D: do_adc(rd(addr_abs_x())); cyc = 4; break;                        // ADC abs,x

    case 0xC9: do_cmp(cpu.a, fetch());        break;                          // CMP #
    case 0xC5: do_cmp(cpu.a, rd(addr_zp()));  cyc = 3; break;                 // CMP zp
    case 0xCD: do_cmp(cpu.a, rd(addr_abs())); cyc = 4; break;                 // CMP abs
    case 0xE0: do_cmp(cpu.x, fetch());        break;                          // CPX #
    case 0xE4: do_cmp(cpu.x, rd(addr_zp()));  cyc = 3; break;                 // CPX zp
    case 0xC0: do_cmp(cpu.y, fetch());        break;                          // CPY #
    case 0xC4: do_cmp(cpu.y, rd(addr_zp()));  cyc = 3; break;                 // CPY zp

    case 0xE8: ++cpu.x; set_nz(cpu.x); break;                                 // INX
    case 0xC8: ++cpu.y; set_nz(cpu.y); break;                                 // INY
    case 0xCA: --cpu.x; set_nz(cpu.x); break;                                 // DEX
    case 0x88: --cpu.y; set_nz(cpu.y); break;                                 // DEY
    case 0x1A: ++cpu.a; set_nz(cpu.a); break;                                 // INC A
    case 0x3A: --cpu.a; set_nz(cpu.a); break;                                 // DEC A
    case 0xE6: { const uint16_t ea = addr_zp(); const uint8_t r = rd(ea) + 1;
                 wr(ea, r); set_nz(r); cyc = 5; } break;                      // INC zp
    case 0xC6: { const uint16_t ea = addr_zp(); const uint8_t r = rd(ea) - 1;
                 wr(ea, r); set_nz(r); cyc = 5; } break;                      // DEC zp

    // --- flags ---
    case 0x18: cpu.c = false; break;                                          // CLC
    case 0x38: cpu.c = true;  break;                                          // SEC
    case 0xD8: cpu.d = false; break;                                          // CLD
    case 0xF8: cpu.d = true;  break;                                          // SED
    case 0x58: break;                                                         // CLI
    case 0x78: break;                                                         // SEI
    case 0xEA: break;                                                         // NOP

    // XCE swaps carry and the emulation bit. We only ever run in emulation mode, so
    // this just consumes the carry -- but assert that, rather than assume it.
    case 0xFB:
        if (!cpu.c) {
            std::fprintf(stderr,
                "a2min: XCE with carry clear would enter NATIVE mode at $%04X.\n"
                "This harness models emulation mode only.\n", op_pc);
            std::exit(3);
        }
        cpu.c = false;
        break;

    // --- branches ---
    case 0x90: branch(!cpu.c); break;                                         // BCC
    case 0xB0: branch(cpu.c);  break;                                         // BCS
    case 0xF0: branch(cpu.z);  break;                                         // BEQ
    case 0xD0: branch(!cpu.z); break;                                         // BNE
    case 0x10: branch(!cpu.n); break;                                         // BPL
    case 0x30: branch(cpu.n);  break;                                         // BMI
    case 0x80: branch(true);   break;                                         // BRA

    // --- control flow ---
    case 0x4C: cpu.pc = addr_abs(); cyc = 3; break;                           // JMP abs
    case 0x20: {                                                              // JSR abs
        const uint16_t target = fetch16();
        const uint16_t ret = static_cast<uint16_t>(cpu.pc - 1);
        push(static_cast<uint8_t>(ret >> 8));
        push(static_cast<uint8_t>(ret & 0xFF));
        cpu.pc = target;
        cyc = 6;
    } break;
    case 0x60: {                                                              // RTS
        const uint8_t lo = pop();
        const uint8_t hi = pop();
        cpu.pc = static_cast<uint16_t>((lo | (hi << 8)) + 1);
        cyc = 6;
    } break;

    default:
        std::fprintf(stderr,
            "a2min: unimplemented opcode $%02X at $%04X after %llu instructions.\n"
            "Add it rather than ignoring it -- a skipped instruction yields a\n"
            "wrong-but-plausible digest.\n",
            op, op_pc, static_cast<unsigned long long>(cpu.instructions));
        std::exit(3);
    }

    cpu_cycles += cyc;
    advance(cyc * CARD_CYCLES_PER_CPU_CYCLE);
}

} // namespace

// --- Benchmark mode for the D5 verb-economics gate ---
//
//   a2min --bench-mul64 <bin> <a> <b>
//
// Runs a 65816 software 64x64 multiply (target/tests/mul64_bench.s) with the operands
// poked into $0340/$0348, checks the product at $0300 against the host's own int64
// multiply, and reports CPU cycles. That number is what ALU64's bus cost is judged
// against. The card is instantiated but unused -- this measures the CPU alone.
int bench_mul64(const char *bin_path, uint64_t a, uint64_t b) {
    std::ifstream bin(bin_path, std::ios::binary);
    if (!bin) { std::fprintf(stderr, "cannot open %s\n", bin_path); return 2; }
    std::memset(ram, 0, sizeof(ram));
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(bin)),
                                     std::istreambuf_iterator<char>());
    for (size_t i = 0; i < image.size(); ++i) ram[LOAD_ADDR + i] = image[i];

    for (int i = 0; i < 8; ++i) {
        ram[0x0340 + i] = static_cast<uint8_t>(a >> (8 * i));
        ram[0x0348 + i] = static_cast<uint8_t>(b >> (8 * i));
    }

    card = new Va2nix_card;
    card->clk = 0; card->rst_n = 0;
    card->c0n_addr = 0; card->c0n_read = 0; card->c0n_write = 0; card->c0n_wdata = 0;
    card->c8_addr = 0; card->mem_rdata = 0;
    for (int i = 0; i < 8; ++i) tick();
    card->rst_n = 1;
    tick();

    const uint16_t seed = HALT_ADDR - 1;
    push(static_cast<uint8_t>(seed >> 8));
    push(static_cast<uint8_t>(seed & 0xFF));

    const uint64_t budget = 50'000'000;
    while (!cpu.halted && cpu_cycles < budget) step();
    if (!cpu.halted) { std::fprintf(stderr, "bench: did not halt\n"); return 1; }

    uint64_t got = 0;
    for (int i = 0; i < 8; ++i) got |= static_cast<uint64_t>(ram[RESULT + i]) << (8 * i);
    const uint64_t want = a * b;    // wrapping, the same signed or unsigned

    std::printf("bench mul64: %llu x %llu\n",
                static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    std::printf("  status       : $%02X\n", ram[RESULT_STAT]);
    std::printf("  instructions : %llu\n", static_cast<unsigned long long>(cpu.instructions));
    std::printf("  CPU cycles   : %llu   <-- the 65816's cost, 8-bit emulation mode\n",
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

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    if (argc >= 5 && std::string(argv[1]) == "--bench-mul64")
        return bench_mul64(argv[2],
                           std::strtoull(argv[3], nullptr, 0),
                           std::strtoull(argv[4], nullptr, 0));

    const char *bin_path = (argc > 1) ? argv[1] : "target/build/storepath_demo_headless.bin";
    const char *vec_path = (argc > 2) ? argv[2] : "tests/vectors/l1.txt";
    if (argc > 3) card_slot = std::atoi(argv[3]);

    // --- load the binary ---
    std::ifstream bin(bin_path, std::ios::binary);
    if (!bin) {
        std::fprintf(stderr, "cannot open %s\n", bin_path);
        std::fprintf(stderr, "build it with: just asm-headless\n");
        return 2;
    }
    std::memset(ram, 0, sizeof(ram));
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(bin)),
                                     std::istreambuf_iterator<char>());
    if (image.empty()) { std::fprintf(stderr, "%s is empty\n", bin_path); return 2; }
    for (size_t i = 0; i < image.size(); ++i) ram[LOAD_ADDR + i] = image[i];

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

    // --- bring the card up ---
    card = new Va2nix_card;
    card->clk = 0;
    card->rst_n = 0;
    card->c0n_addr = 0;
    card->c0n_read = 0;
    card->c0n_write = 0;
    card->c0n_wdata = 0;
    card->c8_addr = 0;
    for (int i = 0; i < 8; ++i) tick();
    card->rst_n = 1;
    tick();

    // --- run ---
    // RTS from the entry point returns to HALT_ADDR, so seed the stack accordingly.
    const uint16_t seed = HALT_ADDR - 1;
    push(static_cast<uint8_t>(seed >> 8));
    push(static_cast<uint8_t>(seed & 0xFF));

    std::printf("a2min: %s (%zu bytes) at $%04X, card in slot %d\n",
                bin_path, image.size(), LOAD_ADDR, card_slot);

    const uint64_t budget = 50'000'000;
    while (!cpu.halted && cpu_cycles < budget) step();

    if (!cpu.halted) {
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
