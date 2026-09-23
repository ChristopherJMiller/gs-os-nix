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
#include "cpu65816.h"

#include <algorithm>
#include <cctype>
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
constexpr uint32_t NBC_BASE    = 0x030000;      // where --run-nbc places the image: bank 3 (docs/VM.md)
constexpr size_t   NBC_MAX     = 0xFFF0;
constexpr uint32_t DIAG        = 0x0310;        // the VM's state on error (target/vm/c/main.c diag_write)

int card_slot = 4;

Va2nix_card *card = nullptr;
HeapMem<Va2nix_card> heap;          // the card's off-chip heap, distinct from CPU RAM
std::vector<uint8_t> ram(RAM_BYTES);// the 65816's own address space

uint64_t card_cycles = 0;
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
// The interpreter itself is sim/cpu65816.h, shared with orcarun. This bus gives it
// the a2min world: 1 MB of RAM, the slot decoder, the Verilated card, and the
// card clock advancing with every instruction.
void dump_trace();

struct A2Bus {
    bool halted = false;

    uint8_t rd8(uint32_t a) { return ::rd8(a); }
    void    wr8(uint32_t a, uint8_t v) { ::wr8(a, v); }

    // RTS from the entry point lands on HALT_ADDR; that ends the run.
    bool trap(uint32_t pcaddr) {
        if (pcaddr == HALT_ADDR || pcaddr == HALT_ADDR + 1) { halted = true; return true; }
        return false;
    }

    void cycles(int n) { advance(n * CARD_CYCLES_PER_CPU_CYCLE); }

    [[noreturn]] void die(const char *what, uint32_t where) { dump_trace(); ::die(what, where); }
};

A2Bus bus;
Cpu65816<A2Bus> cpu(bus);

void dump_trace() { cpu.print_trace(stderr); }

// --- Harness plumbing ----------------------------------------------------------------
bool load_image(const char *bin_path, size_t *size_out = nullptr) {
    std::ifstream bin(bin_path, std::ios::binary);
    if (!bin) { std::fprintf(stderr, "cannot open %s\n", bin_path); return false; }
    std::fill(ram.begin(), ram.end(), 0);
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(bin)),
                                     std::istreambuf_iterator<char>());
    if (image.empty()) { std::fprintf(stderr, "%s is empty\n", bin_path); return false; }
    // A multi-bank image from omf2bin (docs/TOOLCHAIN.md): "GSIM", a chunk count,
    // then (address, length, bytes) per chunk. Anything else is a flat image at
    // LOAD_ADDR; no raw program starts with those four bytes.
    if (image.size() >= 6 && std::memcmp(image.data(), "GSIM", 4) == 0) {
        size_t p = 4, total = 0;
        const unsigned count = image[p] | (image[p + 1] << 8);
        p += 2;
        for (unsigned c = 0; c < count; ++c) {
            if (p + 8 > image.size()) { std::fprintf(stderr, "%s: truncated GSIM chunk table\n", bin_path); return false; }
            const uint32_t addr = image[p] | (image[p + 1] << 8) | (image[p + 2] << 16) | (static_cast<uint32_t>(image[p + 3]) << 24);
            const uint32_t len  = image[p + 4] | (image[p + 5] << 8) | (image[p + 6] << 16) | (static_cast<uint32_t>(image[p + 7]) << 24);
            p += 8;
            if (p + len > image.size() || addr + len > RAM_BYTES) { std::fprintf(stderr, "%s: GSIM chunk at $%06X (%u bytes) does not fit\n", bin_path, addr, len); return false; }
            std::memcpy(&ram[addr], &image[p], len);
            p += len; total += len;
        }
        if (size_out) *size_out = total;
        return true;
    }
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
bool trace_on = false;   // --trace-cpu: every instruction to stderr

