// Regression suite for the SILENT-CORRUPTION bug class.
//
// Every bug found in this card so far shared a shape: it produced a plausible-looking
// wrong answer rather than a failure. A byte-reversed digest is still 32 hex bytes. A
// truncated message still hashes. A rotated LEN still streams. An ALLOC header whose
// tag byte survived still looked almost right.
//
// That is the worst failure mode available to a hash accelerator, because the caller
// has no way to notice. So each bug gets a permanent, named test here, with the
// mechanism recorded -- these are cheap to keep and expensive to rediscover.
//
// Every test in this file must be able to FAIL. A test that cannot distinguish a
// working card from a broken one is worse than none: an earlier install check for
// AppleCommander only required non-empty output and passed happily on a Java stack
// trace.

#include "Va2nix_card.h"
#include "verilated.h"

#include "cardbus.h"
#include "heapmem.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

Va2nix_card *card = nullptr;
HeapMem<Va2nix_card> heap;
CardBus<Va2nix_card> *bus = nullptr;

int failures = 0;
int checks = 0;

void check(bool ok, const char *name, const char *guards) {
    ++checks;
    if (ok) {
        std::printf("  ok    %s\n", name);
    } else {
        std::printf("  FAIL  %s\n        guards against: %s\n", name, guards);
        ++failures;
    }
}

const char *SHA_EMPTY =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const char *SHA_ABC =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

std::string hex(const std::uint8_t *b, int n) {
    static const char *H = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < n; ++i) { s += H[b[i] >> 4]; s += H[b[i] & 0xf]; }
    return s;
}

// Hash a message by pushing it over the bus, then read back the digest.
std::string hash_push(const std::string &msg, bool init = true) {
    if (init) bus->wr(R_CMD, V_HASH_INIT);
    bus->wr(R_CMD, V_HASH_PUSH);
    std::size_t i = 0;
    while (i < msg.size()) {
        while (!(bus->rd(R_CMD) & ST_READY)) {}
        const std::size_t burst = std::min<std::size_t>(FIFO_BURST, msg.size() - i);
        for (std::size_t k = 0; k < burst; ++k)
            bus->wr(R_DATA, static_cast<std::uint8_t>(msg[i + k]));
        i += burst;
    }
    bus->wr(R_CMD, V_HASH_FINAL);
    bus->wait_verb();
    std::uint8_t d[32];
    bus->read_pslot(0, d, 32);
    return hex(d, 32);
}

// ---------------------------------------------------------------------------
// R1: a stale h_done must not hijack a non-hash verb.
//
// sha256 parks in its done state holding h_done asserted until the next init. The
// completion path was gated on h_done alone, so ANY verb that set `busy` was
// completed one cycle later -- and had P0 overwritten with the previous message's
// digest. ALLOC's header became digest bytes while the tag byte stayed correct,
// which is precisely why it looked almost right.
void r1_stale_hash_completion() {
    std::printf("R1 stale h_done hijacking a non-hash verb\n");

    // Leave a completed hash behind, so h_done is asserted and P0 is dirty.
    const std::string dig = hash_push("");
    check(dig == SHA_EMPTY, "precondition: a hash completed and P0 holds its digest",
          "test setup, not the card");

    // Now ALLOC with a distinctive header. Every aux byte must survive.
    const std::uint8_t header[4] = { TAG_LIST, 0xAB, 0xCD, 0xEF };
    bus->write_pslot(0, header, 4);
    bus->set_len(16);
    bus->wr(R_CMD, V_ALLOC);
    const std::uint8_t st = bus->wait_verb();
    const std::uint32_t obj = bus->get_areg(1);

    check((st & ST_ERR) == 0, "ALLOC after a hash does not error",
          "stale h_done terminating the verb early");
    check(heap.peek(obj + 0) == TAG_LIST &&
          heap.peek(obj + 1) == 0xAB &&
          heap.peek(obj + 2) == 0xCD &&
          heap.peek(obj + 3) == 0xEF,
          "ALLOC header survives intact after a completed hash",
          "P0 being overwritten by the previous digest mid-verb; the tag byte "
          "survived, so only the aux bytes were wrong");

    // P0 must still read back the header the caller wrote, not a digest. This is the
    // same property from the other side: the verb must not have swapped P0 out.
    std::uint8_t p0[4];
    bus->read_pslot(0, p0, 4);
    check(p0[0] == TAG_LIST && p0[1] == 0xAB && p0[2] == 0xCD && p0[3] == 0xEF,
          "P0 still holds the caller's header after ALLOC",
          "the completion path swapping P0 for a digest during a non-hash verb");
}

// ---------------------------------------------------------------------------
// R2: port byte indices must be recoverable.
//
// APORT and PPORT rewind via ASEL/PSEL, but LEN had no select register. A caller
// that read a PARTIAL LEN left its index rotated permanently, and every later
// 4-byte write landed shifted -- so the card streamed a wrong length, silently.
void r2_port_index_desync() {
    std::printf("R2 desynchronised port indices\n");

    // Deliberately desynchronise LEN by reading a single byte.
    bus->wr(R_CTRL, CTRL_RESET_PORTS);
    for (int i = 0; i < 4; ++i) bus->wr(R_LEN, 0x11 * (i + 1));
    (void)bus->rd(R_LEN);          // one read: index now at 1

    // A correct set_len must recover. Hash 3 card-resident bytes via HASH_STREAM,
    // which depends entirely on LEN being right.
    const std::uint32_t at = 0x5000;
    heap.poke(at + 0, 'a'); heap.poke(at + 1, 'b'); heap.poke(at + 2, 'c');
    bus->wr(R_CMD, V_HASH_INIT);
    bus->set_areg(0, at);
    bus->set_len(3);
    bus->wr(R_CMD, V_HASH_STREAM);
    bus->wait_verb();
    bus->wr(R_CMD, V_HASH_FINAL);
    bus->wait_verb();
    std::uint8_t d[32];
    bus->read_pslot(0, d, 32);

    check(hex(d, 32) == SHA_ABC,
          "HASH_STREAM length is correct after a partial LEN read",
          "a rotated LEN index streaming the wrong byte count, producing a valid "
          "hash of the wrong data");

    // CTRL b3 must rewind APORT and PPORT too.
    bus->set_areg(1, 0x12345678);
    (void)bus->rd(R_APORT);        // desync aidx
    bus->wr(R_CTRL, CTRL_RESET_PORTS);
    const std::uint8_t b0 = bus->rd(R_APORT);
    check(b0 == 0x78, "CTRL b3 rewinds the APORT index",
          "a rotated APORT index reading pointer bytes out of order");
}

