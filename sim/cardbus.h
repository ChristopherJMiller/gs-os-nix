// Shared card ISA constants and a bus-access helper for the test harnesses.
//
// The constants live here rather than in each harness so the two cannot drift apart
// from docs/CARD-ISA.md independently -- a harness testing a stale register map would
// pass while the card was wrong.

#ifndef A2NIX_CARDBUS_H
#define A2NIX_CARDBUS_H

#include "heapmem.h"

#include <cstdint>

// --- Register offsets (docs/CARD-ISA.md) ---
enum : std::uint8_t {
    R_CMD = 0x0, R_CTRL = 0x1, R_ASEL = 0x2, R_APORT = 0x3,
    R_DATA = 0x4, R_TAG = 0x5, R_PSEL = 0x6, R_PPORT = 0x7,
    R_LEN = 0x8, R_IRQ = 0x9,
};

// --- Verbs ---
enum : std::uint8_t {
    V_NOP = 0x00, V_RESET = 0x01,
    V_HASH_INIT = 0x10, V_HASH_STREAM = 0x11, V_HASH_FINAL = 0x12,
    V_NARHASH = 0x13, V_STOREPATH = 0x14, V_HASH_PUSH = 0x15,
    V_ALLOC = 0x20, V_FOLLOW = 0x21, V_MEMCMP = 0x22, V_MEMCPY = 0x23,
    V_LOOKUP = 0x30,
    V_MARK = 0x40, V_COMPACT = 0x41,
    V_ALU64 = 0x50,
};

// --- STATUS bits ---
enum : std::uint8_t {
    ST_BUSY = 0x80, ST_DONE = 0x40, ST_ERR = 0x20, ST_READY = 0x10,
};

// --- Error codes (STATUS[3:0] when ERR) ---
enum : std::uint8_t {
    E_NONE = 0x0, E_BADVERB = 0x1, E_NOTFOUND = 0x2, E_OOM = 0x3,
    E_BADADDR = 0x4, E_BUSYERR = 0x5, E_BADPARAM = 0x6, E_OVERRUN = 0x7,
};

// --- CTRL bits ---
enum : std::uint8_t {
    CTRL_RESET = 0x01, CTRL_IRQ_EN = 0x02, CTRL_ABORT = 0x04,
    CTRL_RESET_PORTS = 0x08,
};

// --- Heap constants (docs/HEAP.md; must match rtl/a2nix_card.v) ---
constexpr std::uint32_t HEAP_BASE = 0x00000010;
enum : std::uint8_t {
    TAG_FREE = 0x00, TAG_INT = 0x01, TAG_BOOL = 0x02, TAG_NULL = 0x03,
    TAG_STRING = 0x04, TAG_PATH = 0x05, TAG_LIST = 0x06, TAG_ATTRS = 0x07,
    TAG_THUNK = 0x08, TAG_LAMBDA = 0x09, TAG_INDIRECT = 0x0A,
    TAG_BLACKHOLE = 0x0B, TAG_FORWARDED = 0x0C,
};

// Semispace geometry (docs/DECISIONS.md D15); must match rtl/a2nix_card.v.
constexpr std::uint32_t SPACE_SIZE = 0x00080000;
constexpr std::uint32_t SPACE_A    = 0x00000010;
constexpr std::uint32_t SPACE_B    = 0x00080010;

constexpr int FIFO_DEPTH = 32;
constexpr int FIFO_BURST = 16;

// The card owns its own oscillator and the slot is throttled to 1.024 MHz, so the card
// advances many cycles per access. Modelling that ratio is not optional: getting it
// wrong measures the harness instead of the hardware (see CARD-ISA.md).
constexpr int CARD_CYCLES_PER_ACCESS = 48;

// Drives a Verilated card over its $C0nX interface, exactly as the MAME device does.
template <typename CardT>
class CardBus {
public:
    CardBus(CardT *card, HeapMem<CardT> *heap)
        : m_card(card), m_heap(heap) {}

    // settle/commit straddle the edge per the port contract in heapmem.h.
    void tick() {
        m_heap->settle(m_card);
        m_card->clk = 0;
        m_card->eval();
        m_card->clk = 1;
        m_card->eval();
        m_heap->commit(m_card);
        ++m_cycles;
    }

    // Mirrors read_c0nx(): sample combinationally, clock to commit side effects.
    std::uint8_t rd(std::uint8_t off) {
        m_card->c0n_addr  = off;
        m_card->c0n_read  = 1;
        m_card->c0n_write = 0;
        m_card->eval();
        const std::uint8_t v = m_card->c0n_rdata;
        tick();
        settle_bus();
        ++m_accesses;
        return v;
    }

    void wr(std::uint8_t off, std::uint8_t v) {
        m_card->c0n_addr  = off;
        m_card->c0n_wdata = v;
        m_card->c0n_write = 1;
        m_card->c0n_read  = 0;
        tick();
        settle_bus();
        ++m_accesses;
    }

    void reset() {
        m_card->rst_n     = 0;
        m_card->c0n_addr  = 0;
        m_card->c0n_read  = 0;
        m_card->c0n_write = 0;
        m_card->c0n_wdata = 0;
        m_card->c8_addr   = 0;
        for (int i = 0; i < 8; ++i) tick();
        m_card->rst_n = 1;
        tick();
    }

    // --- Typed port access ---
    // A registers and LEN are little-endian integers; P slots are byte arrays.
    void set_areg(int n, std::uint32_t v) {
        wr(R_ASEL, static_cast<std::uint8_t>(n));
        for (int i = 0; i < 4; ++i) wr(R_APORT, static_cast<std::uint8_t>(v >> (8 * i)));
    }

    std::uint32_t get_areg(int n) {
        wr(R_ASEL, static_cast<std::uint8_t>(n));
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(rd(R_APORT)) << (8 * i);
        return v;
    }

    // LEN has no select register, so rewind the port indices first (CTRL b3).
    // Skipping this lets any earlier partial LEN access silently rotate every
    // subsequent write.
    void set_len(std::uint32_t v) {
        wr(R_CTRL, CTRL_RESET_PORTS);
        for (int i = 0; i < 4; ++i) wr(R_LEN, static_cast<std::uint8_t>(v >> (8 * i)));
    }

    void write_pslot(int slot, const std::uint8_t *in, int n) {
        wr(R_PSEL, static_cast<std::uint8_t>(slot));
        for (int i = 0; i < n; ++i) wr(R_PPORT, in[i]);
    }

    void read_pslot(int slot, std::uint8_t *out, int n) {
        wr(R_PSEL, static_cast<std::uint8_t>(slot));
        for (int i = 0; i < n; ++i) out[i] = rd(R_PPORT);
    }

    // Poll until DONE or ERR; returns the final status, or 0 on timeout.
    std::uint8_t wait_verb(int guard = 200000) {
        for (int i = 0; i < guard; ++i) {
            const std::uint8_t s = rd(R_CMD);
            if (s & (ST_DONE | ST_ERR)) return s;
        }
        return 0;
    }

    std::uint64_t accesses() const { return m_accesses; }
    std::uint64_t cycles() const { return m_cycles; }
    void reset_accesses() { m_accesses = 0; }

private:
    void settle_bus() {
        m_card->c0n_read  = 0;
        m_card->c0n_write = 0;
        for (int i = 1; i < CARD_CYCLES_PER_ACCESS; ++i) tick();
    }

    CardT *m_card;
    HeapMem<CardT> *m_heap;
    std::uint64_t m_cycles = 0;
    std::uint64_t m_accesses = 0;
};

#endif // A2NIX_CARDBUS_H