bool run_to_halt(uint64_t budget = 50'000'000) {
    // A2MIN_BUDGET overrides the cycle budget: GC-stress runs of the C VM need more.
    if (const char *b = std::getenv("A2MIN_BUDGET")) budget = std::strtoull(b, nullptr, 0);
    cpu.reset(LOAD_ADDR);
    bus.halted = false;
    const uint16_t seed = HALT_ADDR - 1;
    cpu.push16(seed);
    while (!bus.halted && cpu.cycles < budget) {
        if (trace_on)
            std::fprintf(stderr, "%06X %02X %02X %02X %02X A=%04X X=%04X Y=%04X S=%04X D=%04X P=%02X\n", cpu.pcaddr(),
                         ram[cpu.pcaddr() & 0xFFFFF], ram[(cpu.pcaddr() + 1) & 0xFFFFF],
                         ram[(cpu.pcaddr() + 2) & 0xFFFFF], ram[(cpu.pcaddr() + 3) & 0xFFFFF],
                         cpu.a, cpu.x, cpu.y, cpu.sp, cpu.dp, cpu.packP());
        cpu.step();
    }
    return bus.halted;
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
        /* 39 */ { "TSB sets bits; Z from A AND M before",     0xFFFF },  // $0F0F | $F0F0, Z=1
        /* 40 */ { "TRB clears bits; Z from A AND M before",   0xFF01 },  // $FFFF & ~$00FF, Z=0
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
                static_cast<unsigned long long>(cpu.cycles));

    card->final();
    delete card;

    if (ram[RESULT_STAT] != 0x01) { std::fprintf(stderr, "selftest: status $%02X\n", ram[RESULT_STAT]); ++fails; }
    if (fails) { std::fprintf(stderr, "SELFTEST FAIL: %d\n", fails); return 1; }
    std::printf("SELFTEST OK: native mode\n");
    return 0;
}

// --- Nix-syntax printer over the two spaces -----------------------------------------
//
// The VM reports a deep-forced value by pointer (BYTECODE.md "Result reporting");
// this walks the object graph and prints it the way `nix-instantiate --eval
// --strict` would, which is what the corpus's .exp files contain. A pointer with
// bit 31 set is in CPU RAM (bank 1 young, bank 0 static); one without is on the
// card (promoted, docs/VM.md), and is read from the card's heap model.
bool is_mature(uint32_t ptr) { return ptr != 0 && !(ptr & 0x80000000u); }

uint8_t mem8(uint32_t ptr, uint32_t off) {
    if (is_mature(ptr)) return heap.peek(ptr + off);
    const uint32_t a = (ptr & 0xFFFFFF) + off;
    return a < RAM_BYTES ? ram[a] : 0;
}
uint32_t mem32(uint32_t ptr, uint32_t off) {
    return static_cast<uint32_t>(mem8(ptr, off)) | (static_cast<uint32_t>(mem8(ptr, off + 1)) << 8) |
           (static_cast<uint32_t>(mem8(ptr, off + 2)) << 16) | (static_cast<uint32_t>(mem8(ptr, off + 3)) << 24);
}
uint32_t ram32(uint32_t a) {
    if (a + 3 >= RAM_BYTES) return 0;   // a corrupt pointer must not take the harness down
    return static_cast<uint32_t>(ram[a]) | (static_cast<uint32_t>(ram[a + 1]) << 8) |
           (static_cast<uint32_t>(ram[a + 2]) << 16) | (static_cast<uint32_t>(ram[a + 3]) << 24);
}

std::string quote_nix(const std::string &s) {
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '$': if (i + 1 < s.size() && s[i + 1] == '{') out += "\\$"; else out += '$'; break;
            default: out += c;
        }
    }
    return out + "\"";
}

bool is_nix_identifier(const std::string &s) {
    // What Nix's printer quotes: its keywords. `or` is not one (it is contextual).
    static const char *keywords[] = { "if", "then", "else", "assert", "with", "let", "in", "rec", "inherit" };
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\'' || c == '-')) return false;
    for (const char *k : keywords) if (s == k) return false;
    return true;
}

std::string nix_string_bytes(uint32_t ptr) {
    const uint32_t len = (mem32(ptr, 0) >> 8) & 0x3FFFFF;
    if (len > 0x10000) return "<OOB>";
    std::string s; s.reserve(len);
    for (uint32_t i = 0; i < len; ++i) s += static_cast<char>(mem8(ptr, 4 + i));
    return s;
}