// ---------------------------------------------------------------------------
// R3: P slots are byte arrays, not little-endian integers.
//
// PPORT originally indexed from the LSB, which byte-REVERSED every digest and every
// path string. A reversed digest is still 32 plausible hex bytes.
void r3_pslot_byte_order() {
    std::printf("R3 P-slot byte order\n");

    const std::string dig = hash_push("abc");
    check(dig == SHA_ABC, "digest reads out in canonical order, not reversed",
          "PPORT indexing from the LSB, byte-reversing every digest");

    // A written-then-read P slot must round-trip in order, index 0 first.
    std::uint8_t out[8];
    const std::uint8_t in[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    bus->write_pslot(6, in, 8);
    bus->read_pslot(6, out, 8);
    bool ordered = true;
    for (int i = 0; i < 8; ++i) ordered &= (out[i] == i);
    check(ordered, "P slot round-trips byte 0 first",
          "byte-array ports being treated as little-endian integers");

    // A registers must be the OPPOSITE convention: LSB first.
    bus->set_areg(2, 0xDEADBEEF);
    check(bus->get_areg(2) == 0xDEADBEEF,
          "A register round-trips as a little-endian integer",
          "confusing the two port conventions");
}

// ---------------------------------------------------------------------------
// R4: bytes must never be dropped between the bus and the hasher.
//
// READY was derived from the hasher's state alone, but a DATA write latches a byte
// the hasher consumes one edge later. For that cycle the CPU saw READY, wrote
// again, and the in-flight byte was overwritten -- only ever at a 64-byte block
// boundary. It showed up as exactly 8 of 12 NIST vectors passing.
void r4_dropped_bytes_at_block_boundary(const char *vec_path) {
    std::printf("R4 dropped bytes at block boundaries\n");

    // Both paths are checked against ABSOLUTE ground truth from tests/vectors/l1.txt
    // (generated by the real nix/hashlib), never against each other.
    //
    // An earlier version of this test compared HASH_PUSH against HASH_STREAM and
    // called agreement success. Mutation testing showed that to be decorative: both
    // paths share the FIFO drain, so a drain bug corrupts them identically and they
    // still agree. Two wrong answers that match are not evidence.
    std::ifstream vec(vec_path);
    if (!vec) {
        check(false, "vectors file is readable", "a missing vectors file passing by default");
        return;
    }

    int n_push = 0, n_stream = 0, bad = 0;
    std::string line;
    while (std::getline(vec, line)) {
        if (line.rfind("SHA ", 0) != 0) continue;
        std::istringstream ls(line);
        std::string kind, in_hex, want;
        ls >> kind >> in_hex >> want;

        std::string msg;
        if (in_hex != "-")
            for (std::size_t i = 0; i + 1 < in_hex.size(); i += 2)
                msg.push_back(static_cast<char>(
                    std::stoul(in_hex.substr(i, 2), nullptr, 16)));

        // Path 1: pushed over the bus.
        const std::string pushed = hash_push(msg);
        if (pushed != want) {
            std::printf("        PUSH len %zu: want %s got %s\n",
                        msg.size(), want.c_str(), pushed.c_str());
            ++bad;
        } else {
            ++n_push;
        }

        // Path 2: streamed from card memory by the engine -- a different route into
        // the hasher, but the same absolute expectation.
        const std::uint32_t at = 0x6000;
        for (std::size_t i = 0; i < msg.size(); ++i)
            heap.poke(at + static_cast<std::uint32_t>(i),
                      static_cast<std::uint8_t>(msg[i]));
        bus->wr(R_CMD, V_HASH_INIT);
        bus->set_areg(0, at);
        bus->set_len(static_cast<std::uint32_t>(msg.size()));
        bus->wr(R_CMD, V_HASH_STREAM);
        bus->wait_verb();
        bus->wr(R_CMD, V_HASH_FINAL);
        bus->wait_verb();
        std::uint8_t d[32];
        bus->read_pslot(0, d, 32);
        const std::string streamed = hex(d, 32);
        if (streamed != want) {
            std::printf("        STREAM len %zu: want %s got %s\n",
                        msg.size(), want.c_str(), streamed.c_str());
            ++bad;
        } else {
            ++n_stream;
        }
    }

    check(bad == 0 && n_push > 0 && n_stream > 0,
          "both hash paths match absolute vectors at every block boundary",
          "a byte dropped or duplicated on either path, at 64-byte boundaries");
    std::printf("        (%d push + %d stream vectors verified)\n", n_push, n_stream);
}

// ---------------------------------------------------------------------------
// R5: a new message must abandon the previous one completely.
//
// sha256's `init` was only honoured in its idle/done states, making "start a new
// message" a no-op mid-message -- so a partial message leaked into the next digest.
void r5_init_abandons_partial_message() {
    std::printf("R5 init must abandon a partial message\n");

    // Push a partial message and never finalise it.
    bus->wr(R_CMD, V_HASH_INIT);
    bus->wr(R_CMD, V_HASH_PUSH);
    for (int i = 0; i < 100; ++i) bus->wr(R_DATA, 0x5A);

    // Now start over. The empty-string digest must be exactly right.
    const std::string dig = hash_push("");
    check(dig == SHA_EMPTY, "empty digest is correct after an abandoned message",
          "init being ignored mid-message, letting the previous partial message "
          "leak into the next digest");
}

// ---------------------------------------------------------------------------
// R6: errors must be raised, not swallowed.
//
// Every failure mode the card can detect has to surface in STATUS. A card that
// absorbs a problem quietly is indistinguishable from one that worked.
void r6_errors_surface() {
    std::printf("R6 detectable errors surface in STATUS\n");

    bus->wr(R_CMD, 0x7E);                      // not a verb
    std::uint8_t st = bus->rd(R_CMD);
    check((st & ST_ERR) && (st & 0x0F) == E_BADVERB,
          "an unknown verb reports BADVERB",
          "unknown verbs being silently ignored");
    bus->wr(R_CTRL, CTRL_RESET);

    bus->set_len(0x00FFFFFF);                  // far beyond HEAP_TOP
    bus->wr(R_CMD, V_ALLOC);
    st = bus->wait_verb();
    check((st & ST_ERR) && (st & 0x0F) == E_OOM,
          "an oversized ALLOC reports OOM",
          "the bump allocator wrapping past the end of memory");
    bus->wr(R_CTRL, CTRL_RESET);

    // A self-referential INDIRECT would spin the card forever without the hop cap,
    // and the bus would simply never see DONE.
    const std::uint32_t loop_at = 0x7000;
    heap.poke(loop_at, TAG_INDIRECT);
    heap.poke32(loop_at + 4, loop_at);
    bus->set_areg(0, loop_at);
    bus->wr(R_CMD, V_FOLLOW);
    st = bus->wait_verb();
    check((st & ST_ERR) != 0, "a cyclic INDIRECT chain reports an error",
          "FOLLOW spinning forever on a cycle instead of reporting");
    bus->wr(R_CTRL, CTRL_RESET);

    check(heap.bad_writes() == 0, "no card write landed outside heap memory",
          "out-of-range writes being absorbed by the harness");
}

// ---------------------------------------------------------------------------
// L3 LOOKUP: attribute-set lookup via the key-hash CAM.
//
// Builds ATTRS objects directly in heap memory using the layout in docs/HEAP.md --
// parallel columns, key hashes first so the CAM can stream them (the position
// column after the names is the CPU's alone, and left out here).
//
// Two properties matter beyond "it finds things":
//   - sets larger than one CAM batch must still be searched (multi-pass);
//   - a hash COLLISION must return the lowest matching index, not an arbitrary one,
//     because attributes are sorted and the caller resolves collisions by name.
std::uint32_t build_attrs(std::uint32_t at,
                          const std::vector<std::uint32_t> &key_hashes,
                          const std::vector<std::uint32_t> &values) {
    const std::uint32_t n = static_cast<std::uint32_t>(key_hashes.size());
    heap.poke(at, TAG_ATTRS);
    heap.poke(at + 1, static_cast<std::uint8_t>(n));
    heap.poke(at + 2, static_cast<std::uint8_t>(n >> 8));
    heap.poke(at + 3, static_cast<std::uint8_t>(n >> 16));
    for (std::uint32_t i = 0; i < n; ++i) {
        heap.poke32(at + 4 + 4 * i, key_hashes[i]);              // key column
        heap.poke32(at + 4 + 4 * n + 4 * i, values[i]);          // value column
        // Name column left null: these tests exercise the CAM, and name resolution
        // is the caller's job (docs/HEAP.md).
        heap.poke32(at + 4 + 8 * n + 4 * i, 0);
    }
    return at;
}

void set_key(std::uint32_t k) {
    const std::uint8_t b[4] = {
        static_cast<std::uint8_t>(k),
        static_cast<std::uint8_t>(k >> 8),
        static_cast<std::uint8_t>(k >> 16),
        static_cast<std::uint8_t>(k >> 24),
    };
    bus->write_pslot(0, b, 4);
}

void l3_lookup() {
    std::printf("L3 LOOKUP via the key-hash CAM\n");

    // --- a small set, one CAM pass ---
    std::vector<std::uint32_t> keys, vals;
    for (int i = 0; i < 10; ++i) {
        keys.push_back(0x1000u + static_cast<std::uint32_t>(i) * 0x111u);
        vals.push_back(0xA000u + static_cast<std::uint32_t>(i) * 0x10u);
    }
    const std::uint32_t small = build_attrs(0x9000, keys, vals);

    bool all = true;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        bus->set_areg(0, small);
        set_key(keys[i]);
        bus->wr(R_CMD, V_LOOKUP);
        const std::uint8_t st = bus->wait_verb();
        if ((st & ST_ERR) || bus->get_areg(1) != vals[i]) all = false;
    }
    check(all, "LOOKUP finds every key in a 10-attribute set",
          "an off-by-one in the value column address");

    // --- the same set while the CPU deep-forces it: bit 23 of aux is the visit
    // mark (docs/HEAP.md), not part of the count. A SELECT inside the deep force
    // of a set that refers to itself (`let a = { b = 1; c = a.b; }; in a`, under
    // GC stress) used to read its value from base + 4 + 4 * (n | 2^23).
    heap.poke(small + 3, 0x80);
    bool marked = true;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        bus->set_areg(0, small);
        set_key(keys[i]);
        bus->wr(R_CMD, V_LOOKUP);
        const std::uint8_t s = bus->wait_verb();
        if ((s & ST_ERR) || bus->get_areg(1) != vals[i]) marked = false;
    }
    heap.poke(small + 3, 0x00);
    check(marked, "LOOKUP finds every key in a set the CPU has marked",
          "counting the visit mark: a value read from far past the set -- null, "
          "or worse, a real pointer from the wrong object");

    // --- a miss must report NOTFOUND, not return a neighbouring value ---
    bus->set_areg(0, small);
    set_key(0xDEADBEEF);
    bus->wr(R_CMD, V_LOOKUP);
    std::uint8_t st = bus->wait_verb();
    check((st & ST_ERR) && (st & 0x0F) == E_NOTFOUND,
          "a missing key reports NOTFOUND",
          "returning a stale or adjacent value pointer on a miss, which would look "
          "like a successful lookup of the wrong attribute");
    bus->wr(R_CTRL, CTRL_RESET);

    // --- more attributes than CAM lanes, forcing multiple passes ---
    keys.clear(); vals.clear();
    for (int i = 0; i < 100; ++i) {
        keys.push_back(0x50000u + static_cast<std::uint32_t>(i));
        vals.push_back(0xB0000u + static_cast<std::uint32_t>(i) * 4u);
    }
    const std::uint32_t big = build_attrs(0x20000, keys, vals);

    // Probe the first lane, a lane inside a later batch, and the very last.
    const int probes[] = { 0, 31, 32, 63, 64, 99 };
    bool multi = true;
    for (int i : probes) {
        bus->set_areg(0, big);
        set_key(keys[static_cast<std::size_t>(i)]);
        bus->wr(R_CMD, V_LOOKUP);
        const std::uint8_t s = bus->wait_verb();
        const std::uint32_t got = bus->get_areg(1);
        if ((s & ST_ERR) || got != vals[static_cast<std::size_t>(i)]) {
            std::printf("        index %d: want %08x got %08x status %02x\n",
                        i, vals[static_cast<std::size_t>(i)], got, s);
            multi = false;
        }
    }
    check(multi, "LOOKUP searches a 100-attribute set across CAM batches",
          "only ever examining the first 32 lanes, so any later attribute silently "
          "reports NOTFOUND");

    // --- a collision must resolve to the LOWEST index ---
    // Attributes are sorted, and the caller disambiguates by name; returning an
    // arbitrary match would hand back a plausible value from the wrong key.
    keys = { 0x11111111, 0x22222222, 0x22222222, 0x33333333 };
    vals = { 0xC0000000, 0xC0000001, 0xC0000002, 0xC0000003 };
    const std::uint32_t dup = build_attrs(0x30000, keys, vals);
    bus->set_areg(0, dup);
    set_key(0x22222222);
    bus->wr(R_CMD, V_LOOKUP);
    st = bus->wait_verb();
    const std::uint32_t got = bus->get_areg(1);
    std::uint8_t p1[4];
    bus->read_pslot(1, p1, 4);
    const std::uint32_t idx = static_cast<std::uint32_t>(p1[0])
                            | (static_cast<std::uint32_t>(p1[1]) << 8)
                            | (static_cast<std::uint32_t>(p1[2]) << 16);
    check((st & ST_ERR) == 0 && got == 0xC0000001 && idx == 1,
          "a hash collision resolves to the lowest matching index",
          "returning an arbitrary colliding attribute -- a valid value from the "
          "wrong key, which no caller could detect");
}

