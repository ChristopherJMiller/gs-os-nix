// L2 testbench: drives a2nix_card through its $C0nX register interface using exactly
// the access pattern the MAME a2bus device will use, and exactly the verb sequence
// documented in docs/CARD-ISA.md.
//
// This is the M2 precursor. It proves the register map, the verb dispatch and the
// READY flow control all work before any MAME integration exists -- so when M2 fails,
// it fails in the MAME glue and not in the card.
//
// It also counts bus accesses per store path, which is the raw input to the
// checks.verb-economics gate (DECISIONS.md D5): a verb has to repay ~10 fast-cycle
// equivalents per access or it gets deleted.

#include "Va2nix_card.h"
#include "verilated.h"

#include "heapmem.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Register offsets -- must match docs/CARD-ISA.md.
enum : uint8_t {
    R_CMD = 0x0, R_CTRL = 0x1, R_ASEL = 0x2, R_APORT = 0x3,
    R_DATA = 0x4, R_TAG = 0x5, R_PSEL = 0x6, R_PPORT = 0x7,
    R_LEN = 0x8, R_IRQ = 0x9,
};

// Verbs.
enum : uint8_t {
    V_HASH_INIT = 0x10, V_HASH_STREAM = 0x11, V_HASH_FINAL = 0x12,
    V_STOREPATH = 0x14, V_HASH_PUSH = 0x15,
    V_ALLOC = 0x20, V_FOLLOW = 0x21,
};

// Heap geometry and tags -- must match rtl/a2nix_card.v and docs/HEAP.md.
constexpr uint32_t HEAP_BASE = 0x00000010;
constexpr uint8_t  TAG_INT = 0x01, TAG_STRING = 0x04, TAG_INDIRECT = 0x0A;

// Status bits.
enum : uint8_t {
    ST_BUSY = 0x80, ST_DONE = 0x40, ST_ERR = 0x20, ST_READY = 0x10,
};

// The card owns its own oscillator; the Apple II reaches it through a slot throttled
// to 1.024 MHz. So the card advances many cycles per bus access, and modelling that
// ratio is not a detail -- it decides how much of a compression stall the CPU ever
// observes, and therefore what the polling actually costs.
//
//   50 MHz card / 1.024 MHz bus ~= 48 card cycles per $C0nX access.
//
// Ticking once per access (the first version of this harness) made the card look 48x
// slower than reality and inflated every poll count accordingly.
constexpr int CARD_CYCLES_PER_ACCESS = 48;

Va2nix_card *dut = nullptr;
HeapMem<Va2nix_card> heap;          // the card's off-chip heap (docs/HEAP.md)
uint64_t cycles = 0;
uint64_t bus_accesses = 0;

// Ordering matters and follows the port contract:
//   settle() makes mem_rdata valid for the address presented on the previous edge;
//   the edge then lets the card sample it and/or issue a new request;
//   commit() applies a write the card just asserted.
void tick() {
    heap.settle(dut);
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
    heap.commit(dut);
    ++cycles;
}

// Let the card run out the rest of the bus period with both strobes deasserted.
void bus_settle() {
    dut->c0n_read  = 0;
    dut->c0n_write = 0;
    for (int i = 1; i < CARD_CYCLES_PER_ACCESS; ++i) tick();
}

// Mirrors MAME's read_c0nx(): sample combinationally, then clock to commit the
// auto-increment side effect.
uint8_t rd(uint8_t off) {
    dut->c0n_addr  = off;
    dut->c0n_read  = 1;
    dut->c0n_write = 0;
    dut->eval();
    const uint8_t v = dut->c0n_rdata;
    tick();
    bus_settle();
    ++bus_accesses;
    return v;
}

// Mirrors MAME's write_c0nx().
void wr(uint8_t off, uint8_t v) {
    dut->c0n_addr  = off;
    dut->c0n_wdata = v;
    dut->c0n_write = 1;
    dut->c0n_read  = 0;
    tick();
    bus_settle();
    ++bus_accesses;
}

void reset() {
    dut->rst_n     = 0;
    dut->c0n_addr  = 0;
    dut->c0n_read  = 0;
    dut->c0n_write = 0;
    dut->c0n_wdata = 0;
    dut->c8_addr   = 0;
    for (int i = 0; i < 8; ++i) tick();
    dut->rst_n = 1;
    tick();
}