// Printing happens after errors too, when a container may still carry the VM's
// transient marks in aux bits 22-23 (HEAP.md) -- mask them, and never index
// past memory on a corrupt pointer.
std::string fmt_value(uint32_t ptr, int depth) {
    if (depth > 64) return "<DEEP>";
    if (ptr == 0) return "<NULLPTR>";
    const uint8_t tag = mem8(ptr, 0);
    const uint32_t aux = (mem32(ptr, 0) >> 8) & 0x3FFFFF;
    if (aux > 0x4000) return "<OOB>";
    switch (tag) {
        case TAG_INT: {
            uint64_t u = 0;
            for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(mem8(ptr, 4 + i)) << (8 * i);
            return std::to_string(static_cast<long long>(static_cast<int64_t>(u)));
        }
        case TAG_BOOL: return mem8(ptr, 4) ? "true" : "false";
        case TAG_NULL: return "null";
        case TAG_STRING: return quote_nix(nix_string_bytes(ptr));
        case TAG_PATH: return nix_string_bytes(ptr);
        case TAG_LIST: {
            if (aux == 0) return "[ ]";
            std::string s = "[";
            for (uint32_t i = 0; i < aux; ++i) s += " " + fmt_value(mem32(ptr, 4 + 4 * i), depth + 1);
            return s + " ]";
        }
        case TAG_ATTRS: {
            if (aux == 0) return "{ }";
            std::string s = "{";
            for (uint32_t i = 0; i < aux; ++i) {
                const uint32_t np = mem32(ptr, 4 + 8 * aux + 4 * i);
                const std::string name = np ? nix_string_bytes(np) : "<NULLPTR>";
                s += " " + (is_nix_identifier(name) ? name : quote_nix(name)) + " = " +
                     fmt_value(mem32(ptr, 4 + 4 * aux + 4 * i), depth + 1) + ";";
            }
            return s + " }";
        }
        case TAG_THUNK: return "<CODE>";
        case TAG_LAMBDA: return "<LAMBDA>";
        case TAG_INDIRECT: return fmt_value(mem32(ptr, 4), depth + 1);
        case TAG_BLACKHOLE: return "<CYCLE>";
        default: { char b[16]; std::snprintf(b, sizeof b, "<?%02X>", tag); return b; }
    }
}