// ---------------------------------------------------------------------------
// L3 ALU64: 64-bit integer arithmetic.
//
// The host's int64 arithmetic is the oracle. That is sound because C's `/` and `%`
// truncate toward zero with the remainder taking the dividend's sign -- exactly
// Nix's semantics -- and a wrapping multiply's low 64 bits are the same signed or
// unsigned. The one case C leaves undefined, INT64_MIN / -1, is checked against a
// hardcoded wrapped value instead of computed.
enum : std::uint8_t {
    A_ADD = 0, A_SUB = 1, A_MUL = 2, A_DIV = 3, A_MOD = 4,
    A_AND = 5, A_OR = 6, A_XOR = 7, A_CMP = 8, A_NEG = 9,
};

void put_i64(int slot, std::int64_t v) {
    std::uint8_t b[8];
    const std::uint64_t u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(u >> (8 * i));
    bus->write_pslot(slot, b, 8);
}

std::int64_t get_i64(int slot) {
    std::uint8_t b[8];
    bus->read_pslot(slot, b, 8);
    std::uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= static_cast<std::uint64_t>(b[i]) << (8 * i);
    return static_cast<std::int64_t>(u);
}

// Returns the status; result via out-param so an ERR case is not misread as a value.
std::uint8_t alu(std::uint8_t op, std::int64_t a, std::int64_t b, std::int64_t &out) {
    put_i64(0, a);
    put_i64(1, b);
    const std::uint8_t opb[1] = { op };
    bus->write_pslot(2, opb, 1);
    bus->wr(R_CMD, V_ALU64);
    const std::uint8_t st = bus->wait_verb();
    out = (st & ST_ERR) ? 0 : get_i64(3);
    return st;
}