// Read a parameter slot as raw bytes.
void read_pslot(int slot, uint8_t *out, int n) {
    wr(R_PSEL, static_cast<uint8_t>(slot));
    for (int i = 0; i < n; ++i) out[i] = rd(R_PPORT);
}

// --- L3 helpers: address registers and LEN are little-endian integers ---
void set_areg(int n, uint32_t v) {
    wr(R_ASEL, static_cast<uint8_t>(n));
    for (int i = 0; i < 4; ++i) wr(R_APORT, static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t get_areg(int n) {
    wr(R_ASEL, static_cast<uint8_t>(n));
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(rd(R_APORT)) << (8 * i);
    return v;
}

void set_len(uint32_t v) {
    // LEN has no select register, so rewind the port indices first (CTRL b3).
    // Without this, any earlier partial LEN access leaves the index rotated and
    // every subsequent write lands in the wrong byte -- silently.
    wr(R_CTRL, 0x08);
    for (int i = 0; i < 4; ++i) wr(R_LEN, static_cast<uint8_t>(v >> (8 * i)));
}

// Wait for DONE or ERR. Returns the final status.
uint8_t wait_verb() {
    for (int guard = 0; guard < 200000; ++guard) {
        const uint8_t s = rd(R_CMD);
        if (s & (ST_DONE | ST_ERR)) return s;
    }
    return 0;
}

// The documented CPU sequence, byte for byte. If this drifts from CARD-ISA.md, one of
// the two is wrong. Returns both the raw digest (P0) and the encoded path (P3), so a
// failure says which stage is broken.
// Accounting for the verb-economics gate: separate the per-byte streaming cost from
// the fixed per-call overhead, because they scale differently and only the first one
// is what "accesses per byte" should mean.
uint64_t acc_stream = 0, acc_fixed = 0, bytes_streamed = 0;

bool hash_push(const std::vector<uint8_t> &msg,
               std::string &digest_hex, std::string &path,
               bool poll_per_byte = false) {
    const uint64_t a0 = bus_accesses;
    wr(R_CMD, V_HASH_INIT);
    wr(R_CMD, V_HASH_PUSH);
    acc_fixed += bus_accesses - a0;

    // Two feeding strategies, measured against each other so the gate has real
    // numbers rather than an estimate:
    //   poll_per_byte - check READY before every byte (what the pre-FIFO card forced)
    //   burst         - one READY poll licenses FIFO_BURST writes
    constexpr size_t FIFO_BURST = 16;
    const uint64_t s0 = bus_accesses;
    size_t i = 0;
    while (i < msg.size()) {
        int guard = 0;
        while (!(rd(R_CMD) & ST_READY)) {
            if (++guard > 100000) return false;
        }
        const size_t burst =
            std::min(poll_per_byte ? size_t{1} : FIFO_BURST, msg.size() - i);
        for (size_t k = 0; k < burst; ++k) wr(R_DATA, msg[i + k]);
        i += burst;
    }
    acc_stream     += bus_accesses - s0;
    bytes_streamed += msg.size();

    const uint64_t f0 = bus_accesses;
    wr(R_CMD, V_HASH_FINAL);

    int guard = 0;
    while (!(rd(R_CMD) & ST_DONE)) {
        if (++guard > 100000) return false;
    }

    uint8_t dig[32], pth[32];
    read_pslot(0, dig, 32);
    read_pslot(3, pth, 32);
    acc_fixed += bus_accesses - f0;

    static const char *H = "0123456789abcdef";
    digest_hex.clear();
    for (int i = 0; i < 32; ++i) {
        digest_hex.push_back(H[dig[i] >> 4]);
        digest_hex.push_back(H[dig[i] & 0xf]);
    }
    path.assign(reinterpret_cast<char *>(pth), 32);
    return true;
}

std::vector<uint8_t> unhex(const std::string &s) {
    std::vector<uint8_t> out;
    if (s == "-") return out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

int fails = 0;

void expect(bool cond, const char *what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++fails;
    }
}

} // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    const char *vec_path = (argc > 1) ? argv[1] : "tests/vectors/l1.txt";

    dut = new Va2nix_card;
    reset();

    // --- Identity and reserved space ---
    expect(rd(R_CTRL) == 0xA2, "ID register reads $A2");
    expect(rd(0xF) == 0x00, "reserved register reads $00");

    // --- Address register auto-increment (the 32-bit pointer the 65816 lacks) ---
    wr(R_ASEL, 1);
    wr(R_APORT, 0x78);
    wr(R_APORT, 0x56);
    wr(R_APORT, 0x34);
    wr(R_APORT, 0x12);
    wr(R_ASEL, 1);                       // re-select resets the byte index
    expect(rd(R_APORT) == 0x78, "A1 byte 0 (LSB first)");
    expect(rd(R_APORT) == 0x56, "A1 byte 1");
    expect(rd(R_APORT) == 0x34, "A1 byte 2");
    expect(rd(R_APORT) == 0x12, "A1 byte 3");

    // Address registers must be independent.
    wr(R_ASEL, 2);
    wr(R_APORT, 0xFF);
    wr(R_ASEL, 1);
    expect(rd(R_APORT) == 0x78, "A1 unaffected by writes to A2");

    // --- Parameter slot auto-increment, and wrap at 32 bytes ---
    wr(R_PSEL, 5);
    for (int i = 0; i < 32; ++i) wr(R_PPORT, static_cast<uint8_t>(i));
    wr(R_PSEL, 5);
    bool pslot_ok = true;
    for (int i = 0; i < 32; ++i) pslot_ok &= (rd(R_PPORT) == i);
    expect(pslot_ok, "P5 holds 32 bytes and the index wraps");

    // --- LEN register ---
    wr(R_LEN, 0xAA);
    wr(R_LEN, 0xBB);
    wr(R_LEN, 0xCC);
    wr(R_LEN, 0xDD);
    // Index wrapped back to 0 after 4 bytes.
    expect(rd(R_LEN) == 0xAA, "LEN byte 0 after wrap");

    // --- An unknown verb must report BADVERB, not fail silently ---
    wr(R_CMD, 0x7E);
    const uint8_t bad = rd(R_CMD);
    expect((bad & ST_ERR) != 0, "unknown verb sets ERR");
    expect((bad & 0x0F) == 0x01, "unknown verb reports BADVERB ($01)");

    // --- The FIFO must absorb an unpolled burst without error ---
    // At the real clock ratio the card drains ~24 bytes per bus access, so a CPU
    // writing back-to-back cannot outrun it and the FIFO never fills. That is the
    // point of the FIFO, so the property to assert is the ABSENCE of overrun across
    // a burst that crosses several block boundaries. (The E_OVERRUN guard stays in
    // the RTL as defence in depth; it is unreachable at this ratio, and would only
    // matter on a much slower card clock.)
    wr(R_CMD, V_HASH_INIT);
    wr(R_CMD, V_HASH_PUSH);
    bool stayed_ready = true;
    for (int i = 0; i < 200; ++i) {
        wr(R_DATA, 0x41);
        if (rd(R_CMD) & ST_ERR) { stayed_ready = false; break; }
    }
    expect(stayed_ready, "FIFO absorbs a 200-byte unpolled burst without OVERRUN");

    // A fresh HASH_INIT must abandon that partial message completely. Without this,
    // `init` was a no-op mid-message and the bytes above leaked into the next digest.
    wr(R_CMD, V_HASH_INIT);
    std::string d_empty, p_empty;
    if (hash_push({}, d_empty, p_empty)) {
        expect(d_empty == "e3b0c44298fc1c149afbf4c8996fb924"
                          "27ae41e4649b934ca495991b7852b855",
               "HASH_INIT abandons a partial message");
    } else {
        expect(false, "empty message after a partial one completes");
    }

    // =========================== L3: heap memory =============================
    std::printf("--- L3: heap memory ---\n");

    // --- DATA port: write, rewind, read back ---
    // This is the auto-incrementing pointer the 65816 does not have.
    const uint32_t probe = 0x00001000;
    set_areg(0, probe);
    for (int i = 0; i < 8; ++i) wr(R_DATA, static_cast<uint8_t>(0xA0 + i));
    expect(get_areg(0) == probe + 8, "DATA writes advance the address register");

    set_areg(0, probe);
    bool data_ok = true;
    for (int i = 0; i < 8; ++i) data_ok &= (rd(R_DATA) == 0xA0 + i);
    expect(data_ok, "DATA reads return what was written, in order");
    expect(get_areg(0) == probe + 8, "DATA reads advance the address register");

    // --- TAG reads the same byte but must NOT advance ---
    set_areg(0, probe);
    expect(rd(R_TAG) == 0xA0, "TAG returns the byte at the selected pointer");
    expect(rd(R_TAG) == 0xA0, "TAG is repeatable");
    expect(get_areg(0) == probe, "TAG does not advance the pointer");

    // --- ALLOC: P0's first 4 bytes are the header; LEN is the payload size ---
    wr(R_PSEL, 0);
    wr(R_PPORT, TAG_INT);      // tag
    wr(R_PPORT, 0x00);         // aux, 3 bytes little-endian
    wr(R_PPORT, 0x00);
    wr(R_PPORT, 0x00);
    set_len(8);                // an INT payload is 8 bytes
    wr(R_CMD, V_ALLOC);
    uint8_t st = wait_verb();
    expect((st & ST_ERR) == 0, "ALLOC succeeds");
    const uint32_t obj1 = get_areg(1);
    expect(obj1 == HEAP_BASE, "first ALLOC returns HEAP_BASE");
    expect(heap.peek(obj1) == TAG_INT, "ALLOC writes the tag byte to memory");

    // A second allocation must clear the first: 4 header + 8 payload = 12.
    wr(R_PSEL, 0);
    wr(R_PPORT, TAG_STRING);
    wr(R_PPORT, 0x05);         // aux = length 5
    wr(R_PPORT, 0x00);
    wr(R_PPORT, 0x00);
    set_len(5);                // 5 rounds up to 8, keeping columns aligned
    wr(R_CMD, V_ALLOC);
    st = wait_verb();
    expect((st & ST_ERR) == 0, "second ALLOC succeeds");
    const uint32_t obj2 = get_areg(1);
    expect(obj2 == obj1 + 12, "ALLOC bumps past the previous object");
    expect(obj2 % 4 == 0, "ALLOC keeps objects 4-byte aligned");
    expect(heap.peek(obj2) == TAG_STRING && heap.peek(obj2 + 1) == 0x05,
           "ALLOC writes tag and aux");

    // Regression: a non-hash verb must not be completed by a stale h_done, nor have
    // P0 overwritten by the previous message's digest. Both happened before the
    // completion path was gated on h_finish, and ALLOC's header silently became
    // digest bytes -- with the tag byte still correct, so it looked almost right.
    expect(heap.peek(obj1 + 1) == 0x00 && heap.peek(obj1 + 2) == 0x00 &&
           heap.peek(obj1 + 3) == 0x00,
           "ALLOC header aux bytes are not clobbered by a stale digest");

    // --- ALLOC refuses to run off the end rather than wrapping ---
    set_len(0x00FFFFFF);
    wr(R_CMD, V_ALLOC);
    st = wait_verb();
    expect((st & ST_ERR) != 0 && (st & 0x0F) == 0x03, "oversized ALLOC reports OOM");
    wr(R_CTRL, 0x01);

    // --- FOLLOW: chase an INDIRECT chain ---
    // Build  i1 -> i2 -> i3 -> obj1  directly in memory, then make the card walk it.
    const uint32_t i1 = 0x2000, i2 = 0x2100, i3 = 0x2200;
    for (auto [from, to] : {std::pair{i1, i2}, {i2, i3}, {i3, obj1}}) {
        heap.poke(from, TAG_INDIRECT);
        heap.poke32(from + 4, to);
    }
    set_areg(0, i1);
    wr(R_CMD, V_FOLLOW);
    st = wait_verb();
    expect((st & ST_ERR) == 0, "FOLLOW succeeds");
    expect(get_areg(1) == obj1, "FOLLOW resolves a 3-deep chain to the real object");

    // A non-indirection resolves to itself in one hop.
    set_areg(0, obj1);
    wr(R_CMD, V_FOLLOW);
    st = wait_verb();
    expect((st & ST_ERR) == 0 && get_areg(1) == obj1,
           "FOLLOW on a non-indirection returns it unchanged");

    // --- FOLLOW must not hang on a cycle ---
    // A self-referential INDIRECT is the degenerate case; without the hop cap the
    // card would spin forever and the bus would never see DONE.
    heap.poke(i1, TAG_INDIRECT);
    heap.poke32(i1 + 4, i1);
    set_areg(0, i1);
    wr(R_CMD, V_FOLLOW);
    st = wait_verb();
    expect((st & ST_ERR) != 0, "FOLLOW reports an error on a cyclic chain");
    wr(R_CTRL, 0x01);

    // --- HASH_STREAM: hash card-resident bytes with ZERO data crossing the bus ---
    std::printf("--- L3: HASH_STREAM ---\n");
    {
        const char *msg = "abc";
        const uint32_t at = 0x3000;
        for (int i = 0; msg[i]; ++i) heap.poke(at + i, static_cast<uint8_t>(msg[i]));

        const uint64_t before = bus_accesses;
        wr(R_CMD, V_HASH_INIT);
        set_areg(0, at);
        set_len(3);
        wr(R_CMD, V_HASH_STREAM);
        st = wait_verb();
        expect((st & ST_ERR) == 0, "HASH_STREAM succeeds");
        wr(R_CMD, V_HASH_FINAL);
        st = wait_verb();
        expect((st & ST_ERR) == 0, "HASH_FINAL after HASH_STREAM succeeds");

        uint8_t dig[32];
        read_pslot(0, dig, 32);
        static const char *H = "0123456789abcdef";
        std::string got;
        for (int i = 0; i < 32; ++i) {
            got.push_back(H[dig[i] >> 4]);
            got.push_back(H[dig[i] & 0xf]);
        }
        expect(got == "ba7816bf8f01cfea414140de5dae2223"
                      "b00361a396177a9cb410ff61f20015ad",
               "HASH_STREAM of card-resident \"abc\" matches the NIST vector");
        std::printf("  HASH_STREAM of 3 bytes cost %llu bus accesses "
                    "(none of them data)\n",
                    static_cast<unsigned long long>(bus_accesses - before));
    }

    expect(heap.bad_writes() == 0, "no card writes landed outside heap memory");

    // --- Store-path parity through the real verb sequence ---
    std::printf("--- store-path parity ---\n");
    std::ifstream in(vec_path);
    if (!in) {
        std::fprintf(stderr, "cannot open vectors: %s\n", vec_path);
        return 2;
    }

    int n_sha = 0, n_sp = 0;
    uint64_t accesses_before = 0, total_bytes = 0;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;

        // Run the NIST vectors through the card too. If these fail, the bug is in how
        // the card feeds the hasher; if only the SP cases fail, it is the fold/encode.
        if (kind == "SHA") {
            std::string in_hex, want;
            ls >> in_hex >> want;
            const auto data = unhex(in_hex);
            std::string dig, pth;
            if (!hash_push(data, dig, pth)) {
                std::fprintf(stderr, "TIMEOUT sha len=%zu\n", data.size());
                ++fails;
            } else if (dig != want) {
                std::fprintf(stderr, "FAIL sha via card len=%zu\n  want %s\n  got  %s\n",
                             data.size(), want.c_str(), dig.c_str());
                ++fails;
            } else {
                ++n_sha;
            }
            continue;
        }

        if (kind != "SP") continue;
        std::string fp_hex, want, full;
        ls >> fp_hex >> want >> full;
        const auto fp = unhex(fp_hex);

        accesses_before = bus_accesses;
        std::string dig, got;
        if (!hash_push(fp, dig, got)) {
            std::fprintf(stderr, "TIMEOUT %s\n", full.c_str());
            ++fails;
            continue;
        }
        if (got != want) {
            std::fprintf(stderr, "FAIL storepath\n  want %s\n  got  %s\n  digest %s\n",
                         want.c_str(), got.c_str(), dig.c_str());
            ++fails;
        } else {
            ++n_sp;
            total_bytes += fp.size();
            std::printf("  ok  %s  (%llu bus accesses for %zu bytes)\n",
                        full.c_str(),
                        static_cast<unsigned long long>(bus_accesses - accesses_before),
                        fp.size());
        }
    }

    std::printf("\n%d SHA vectors + %d store-path vectors via the card ISA, %d failures\n",
                n_sha, n_sp, fails);

    // --- Verb economics (DECISIONS.md D5) ---
    // NB: this drives the model, so it must run before final()/delete.
    // Re-run the store-path vectors with per-byte polling to measure what the input
    // FIFO actually buys, rather than asserting a number.
    const uint64_t burst_stream = acc_stream, burst_bytes = bytes_streamed;
    const uint64_t burst_fixed = acc_fixed;
    const int      burst_calls = n_sha + n_sp + 1;
    // Snapshot before the comparison pass so the end-to-end figure reflects the real
    // (burst) strategy only.
    const uint64_t sp_accesses = bus_accesses;

    acc_stream = 0; bytes_streamed = 0;
    in.clear();
    in.seekg(0);
    while (std::getline(in, line)) {
        if (line.rfind("SP ", 0) != 0) continue;
        std::istringstream ls2(line);
        std::string k2, fp_hex, want, full;
        ls2 >> k2 >> fp_hex >> want >> full;
        std::string d, p;
        hash_push(unhex(fp_hex), d, p, /*poll_per_byte=*/true);
    }

    // HASH_STREAM's cost is a FIXED handful of accesses regardless of length, because
    // no data crosses the bus -- only the pointer, the count, and the verb. That is
    // the principle NARHASH takes to its conclusion (CARD-ISA.md).
    uint64_t stream_acc = 0, stream_bytes = 0;
    {
        const uint32_t at = 0x4000;
        // Reuse a real fingerprint so the comparison is against like work.
        std::ifstream v2(vec_path);
        std::string l2;
        std::vector<uint8_t> msg;
        while (std::getline(v2, l2)) {
            if (l2.rfind("SP ", 0) != 0) continue;
            if (l2.find("nix-on-an-apple-iigs") == std::string::npos) continue;
            std::istringstream ls2(l2);
            std::string k, fp;
            ls2 >> k >> fp;
            msg = unhex(fp);
            break;
        }
        for (size_t i = 0; i < msg.size(); ++i)
            heap.poke(at + static_cast<uint32_t>(i), msg[i]);

        const uint64_t a0 = bus_accesses;
        wr(R_CMD, V_HASH_INIT);
        set_areg(0, at);
        set_len(static_cast<uint32_t>(msg.size()));
        wr(R_CMD, V_HASH_STREAM);
        wait_verb();
        wr(R_CMD, V_HASH_FINAL);
        wait_verb();
        uint8_t pth[32];
        read_pslot(3, pth, 32);
        stream_acc   = bus_accesses - a0;
        stream_bytes = msg.size();

        const std::string sp(reinterpret_cast<char *>(pth), 32);
        expect(sp == "fsl59pk3f4c65048v3w7vgazqwg4d75w",
               "HASH_STREAM over a card-resident fingerprint yields the right path");
    }

    std::printf("\n--- verb economics: HASH_PUSH (card %d cycles per bus access) ---\n",
                CARD_CYCLES_PER_ACCESS);
    std::printf("  streaming, 16-byte bursts : %.2f accesses/byte\n",
                static_cast<double>(burst_stream) / static_cast<double>(burst_bytes));
    std::printf("  streaming, poll per byte  : %.2f accesses/byte\n",
                static_cast<double>(acc_stream) / static_cast<double>(bytes_streamed));
    std::printf("  fixed overhead per call   : %.1f accesses"
                " (verbs + reading P0 and P3)\n",
                static_cast<double>(burst_fixed) / static_cast<double>(burst_calls));
    if (total_bytes)
        std::printf("  end-to-end per store path : %.2f accesses/byte\n",
                    static_cast<double>(sp_accesses) / static_cast<double>(total_bytes));
    if (stream_bytes)
        std::printf("  HASH_STREAM, %llu bytes    : %.2f accesses/byte"
                    " (%llu accesses total, length-independent)\n",
                    static_cast<unsigned long long>(stream_bytes),
                    static_cast<double>(stream_acc) / static_cast<double>(stream_bytes),
                    static_cast<unsigned long long>(stream_acc));
    std::printf("  card cycles simulated     : %llu\n",
                static_cast<unsigned long long>(cycles));

    dut->final();
    delete dut;

    if (fails == 0 && n_sp > 0)
        std::printf("L2 CARD PASS: register map and verb dispatch match CARD-ISA.md\n");

    return fails == 0 ? 0 : 1;
}