// --- L4: run one .nbc fixture on the 65816 VM --------------------------------------
//
//   a2min --run-nbc <cvm.bin> <fixture.nbc>
//
// Loads the VM (target/vm/c, flattened by omf2bin) at $2000 and the fixture at
// NBC_BASE in bank 3, runs to the halt, and
// prints the VM's answer in Nix syntax on a `result:` line (or `error: E_NAME`), which
// is what scripts/scoreboard.py compares with the corpus's .exp files. The result
// contract is BYTECODE.md "Result reporting".
int run_nbc(const char *vm_path, const char *nbc_path, bool gc_stress) {
    if (!load_image(vm_path)) return 2;
    // The VM collects at every bytecode boundary when this byte is set (gc.c),
    // which turns every fixture into a collector test.
    ram[0x0330] = gc_stress ? 1 : 0;
    // A2MIN_VERB_MASK makes the VM refuse card verbs and do them on the CPU
    // instead (vm.h VERB_*), which is how checks.verb-economics prices a verb on
    // a real program; A2MIN_NO_CARD hides the card altogether, so card_find()
    // comes up empty and promotion falls back to an in-RAM collection.
    if (const char *m = std::getenv("A2MIN_VERB_MASK"))
        ram[0x0331] = static_cast<uint8_t>(std::strtoul(m, nullptr, 0));
    if (const char *n = std::getenv("A2MIN_NO_CARD"))
        if (n[0] == '1') card_slot = -1;        // no slot decodes, so the scan finds nothing
    {
        std::ifstream f(nbc_path, std::ios::binary);
        if (!f) { std::fprintf(stderr, "cannot open %s\n", nbc_path); return 2; }
        const std::vector<uint8_t> nbc((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
        if (nbc.empty() || nbc.size() > NBC_MAX) {
            std::fprintf(stderr, "%s: %zu bytes; the VM accepts 1..%zu\n", nbc_path, nbc.size(), NBC_MAX);
            return 2;
        }
        for (size_t i = 0; i < nbc.size(); ++i) ram[NBC_BASE + i] = nbc[i];
    }
    bring_up_card();
    const bool halted = run_to_halt();
    card->final();
    delete card;

    std::printf("a2min --run-nbc %s: %llu instructions, %llu CPU cycles, %llu slot accesses\n",
                nbc_path, static_cast<unsigned long long>(cpu.instructions),
                static_cast<unsigned long long>(cpu.cycles),
                static_cast<unsigned long long>(slot_accesses));
    if (!halted) { std::printf("error: did not halt\n"); return 1; }

    const uint8_t status = ram[RESULT_STAT];
    if (status == 0xFF) {
        static const char *names[] = { "E_NONE?", "E_BADOP", "E_TYPE", "E_DIVZERO",
                                       "E_INFREC", "E_NOMEM", "E_BADNBC", "E_NOCARD",
                                       "E_ASSERT", "E_THROW", "E_ABORT", "E_NOATTR",
                                       "E_INDEX", "E_NOVAR" };
        const uint8_t code = ram[RESULT_STAT + 1];
        const char *name = code < sizeof(names) / sizeof(names[0]) ? names[code] : "E_?";
        cpu.print_trace(stdout);
        {
            // DIAG (target/vm/c/main.c): pc offset, vsp, rsp, nfree, fstack_n, catch_n,
            // then the value stack's address, so the top entries can be printed.
            const uint32_t dv = r16at(DIAG, 2), vbase = ram32(DIAG + 12);
            std::printf("\n  pc=%04X vsp=%u rsp=%u nfree=%04X fstack=%u catch=%u sp=%04X\n",
                        r16at(DIAG, 0), dv, r16at(DIAG, 4), r16at(DIAG, 6), r16at(DIAG, 8),
                        r16at(DIAG, 10), cpu.sp);
            for (uint32_t i = dv; i > 0 && i + 3 > dv; --i)
                std::printf("  stack[%u] = %08X %s\n", dv - i, ram32(vbase + 4 * (i - 1)),
                            fmt_value(ram32(vbase + 4 * (i - 1)), 0).c_str());
        }
        if (code == 0x09 || code == 0x0A)   // the message string travels in RESULT
            std::printf("error: %s %s\n", name, fmt_value(ram32(RESULT), 0).c_str());
        else
            std::printf("error: %s\n", name);
        return 1;
    }
    if (status != 0x01) { std::printf("error: status $%02X\n", status); return 1; }

    std::printf("result: %s\n", fmt_value(ram32(RESULT), 0).c_str());
    return 0;
}

// --- Run a bare binary and report the result area ---------------------------------
//
//   a2min --run-bin <bin>
//
// The general entry for programs that are not the VM: load at $2000, run to the
// halt, print the status byte and the first 16 bytes of RESULT. The ORCA/C smoke
// test (docs/TOOLCHAIN.md) checks its answers against this output.
int run_bin(const char *bin_path) {
    if (!load_image(bin_path)) return 2;
    bring_up_card();
    const bool halted = run_to_halt();
    card->final();
    delete card;
    std::printf("a2min --run-bin %s: %llu instructions, %llu CPU cycles\n", bin_path,
                static_cast<unsigned long long>(cpu.instructions),
                static_cast<unsigned long long>(cpu.cycles));
    if (!halted) { std::printf("error: did not halt\n"); return 1; }
    std::printf("status: $%02X\nresult:", ram[RESULT_STAT]);
    for (int i = 0; i < 16; ++i) std::printf(" %02X", ram[RESULT + i]);
    std::printf("\n");
    return ram[RESULT_STAT] == 0x01 ? 0 : 1;
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
                static_cast<unsigned long long>(cpu.cycles));
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
    std::printf("  CPU cycles        : %llu\n", static_cast<unsigned long long>(cpu.cycles));
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
    if (argc > 1 && std::string(argv[1]) == "--trace-cpu") { trace_on = true; --argc; ++argv; }

    if (argc >= 3 && std::string(argv[1]) == "--selftest")
        return selftest(argv[2]);

    if (argc >= 3 && std::string(argv[1]) == "--bench-nursery")
        return bench_nursery(argv[2]);

    if (argc >= 3 && std::string(argv[1]) == "--run-bin")
        return run_bin(argv[2]);

    if (argc >= 4 && std::string(argv[1]) == "--run-nbc")
        return run_nbc(argv[2], argv[3], false);
    if (argc >= 4 && std::string(argv[1]) == "--run-nbc-gc-stress")
        return run_nbc(argv[2], argv[3], true);

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
                     static_cast<unsigned long long>(cpu.cycles));
        return 1;
    }

    // --- check ---
    const uint8_t status = ram[RESULT_STAT];
    std::string got(reinterpret_cast<char *>(&ram[RESULT]), 32);

    std::printf("  instructions : %llu\n", static_cast<unsigned long long>(cpu.instructions));
    std::printf("  CPU cycles   : %llu\n", static_cast<unsigned long long>(cpu.cycles));
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