// L3 MEMCMP: compare two heap regions card-side and report the sign.
//
// The verb exists so the CPU can confirm a LOOKUP candidate's name without
// copying the whole attribute set back across the bus (docs/CARD-ISA.md).
// Four properties matter beyond "equal strings compare equal":
//
//   - the sign must be in ADDRESS order, not little-endian word order. The
//     memory port returns whole words, so a raw 32-bit compare gets the sign
//     backwards for names differing in a low byte, and every sorted-name merge
//     built on it would be wrong;
//   - a difference in the LAST partial word must still be seen;
//   - bytes beyond LEN must NOT be seen -- the caller's length is what decides,
//     not the zero padding HEAP.md happens to guarantee;
//   - an unaligned operand must be refused, because a word-at-a-time compare
//     would silently answer about the word CONTAINING the address instead.
void l3_memcmp() {
    std::printf("L3 MEMCMP card-side comparison\n");

    auto put = [](std::uint32_t at, const char *s, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            heap.poke(at + i, static_cast<std::uint8_t>(s[i]));
    };
    // Returns the sign, or -128 if the card reported an error.
    auto cmp = [](std::uint32_t a, std::uint32_t b, std::uint32_t len) -> int {
        bus->set_areg(0, a);
        bus->set_areg(1, b);
        bus->set_len(len);
        bus->wr(R_CMD, V_MEMCMP);
        const std::uint8_t st = bus->wait_verb();
        if (st & ST_ERR) { bus->wr(R_CTRL, CTRL_RESET); return -128; }
        std::uint8_t p0 = 0;
        bus->read_pslot(0, &p0, 1);
        return static_cast<std::int8_t>(p0);
    };

    const std::uint32_t A = 0xB000, B = 0xB100, C = 0xB200;

    put(A, "attribute_name\0\0", 16);
    put(B, "attribute_name\0\0", 16);
    check(cmp(A, B, 14) == 0, "MEMCMP reports 0 for equal regions",
          "a LOOKUP candidate's name never confirming, so every promoted "
          "attribute set would look empty");

    // "abcd" and "abdc" differ at byte 2, so in address order "abcd" is the
    // SMALLER. Read as little-endian words they are 0x64636261 and 0x63646261,
    // which makes "abcd" the LARGER -- opposite signs, so this pair is what
    // catches a raw word compare.
    put(A, "abcd", 4);
    put(B, "abdc", 4);
    check(cmp(A, B, 4) < 0 && cmp(B, A, 4) > 0,
          "MEMCMP orders by ADDRESS order, so the byte at +0 is most significant",
          "comparing the raw little-endian words, which sorts names by their "
          "last byte first and corrupts every sorted-name merge");

    // A difference in the final partial word must be seen...
    put(A, "aaaaa", 5);
    put(B, "aaaab", 5);
    check(cmp(A, B, 5) < 0, "MEMCMP sees a difference in the last partial word",
          "stopping at the last whole word, so two names differing only in "
          "their tail would compare equal");

    // ...and bytes past LEN must not be: same first four, differ at byte 4.
    check(cmp(A, B, 4) == 0, "MEMCMP ignores bytes beyond LEN",
          "reading the final word unmasked, so a short name would be decided "
          "by whatever follows it in the heap");

    // The mask must hold even when the bytes past LEN are not padding: three
    // significant bytes, differing in the fourth byte of the same word.
    put(A, "aaaX", 4);
    put(B, "aaaY", 4);
    check(cmp(A, B, 3) == 0, "MEMCMP masks the bytes past LEN in a partial word",
          "comparing the whole final word, so a three-byte name would be "
          "decided by whichever byte happens to follow it");

    check(cmp(A, B, 0) == 0, "MEMCMP of zero bytes is equal",
          "an empty-name compare hanging the card or reporting a difference");

    // More than one word, differing only in the third.
    put(A, "0123456789abXdef", 16);
    put(B, "0123456789abYdef", 16);
    check(cmp(A, B, 16) < 0, "MEMCMP walks past the first word",
          "comparing only the first four bytes, which would confirm the wrong "
          "attribute for any name sharing a prefix");

    put(C, "abcd", 4);
    check(cmp(A + 1, C, 4) == -128 && cmp(A, C + 2, 4) == -128,
          "MEMCMP refuses an unaligned operand",
          "comparing the aligned word that CONTAINS the address, which would "
          "answer about bytes the caller never asked about");
}

