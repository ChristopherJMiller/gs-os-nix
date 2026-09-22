// Backing store for the card's heap memory port, shared by the test harnesses.
//
// The RTL exposes a memory port rather than containing an array (docs/HEAP.md): an
// ECP5-25F's ~128 KB of block RAM cannot hold a Nix heap, so on real hardware this is
// off-chip SDRAM. Here it is a plain byte array.
//
// Contract, matching the port's comment in rtl/a2nix_card.v:
//   - mem_rdata must be COMBINATIONAL on mem_addr, so the cycle after the card
//     presents an address, the data is there.
//   - a write happens on the clock edge where mem_we is high.
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

    // Present the byte the card is currently addressing.
    void settle(CardT *card) {
        const std::uint32_t a = card->mem_addr;
        card->mem_rdata = (a < m_ram.size()) ? m_ram[a] : 0x00;
    }

    // Honour a write issued on the edge just clocked.
    void commit(CardT *card) {
        if (!card->mem_we) return;
        const std::uint32_t a = card->mem_addr;
        if (a < m_ram.size()) {
            m_ram[a] = card->mem_wdata;
        } else {
            // Out-of-range writes are a bug in the card or the test, not something to
            // absorb quietly -- silently dropping them would show up much later as a
            // mysteriously wrong hash.
            std::fprintf(stderr,
                         "heapmem: write outside memory at 0x%08x (size 0x%zx)\n",
                         a, m_ram.size());
            ++m_bad_writes;
        }
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
