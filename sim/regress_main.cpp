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