void l3_alu64() {
    std::printf("L3 ALU64 64-bit integer arithmetic\n");

    // The D5 gate: the whole cost of a card multiply, in bus accesses, against the
    // 9819 CPU cycles `just bench-mul64` measures for the 65816 doing it alone.
    {
        const std::uint64_t a0 = bus->accesses();
        std::int64_t tmp;
        alu(A_MUL, 123456789LL, 987654321LL, tmp);
        std::printf("        ALU64 MUL: %llu bus accesses end to end "
                    "(operands in, verb, poll, result out)\n",
                    static_cast<unsigned long long>(bus->accesses() - a0));
    }

    struct Case { std::uint8_t op; std::int64_t a, b; const char *name; };
    // Values chosen to exercise carries across every 16-bit word the 65816 would
    // have to juggle, negative operands, and sign fixups on division.
    const Case cases[] = {
        { A_ADD, 0x0123456789ABCDEFLL, 0x0FEDCBA987654321LL, "ADD with carries across words" },
        { A_ADD, -1, 1, "ADD to zero across the sign" },
        { A_SUB, 5, 7, "SUB going negative" },
        { A_SUB, INT64_MIN, 1, "SUB wrapping at INT64_MIN" },
        { A_MUL, 123456789LL, 987654321LL, "MUL 64-bit product" },
        { A_MUL, -3, 7, "MUL negative x positive" },
        { A_MUL, -3, -7, "MUL negative x negative" },
        { A_MUL, 0x100000000LL, 0x100000000LL, "MUL overflow wraps to zero" },
        { A_DIV, 100, 7, "DIV positive" },
        { A_DIV, -7, 2, "DIV truncates toward zero: -7/2 = -3" },
        { A_DIV, 7, -2, "DIV negative divisor" },
        { A_DIV, -7, -2, "DIV both negative" },
        { A_MOD, 100, 7, "MOD positive" },
        { A_MOD, -7, 2, "MOD takes the dividend's sign: -7%2 = -1" },
        { A_MOD, 7, -2, "MOD positive dividend, negative divisor: 1" },
        { A_MOD, -7, -2, "MOD both negative: -1" },
        { A_DIV, INT64_MIN, 2, "DIV of INT64_MIN" },
        { A_AND, 0x0F0F0F0F0F0F0F0FLL, 0x00FF00FF00FF00FFLL, "AND" },
        { A_OR,  0x0F0F0F0F0F0F0F0FLL, 0x00FF00FF00FF00FFLL, "OR" },
        { A_XOR, 0x0F0F0F0F0F0F0F0FLL, 0x00FF00FF00FF00FFLL, "XOR" },
        { A_NEG, 42, 0, "NEG" },
        { A_NEG, INT64_MIN, 0, "NEG of INT64_MIN wraps to itself" },
    };

    int ok = 0, bad = 0;
    for (const Case &c : cases) {
        std::int64_t want;
        const std::uint64_t ua = static_cast<std::uint64_t>(c.a);
        const std::uint64_t ub = static_cast<std::uint64_t>(c.b);
        switch (c.op) {
            case A_ADD: want = static_cast<std::int64_t>(ua + ub); break;
            case A_SUB: want = static_cast<std::int64_t>(ua - ub); break;
            case A_MUL: want = static_cast<std::int64_t>(ua * ub); break;
            case A_DIV: want = c.a / c.b; break;
            case A_MOD: want = c.a % c.b; break;
            case A_AND: want = c.a & c.b; break;
            case A_OR:  want = c.a | c.b; break;
            case A_XOR: want = c.a ^ c.b; break;
            case A_NEG: want = static_cast<std::int64_t>(0 - ua); break;
            default:    want = 0; break;
        }
        std::int64_t got;
        const std::uint8_t st = alu(c.op, c.a, c.b, got);
        if ((st & ST_ERR) || got != want) {
            std::printf("        %s: want %lld got %lld status %02x\n",
                        c.name, static_cast<long long>(want),
                        static_cast<long long>(got), st);
            ++bad;
        } else {
            ++ok;
        }
    }
    check(bad == 0, "every ALU64 case matches int64 arithmetic",
          "a carry dropped between words, or a sign fixup applied to the wrong "
          "operand -- an answer of the right magnitude and wrong sign");
    std::printf("        (%d cases)\n", ok);

    // --- CMP returns -1 / 0 / 1 ---
    std::int64_t r;
    bool cmp_ok = true;
    alu(A_CMP, -5, 3, r);  cmp_ok &= (r == -1);
    alu(A_CMP, 3, 3, r);   cmp_ok &= (r == 0);
    alu(A_CMP, 3, -5, r);  cmp_ok &= (r == 1);
    // The case a naive subtract-and-check-sign gets wrong: the difference overflows.
    alu(A_CMP, INT64_MAX, INT64_MIN, r); cmp_ok &= (r == 1);
    alu(A_CMP, INT64_MIN, INT64_MAX, r); cmp_ok &= (r == -1);
    check(cmp_ok, "CMP is a true signed compare, not a subtract",
          "comparing by subtraction, which overflows for operands of opposite sign "
          "and reports INT64_MAX < INT64_MIN");

    // --- Division by zero must be an ERROR, never a silent value ---
    std::uint8_t st = alu(A_DIV, 42, 0, r);
    check((st & ST_ERR) && (st & 0x0F) == E_BADPARAM,
          "DIV by zero reports BADPARAM",
          "returning a wrapped or zero result for x/0, which Nix would have thrown on");
    bus->wr(R_CTRL, CTRL_RESET);
    st = alu(A_MOD, 42, 0, r);
    check((st & ST_ERR) != 0, "MOD by zero reports an error",
          "the modulo path missing the zero-divisor check the divide path has");
    bus->wr(R_CTRL, CTRL_RESET);

    // --- The one case C leaves undefined: INT64_MIN / -1 wraps to INT64_MIN ---
    st = alu(A_DIV, INT64_MIN, -1, r);
    check((st & ST_ERR) == 0 && r == INT64_MIN,
          "INT64_MIN / -1 wraps rather than hanging or erroring",
          "the magnitude of INT64_MIN not fitting the divider's unsigned path");

    // --- An unknown op must be rejected ---
    st = alu(0xF, 1, 1, r);
    check((st & ST_ERR) != 0, "an unknown ALU op reports an error",
          "an unknown op silently producing some default result");
    bus->wr(R_CTRL, CTRL_RESET);
}

