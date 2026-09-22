// L1 testbench: drives the real RTL against NIST SHA-256 vectors and against store
// paths produced by the actual `nix` binary on the build host.
//
// Passing the SP cases is milestone M1 (docs/STATUS.md): the RTL emits a store path
// byte-identical to upstream Nix. That parity is what separates an implementation
// from a tribute.
//
// This harness is deliberately plain C++ over the Verilated model rather than a
// cocotb suite, because the same shape of code is what the MAME a2bus device will use
// to drive the card at L2 -- so none of it is throwaway.

#include "Vtb_top.h"
#include "verilated.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

Vtb_top *dut = nullptr;
uint64_t cycles = 0;

void tick() {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
    ++cycles;
}

// Extract byte `i` (0 = most significant) from a 256-bit Verilated wide signal.
uint8_t wide_byte(const uint32_t *w, int i) {
    const int lsb   = 248 - 8 * i;
    const int word  = lsb / 32;
    const int shift = lsb % 32;
    return static_cast<uint8_t>((w[word] >> shift) & 0xff);
}

std::string digest_hex() {
    static const char *H = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (int i = 0; i < 32; ++i) {
        const uint8_t b = wide_byte(dut->digest.data(), i);
        s.push_back(H[b >> 4]);
        s.push_back(H[b & 0xf]);
    }
    return s;
}

std::string path_str() {
    std::string s;
    s.reserve(32);
    for (int i = 0; i < 32; ++i)
        s.push_back(static_cast<char>(wide_byte(dut->path.data(), i)));
    return s;
}

void reset() {
    dut->rst_n    = 0;
    dut->init     = 0;
    dut->in_valid = 0;
    dut->in_byte  = 0;
    dut->finish   = 0;
    for (int i = 0; i < 8; ++i) tick();
    dut->rst_n = 1;
    tick();
}

// Push a whole message through the streaming interface and wait for the digest.
bool run_message(const std::vector<uint8_t> &data) {
    dut->init = 1;
    tick();
    dut->init = 0;
    tick();

    size_t i = 0;
    while (i < data.size()) {
        if (dut->in_ready) {
            dut->in_valid = 1;
            dut->in_byte  = data[i];
            ++i;
        } else {
            dut->in_valid = 0;
        }
        tick();
    }
    dut->in_valid = 0;
    tick();

    // Hold `finish` asserted while the padding path drains; the FSM consumes it once
    // it is back in S_ACCEPT.
    dut->finish = 1;
    const uint64_t budget = 200000;
    for (uint64_t c = 0; c < budget && !dut->done; ++c) tick();
    const bool ok = dut->done;
    dut->finish = 0;
    tick();
    return ok;
}

std::vector<uint8_t> unhex(const std::string &s) {
    std::vector<uint8_t> out;
    if (s == "-") return out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

} // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    const char *vec_path = (argc > 1) ? argv[1] : "tests/vectors/l1.txt";
    std::ifstream in(vec_path);
    if (!in) {
        std::fprintf(stderr, "cannot open vectors: %s\n", vec_path);
        std::fprintf(stderr, "run: python3 scripts/gen-vectors.py %s\n", vec_path);
        return 2;
    }

    dut = new Vtb_top;
    reset();

    int n_sha = 0, n_sp = 0, fails = 0;
    std::string line;

    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;

        if (kind == "SHA") {
            std::string in_hex, want;
            ls >> in_hex >> want;
            const auto data = unhex(in_hex);

            if (!run_message(data)) {
                std::fprintf(stderr, "TIMEOUT sha len=%zu\n", data.size());
                ++fails;
                continue;
            }
            const std::string got = digest_hex();
            if (got != want) {
                std::fprintf(stderr,
                             "FAIL sha len=%zu\n  want %s\n  got  %s\n",
                             data.size(), want.c_str(), got.c_str());
                ++fails;
            } else {
                ++n_sha;
            }

        } else if (kind == "SP") {
            std::string fp_hex, want, full;
            ls >> fp_hex >> want >> full;
            const auto fp = unhex(fp_hex);

            if (!run_message(fp)) {
                std::fprintf(stderr, "TIMEOUT storepath %s\n", full.c_str());
                ++fails;
                continue;
            }
            const std::string got = path_str();
            if (got != want) {
                std::fprintf(stderr,
                             "FAIL storepath\n  fingerprint %.*s\n"
                             "  want %s\n  got  %s\n",
                             static_cast<int>(fp.size()),
                             reinterpret_cast<const char *>(fp.data()),
                             want.c_str(), got.c_str());
                ++fails;
            } else {
                ++n_sp;
                std::printf("  ok  %s\n", full.c_str());
            }
        }
    }

    dut->final();
    delete dut;

    std::printf("\n%d SHA-256 vectors, %d store-path vectors, %d failures (%llu cycles)\n",
                n_sha, n_sp, fails, static_cast<unsigned long long>(cycles));

    if (fails == 0 && n_sp > 0)
        std::printf("M1 PASS: RTL store paths are byte-identical to upstream nix\n");

    return fails == 0 ? 0 : 1;
}
