// Backing store for the card's heap memory port, shared by the test harnesses.
//
// The RTL exposes a memory port rather than containing an array (docs/HEAP.md): an
// ECP5-25F's ~128 KB of block RAM cannot hold a Nix heap, so on real hardware this is
// off-chip SDRAM. Here it is a plain byte array.
//
// Contract, matching the port's comment in rtl/a2nix_card.v:
//   - The port is 32 bits wide and BYTE-addressed. mem_rdata must be COMBINATIONAL
//     on mem_addr and return the ALIGNED WORD CONTAINING that byte, so the low two
//     address bits select a lane rather than shifting the word.
//   - A write happens on the clock edge where mem_we is high, under mem_wstrb.
//
// So a harness tick is: settle read data, clock the card, then honour any write.

#ifndef A2NIX_HEAPMEM_H
#define A2NIX_HEAPMEM_H

#include <cstdint>
#include <cstdio>
#include <vector>

template <typename CardT>
class HeapMem {
public:
    // 1 MB, matching HEAP_TOP in rtl/a2nix_card.v. Keep the two in step.
    explicit HeapMem(std::size_t bytes = 1u << 20) : m_ram(bytes, 0) {}

    // Present the aligned word containing the addressed byte.
    void settle(CardT *card) {
        const std::uint32_t w = card->mem_addr & ~3u;
        std::uint32_t v = 0;
        if (w + 3 < m_ram.size())
            v = static_cast<std::uint32_t>(m_ram[w])
              | (static_cast<std::uint32_t>(m_ram[w + 1]) << 8)
              | (static_cast<std::uint32_t>(m_ram[w + 2]) << 16)
              | (static_cast<std::uint32_t>(m_ram[w + 3]) << 24);
        card->mem_rdata = v;
    }

    // Honour a write issued on the edge just clocked, lane by lane.
    void commit(CardT *card) {
        if (!card->mem_we) return;
        const std::uint32_t w = card->mem_addr & ~3u;
        const std::uint32_t strb = card->mem_wstrb;
        const std::uint32_t data = card->mem_wdata;

        if (w + 3 >= m_ram.size()) {
            // Out-of-range writes are a bug in the card or the test, not something to
            // absorb quietly -- silently dropping them would show up much later as a
            // mysteriously wrong hash.
            std::fprintf(stderr,
                         "heapmem: write outside memory at 0x%08x (size 0x%zx)\n",
                         w, m_ram.size());
            ++m_bad_writes;
            return;
        }
        for (int lane = 0; lane < 4; ++lane)
            if (strb & (1u << lane))
                m_ram[w + static_cast<std::uint32_t>(lane)] =
                    static_cast<std::uint8_t>(data >> (8 * lane));
    }

    std::uint8_t  peek(std::uint32_t a) const { return a < m_ram.size() ? m_ram[a] : 0; }
    void          poke(std::uint32_t a, std::uint8_t v) { if (a < m_ram.size()) m_ram[a] = v; }
    std::size_t   size() const { return m_ram.size(); }
    unsigned      bad_writes() const { return m_bad_writes; }

    // Read a 32-bit little-endian pointer, as the heap stores them.
    std::uint32_t peek32(std::uint32_t a) const {
        return static_cast<std::uint32_t>(peek(a))
             | (static_cast<std::uint32_t>(peek(a + 1)) << 8)
             | (static_cast<std::uint32_t>(peek(a + 2)) << 16)
             | (static_cast<std::uint32_t>(peek(a + 3)) << 24);
    }

    void poke32(std::uint32_t a, std::uint32_t v) {
        poke(a + 0, static_cast<std::uint8_t>(v));
        poke(a + 1, static_cast<std::uint8_t>(v >> 8));
        poke(a + 2, static_cast<std::uint8_t>(v >> 16));
        poke(a + 3, static_cast<std::uint8_t>(v >> 24));
    }

private:
    std::vector<std::uint8_t> m_ram;
    unsigned m_bad_writes = 0;
};

#endif // A2NIX_HEAPMEM_H