// ---------------------------------------------------------------------------
// L3 MARK/COMPACT: the Cheney semispace collector.
//
// Objects are laid down directly in from-space with heap.poke, a root list is placed
// in a scratch area, and the collector is asked to copy. What is checked is what a
// copying collector gets SILENTLY wrong:
//   - garbage copied along with live data (wasted space, but "works");
//   - a shared object copied twice (aliasing broken, but both copies look fine);
//   - a cycle never terminating;
//   - ATTRS hash values traced as pointers (random memory "evacuated").
// The to-space watermark -- where the next ALLOC lands after COMPACT -- is the
// instrument: it says exactly how many bytes survived.

// Write a header + payload words at `at`, returning `at`.
std::uint32_t lay(std::uint32_t at, std::uint8_t tag, std::uint32_t aux,
                  std::initializer_list<std::uint32_t> words) {
    heap.poke32(at, static_cast<std::uint32_t>(tag) | (aux << 8));
    std::uint32_t off = 4;
    for (std::uint32_t w : words) { heap.poke32(at + off, w); off += 4; }
    return at;
}

// Root list lives high in from-space, well clear of the test objects.
constexpr std::uint32_t ROOTS = SPACE_A + 0x70000;

std::uint8_t mark(std::initializer_list<std::uint32_t> roots) {
    std::uint32_t i = 0;
    for (std::uint32_t r : roots) { heap.poke32(ROOTS + 4 * i, r); ++i; }
    bus->set_areg(0, ROOTS);
    bus->set_len(i);
    bus->wr(R_CMD, V_MARK);
    return bus->wait_verb();
}

std::uint32_t root(int i) { return heap.peek32(ROOTS + 4 * static_cast<std::uint32_t>(i)); }

// Where the next allocation would land: the survivors' watermark after COMPACT.
std::uint32_t watermark() {
    const std::uint8_t hdr[4] = { TAG_NULL, 0, 0, 0 };
    bus->write_pslot(0, hdr, 4);
    bus->set_len(0);
    bus->wr(R_CMD, V_ALLOC);
    bus->wait_verb();
    return bus->get_areg(1);
}

void l3_gc() {
    std::printf("L3 MARK/COMPACT semispace collector\n");
    // Fresh card state for the collector: allocation is in space A, to-space is B.
    bus->wr(R_CTRL, CTRL_RESET);

    // --- (a) live copied, garbage not; roots rewritten in place ---
    const std::uint32_t A = lay(0x1000, TAG_INT, 0, { 0x11111111, 0x22222222 });
    /* dead: */          lay(0x1010, TAG_INT, 0, { 0xDEADDEAD, 0xDEADDEAD });
    const std::uint32_t F = lay(0x1020, TAG_FLOAT, 0, { 0x33333333, 0x44444444 });
    std::uint8_t st = mark({ A, F });
    check((st & ST_ERR) == 0, "MARK completes", "the collector erroring on a live INT");
    const std::uint32_t A2 = root(0);
    check(A2 >= SPACE_B && A2 < SPACE_B + SPACE_SIZE,
          "the root is rewritten to a to-space address",
          "roots not written back, leaving every caller pointer dangling after the flip");
    check(heap.peek32(A2 + 4) == 0x11111111 && heap.peek32(A2 + 8) == 0x22222222,
          "the copied INT carries its payload",
          "copying the header only, or copying from the wrong offset");
    check(heap.peek(A) == TAG_FORWARDED && heap.peek32(A + 4) == A2,
          "the original is left as a forwarding pointer",
          "a shared object being copied once per reference");
    const std::uint32_t F2 = root(1);
    check(heap.peek(F2) == TAG_FLOAT && heap.peek32(F2 + 4) == 0x33333333 && heap.peek32(F2 + 8) == 0x44444444,
          "a copied FLOAT carries all 8 bytes of its double",
          "FLOAT sized by the 8-byte default: the double's high half left behind");
    bus->wr(R_CMD, V_COMPACT);
    bus->wait_verb();
    check(watermark() == SPACE_B + 12 + 12,
          "after COMPACT an INT and a FLOAT survived, 12 bytes each; the dead INT never touched",
          "garbage being copied along with live data, or a FLOAT sized by the 8-byte default");
    // Now allocating in B; A is the empty to-space.

    // --- (b) a DAG: two references to one object -> copied once, both updated ---
    const std::uint32_t X = lay(SPACE_B + 0x2000, TAG_INT, 0, { 0xAAAAAAAA, 0 });
    const std::uint32_t L = lay(SPACE_B + 0x2010, TAG_LIST, 2, { X, X });
    const std::uint64_t c0 = bus->cycles(), a0 = bus->accesses();
    st = mark({ L });
    // The D5 view of a collection: the bus sees only the verb and its polls; the
    // work is O(survivors) in card cycles, and the bus never carries an object.
    std::printf("        MARK of 24 live bytes: %llu bus accesses, %llu card cycles\n",
                static_cast<unsigned long long>(bus->accesses() - a0),
                static_cast<unsigned long long>(bus->cycles() - c0));
    check((st & ST_ERR) == 0, "MARK of a DAG completes", "n/a");
    const std::uint32_t L2 = root(0);
    const std::uint32_t e0 = heap.peek32(L2 + 4), e1 = heap.peek32(L2 + 8);
    if (e0 != e1)
        std::printf("        X=%08x L=%08x -> L2=%08x e0=%08x e1=%08x  X.hdr=%02x X+4=%08x\n",
                    X, L, L2, e0, e1, heap.peek(X), heap.peek32(X + 4));
    check(e0 == e1 && e0 >= SPACE_A && e0 < SPACE_A + SPACE_SIZE,
          "both list elements point at the SAME to-space copy",
          "the scan not tracing list elements (they would still point at from-space), "
          "or the shared object copied twice (they would differ)");
    check(heap.peek32(e0 + 4) == 0xAAAAAAAA, "the shared INT's payload survived", "n/a");
    bus->wr(R_CMD, V_COMPACT);
    bus->wait_verb();
    check(watermark() == SPACE_A + 12 + 12,
          "a 2-element list sharing one INT costs 12 + 12, not 12 + 24",
          "a shared object copied once per reference -- aliasing silently broken");
    // Allocating in A again.

    // --- (c) a cycle must terminate ---
    // (a closure's environment is its word 1: word 0 is the VM's code offsets)
    const std::uint32_t T1 = SPACE_A + 0x3000, T2 = SPACE_A + 0x3010;
    lay(T1, TAG_THUNK, 0, { 0, T2 });
    lay(T2, TAG_THUNK, 0, { 0, T1 });
    st = mark({ T1 });
    check((st & (ST_DONE | ST_ERR)) == ST_DONE,
          "MARK terminates on a cyclic graph",
          "the collector chasing a cycle forever; without forwarding it never stops");
    const std::uint32_t T1n = root(0);
    const std::uint32_t T2n = heap.peek32(T1n + 8);
    check(heap.peek32(T2n + 8) == T1n,
          "the cycle is intact in to-space, pointing at the copies",
          "one arm of the cycle still pointing back into from-space");
    bus->wr(R_CMD, V_COMPACT);
    bus->wait_verb();
    check(watermark() == SPACE_B + 24, "a 2-thunk cycle costs exactly 2 x 12 bytes",
          "an object in a cycle being copied more than once");
    // Allocating in B.

    // --- (d) ATTRS: the hash column must NOT be traced ---
    // A canary object that is unreachable except via values that merely LOOK like
    // pointers to it. If the collector traces the hash column, it will copy the
    // canary and the watermark will show it.
    const std::uint32_t canary = lay(SPACE_B + 0x4000, TAG_INT, 0, { 0xCA11A57, 0 });
    const std::uint32_t S = lay(SPACE_B + 0x4010, TAG_ATTRS, 2,
                                { canary, canary,      // "hashes" == the canary's address
                                  0, 0,                // values: null
                                  0, 0,                // names: null
                                  0 });                // two positions, none (HEAP.md)
    st = mark({ S });
    check((st & ST_ERR) == 0, "MARK of an ATTRS completes", "n/a");
    const std::uint32_t S2 = root(0);
    check(heap.peek32(S2 + 4) == canary && heap.peek32(S2 + 8) == canary,
          "the hash column is copied verbatim, not rewritten as pointers",
          "hashes that happen to equal an address being 'forwarded'");
    check(heap.peek(canary) != TAG_FORWARDED,
          "the canary reachable only through the hash column was NOT evacuated",
          "the ATTRS hash column traced as pointers -- random memory copied, and "
          "hash values corrupted into addresses");
    bus->wr(R_CMD, V_COMPACT);
    bus->wait_verb();
    check(watermark() == SPACE_A + 4 + 24 + 4, "only the 32-byte ATTRS survived",
          "the canary being copied");
    // Allocating in A.

    // --- (e) null passes through; a second MARK appends and leaves evacuated roots alone ---
    const std::uint32_t P = lay(SPACE_A + 0x5000, TAG_INT, 0, { 1, 0 });
    const std::uint32_t Q = lay(SPACE_A + 0x5010, TAG_INT, 0, { 2, 0 });
    st = mark({ P, 0 });
    check((st & ST_ERR) == 0 && root(1) == 0, "a null root stays null",
          "null being 'evacuated' to some address");
    const std::uint32_t Pn = root(0);
    st = mark({ Q, Pn });                     // Pn is already in to-space
    check((st & ST_ERR) == 0 && root(1) == Pn,
          "a root already in to-space passes through unchanged on a later MARK",
          "re-evacuating an evacuated object, duplicating it");
    check(root(0) >= SPACE_B && root(0) != Pn, "the second MARK appended a new survivor",
          "n/a");
    bus->wr(R_CMD, V_COMPACT);
    bus->wait_verb();
    check(watermark() == SPACE_B + 24, "two MARKs before one COMPACT: 2 x 12 survived",
          "the second MARK restarting to-space and orphaning the first's survivors");
    // Allocating in B.

    // --- (f) a closure's word 0 is not a pointer ---
    // The VM keeps its code and constant-table offsets there (HEAP.md CL_CODE,
    // CL_CTAB): (ctab << 16) | code, which for a constant index under 8 or 16
    // is an address inside a semispace. A canary sits at the address a THUNK's
    // and a LAMBDA's word 0 name; only their environments are pointers.
    const std::uint32_t can = lay(SPACE_B + 0x6000, TAG_INT, 0, { 0xC0DE0FF5, 0 });
    const std::uint32_t env = lay(SPACE_B + 0x6010, TAG_INT, 0, { 0xE1, 0 });
    const std::uint32_t Th = lay(SPACE_B + 0x6020, TAG_THUNK, 0, { can, env });
    const std::uint32_t La = lay(SPACE_B + 0x6030, TAG_LAMBDA, 0, { can, env });
    st = mark({ Th, La });
    check((st & ST_ERR) == 0, "MARK of two closures completes", "n/a");
    check(heap.peek(can) != TAG_FORWARDED && heap.peek32(root(0) + 4) == can && heap.peek32(root(1) + 4) == can,
          "a closure's word 0 is copied verbatim and the INT it names is not evacuated",
          "word 0 of a THUNK or LAMBDA -- the VM's code and constant offsets -- traced as a "
          "pointer: whatever lay at that address copied, and the offsets rewritten");
    check(heap.peek32(root(0) + 8) == heap.peek32(root(1) + 8) && heap.peek32(root(0) + 8) != env,
          "their shared environment, word 1, is evacuated once and rewritten in both", "n/a");
    bus->wr(R_CMD, V_COMPACT);
    bus->wait_verb();
    check(watermark() == SPACE_A + 12 + 12 + 12, "two closures and their environment survived, the canary not",
          "a closure's word 0 traced");
    // Allocating in A.

    // --- (g) a BLACKHOLE is a THUNK under evaluation: 12 bytes, its environment traced ---
    const std::uint32_t benv = lay(SPACE_A + 0x7000, TAG_INT, 0, { 0xB1, 0 });
    const std::uint32_t Bh = lay(SPACE_A + 0x7010, TAG_BLACKHOLE, 0, { 0x00050123, benv });
    st = mark({ Bh });
    check((st & ST_ERR) == 0, "MARK of a BLACKHOLE completes", "n/a");
    const std::uint32_t Bn = root(0);
    check(heap.peek(Bn) == TAG_BLACKHOLE && heap.peek32(Bn + 4) == 0x00050123 && heap.peek32(Bn + 8) != benv &&
          heap.peek32(heap.peek32(Bn + 8) + 4) == 0xB1,
          "a BLACKHOLE keeps its third word, its environment, rewritten to the copy",
          "a BLACKHOLE copied as 8 bytes: the update that ends its force writes 12, over "
          "the next object's header");
    bus->wr(R_CMD, V_COMPACT);
    bus->wait_verb();
    check(watermark() == SPACE_B + 12 + 12, "a BLACKHOLE costs 12 bytes, and its environment 12",
          "a BLACKHOLE sized by the 8-byte default");
    // Allocating in B.

    // --- (h) the count is 16 bits: the header's top byte is the CPU's marks ---
    // A `with` environment is a LIST of two (the outer environment and the
    // set) with bit 6 of the top byte set (HEAP.md). Counted from 23 bits it
    // was a list of 4,194,306 elements.
    const std::uint32_t w0 = lay(SPACE_B + 0x7000, TAG_INT, 0, { 0x0E, 0 });
    const std::uint32_t w1 = lay(SPACE_B + 0x7010, TAG_INT, 0, { 0x5E, 0 });
    const std::uint32_t We = lay(SPACE_B + 0x7020, TAG_LIST, 2u | (0x40u << 16), { w0, w1 });
    st = mark({ We });
    check((st & ST_ERR) == 0, "MARK of a marked `with` environment completes", "n/a");
    const std::uint32_t Wn = root(0);
    check(heap.peek32(Wn) == (TAG_LIST | ((2u | (0x40u << 16)) << 8)) &&
          heap.peek32(heap.peek32(Wn + 4) + 4) == 0x0E && heap.peek32(heap.peek32(Wn + 8) + 4) == 0x5E,
          "a LIST with a mark in its top byte is copied with its mark and both elements traced", "n/a");
    bus->wr(R_CMD, V_COMPACT);
    bus->wait_verb();
    check(watermark() == SPACE_A + 12 + 12 + 12, "a marked 2-element LIST costs 12 bytes, and its elements 24",
          "the CPU's marks counted as part of the length: a `with` environment sized as "
          "four million elements, MARK copying garbage over to-space");
}

// ---------------------------------------------------------------------------
// L3 RESERVE: a headerless block for the promotion stream.
void l3_reserve() {
    std::printf("L3 RESERVE\n");
    // NB: CTRL reset deliberately does not touch the allocator -- a card reset must
    // not wipe the heap -- so the allocator is wherever the collector tests left it.
    // Every assertion here is relative to the first reservation for that reason.
    bus->wr(R_CTRL, CTRL_RESET);

    // Two reservations must be contiguous and exactly sized: the promoter relies on
    // card = base + offset, so any padding the card inserted would shift every
    // translated pointer.
    bus->set_len(72);
    bus->wr(R_CMD, V_RESERVE);
    std::uint8_t st = bus->wait_verb();
    const std::uint32_t b1 = bus->get_areg(1);
    bus->set_len(24);
    bus->wr(R_CMD, V_RESERVE);
    st |= bus->wait_verb();
    const std::uint32_t b2 = bus->get_areg(1);
    check((st & ST_ERR) == 0 && (b1 % 4) == 0 && b2 == b1 + 72,
          "RESERVE hands out exactly LEN contiguous bytes with no header",
          "the block being sized like an ALLOC (header added), which would shift "
          "every pointer the promoter translates by a constant");

    // It must still round like ALLOC so the next object stays aligned.
    bus->set_len(5);
    bus->wr(R_CMD, V_RESERVE);
    bus->wait_verb();
    const std::uint32_t b3 = bus->get_areg(1);
    bus->set_len(8);
    bus->wr(R_CMD, V_RESERVE);
    bus->wait_verb();
    check(bus->get_areg(1) == b3 + 8,
          "RESERVE rounds to 4 with an 8-byte minimum, like ALLOC",
          "a misaligned block breaking single-access word reads of what follows");

    bus->set_len(0x00FFFFFF);
    bus->wr(R_CMD, V_RESERVE);
    st = bus->wait_verb();
    check((st & ST_ERR) && (st & 0x0F) == E_OOM, "RESERVE reports OOM past the semispace",
          "the bump pointer running into to-space");
    bus->wr(R_CTRL, CTRL_RESET);
}

} // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    const char *vec_path = (argc > 1) ? argv[1] : "tests/vectors/l1.txt";

    card = new Va2nix_card;
    CardBus<Va2nix_card> b(card, &heap);
    bus = &b;
    bus->reset();

    std::printf("=== silent-corruption regressions ===\n\n");

    r1_stale_hash_completion();
    r2_port_index_desync();
    r3_pslot_byte_order();
    r4_dropped_bytes_at_block_boundary(vec_path);
    r5_init_abandons_partial_message();
    r6_errors_surface();
    l3_lookup();
    l3_memcmp();
    l3_alu64();
    l3_gc();
    l3_reserve();

    card->final();
    delete card;

    std::printf("\n%d checks, %d failures (%llu bus accesses, %llu cycles)\n",
                checks, failures,
                static_cast<unsigned long long>(bus->accesses()),
                static_cast<unsigned long long>(bus->cycles()));

    if (failures == 0)
        std::printf("REGRESS PASS: every known silent-corruption mode is guarded\n");

    return failures == 0 ? 0 : 1;
}
