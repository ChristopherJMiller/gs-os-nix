// orcarun -- runs ORCA shell programs (the ORCA/C compiler, the ORCA linker) on
// Linux, without an Apple IIgs ROM or GS/OS.
//
// WHY THIS EXISTS (docs/TOOLCHAIN.md, DECISIONS.md D18)
// ----------------------------------------------------
// The on-target code moves from 65816 assembly to C, and the only gratis
// native-mode 65816 C compiler is ORCA/C -- a IIgs program. Running it under
// MAME needs Apple's ROM and GS/OS; running it here needs only what a shell
// program can see: GS/OS file calls, the ORCA shell's calls, and a small part of
// the Toolbox. Those are implemented on the host around the same 65816 core
// that a2min uses (sim/cpu65816.h). Golden Gate takes exactly this approach; this
// is a free re-implementation of the subset ORCA's own tools need.
//
// DISCIPLINE
// ----------
// Every call the harness does not implement ABORTS naming the call. A missing
// call must be an error, never a silent no-op: a compiler that "succeeds" over
// an unimplemented OS call produces a wrong-but-plausible object file, which is
// the failure mode this project is most careful about.
//
// THE WORLD IT PROVIDES
// ---------------------
//   * 16 MB of RAM. Bank $E0 holds Memory Manager handle records; the vectors
//     at $E1/0000 (Toolbox), $E1/00A8 (GS/OS inline) and $E1/00B0 (GS/OS stack)
//     are traps, exactly where real programs JSL to.
//   * A Memory Manager: first-fit blocks that never straddle a bank unless they
//     must, real 20-byte handle records that programs may dereference.
//   * GS/OS classes 0 and 1: files, prefixes, directories, the console.
//   * The ORCA shell: GET/SET_LINFO, FastFile, variables, STOP, VERSION...
//   * Toolbox: Tool Locator, Memory Manager, Misc Tools, Text Tools, Integer
//     Math -- the sets ORCA/C, its Pascal runtime and the linker use.
//   * The System Loader's job: OMF v1/v2 load files (ExpressLoad-indexed or
//     plain), relocation records including SUPER records, jump tables for
//     dynamic segments (all segments are loaded eagerly and the jump table is
//     patched to JML, so no run-time loading ever happens).
//
// The filesystem is the host's. GS/OS prefixes 0..31 map to host directories
// (--map N=DIR); a full pathname `:H:a:b:c` is the host path /a/b/c. Names are
// matched case-insensitively (ProDOS semantics) and a file's ProDOS type and
// auxtype ride on its name as cadius writes them: `cc#B5DB01`. Files the
// programs create get that suffix; the wrapper scripts strip it.
//
// USAGE
// -----
//   orcarun [options] <program-on-host> [command line...]
//     --map N=DIR        GS/OS prefix N (0-31) is host directory DIR
//     --orca ROOT        the extracted ORCA release: sets 13,15,16,17,*,@
//     --var NAME=VALUE   an ORCA shell variable (Libraries, KeepType, ...)
//     --sfile S --dfile D --parms P --istring I
//     --minus abc --plus xyz --lops N --merr N --keep --org N
//                        the LInfo the shell would pass for `compile`/`link`
//     --trace            print every OS/shell/Toolbox call
//     --stack N          DP/stack bytes when the program has no ~stack segment
//   orcarun --omf2bin <load-file> <out.bin> [--org 0x2000]
//     Converts an OMF load file into the flat binary a2min runs: segments are
//     placed contiguously from --org in bank 0, relocated, and prefixed with a
//     stub that enters native mode, JSLs the entry, and RTSes to a2min's halt.
//
// Exit status: 0 if the program returned with merrf <= merr, 1 otherwise, 3 on
// a harness abort.

#include "cpu65816.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t RAM_BYTES   = 0x1000000;   // all 16 MB
constexpr uint32_t HALT_ADDR   = 0x00FFF0;    // RTL from the program lands here
constexpr uint32_t TOOL_ENTRY  = 0xE10000;
constexpr uint32_t TOOL_ENTRY2 = 0xE10004;
constexpr uint32_t GSOS_INLINE = 0xE100A8;
constexpr uint32_t GSOS_STACK  = 0xE100B0;
constexpr uint32_t OS_KIND     = 0xE100BC;    // 1 = GS/OS is running
constexpr uint32_t HANDLE_AREA = 0xE00000;    // 20-byte handle records
constexpr uint32_t HANDLE_END  = 0xE10000;
constexpr uint32_t BANK0_LO    = 0x001000;    // bank-0 allocations
constexpr uint32_t BANK0_HI    = 0x00F000;
constexpr uint32_t GEN_LO      = 0x020000;    // general allocations
constexpr uint32_t GEN_HI      = 0xE00000;

constexpr uint16_t USER_ID     = 0x1002;      // the application's memory ID

bool trace = false;
uint32_t stack_default = 16384;

[[noreturn]] void fatal(const char *fmt, ...);

// --- RAM ---------------------------------------------------------------------------
std::vector<uint8_t> ram(RAM_BYTES);

uint8_t  r8(uint32_t a) { return ram[a & 0xFFFFFF]; }
uint16_t r16(uint32_t a) { return static_cast<uint16_t>(r8(a) | (r8(a + 1) << 8)); }
uint32_t r24(uint32_t a) { return r16(a) | (static_cast<uint32_t>(r8(a + 2)) << 16); }
uint32_t r32(uint32_t a) { return r16(a) | (static_cast<uint32_t>(r16(a + 2)) << 16); }
void w8(uint32_t a, uint8_t v) { ram[a & 0xFFFFFF] = v; }
void w16(uint32_t a, uint16_t v) { w8(a, v & 0xFF); w8(a + 1, v >> 8); }
void w24(uint32_t a, uint32_t v) { w16(a, v & 0xFFFF); w8(a + 2, (v >> 16) & 0xFF); }
void w32(uint32_t a, uint32_t v) { w16(a, v & 0xFFFF); w16(a + 2, v >> 16); }

// GS/OS strings: a length word then the bytes. Pascal strings: a length byte.
std::string gs_string(uint32_t a) {
    if (a == 0) return "";
    const uint16_t n = r16(a);
    std::string s; s.reserve(n);
    for (uint16_t i = 0; i < n; ++i) s += static_cast<char>(r8(a + 2 + i));
    return s;
}
std::string p_string(uint32_t a) {
    if (a == 0) return "";
    const uint8_t n = r8(a);
    std::string s; s.reserve(n);
    for (uint8_t i = 0; i < n; ++i) s += static_cast<char>(r8(a + 1 + i));
    return s;
}
std::string c_string(uint32_t a) {
    std::string s;
    for (uint32_t i = 0; i < 65536 && r8(a + i); ++i) s += static_cast<char>(r8(a + i));
    return s;
}

// A GS/OS result buffer: bufSize word (the whole buffer, its two header words
// included), then a GS string. Too small: buffTooSmall, with the length word set to
// what was needed, exactly as GS/OS does.
constexpr uint16_t buffTooSmall = 0x4F;
uint16_t put_result(uint32_t rb, const std::string &s) {
    if (rb == 0) return 0x53;               // paramRangeErr: no buffer at all
    const uint16_t cap = r16(rb);
    w16(rb + 2, static_cast<uint16_t>(s.size()));
    if (cap < 4 + s.size()) return buffTooSmall;
    for (size_t i = 0; i < s.size(); ++i) w8(rb + 4 + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
    return 0;
}
// A class-0 P-string result: a 256-byte buffer the caller supplied.
void put_pstring(uint32_t a, const std::string &s) {
    const size_t n = std::min<size_t>(s.size(), 255);
    w8(a, static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) w8(a + 1 + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
}

// --- Memory Manager ------------------------------------------------------------------
// Blocks are tracked host-side; handle records live in RAM so that `lda [h]`
// works. Attributes: $8000 locked, $4000 fixed, $0300 purge level, $0010 may not
// cross a bank, $0008 no special memory, $0004 page aligned, $0002 fixed address,
// $0001 fixed bank.
struct Block { uint32_t addr = 0, size = 0; uint16_t attr = 0, uid = 0; uint32_t handle = 0; };

class MemMgr {
public:
    std::map<uint32_t, Block> blocks;       // by address
    std::vector<uint32_t> free_handles;
    uint32_t next_handle = HANDLE_AREA;

    uint32_t new_handle_record() {
        uint32_t h;
        if (!free_handles.empty()) { h = free_handles.back(); free_handles.pop_back(); }
        else {
            if (next_handle + 20 > HANDLE_END) fatal("out of handle records (bank $E0 is full)");
            h = next_handle; next_handle += 20;
        }
        for (int i = 0; i < 20; ++i) w8(h + i, 0);
        return h;
    }

    // Find room for `size` bytes under the constraints; 0 if none.
    uint32_t find_room(uint32_t size, uint32_t lo, uint32_t hi, bool page, bool nocross) {
        uint32_t cand = lo;
        auto it = blocks.lower_bound(lo);
        for (;;) {
            uint32_t start = cand;
            if (page) start = (start + 0xFF) & ~0xFFu;
            if (nocross && size <= 0x10000 && ((start >> 16) != ((start + size - 1) >> 16)))
                start = (start + 0x10000) & ~0xFFFFu;
            const uint32_t limit = (it == blocks.end() || it->first >= hi) ? hi : it->first;
            if (start + size <= limit && start >= cand) return start;
            if (it == blocks.end() || it->first >= hi) return 0;
            cand = it->first + it->second.size;
            ++it;
        }
    }

    // NewHandle. Returns the handle, or 0 with err set.
    uint32_t alloc(uint32_t size, uint16_t uid, uint16_t attr, uint32_t loc, uint16_t &err) {
        err = 0;
        const bool page = attr & 0x0004, nocross = (attr & 0x0010) || size <= 0x10000;
        uint32_t lo = GEN_LO, hi = GEN_HI;
        if (attr & 0x0002) {                        // fixed address
            lo = loc; hi = loc + size;
        } else if (attr & 0x0001) {                 // fixed bank
            const uint32_t b = (loc >> 16) & 0xFF;
            if (b == 0) { lo = BANK0_LO; hi = BANK0_HI; }
            else { lo = b << 16; hi = (b << 16) + 0x10000; }
        }
        const uint32_t sz = size ? size : 1;
        const uint32_t addr = find_room(sz, lo, hi, page, nocross);
        if (addr == 0) { err = 0x0201; return 0; }  // memErr: out of memory
        Block b; b.addr = addr; b.size = size; b.attr = attr; b.uid = uid;
        b.handle = new_handle_record();
        w32(b.handle, addr); w16(b.handle + 4, attr); w16(b.handle + 6, uid); w32(b.handle + 8, size);
        blocks[addr] = b;
        return b.handle;
    }

    Block *find_by_handle(uint32_t h) {
        if (h < HANDLE_AREA || h >= next_handle || ((h - HANDLE_AREA) % 20) != 0) return nullptr;
        const uint32_t addr = r32(h);
        auto it = blocks.find(addr);
        if (it == blocks.end() || it->second.handle != h) return nullptr;
        return &it->second;
    }

    Block *find_by_addr(uint32_t a) {
        auto it = blocks.upper_bound(a);
        if (it == blocks.begin()) return nullptr;
        --it;
        if (a >= it->second.addr && a < it->second.addr + std::max<uint32_t>(1, it->second.size)) return &it->second;
        return nullptr;
    }

    void dispose(uint32_t h) {
        Block *b = find_by_handle(h);
        if (!b) fatal("DisposeHandle: $%06X is not a live handle", h);
        free_handles.push_back(h);
        for (int i = 0; i < 20; ++i) w8(h + i, 0);
        blocks.erase(b->addr);
    }

    void dispose_all(uint16_t uid) {
        std::vector<uint32_t> hs;
        for (auto &kv : blocks) if (kv.second.uid == uid) hs.push_back(kv.second.handle);
        for (uint32_t h : hs) dispose(h);
    }

    // SetHandleSize: grow in place when possible, else move (the record's pointer
    // changes, which is what double indirection is for).
    uint16_t resize(uint32_t h, uint32_t nsize) {
        Block *b = find_by_handle(h);
        if (!b) fatal("SetHandleSize: $%06X is not a live handle", h);
        if (nsize <= b->size) { b->size = nsize; w32(h + 8, nsize); return 0; }
        // room right after?
        auto it = blocks.find(b->addr);
        auto nx = std::next(it);
        const uint32_t limit = (nx == blocks.end()) ? RAM_BYTES : nx->first;
        const bool cross_ok = !((b->attr & 0x0010) || nsize <= 0x10000) ||
                              ((b->addr >> 16) == ((b->addr + nsize - 1) >> 16));
        if (b->addr + nsize <= limit && cross_ok && b->addr + nsize <= (b->addr < 0x10000 ? BANK0_HI : GEN_HI)) {
            b->size = nsize; w32(h + 8, nsize); return 0;
        }
        if (b->attr & 0xC000) {                     // locked or fixed: cannot move
            return 0x0204;                          // lockErr
        }
        const bool page = b->attr & 0x0004;
        const uint32_t lo = b->addr < 0x10000 ? BANK0_LO : GEN_LO, hi = b->addr < 0x10000 ? BANK0_HI : GEN_HI;
        Block old = *b;
        blocks.erase(old.addr);
        const uint32_t naddr = find_room(nsize, lo, hi, page, true);
        if (naddr == 0) { blocks[old.addr] = old; return 0x0201; }
        for (uint32_t i = 0; i < old.size; ++i) w8(naddr + i, r8(old.addr + i));
        old.addr = naddr; old.size = nsize;
        blocks[naddr] = old;
        w32(h, naddr); w32(h + 8, nsize);
        return 0;
    }

    uint32_t max_block() {
        uint32_t best = 0, cand = GEN_LO;
        for (auto it = blocks.lower_bound(GEN_LO); ; ++it) {
            const uint32_t limit = (it == blocks.end() || it->first >= GEN_HI) ? GEN_HI : it->first;
            if (limit > cand) best = std::max(best, limit - cand);
            if (it == blocks.end() || it->first >= GEN_HI) break;
            cand = it->first + it->second.size;
        }
        return best;
    }
} mm;

// --- the filesystem ------------------------------------------------------------------
// ProDOS file types we name. The suffix `#TTAAAA` (cadius's convention) carries
// type and auxtype on the host; files without one get a type from the extension.
constexpr uint16_t FT_TXT = 0x04, FT_BIN = 0x06, FT_DIR = 0x0F, FT_SRC = 0xB0, FT_OBJ = 0xB1,
                   FT_LIB = 0xB2, FT_S16 = 0xB3, FT_EXE = 0xB5;

std::string lower(std::string s) { for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

bool parse_suffix(const std::string &name, std::string &base, uint16_t &type, uint32_t &aux) {
    const size_t h = name.rfind('#');
    if (h == std::string::npos || name.size() - h != 7) return false;
    for (size_t i = h + 1; i < name.size(); ++i) if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return false;
    base = name.substr(0, h);
    type = static_cast<uint16_t>(std::strtoul(name.substr(h + 1, 2).c_str(), nullptr, 16));
    aux = static_cast<uint32_t>(std::strtoul(name.substr(h + 3, 4).c_str(), nullptr, 16));
    return true;
}

void type_from_extension(const std::string &name, uint16_t &type, uint32_t &aux) {
    const std::string l = lower(name);
    auto ends = [&](const char *e) { const size_t n = std::strlen(e); return l.size() >= n && l.compare(l.size() - n, n, e) == 0; };
    type = FT_TXT; aux = 0;
    if (ends(".c") || ends(".cc") || ends(".h") || ends(".cpp")) { type = FT_SRC; aux = 0x0008; }   // C
    else if (ends(".asm") || ends(".s") || ends(".mac")) { type = FT_SRC; aux = 0x0003; }         // ORCA/M
    else if (ends(".pas")) { type = FT_SRC; aux = 0x0005; }
    else if (ends(".root") || ends(".a") || ends(".o") || ends(".obj")) { type = FT_OBJ; }
    else if (ends(".lib")) { type = FT_LIB; }
    else if (ends(".bin")) { type = FT_BIN; }
}

std::string suffix_for(uint16_t type, uint32_t aux) {
    char b[16]; std::snprintf(b, sizeof b, "#%02X%04X", type & 0xFF, aux & 0xFFFF);
    return b;
}

// A host file as the IIgs sees it.
struct HostEntry {
    std::string host;       // full host path (with any suffix)
    std::string name;       // the ProDOS-visible name (no suffix)
    bool is_dir = false;
    uint16_t type = 0; uint32_t aux = 0;
    uint32_t size = 0;
};

bool stat_host(const std::string &path, HostEntry &e) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    e.host = path;
    const size_t slash = path.rfind('/');
    const std::string leaf = (slash == std::string::npos) ? path : path.substr(slash + 1);
    e.is_dir = S_ISDIR(st.st_mode);
    e.size = static_cast<uint32_t>(st.st_size);
    if (e.is_dir) { e.name = leaf; e.type = FT_DIR; e.aux = 0; return true; }
    if (!parse_suffix(leaf, e.name, e.type, e.aux)) { e.name = leaf; type_from_extension(leaf, e.type, e.aux); }
    return true;
}

// Case-insensitive, suffix-blind lookup of one component in a host directory.
bool lookup_component(const std::string &dir, const std::string &comp, std::string &host_leaf) {
    const std::string exact = dir + "/" + comp;
    struct stat st;
    if (::stat(exact.c_str(), &st) == 0) { host_leaf = comp; return true; }
    DIR *d = ::opendir(dir.c_str());
    if (!d) return false;
    const std::string want = lower(comp);
    bool found = false;
    while (dirent *de = ::readdir(d)) {
        std::string n = de->d_name, base; uint16_t t; uint32_t a;
        if (!parse_suffix(n, base, t, a)) base = n;
        if (lower(base) == want) { host_leaf = n; found = true; break; }
    }
    ::closedir(d);
    return found;
}

std::string prefixes[32];
std::string boot_volume, home;

// Split an ORCA/GS/OS pathname on `:` and `/`.
std::vector<std::string> split_path(const std::string &p) {
    std::vector<std::string> parts; std::string cur;
    for (char c : p) {
        if (c == ':' || c == '/') { if (!cur.empty()) parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

// Resolve a pathname to a host path. The last component need not exist (for
// Create); every earlier one must. Returns false with err set otherwise.
constexpr uint16_t fileNotFound = 0x46, pathNotFound = 0x44, badPathSyntax = 0x40, volNotFound = 0x45;

bool resolve(const std::string &path, std::string &host, uint16_t &err, bool must_exist = true) {
    err = 0;
    std::string base;
    std::string rest = path;
    if (path.empty()) { host = prefixes[0]; return !host.empty() || (err = pathNotFound, false); }
    const char c0 = path[0];
    if (c0 == ':' || c0 == '/') {
        // full pathname; the only volume is `H`, the host root
        std::vector<std::string> parts = split_path(path);
        if (parts.empty()) { err = badPathSyntax; return false; }
        if (lower(parts[0]) != "h") { err = volNotFound; return false; }
        base = "";
        std::string tail;
        for (size_t i = 1; i < parts.size(); ++i) tail += (tail.empty() ? "" : ":") + parts[i];
        rest = tail;
        if (rest.empty()) { host = "/"; return true; }
        base = "/";
    } else if (c0 == '*' && path.size() >= 2 && (path[1] == ':' || path[1] == '/')) {
        base = boot_volume; rest = path.substr(2);
    } else if (c0 == '@' && path.size() >= 2 && (path[1] == ':' || path[1] == '/')) {
        base = home; rest = path.substr(2);
    } else if (std::isdigit(static_cast<unsigned char>(c0))) {
        size_t i = 0; unsigned n = 0;
        while (i < path.size() && std::isdigit(static_cast<unsigned char>(path[i]))) n = n * 10 + (path[i++] - '0');
        if (i < path.size() && (path[i] == ':' || path[i] == '/') && n < 32) {
            base = prefixes[n]; rest = path.substr(i + 1);
            if (base.empty()) { err = pathNotFound; return false; }
        } else { base = prefixes[0]; rest = path; }
    } else if (c0 == '.' && path.find_first_of(":/") == std::string::npos) {
        host = path;                        // a device name: .CONSOLE, .NULL
        return true;
    } else {
        base = prefixes[0]; rest = path;
        if (base.empty()) { err = pathNotFound; return false; }
    }
    std::vector<std::string> parts = split_path(rest);
    std::string cur = base;
    if (cur.size() > 1 && cur.back() == '/') cur.pop_back();
    for (size_t i = 0; i < parts.size(); ++i) {
        std::string leaf;
        const bool last = (i + 1 == parts.size());
        if (lookup_component(cur.empty() ? "/" : cur, parts[i], leaf)) {
            cur = (cur == "/" ? "" : cur) + "/" + leaf;
        } else if (last && !must_exist) {
            cur = (cur == "/" ? "" : cur) + "/" + parts[i];
        } else {
            err = last ? fileNotFound : pathNotFound;
            return false;
        }
    }
    host = cur.empty() ? "/" : cur;
    return true;
}

// The GS/OS full pathname of a host path: `:H:a:b:c`.
std::string gs_full_path(const std::string &host) {
    std::string s = ":H";
    std::string base; uint16_t t; uint32_t a;
    const std::vector<std::string> parts = [&] {
        std::vector<std::string> v; std::string cur;
        for (char c : host) { if (c == '/') { if (!cur.empty()) v.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) v.push_back(cur);
        return v;
    }();
    for (const std::string &p : parts) {
        std::string shown = parse_suffix(p, base, t, a) ? base : p;
        s += ":" + shown;
    }
    return s;
}

// --- open files ------------------------------------------------------------------------
struct OpenFile {
    bool console = false;       // .CONSOLE: stdin/stdout
    bool errout = false;        // error output (stderr)
    bool is_dir = false;
    bool dirty = false;
    std::string host;
    std::vector<uint8_t> data;
    uint32_t mark = 0;
    // Newline mode
    bool newline = false; uint16_t nl_mask = 0; std::vector<uint8_t> nl_table;
    // directories
    std::vector<HostEntry> entries;
    uint16_t dir_pos = 0;
};
std::map<uint16_t, OpenFile> files;
uint16_t next_refnum = 1;

void console_out(const std::string &s, bool err) {
    std::FILE *f = err ? stderr : stdout;
    for (char c : s) std::fputc(c == '\r' ? '\n' : c, f);
    std::fflush(f);
}

bool read_host_file(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
bool write_host_file(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

std::vector<HostEntry> list_dir(const std::string &dir) {
    std::vector<HostEntry> v;
    DIR *d = ::opendir(dir.c_str());
    if (!d) return v;
    while (dirent *de = ::readdir(d)) {
        const std::string n = de->d_name;
        if (n == "." || n == ".." || n == "_FileInformation.txt") continue;
        if (n.find("_ResourceFork") != std::string::npos) continue;
        HostEntry e;
        if (stat_host(dir + "/" + n, e)) v.push_back(e);
    }
    ::closedir(d);
    std::sort(v.begin(), v.end(), [](const HostEntry &a, const HostEntry &b) { return lower(a.name) < lower(b.name); });
    return v;
}

// TimeRec: seconds, minutes, hours, year (since 1900), day (0-based), month (0-based), 0, weekday
// A fixed, deterministic timestamp (builds must not depend on the clock).
const uint8_t FIXED_TIME[8] = { 0, 0, 12, 86, 14, 8, 0, 2 };   // 1986-09-15 12:00 Monday

// --- OMF loader ---------------------------------------------------------------------------
struct OmfSegment {
    uint32_t file_off = 0, bytecnt = 0, resspc = 0, length = 0;
    uint32_t banksize = 0, org = 0, align = 0, entry = 0, body = 0;
    uint16_t kind = 0, segnum = 0; int version = 0; uint8_t lablen = 0, numlen = 0;
    std::string loadname, segname;
    bool express = false;
    uint32_t base = 0;              // where it was loaded
};

uint32_t le32(const std::vector<uint8_t> &d, size_t o) { return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24); }
uint16_t le16(const std::vector<uint8_t> &d, size_t o) { return static_cast<uint16_t>(d[o] | (d[o + 1] << 8)); }

std::vector<OmfSegment> parse_omf(const std::vector<uint8_t> &d, const std::string &what) {
    std::vector<OmfSegment> segs;
    size_t off = 0;
    while (off + 0x30 <= d.size()) {
        OmfSegment s;
        s.file_off = static_cast<uint32_t>(off);
        s.bytecnt = le32(d, off + 0x00);
        s.resspc  = le32(d, off + 0x04);
        s.length  = le32(d, off + 0x08);
        const uint8_t kind1 = d[off + 0x0C];
        s.lablen  = d[off + 0x0D];
        s.numlen  = d[off + 0x0E];
        s.version = d[off + 0x0F];
        s.banksize = le32(d, off + 0x10);
        const uint16_t kind2 = le16(d, off + 0x14);
        s.org   = le32(d, off + 0x18);
        s.align = le32(d, off + 0x1C);
        const uint8_t numsex = d[off + 0x20];
        s.segnum = le16(d, off + 0x22);
        s.entry  = le32(d, off + 0x24);
        const uint16_t nam_disp = le16(d, off + 0x28);
        const uint16_t bod_disp = le16(d, off + 0x2A);
        if (s.version == 2) s.kind = kind2;
        else if (s.version == 1) s.kind = static_cast<uint16_t>(((kind1 & 0xE0) << 8) | (kind1 & 0x1F));
        else fatal("%s: OMF version %d at offset %zu is not supported", what.c_str(), s.version, off);
        if (numsex != 0 || s.numlen != 4) fatal("%s: foreign OMF (numsex %d, numlen %d)", what.c_str(), numsex, s.numlen);
        if (s.bytecnt == 0) fatal("%s: zero-length segment at offset %zu", what.c_str(), off);
        size_t p = off + nam_disp;
        s.loadname.assign(reinterpret_cast<const char *>(&d[p]), 10); p += 10;
        size_t nlen = s.lablen;
        if (nlen == 0) { nlen = d[p]; ++p; }
        s.segname.assign(reinterpret_cast<const char *>(&d[p]), nlen);
        s.body = static_cast<uint32_t>(off + bod_disp);
        {
            std::string n = lower(s.segname);
            while (!n.empty() && n.back() == ' ') n.pop_back();
            s.express = (n == "~expressload" || n == "expressload") && (s.kind & 0x1F) == 0x01;
        }
        segs.push_back(s);
        // v1 non-library files count blocks of 512; v2 counts bytes.
        const size_t adv = (s.version == 1) ? s.bytecnt * 512 : s.bytecnt;
        off += adv;
    }
    return segs;
}

struct Reloc { uint32_t seg_index; uint8_t width; int8_t shift; uint32_t off; uint32_t rel; int target_segnum; /* -1 = self */ };

struct Loaded {
    std::vector<OmfSegment> segs;
    uint32_t entry = 0;
    uint32_t dp_addr = 0, dp_size = 0;
};

// Place every non-ExpressLoad segment, fill it from its records, collect relocations,
// apply them, then turn jump-table entries into JMLs.
// `placer` chooses each segment's address (the Memory Manager for programs, a
// bump pointer for --omf2bin).
template <class Placer>
Loaded load_omf(const std::vector<uint8_t> &d, const std::string &what, Placer place) {
    Loaded L;
    L.segs = parse_omf(d, what);
    std::map<int, size_t> by_num;
    for (size_t i = 0; i < L.segs.size(); ++i) {
        OmfSegment &s = L.segs[i];
        if (s.express) continue;
        if (by_num.count(s.segnum)) fatal("%s: duplicate segment number %d", what.c_str(), s.segnum);
        by_num[s.segnum] = i;
        const uint32_t k = s.kind & 0x1F;
        const bool dpstack = (k == 0x12);
        uint32_t alloc = std::max<uint32_t>(s.length, 1);
        if (s.org != 0) fatal("%s: segment %d '%s' has ORG $%06X; absolute segments are not supported",
                              what.c_str(), s.segnum, s.segname.c_str(), s.org);
        if (s.length > 0x10000 && s.banksize == 0x10000)
            fatal("%s: segment '%s' is %u bytes but must not cross a bank", what.c_str(), s.segname.c_str(), s.length);
        s.base = place(s, alloc, dpstack);
        if (dpstack) { L.dp_addr = s.base; L.dp_size = s.length; }
    }
    // records
    std::vector<Reloc> relocs;
    for (size_t i = 0; i < L.segs.size(); ++i) {
        const OmfSegment &s = L.segs[i];
        if (s.express) continue;
        const uint32_t k = s.kind & 0x1F;
        size_t p = s.body;
        uint32_t pc = 0;
        const size_t end = s.file_off + ((s.version == 1) ? s.bytecnt * 512 : s.bytecnt);
        for (;;) {
            if (p >= d.size() || p >= end) fatal("%s: segment '%s' runs off the end of the file", what.c_str(), s.segname.c_str());
            const uint8_t op = d[p];
            if (op == 0x00) break;                                  // END
            if (op >= 0x01 && op <= 0xDF) {                         // CONST
                if (pc + op > s.length) fatal("%s: '%s' CONST overflows the segment", what.c_str(), s.segname.c_str());
                for (uint32_t j = 0; j < op; ++j) w8(s.base + pc + j, d[p + 1 + j]);
                pc += op; p += 1 + op;
            } else if (op == 0xF2) {                                // LCONST
                const uint32_t n = le32(d, p + 1);
                if (pc + n > s.length) fatal("%s: '%s' LCONST overflows the segment (%u + %u > %u)", what.c_str(), s.segname.c_str(), pc, n, s.length);
                for (uint32_t j = 0; j < n; ++j) w8(s.base + pc + j, d[p + 5 + j]);
                pc += n; p += 5 + n;
            } else if (op == 0xF1) {                                // DS
                const uint32_t n = le32(d, p + 1);
                pc += n; p += 5;
            } else if (op == 0xE0) {                                // ALIGN: pad to the boundary
                const uint32_t n = le32(d, p + 1);
                if (n) pc = (pc + n - 1) / n * n;
                p += 5;
            } else if (op == 0xE2) {                                // RELOC
                relocs.push_back({static_cast<uint32_t>(i), d[p + 1], static_cast<int8_t>(d[p + 2]), le32(d, p + 3), le32(d, p + 7), -1});
                p += 11;
            } else if (op == 0xF5) {                                // cRELOC
                relocs.push_back({static_cast<uint32_t>(i), d[p + 1], static_cast<int8_t>(d[p + 2]), le16(d, p + 3), le16(d, p + 5), -1});
                p += 7;
            } else if (op == 0xE3) {                                // INTERSEG
                const uint16_t file = le16(d, p + 7), seg = le16(d, p + 9);
                if (file != 1) fatal("%s: INTERSEG to load file %d (run-time libraries are not supported)", what.c_str(), file);
                relocs.push_back({static_cast<uint32_t>(i), d[p + 1], static_cast<int8_t>(d[p + 2]), le32(d, p + 3), le32(d, p + 11), seg});
                p += 15;
            } else if (op == 0xF6) {                                // cINTERSEG
                relocs.push_back({static_cast<uint32_t>(i), d[p + 1], static_cast<int8_t>(d[p + 2]), le16(d, p + 3), le16(d, p + 6), d[p + 5]});
                p += 8;
            } else if (op == 0xF7) {                                // SUPER
                const uint32_t n = le32(d, p + 1);
                const uint8_t type = d[p + 5];
                size_t q = p + 6; long remaining = static_cast<long>(n) - 1;
                uint8_t width; int8_t shift = 0; int seg = -1; bool seg_in_data = false;
                if (type == 0) { width = 2; }
                else if (type == 1) { width = 3; }
                else if (type >= 2 && type <= 13) { width = 3; seg_in_data = true; if (type != 2) fatal("%s: SUPER INTERSEG to load file %d", what.c_str(), type - 1); }
                else if (type >= 14 && type <= 25) { width = 2; seg = type - 13; }
                else if (type >= 26 && type <= 37) { width = 2; shift = -16; seg = type - 25; }
                else fatal("%s: SUPER record type %d", what.c_str(), type);
                uint32_t page = 0;
                while (remaining > 0) {
                    const uint8_t cnt = d[q++]; --remaining;
                    if (cnt & 0x80) { page += (cnt & 0x7F); continue; }
                    for (int j = 0; j <= cnt; ++j) {
                        const uint8_t lo = d[q++]; --remaining;
                        const uint32_t at = page * 256 + lo;
                        const uint32_t rel = r16(s.base + at);
                        const int tseg = seg_in_data ? r8(s.base + at + 2) : seg;
                        relocs.push_back({static_cast<uint32_t>(i), width, shift, at, rel, tseg});
                    }
                    ++page;
                }
                if (remaining < 0) fatal("%s: SUPER record overran its length", what.c_str());
                p += 5 + n;
            } else {
                fatal("%s: record $%02X in segment '%s' -- an object-file record in a load file?", what.c_str(), op, s.segname.c_str());
            }
        }
        (void)k;
    }
    // apply
    for (const Reloc &r : relocs) {
        const OmfSegment &s = L.segs[r.seg_index];
        uint32_t base;
        if (r.target_segnum < 0) base = s.base;
        else {
            auto it = by_num.find(r.target_segnum);
            if (it == by_num.end()) fatal("%s: relocation to missing segment %d", what.c_str(), r.target_segnum);
            base = L.segs[it->second].base;
        }
        uint32_t v = base + r.rel;
        if (r.shift > 0) v <<= r.shift; else if (r.shift < 0) v >>= -r.shift;
        if (r.off + r.width > std::max<uint32_t>(s.length, 1)) fatal("%s: relocation outside segment '%s'", what.c_str(), s.segname.c_str());
        for (int j = 0; j < r.width; ++j) w8(s.base + r.off + j, static_cast<uint8_t>(v >> (8 * j)));
    }
    // jump tables: every entry becomes a JML to its (now loaded) target
    for (const OmfSegment &s : L.segs) {
        if (s.express || (s.kind & 0x1F) != 0x02) continue;
        uint32_t start = 8;
        if ((s.length - 8) % 14 != 0 || r8(s.base + 8 + 10) != 0x22) {
            if (s.length % 14 == 0 && r8(s.base + 10) == 0x22) start = 0;
            else fatal("%s: jump table segment '%s' (%u bytes) has an unexpected layout", what.c_str(), s.segname.c_str(), s.length);
        }
        for (uint32_t e = start; e + 14 <= s.length; e += 14) {
            const uint32_t at = s.base + e;
            if (r8(at + 10) != 0x22) fatal("%s: jump table entry at +%u is not a JSL", what.c_str(), e);
            const uint16_t file = r16(at + 2), seg = r16(at + 4);
            const uint32_t off = r32(at + 6);
            if (file != 1) fatal("%s: jump table entry to load file %d", what.c_str(), file);
            auto it = by_num.find(seg);
            if (it == by_num.end()) fatal("%s: jump table entry to missing segment %d", what.c_str(), seg);
            w16(at, USER_ID);
            w8(at + 10, 0x5C);
            w24(at + 11, L.segs[it->second].base + off);
        }
    }
    // entry: the first code segment
    bool have_entry = false;
    for (const OmfSegment &s : L.segs) {
        if (s.express || (s.kind & 0x1F) != 0x00) continue;
        L.entry = s.base + s.entry; have_entry = true; break;
    }
    if (!have_entry) fatal("%s: no code segment", what.c_str());
    return L;
}

// --- the shell's state -------------------------------------------------------------------
struct LInfo {
    std::string sfile, dfile, parms, istring;
    uint8_t merr = 0, merrf = 0, lops = 1, kflag = 0;
    uint32_t mflags = 0, pflags = 0, org = 0;
} linfo;
std::map<std::string, std::string> shell_vars;   // by lowercase name
uint16_t language = 8;                           // C

struct FastEntry { std::string host; uint32_t handle; uint32_t length; uint16_t access, type; uint32_t aux; uint16_t storage; uint16_t flags; };
std::vector<FastEntry> fastfiles;

bool finished = false;
uint64_t call_count = 0;

// --- the bus ---------------------------------------------------------------------------
struct OrcaBus;
Cpu65816<OrcaBus> *cpu_ptr = nullptr;

struct OrcaBus {
    uint8_t rd8(uint32_t a) { return ram[a]; }
    void    wr8(uint32_t a, uint8_t v) { ram[a] = v; }
    void    cycles(int) {}
    [[noreturn]] void die(const char *what, uint32_t where) {
        std::fprintf(stderr, "orcarun: %s at $%06X\n", what, where);
        if (cpu_ptr) cpu_ptr->print_trace(stderr);
        std::exit(3);
    }
    bool trap(uint32_t pcaddr);
};

OrcaBus bus;
Cpu65816<OrcaBus> cpu(bus);

[[noreturn]] void fatal(const char *fmt, ...) {
    std::va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "orcarun: ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    if (cpu.instructions) cpu.print_trace(stderr);
    std::exit(3);
}

void tlog(const char *fmt, ...) {
    if (!trace) return;
    std::va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "  [%llu] ", static_cast<unsigned long long>(call_count));
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// --- GS/OS -------------------------------------------------------------------------------
uint16_t gs_error_of_errno() {
    switch (errno) {
        case ENOENT: return fileNotFound;
        case EACCES: return 0x4E;   // invalidAccess
        case EEXIST: return 0x47;   // dupPathname
        default: return 0x27;       // drvrIOError
    }
}

uint16_t do_open(const std::string &path, uint16_t &refnum, HostEntry &info) {
    std::string host; uint16_t err;
    if (!resolve(path, host, err)) return err;
    OpenFile f;
    if (host.size() > 1 && host[0] == '.' && host.find('/') == std::string::npos) {
        const std::string dev = lower(host);
        if (dev == ".console") { f.console = true; }
        else if (dev == ".null") { f.console = true; f.errout = false; }
        else return 0x2F;                                       // drvrOffLine: no such device
        f.host = host;
        info = HostEntry{}; info.name = host; info.type = 0; info.aux = 0;
    } else {
        if (!stat_host(host, info)) return fileNotFound;
        f.host = host;
        if (info.is_dir) { f.is_dir = true; f.entries = list_dir(host); }
        else if (!read_host_file(host, f.data)) return gs_error_of_errno();
    }
    refnum = next_refnum++;
    files[refnum] = f;
    return 0;
}

uint16_t do_close(uint16_t refnum) {
    auto it = files.find(refnum);
    if (it == files.end()) return 0x43;                         // invalidRefNum
    OpenFile &f = it->second;
    if (f.dirty && !f.console && !f.is_dir) {
        if (!write_host_file(f.host, f.data)) fatal("cannot write %s", f.host.c_str());
    }
    files.erase(it);
    return 0;
}

OpenFile *file_of(uint16_t refnum, uint16_t &err) {
    auto it = files.find(refnum);
    if (it == files.end()) { err = 0x43; return nullptr; }
    err = 0; return &it->second;
}

uint32_t do_read(OpenFile &f, uint32_t buf, uint32_t count) {
    if (f.console) {
        // one line from stdin, CR-terminated the way the console driver delivers it
        std::string line; int ch; uint32_t n = 0;
        while (n < count && (ch = std::fgetc(stdin)) != EOF) {
            w8(buf + n, ch == '\n' ? 0x0D : static_cast<uint8_t>(ch)); ++n;
            if (ch == '\n') break;
        }
        return n;
    }
    uint32_t n = 0;
    while (n < count && f.mark < f.data.size()) {
        const uint8_t b = f.data[f.mark++];
        w8(buf + n, b); ++n;
        if (f.newline) {
            const uint8_t m = static_cast<uint8_t>(b & f.nl_mask);
            bool hit = false;
            for (uint8_t t : f.nl_table) if (t == m) { hit = true; break; }
            if (hit) break;
        }
    }
    return n;
}

uint32_t do_write(OpenFile &f, uint32_t buf, uint32_t count) {
    if (f.console) {
        std::string s; s.reserve(count);
        for (uint32_t i = 0; i < count; ++i) s += static_cast<char>(r8(buf + i));
        console_out(s, f.errout);
        return count;
    }
    if (f.mark + count > f.data.size()) f.data.resize(f.mark + count, 0);
    for (uint32_t i = 0; i < count; ++i) f.data[f.mark + i] = r8(buf + i);
    f.mark += count; f.dirty = true;
    return count;
}

uint16_t do_create(const std::string &path, uint16_t access, uint16_t type, uint32_t aux, uint16_t storage) {
    std::string host; uint16_t err;
    if (!resolve(path, host, err, false)) return err;
    struct stat st;
    // an existing file under any suffix is a duplicate
    if (::stat(host.c_str(), &st) == 0) return 0x47;            // dupPathname
    (void)access;
    if (storage == 0x0D) {                                      // directory
        if (::mkdir(host.c_str(), 0755) != 0) return gs_error_of_errno();
        return 0;
    }
    const std::string named = host + suffix_for(type, aux);
    std::ofstream f(named, std::ios::binary | std::ios::trunc);
    if (!f) return gs_error_of_errno();
    return 0;
}

uint16_t do_destroy(const std::string &path) {
    std::string host; uint16_t err;
    if (!resolve(path, host, err)) return err;
    for (auto &kv : files) if (kv.second.host == host) return 0x50;   // fileBusy
    if (::remove(host.c_str()) != 0) return gs_error_of_errno();
    return 0;
}

uint16_t do_get_file_info(const std::string &path, HostEntry &e) {
    std::string host; uint16_t err;
    if (!resolve(path, host, err)) return err;
    if (host[0] == '.' && host.find('/') == std::string::npos) { e = HostEntry{}; e.name = host; return 0; }
    if (!stat_host(host, e)) return fileNotFound;
    return 0;
}

// SetFileInfo changing the type renames the host file's suffix.
uint16_t do_set_file_info(const std::string &path, uint16_t type, uint32_t aux) {
    std::string host; uint16_t err;
    if (!resolve(path, host, err)) return err;
    HostEntry e;
    if (!stat_host(host, e)) return fileNotFound;
    if (e.is_dir) return 0;
    if (e.type == type && e.aux == aux) return 0;
    const size_t slash = host.rfind('/');
    const std::string dir = host.substr(0, slash);
    const std::string renamed = dir + "/" + e.name + suffix_for(type, aux);
    if (::rename(host.c_str(), renamed.c_str()) != 0) return gs_error_of_errno();
    for (auto &kv : files) if (kv.second.host == host) kv.second.host = renamed;
    for (auto &fe : fastfiles) if (fe.host == host) fe.host = renamed;
    return 0;
}

void put_timerec(uint32_t a) { for (int i = 0; i < 8; ++i) w8(a + i, FIXED_TIME[i]); }

// The class-1 file-info fields shared by Open, GetFileInfo, GetDirEntry callers:
// writes as many as pCount asks for, starting at field index `first`.
void put_info_fields(uint32_t pb, uint16_t pcount, int first, const HostEntry &e, uint32_t &off, int &field) {
    // access, fileType, auxType, storageType, createDateTime, modDateTime, optionList, eof, blocksUsed, resourceEOF, resourceBlocks
    auto have = [&](int f) { return f < pcount; };
    (void)first;
    if (have(field)) { w16(pb + off, e.is_dir ? 0xE3 : 0xC3); }  ++field; off += 2;
    if (have(field)) { w16(pb + off, e.type); }                  ++field; off += 2;
    if (have(field)) { w32(pb + off, e.aux); }                   ++field; off += 4;
    if (have(field)) { w16(pb + off, e.is_dir ? 0x0D : 0x01); }  ++field; off += 2;
    if (have(field)) { put_timerec(pb + off); }                  ++field; off += 8;
    if (have(field)) { put_timerec(pb + off); }                  ++field; off += 8;
    if (have(field)) { const uint32_t ol = r32(pb + off); if (ol) w16(ol + 2, 0); }  ++field; off += 4;  // optionList: empty
    if (have(field)) { w32(pb + off, e.size); }                  ++field; off += 4;
    if (have(field)) { w32(pb + off, (e.size + 511) / 512); }    ++field; off += 4;
    if (have(field)) { w32(pb + off, 0); }                       ++field; off += 4;
    if (have(field)) { w32(pb + off, 0); }                       ++field; off += 4;
}

uint16_t gsos_class1(uint16_t num, uint32_t pb) {
    const uint16_t pcount = r16(pb);
    auto P = [&](int i) { return pb + 2 + i; };     // byte offset helper: P(off) = address of the field at byte offset off past pCount
    switch (num) {
    case 0x2001: {  // Create: pathname access fileType auxType storageType eof resourceEOF
        const std::string path = gs_string(r32(P(0)));
        const uint16_t access = pcount > 1 ? r16(P(4)) : 0xC3;
        const uint16_t type = pcount > 2 ? r16(P(6)) : 0;
        const uint32_t aux = pcount > 3 ? r32(P(8)) : 0;
        const uint16_t storage = pcount > 4 ? r16(P(12)) : 1;
        const uint16_t err = do_create(path, access, type, aux, storage);
        tlog("CreateGS %s type $%02X aux $%04X -> %02X", path.c_str(), type, aux, err);
        return err;
    }
    case 0x2002: { const std::string path = gs_string(r32(P(0))); const uint16_t err = do_destroy(path); tlog("DestroyGS %s -> %02X", path.c_str(), err); return err; }
    case 0x2005: {  // SetFileInfo: pathname access fileType auxType ...
        const std::string path = gs_string(r32(P(0)));
        const uint16_t type = pcount > 2 ? r16(P(6)) : 0;
        const uint32_t aux = pcount > 3 ? r32(P(8)) : 0;
        const uint16_t err = (pcount > 2) ? do_set_file_info(path, type, aux) : 0;
        tlog("SetFileInfoGS %s type $%02X -> %02X", path.c_str(), type, err);
        return err;
    }
    case 0x2006: {  // GetFileInfo
        const std::string path = gs_string(r32(P(0)));
        HostEntry e; const uint16_t err = do_get_file_info(path, e);
        tlog("GetFileInfoGS %s -> %02X (type $%02X, %u bytes)", path.c_str(), err, e.type, e.size);
        if (err) return err;
        uint32_t off = 6; int field = 1;
        put_info_fields(pb, pcount, 1, e, off, field);
        return 0;
    }
    case 0x2009: {  // SetPrefix: prefixNum, prefix
        const uint16_t n = r16(P(0)); const std::string p = gs_string(r32(P(2)));
        std::string host; uint16_t err;
        if (n >= 32) return 0x53;
        if (p.empty()) { prefixes[n].clear(); tlog("SetPrefixGS %u <- (empty)", n); return 0; }
        if (!resolve(p, host, err)) { tlog("SetPrefixGS %u <- %s -> %02X", n, p.c_str(), err); return err; }
        prefixes[n] = host; tlog("SetPrefixGS %u <- %s (%s)", n, p.c_str(), host.c_str());
        return 0;
    }
    case 0x200A: {  // GetPrefix: prefixNum, buffer (ResultBuf)
        const uint16_t n = r16(P(0));
        if (n >= 32) return 0x53;
        const std::string full = prefixes[n].empty() ? "" : gs_full_path(prefixes[n]) + ":";
        const uint16_t err = put_result(r32(P(2)), full);
        tlog("GetPrefixGS %u -> %s (%02X)", n, full.c_str(), err);
        return err;
    }
    case 0x200E: {  // ExpandPath: inputPath, outputPath (ResultBuf), flags
        const std::string in = gs_string(r32(P(0)));
        std::string host; uint16_t err;
        std::string out;
        if (!resolve(in, host, err, false)) { tlog("ExpandPathGS %s -> %02X", in.c_str(), err); return err; }
        out = (host[0] == '.') ? host : gs_full_path(host);
        if (pcount > 2 && (r16(P(8)) & 0x8000)) out = [&] { std::string u = out; for (char &c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return u; }();
        err = put_result(r32(P(4)), out);
        tlog("ExpandPathGS %s -> %s (%02X)", in.c_str(), out.c_str(), err);
        return err;
    }
    case 0x2010: {  // Open: refNum(out) pathname requestAccess resourceNumber access fileType auxType storageType create mod option eof blocks reof rblocks
        const std::string path = gs_string(r32(P(2)));
        uint16_t refnum = 0; HostEntry info;
        const uint16_t err = do_open(path, refnum, info);
        tlog("OpenGS %s -> %02X refnum %u", path.c_str(), err, refnum);
        if (err) return err;
        w16(P(0), refnum);
        // fields: refNum(2) pathname(4) requestAccess(2) resourceNumber(2), then the
        // file-info fields starting with `access` as field 4 at byte offset 10
        uint32_t off = 10; int field = 4;
        put_info_fields(pb, pcount, 4, info, off, field);
        return 0;
    }
    case 0x2011: {  // Newline: refNum enableMask numChars newlineTable
        uint16_t err; OpenFile *f = file_of(r16(P(0)), err); if (!f) return err;
        f->nl_mask = r16(P(2)); const uint16_t n = r16(P(4)); const uint32_t t = r32(P(6));
        f->nl_table.clear(); for (uint16_t i = 0; i < n; ++i) f->nl_table.push_back(r8(t + i));
        f->newline = (f->nl_mask != 0 && n != 0);
        tlog("NewlineGS refnum %u mask $%02X %u chars", r16(P(0)), f->nl_mask, n);
        return 0;
    }
    case 0x2012: case 0x2013: {  // Read / Write: refNum dataBuffer requestCount transferCount cachePriority
        uint16_t err; OpenFile *f = file_of(r16(P(0)), err); if (!f) return err;
        const uint32_t buf = r32(P(2)), req = r32(P(6));
        const uint32_t n = (num == 0x2012) ? do_read(*f, buf, req) : do_write(*f, buf, req);
        if (pcount > 3) w32(P(10), n);
        if (num == 0x2012 && n == 0 && req != 0 && !f->console) { tlog("ReadGS refnum %u: EOF", r16(P(0))); return 0x4C; }  // eofEncountered
        if (!f->console) tlog("%s refnum %u %u bytes -> %u", num == 0x2012 ? "ReadGS" : "WriteGS", r16(P(0)), req, n);
        return 0;
    }
    case 0x2014: { const uint16_t rn = r16(P(0)); const uint16_t err = do_close(rn); tlog("CloseGS %u -> %02X", rn, err); return err; }
    case 0x2015: {  // Flush
        uint16_t err; OpenFile *f = file_of(r16(P(0)), err); if (!f) return err;
        if (f->dirty && !f->console && !write_host_file(f->host, f->data)) fatal("cannot write %s", f->host.c_str());
        return 0;
    }
    case 0x2016: {  // SetMark: refNum base displacement
        uint16_t err; OpenFile *f = file_of(r16(P(0)), err); if (!f) return err;
        const uint16_t base = r16(P(2)); const uint32_t disp = r32(P(4));
        uint32_t pos;
        switch (base) { case 0: pos = disp; break; case 1: pos = static_cast<uint32_t>(f->data.size()) - disp; break;
                        case 2: pos = f->mark + disp; break; case 3: pos = f->mark - disp; break; default: return 0x53; }
        if (pos > f->data.size()) return 0x4D;                   // outOfRange
        f->mark = pos; return 0;
    }
    case 0x2017: { uint16_t err; OpenFile *f = file_of(r16(P(0)), err); if (!f) return err; w32(P(2), f->mark); return 0; }   // GetMark
    case 0x2018: {  // SetEOF: refNum base displacement
        uint16_t err; OpenFile *f = file_of(r16(P(0)), err); if (!f) return err;
        const uint16_t base = r16(P(2)); const uint32_t disp = r32(P(4));
        uint32_t eof;
        switch (base) { case 0: eof = disp; break; case 1: eof = static_cast<uint32_t>(f->data.size()) - disp; break;
                        case 2: eof = f->mark + disp; break; case 3: eof = f->mark - disp; break; default: return 0x53; }
        f->data.resize(eof, 0); if (f->mark > eof) f->mark = eof; f->dirty = true;
        tlog("SetEOFGS refnum %u -> %u", r16(P(0)), eof);
        return 0;
    }
    case 0x2019: { uint16_t err; OpenFile *f = file_of(r16(P(0)), err); if (!f) return err; w32(P(2), static_cast<uint32_t>(f->data.size())); return 0; }   // GetEOF
    case 0x201A: case 0x201B: return 0;                           // Set/GetLevel
    case 0x201C: {  // GetDirEntry: refNum flags base displacement name entryNum fileType eof blockCount create mod access auxType fileSysID optionList reof rblocks
        uint16_t err; OpenFile *f = file_of(r16(P(0)), err); if (!f) return err;
        if (!f->is_dir) return 0x4A;                              // badFileFormat: not a directory
        const uint16_t base = r16(P(4)), disp = r16(P(6));
        uint16_t idx;
        switch (base) { case 0: idx = disp; break; case 1: idx = f->dir_pos + disp; break; case 2: idx = f->dir_pos - disp; break; default: return 0x53; }
        if (idx == 0) { if (pcount > 5) w16(P(12), static_cast<uint16_t>(f->entries.size())); f->dir_pos = 0; return 0; }   // entry 0: count
        if (idx > f->entries.size()) return 0x61;                 // endOfDir
        f->dir_pos = idx;
        const HostEntry &e = f->entries[idx - 1];
        if (pcount > 1) w16(P(2), 0);
        if (pcount > 4) { const uint16_t er = put_result(r32(P(8)), e.name); if (er) return er; }
        if (pcount > 5) w16(P(12), idx);
        if (pcount > 6) w16(P(14), e.type);
        if (pcount > 7) w32(P(16), e.size);
        if (pcount > 8) w32(P(20), (e.size + 511) / 512);
        if (pcount > 9) put_timerec(P(24));
        if (pcount > 10) put_timerec(P(32));
        if (pcount > 11) w16(P(40), e.is_dir ? 0xE3 : 0xC3);
        if (pcount > 12) w32(P(42), e.aux);
        if (pcount > 13) w16(P(46), 1);
        if (pcount > 14) { const uint32_t ol = r32(P(48)); if (ol) w16(ol + 2, 0); }
        if (pcount > 15) w32(P(52), 0);
        if (pcount > 16) w32(P(56), 0);
        tlog("GetDirEntryGS refnum %u #%u -> %s type $%02X", r16(P(0)), idx, e.name.c_str(), e.type);
        return 0;
    }
    case 0x2020: {  // GetDevNumber: devName -> devNum
        const std::string dev = lower(gs_string(r32(P(0))));
        if (dev == ".console") { w16(P(4), 1); return 0; }
        return 0x11;                                              // invalidDevNum
    }
    case 0x2027: {  // GetName: the current program's name
        return put_result(r32(P(0)), shell_vars.count("~program") ? shell_vars["~program"] : "PROGRAM");
    }
    case 0x2029: {  // Quit
        tlog("QuitGS");
        finished = true; return 0;
    }
    case 0x202A: w16(P(0), 0x0601); return 0;                   // GetVersion: 6.0.1
    case 0x2037: {  // GetStdRefNum: prefixNum (10 stdin 11 stdout 12 errout) -> refNum
        const uint16_t which = r16(P(0));
        uint16_t rn = 0; HostEntry info;
        const uint16_t err = do_open(".CONSOLE", rn, info);
        if (err) return err;
        if (which == 12) files[rn].errout = true;
        w16(P(2), rn);
        tlog("GetStdRefNumGS %u -> refnum %u", which, rn);
        return 0;
    }
    case 0x2039: {  // GetRefInfo: refNum access pathname(ResultBuf) resourceNumber level
        uint16_t err; OpenFile *f = file_of(r16(P(0)), err); if (!f) return err;
        if (pcount > 1) w16(P(2), 3);
        if (pcount > 2) { const uint16_t er = put_result(r32(P(4)), f->console ? std::string(".CONSOLE") : gs_full_path(f->host)); if (er) return er; }
        if (pcount > 3) w16(P(8), 0);
        if (pcount > 4) w16(P(10), 0);
        tlog("GetRefInfoGS %u", r16(P(0)));
        return 0;
    }
    case 0x200F: w16(P(0), 0); return 0;                          // GetSysPrefs
    case 0x201D: case 0x201E: return 0;                           // Begin/EndSession
    case 0x200D: return 0;                                        // Null
    default: break;
    }
    fatal("GS/OS class-1 call $%04X is not implemented (pCount %u)", num, pcount);
}

// ProDOS 16 (class 0): P-string names, no pCount.
uint16_t gsos_class0(uint16_t num, uint32_t pb) {
    switch (num) {
    case 0x0001: {  // CREATE: pathname access file_type aux_type storage_type create_date create_time
        const std::string path = p_string(r32(pb));
        const uint16_t err = do_create(path, r16(pb + 4), r16(pb + 6), r32(pb + 8), r16(pb + 12));
        tlog("CREATE %s type $%02X -> %02X", path.c_str(), r16(pb + 6), err);
        return err;
    }
    case 0x0002: { const std::string path = p_string(r32(pb)); const uint16_t err = do_destroy(path); tlog("DESTROY %s -> %02X", path.c_str(), err); return err; }
    case 0x0005: { const std::string path = p_string(r32(pb)); const uint16_t err = do_set_file_info(path, r16(pb + 6), r32(pb + 8)); tlog("SET_FILE_INFO %s -> %02X", path.c_str(), err); return err; }
    case 0x0006: {  // GET_FILE_INFO: pathname access file_type aux_type storage_type create_date create_time mod_date mod_time blocks_used
        const std::string path = p_string(r32(pb));
        HostEntry e; const uint16_t err = do_get_file_info(path, e);
        tlog("GET_FILE_INFO %s -> %02X (type $%02X)", path.c_str(), err, e.type);
        if (err) return err;
        w16(pb + 4, e.is_dir ? 0xE3 : 0xC3); w16(pb + 6, e.type); w32(pb + 8, e.aux); w16(pb + 12, e.is_dir ? 0x0D : 1);
        w16(pb + 14, 0x0D2F); w16(pb + 16, 0x0C00); w16(pb + 18, 0x0D2F); w16(pb + 20, 0x0C00);
        w32(pb + 22, (e.size + 511) / 512);
        return 0;
    }
    case 0x0009: {  // SET_PREFIX: prefix_num, prefix (P-string)
        const uint16_t n = r16(pb); const std::string p = p_string(r32(pb + 2));
        std::string host; uint16_t err;
        if (n >= 32) return 0x53;
        if (p.empty()) { prefixes[n].clear(); return 0; }
        if (!resolve(p, host, err)) return err;
        prefixes[n] = host; tlog("SET_PREFIX %u <- %s", n, p.c_str());
        return 0;
    }
    case 0x000A: {  // GET_PREFIX: prefix_num, prefix (buffer for a P-string)
        const uint16_t n = r16(pb);
        if (n >= 32) return 0x53;
        const std::string full = prefixes[n].empty() ? "" : gs_full_path(prefixes[n]) + ":";
        put_pstring(r32(pb + 2), full);
        tlog("GET_PREFIX %u -> %s", n, full.c_str());
        return 0;
    }
    case 0x0010: {  // OPEN: ref_num(out) pathname io_buffer(out)
        const std::string path = p_string(r32(pb + 2));
        uint16_t refnum = 0; HostEntry info;
        const uint16_t err = do_open(path, refnum, info);
        tlog("OPEN %s -> %02X refnum %u", path.c_str(), err, refnum);
        if (err) return err;
        w16(pb, refnum); w32(pb + 6, 0);
        return 0;
    }
    case 0x0011: {  // NEWLINE: ref_num enable_mask newline_char
        uint16_t err; OpenFile *f = file_of(r16(pb), err); if (!f) return err;
        f->nl_mask = r16(pb + 2); f->nl_table.assign(1, static_cast<uint8_t>(r16(pb + 4)));
        f->newline = f->nl_mask != 0;
        return 0;
    }
    case 0x0012: case 0x0013: {  // READ / WRITE: ref_num data_buffer request_count transfer_count
        uint16_t err; OpenFile *f = file_of(r16(pb), err); if (!f) return err;
        const uint32_t buf = r32(pb + 2), req = r32(pb + 6);
        const uint32_t n = (num == 0x0012) ? do_read(*f, buf, req) : do_write(*f, buf, req);
        w32(pb + 10, n);
        if (num == 0x0012 && n == 0 && req != 0 && !f->console) return 0x4C;
        if (!f->console) tlog("%s refnum %u %u bytes -> %u", num == 0x0012 ? "READ" : "WRITE", r16(pb), req, n);
        return 0;
    }
    case 0x0014: { const uint16_t rn = r16(pb); const uint16_t err = do_close(rn); tlog("CLOSE %u -> %02X", rn, err); return err; }
    case 0x0015: { uint16_t err; OpenFile *f = file_of(r16(pb), err); if (!f) return err; if (f->dirty && !f->console) write_host_file(f->host, f->data); return 0; }
    case 0x0016: {  // SET_MARK: ref_num position
        uint16_t err; OpenFile *f = file_of(r16(pb), err); if (!f) return err;
        const uint32_t pos = r32(pb + 2);
        if (pos > f->data.size()) return 0x4D;
        f->mark = pos; return 0;
    }
    case 0x0017: { uint16_t err; OpenFile *f = file_of(r16(pb), err); if (!f) return err; w32(pb + 2, f->mark); return 0; }
    case 0x0018: { uint16_t err; OpenFile *f = file_of(r16(pb), err); if (!f) return err; const uint32_t eof = r32(pb + 2); f->data.resize(eof, 0); if (f->mark > eof) f->mark = eof; f->dirty = true; return 0; }
    case 0x0019: { uint16_t err; OpenFile *f = file_of(r16(pb), err); if (!f) return err; w32(pb + 2, static_cast<uint32_t>(f->data.size())); return 0; }
    case 0x001A: case 0x001B: return 0;
    case 0x0029: tlog("QUIT"); finished = true; return 0;
    case 0x002A: w16(pb, 0x0601); return 0;
    default: break;
    }
    fatal("ProDOS 16 (class 0) call $%04X is not implemented", num);
}

// --- the ORCA shell ------------------------------------------------------------------------
uint32_t alloc_block(uint32_t size, uint16_t attr, uint32_t loc = 0) {
    uint16_t err; const uint32_t h = mm.alloc(size, USER_ID, attr, loc, err);
    if (!h) fatal("out of memory allocating %u bytes", size);
    return h;
}

uint16_t fastfile(uint32_t pb, uint16_t pcount) {
    // class 1 layout: pCount action index flags fileHandle pathName access fileType auxType
    //                 storageType createDate modDate option fileLength blocksUsed
    const uint16_t action = r16(pb + 2);
    const uint16_t flags = pcount > 3 ? r16(pb + 6) : 0;
    auto put_entry = [&](const FastEntry &e) {
        if (pcount > 4) w32(pb + 8, e.handle);
        if (pcount > 6) w16(pb + 16, e.access);
        if (pcount > 7) w16(pb + 18, e.type);
        if (pcount > 8) w32(pb + 20, e.aux);
        if (pcount > 9) w16(pb + 24, e.storage);
        if (pcount > 10) put_timerec(pb + 26);
        if (pcount > 11) put_timerec(pb + 34);
        if (pcount > 12) w32(pb + 42, 0);
        if (pcount > 13) w32(pb + 46, e.length);
        if (pcount > 14) w32(pb + 50, (e.length + 511) / 512);
    };
    auto find = [&](const std::string &host) -> FastEntry * {
        for (auto &e : fastfiles) if (e.host == host) return &e;
        return nullptr;
    };
    std::string path, host; uint16_t err = 0;
    if (action != 1) {
        path = gs_string(r32(pb + 12));
        const bool creating = (action == 3 || action == 4);
        if (!resolve(path, host, err, !creating)) { tlog("FastFile action %u %s -> %02X", action, path.c_str(), err); return err; }
    }
    switch (action) {
    case 0: {   // Load
        if (FastEntry *e = find(host)) { put_entry(*e); tlog("FastFile Load %s (cached, %u bytes)", path.c_str(), e->length); return 0; }
        HostEntry info;
        if (!stat_host(host, info) || info.is_dir) { tlog("FastFile Load %s -> not found", path.c_str()); return fileNotFound; }
        std::vector<uint8_t> data;
        if (!read_host_file(host, data)) return gs_error_of_errno();
        const uint32_t h = alloc_block(static_cast<uint32_t>(data.size()), 0xC000);
        const uint32_t addr = r32(h);
        for (size_t i = 0; i < data.size(); ++i) w8(addr + static_cast<uint32_t>(i), data[i]);
        FastEntry e{host, h, static_cast<uint32_t>(data.size()), 0xC3, info.type, info.aux, 1, flags};
        fastfiles.push_back(e);
        put_entry(e);
        tlog("FastFile Load %s -> handle $%06X, %u bytes, type $%02X", path.c_str(), h, e.length, e.type);
        return 0;
    }
    case 1: {   // IndexedLoad
        const uint16_t idx = r16(pb + 4);
        if (idx >= fastfiles.size()) return fileNotFound;
        const FastEntry &e = fastfiles[idx];
        put_entry(e);
        if (pcount > 5) { const uint16_t er = put_result(r32(pb + 12), gs_full_path(e.host)); if (er) return er; }
        return 0;
    }
    case 2: {   // LoadMemory: only if already in the list
        if (FastEntry *e = find(host)) { put_entry(*e); return 0; }
        return fileNotFound;
    }
    case 3: case 4: {   // Save (to disk) / AddFile (memory only)
        const uint32_t h = r32(pb + 8);
        Block *b = mm.find_by_handle(h);
        if (!b) fatal("FastFile Save: handle $%06X is not live", h);
        const uint32_t len = r32(pb + 46);
        const uint16_t type = r16(pb + 18); const uint32_t aux = r32(pb + 20);
        if (FastEntry *old = find(host)) { if (old->handle != h) mm.dispose(old->handle); fastfiles.erase(fastfiles.begin() + (old - &fastfiles[0])); }
        if (action == 3) {
            // remove any existing file under another suffix, then write with this one
            HostEntry existing;
            if (stat_host(host, existing) && !existing.is_dir) ::remove(host.c_str());
            std::string base = host; std::string leafbase; uint16_t t; uint32_t a;
            const size_t slash = host.rfind('/');
            const std::string leaf = host.substr(slash + 1);
            if (parse_suffix(leaf, leafbase, t, a)) base = host.substr(0, slash + 1) + leafbase;
            host = base + suffix_for(type, aux);
            std::vector<uint8_t> data(len);
            for (uint32_t i = 0; i < len; ++i) data[i] = r8(b->addr + i);
            if (!write_host_file(host, data)) fatal("cannot write %s", host.c_str());
        }
        b->uid = 0x1003; w16(h + 6, 0x1003);                  // the shell owns it now
        b->attr &= 0x3FFF; w16(h + 4, b->attr);
        fastfiles.push_back(FastEntry{host, h, len, r16(pb + 16), type, aux, 1, flags});
        tlog("FastFile %s %s: %u bytes type $%02X", action == 3 ? "Save" : "AddFile", path.c_str(), len, type);
        return 0;
    }
    case 5: case 6: {   // Delete (and the disk file) / Remove (from memory)
        if (FastEntry *e = find(host)) { mm.dispose(e->handle); fastfiles.erase(fastfiles.begin() + (e - &fastfiles[0])); }
        if (action == 5) ::remove(host.c_str());
        tlog("FastFile %s %s", action == 5 ? "Delete" : "Remove", path.c_str());
        return 0;
    }
    case 7: {   // Purge: the copy may go
        if (FastEntry *e = find(host)) {
            if (e->flags & 0x4000) { mm.dispose(e->handle); fastfiles.erase(fastfiles.begin() + (e - &fastfiles[0])); tlog("FastFile Purge %s (freed)", path.c_str()); }
            else tlog("FastFile Purge %s (kept)", path.c_str());
        }
        return 0;
    }
    case 8: {   // SetInfo
        if (FastEntry *e = find(host)) { e->flags = flags; e->handle = r32(pb + 8); return 0; }
        return fileNotFound;
    }
    default: break;
    }
    fatal("FastFile action %u is not implemented", action);
}

uint16_t shell_call(uint16_t num, uint32_t pb) {
    const bool class1 = num & 0x0040;
    const uint16_t base = num & ~0x0040;
    const uint16_t pcount = class1 ? r16(pb) : 0;
    auto P = [&](int off) { return pb + (class1 ? 2 : 0) + off; };
    switch (base) {
    case 0x0101: {  // GET_LINFO
        if (!class1) fatal("class-0 GET_LINFO (P-strings) is not implemented");
        if (pcount != 11) return 0x04;
        uint16_t err = 0;
        err |= put_result(r32(P(0)), linfo.sfile);
        err |= put_result(r32(P(4)), linfo.dfile);
        err |= put_result(r32(P(8)), linfo.parms);
        err |= put_result(r32(P(12)), linfo.istring);
        w8(P(16), linfo.merr); w8(P(17), linfo.merrf); w8(P(18), linfo.lops); w8(P(19), linfo.kflag);
        w32(P(20), linfo.mflags); w32(P(24), linfo.pflags); w32(P(28), linfo.org);
        tlog("GetLInfoGS: sfile=%s dfile=%s parms=%s lops=%u kflag=%u -%08X +%08X -> %02X",
             linfo.sfile.c_str(), linfo.dfile.c_str(), linfo.parms.c_str(), linfo.lops, linfo.kflag, linfo.mflags, linfo.pflags, err);
        return err;
    }
    case 0x0102: {  // SET_LINFO
        if (!class1) fatal("class-0 SET_LINFO is not implemented");
        if (r32(P(0))) linfo.sfile = gs_string(r32(P(0)));
        if (r32(P(4))) linfo.dfile = gs_string(r32(P(4)));
        if (r32(P(8))) linfo.parms = gs_string(r32(P(8)));
        if (r32(P(12))) linfo.istring = gs_string(r32(P(12)));
        linfo.merr = r8(P(16)); linfo.merrf = r8(P(17)); linfo.lops = r8(P(18)); linfo.kflag = r8(P(19));
        linfo.mflags = r32(P(20)); linfo.pflags = r32(P(24)); linfo.org = r32(P(28));
        tlog("SetLInfoGS: dfile=%s merrf=%u lops=%u kflag=%u", linfo.dfile.c_str(), linfo.merrf, linfo.lops, linfo.kflag);
        return 0;
    }
    case 0x0103: w16(P(0), language); return 0;                  // GET_LANG
    case 0x0104: language = r16(P(0)); return 0;                 // SET_LANG
    case 0x0105: {  // ERROR: print a GS/OS error message
        const uint16_t e = r16(P(0));
        char b[64]; std::snprintf(b, sizeof b, "GS/OS error $%04X\r", e);
        console_out(b, true);
        return 0;
    }
    case 0x0106: {  // SET (variable)
        const std::string name = lower(gs_string(r32(P(0)))), value = gs_string(r32(P(4)));
        shell_vars[name] = value; tlog("SetGS %s = %s", name.c_str(), value.c_str());
        return 0;
    }
    case 0x0107: {  // VERSION: 4 ASCII bytes; ORCA/C checks version[1] >= '2'
        w8(P(0), '2'); w8(P(1), '.'); w8(P(2), '0'); w8(P(3), '6');
        return 0;
    }
    case 0x010B: {  // READ_VARIABLE: name value(ResultBuf) export
        const std::string name = lower(gs_string(r32(P(0))));
        auto it = shell_vars.find(name);
        const std::string value = it == shell_vars.end() ? "" : it->second;
        const uint16_t err = put_result(r32(P(4)), value);
        if (pcount > 2) w16(P(8), 0);
        tlog("ReadVariableGS %s -> '%s' (%02X)", name.c_str(), value.c_str(), err);
        return err;
    }
    case 0x010C: return 0;                                       // CHANGE_VECTOR: nothing to hook
    case 0x010D: fatal("EXECUTE '%s': the harness runs one program, not scripts", gs_string(r32(P(2))).c_str());
    case 0x010E: return fastfile(pb, pcount);
    case 0x010F: w16(P(2), 0); return 0;                         // DIRECTION: not redirected
    case 0x0110: {  // REDIRECT: device append file
        const std::string file = gs_string(r32(P(4)));
        if (lower(file) == ".console" || file.empty()) return 0;
        fatal("REDIRECT to '%s' is not implemented", file.c_str());
    }
    case 0x0113: w16(P(0), 0); return 0;                         // STOP: no ctrl-C
    case 0x0114: {  // EXPAND_DEVICES: inName outName
        const std::string in = gs_string(r32(P(0)));
        return put_result(r32(P(4)), in);
    }
    case 0x0115: shell_vars.erase(lower(gs_string(r32(P(0))))); return 0;   // UNSET_VARIABLE
    case 0x0116: return 0;                                       // EXPORT
    case 0x0117: case 0x0118: return 0;                          // POP/PUSH_VARIABLES
    case 0x0119: return 0;                                       // SET_STOP_FLAG
    case 0x011A: { const char c = static_cast<char>(r16(P(0))); console_out(std::string(1, c), false); return 0; }  // WRITE_CONSOLE
    case 0x011E: w16(P(0), 0); w16(P(2), 0); w16(P(4), 0); return 0;   // KEYPRESS: none available
    default: break;
    }
    fatal("ORCA shell call $%04X is not implemented", num);
}

// --- the Toolbox ---------------------------------------------------------------------------
struct ToolFrame {
    uint32_t args;      // address of the rightmost (last-pushed) parameter
    int nin;            // bytes of input parameters to remove
    uint16_t w(int off) { return r16(args + off); }
    uint32_t l(int off) { return r32(args + off); }
    // results live above the inputs
    void rw(int off, uint16_t v) { w16(args + nin + off, v); }
    void rl(int off, uint32_t v) { w32(args + nin + off, v); }
};

std::string tool_name(uint16_t num) {
    static const std::map<uint16_t, const char *> names = {
        {0x0201, "TLStartUp"}, {0x0301, "TLShutDown"}, {0x0401, "TLVersion"}, {0x0601, "TLStatus"}, {0x0901, "GetTSPtr"},
        {0x0E01, "LoadTools"}, {0x0F01, "LoadOneTool"}, {0x1001, "UnloadOneTool"}, {0x1801, "StartUpTools"}, {0x1901, "ShutDownTools"},
        {0x0202, "MMStartUp"}, {0x0302, "MMShutDown"}, {0x0402, "MMVersion"}, {0x0902, "NewHandle"}, {0x0A02, "ReAllocHandle"},
        {0x1002, "DisposeHandle"}, {0x1102, "DisposeAll"}, {0x1202, "PurgeHandle"}, {0x1302, "PurgeAll"}, {0x1802, "GetHandleSize"},
        {0x1902, "SetHandleSize"}, {0x1A02, "FindHandle"}, {0x1B02, "FreeMem"}, {0x1C02, "MaxBlock"}, {0x1D02, "TotalMem"},
        {0x1E02, "CheckHandle"}, {0x1F02, "CompactMem"}, {0x2002, "HLock"}, {0x2102, "HLockAll"}, {0x2202, "HUnlock"},
        {0x2302, "HUnlockAll"}, {0x2402, "SetPurge"}, {0x2502, "SetPurgeAll"}, {0x2802, "PtrToHand"}, {0x2902, "HandToPtr"},
        {0x2A02, "HandToHand"}, {0x2B02, "BlockMove"}, {0x2F02, "RealFreeMem"}, {0x3002, "SetHandleID"},
        {0x0203, "MTStartUp"}, {0x0303, "MTShutDown"}, {0x0D03, "ReadTimeHex"}, {0x0F03, "ReadAsciiTime"}, {0x1003, "SetVector"},
        {0x1103, "GetVector"}, {0x1503, "SysFailMgr"}, {0x2003, "GetNewID"}, {0x2103, "DeleteID"}, {0x2503, "GetTick"}, {0x2C03, "SysBeep"},
        {0x020A, "SANEStartUp"}, {0x030A, "SANEShutDown"}, {0x060A, "SANEStatus"}, {0x090A, "FPNum"}, {0x0A0A, "DecStrNum"}, {0x0B0A, "ElemNum"},
        {0x020B, "IMStartUp"}, {0x030B, "IMShutDown"}, {0x090B, "Multiply"}, {0x0A0B, "SDivide"}, {0x0B0B, "UDivide"}, {0x0C0B, "LongMul"},
        {0x0D0B, "LongDivide"}, {0x180B, "HiWord"}, {0x190B, "LoWord"}, {0x220B, "Int2Hex"}, {0x230B, "Long2Hex"}, {0x240B, "Hex2Int"},
        {0x250B, "Hex2Long"}, {0x260B, "Int2Dec"}, {0x270B, "Long2Dec"}, {0x280B, "Dec2Int"}, {0x290B, "Dec2Long"},
        {0x020C, "TextStartUp"}, {0x030C, "TextShutDown"}, {0x090C, "SetInGlobals"}, {0x0A0C, "SetOutGlobals"}, {0x0B0C, "SetErrGlobals"},
        {0x0C0C, "GetInGlobals"}, {0x0D0C, "GetOutGlobals"}, {0x0E0C, "GetErrGlobals"}, {0x0F0C, "SetInputDevice"}, {0x100C, "SetOutputDevice"},
        {0x110C, "SetErrorDevice"}, {0x150C, "InitTextDev"}, {0x160C, "CtlTextDev"}, {0x180C, "WriteChar"}, {0x190C, "ErrWriteChar"},
        {0x1A0C, "WriteLine"}, {0x1B0C, "ErrWriteLine"}, {0x1C0C, "WriteString"}, {0x1D0C, "ErrWriteString"}, {0x1E0C, "TextWriteBlock"},
        {0x1F0C, "ErrWriteBlock"}, {0x200C, "WriteCString"}, {0x210C, "ErrWriteCString"}, {0x220C, "ReadChar"}, {0x230C, "TextReadBlock"}, {0x240C, "ReadLine"},
        {0x0411, "LoaderVersion"}, {0x0911, "InitialLoad"}, {0x0A11, "Restart"}, {0x0B11, "LoadSegNum"}, {0x0C11, "UnloadSegNum"},
        {0x1011, "GetUserID"}, {0x1211, "UserShutDown"}, {0x2011, "InitialLoad2"},
    };
    auto it = names.find(num);
    if (it != names.end()) return it->second;
    char b[32]; std::snprintf(b, sizeof b, "tool $%04X", num);
    return b;
}

uint16_t next_id = 0x1010;

// Writes a right-justified decimal/hex string the way Int2Dec/Long2Dec do.
void put_padded(uint32_t at, uint16_t len, const std::string &digits) {
    std::string s = digits;
    if (s.size() > len) s = s.substr(s.size() - len);          // overflow: the low digits
    const std::string out = std::string(len - s.size(), ' ') + s;
    for (uint16_t i = 0; i < len; ++i) w8(at + i, static_cast<uint8_t>(out[i]));
}

// Returns the error code; `f.nin` must be set for every call.
uint16_t tool_call(uint16_t num, ToolFrame &f) {
    switch (num) {
    // Tool Locator
    case 0x0201: f.nin = 0; return 0;
    case 0x0301: f.nin = 0; return 0;
    case 0x0401: f.nin = 0; f.rw(0, 0x0301); return 0;
    case 0x0601: f.nin = 0; f.rw(0, 1); return 0;
    case 0x0E01: f.nin = 4; return 0;                            // LoadTools(table)
    case 0x0F01: f.nin = 4; return 0;                            // LoadOneTool(tool, version)
    case 0x1001: f.nin = 2; return 0;
    case 0x1801: f.nin = 8; f.rl(0, 0); return 0;                // StartUpTools -> Ref
    case 0x1901: f.nin = 6; return 0;
    // Memory Manager
    case 0x0202: f.nin = 0; f.rw(0, USER_ID); return 0;
    case 0x0302: f.nin = 2; return 0;
    case 0x0402: f.nin = 0; f.rw(0, 0x0301); return 0;
    case 0x0902: {  // NewHandle(size:L, userID:W, attr:W, loc:L) -> handle
        f.nin = 12;
        const uint32_t size = f.l(8); const uint16_t uid = f.w(6), attr = f.w(4); const uint32_t loc = f.l(0);
        uint16_t err; const uint32_t h = mm.alloc(size, uid, attr, loc, err);
        f.rl(0, h);
        tlog("NewHandle %u bytes attr $%04X -> $%06X%s", size, attr, h, err ? " FAILED" : "");
        return err;
    }
    case 0x0A02: {  // ReAllocHandle(size, userID, attr, loc, handle)
        f.nin = 16;
        const uint32_t h = f.l(0);
        Block *b = mm.find_by_handle(h);
        if (b) { mm.blocks.erase(b->addr); }
        const uint32_t size = f.l(12); const uint16_t uid = f.w(10), attr = f.w(8); const uint32_t loc = f.l(4);
        uint16_t err; const uint32_t addr = mm.find_room(size ? size : 1, (attr & 1) && ((loc >> 16) == 0) ? BANK0_LO : GEN_LO,
                                                         (attr & 1) && ((loc >> 16) == 0) ? BANK0_HI : GEN_HI, attr & 4, true);
        if (!addr) return 0x0201;
        Block nb; nb.addr = addr; nb.size = size; nb.attr = attr; nb.uid = uid; nb.handle = h;
        w32(h, addr); w16(h + 4, attr); w16(h + 6, uid); w32(h + 8, size);
        mm.blocks[addr] = nb; (void)err;
        return 0;
    }
    case 0x1002: f.nin = 4; mm.dispose(f.l(0)); return 0;
    case 0x1102: f.nin = 2; mm.dispose_all(f.w(0)); tlog("DisposeAll $%04X", f.w(0)); return 0;
    case 0x1202: f.nin = 4; return 0;                            // PurgeHandle: nothing purges here
    case 0x1302: f.nin = 2; return 0;
    case 0x1802: { f.nin = 4; Block *b = mm.find_by_handle(f.l(0)); if (!b) fatal("GetHandleSize: bad handle $%06X", f.l(0)); f.rl(0, b->size); return 0; }
    case 0x1902: { f.nin = 8; const uint16_t err = mm.resize(f.l(0), f.l(4)); tlog("SetHandleSize $%06X -> %u bytes (%04X)", f.l(0), f.l(4), err); return err; }
    case 0x1A02: { f.nin = 4; Block *b = mm.find_by_addr(f.l(0)); f.rl(0, b ? b->handle : 0); return 0; }
    case 0x1B02: f.nin = 0; f.rl(0, mm.max_block()); return 0;
    case 0x1C02: f.nin = 0; f.rl(0, mm.max_block()); return 0;
    case 0x1D02: f.nin = 0; f.rl(0, 0x800000); return 0;
    case 0x1E02: { f.nin = 4; if (!mm.find_by_handle(f.l(0))) return 0x0206; return 0; }
    case 0x1F02: f.nin = 0; return 0;
    case 0x2002: case 0x2202: {   // HLock / HUnlock
        f.nin = 4; Block *b = mm.find_by_handle(f.l(0)); if (!b) return 0x0206;
        if (num == 0x2002) b->attr |= 0x8000; else b->attr &= 0x7FFF;
        w16(b->handle + 4, b->attr); return 0;
    }
    case 0x2102: case 0x2302: f.nin = 2; return 0;
    case 0x2402: { f.nin = 6; Block *b = mm.find_by_handle(f.l(0)); if (!b) return 0x0206; b->attr = static_cast<uint16_t>((b->attr & ~0x0300) | ((f.w(4) & 3) << 8)); w16(b->handle + 4, b->attr); return 0; }
    case 0x2502: f.nin = 4; return 0;
    case 0x2802: {  // PtrToHand(src ptr, dst handle, count)
        f.nin = 12; const uint32_t src = f.l(8), h = f.l(4), n = f.l(0);
        Block *b = mm.find_by_handle(h); if (!b) return 0x0206;
        const uint32_t dst = r32(h);
        for (uint32_t i = 0; i < n; ++i) w8(dst + i, r8(src + i));
        return 0;
    }
    case 0x2902: {  // HandToPtr(src handle, dst ptr, count)
        f.nin = 12; const uint32_t h = f.l(8), dst = f.l(4), n = f.l(0);
        if (!mm.find_by_handle(h)) return 0x0206;
        const uint32_t src = r32(h);
        for (uint32_t i = 0; i < n; ++i) w8(dst + i, r8(src + i));
        return 0;
    }
    case 0x2A02: {  // HandToHand(src, dst, count)
        f.nin = 12; const uint32_t hs = f.l(8), hd = f.l(4), n = f.l(0);
        if (!mm.find_by_handle(hs) || !mm.find_by_handle(hd)) return 0x0206;
        const uint32_t src = r32(hs), dst = r32(hd);
        std::vector<uint8_t> tmp(n); for (uint32_t i = 0; i < n; ++i) tmp[i] = r8(src + i);
        for (uint32_t i = 0; i < n; ++i) w8(dst + i, tmp[i]);
        return 0;
    }
    case 0x2B02: {  // BlockMove(src, dst, count)
        f.nin = 12; const uint32_t src = f.l(8), dst = f.l(4), n = f.l(0);
        if (n > 0x1000000) fatal("BlockMove of %u bytes", n);
        std::vector<uint8_t> tmp(n); for (uint32_t i = 0; i < n; ++i) tmp[i] = r8(src + i);
        for (uint32_t i = 0; i < n; ++i) w8(dst + i, tmp[i]);
        return 0;
    }
    case 0x2F02: f.nin = 0; f.rl(0, mm.max_block()); return 0;
    case 0x3002: { f.nin = 6; Block *b = mm.find_by_handle(f.l(0)); if (!b) return 0x0206; f.rw(0, b->uid); b->uid = f.w(4); w16(b->handle + 6, b->uid); return 0; }
    // Misc Tools
    case 0x0203: f.nin = 0; return 0;
    case 0x0303: f.nin = 0; return 0;
    case 0x0D03: {  // ReadTimeHex -> 8 bytes: seconds minutes hour year day month 0 weekday
        f.nin = 0; for (int i = 0; i < 8; ++i) w8(f.args + i, FIXED_TIME[i]); return 0;
    }
    case 0x0F03: {  // ReadAsciiTime(ptr): 20 bytes with the high bit set
        f.nin = 4; const char *t = "09/15/86 12:00:00 PM"; for (int i = 0; i < 20; ++i) w8(f.l(0) + i, static_cast<uint8_t>(t[i] | 0x80)); return 0;
    }
    case 0x1003: f.nin = 6; return 0;                            // SetVector
    case 0x1103: f.nin = 2; f.rl(0, 0); return 0;                // GetVector
    case 0x1503: {  // SysFailMgr(code, message)
        f.nin = 6; const std::string msg = p_string(f.l(0));
        fatal("SysFailMgr: code $%04X %s", f.w(4), msg.c_str());
    }
    case 0x2003: f.nin = 2; f.rw(0, next_id++); return 0;
    case 0x2103: f.nin = 2; return 0;
    case 0x2503: f.nin = 0; f.rl(0, static_cast<uint32_t>(cpu.instructions / 50000)); return 0;
    case 0x2C03: f.nin = 0; return 0;
    // SANE: present, but nothing computes here
    case 0x020A: f.nin = 4; return 0;
    case 0x030A: f.nin = 0; return 0;
    case 0x060A: f.nin = 0; f.rw(0, 1); return 0;
    // Integer Math
    case 0x020B: case 0x030B: f.nin = 0; return 0;
    case 0x090B: { f.nin = 4; const int32_t r = static_cast<int16_t>(f.w(2)) * static_cast<int32_t>(static_cast<int16_t>(f.w(0))); f.rl(0, static_cast<uint32_t>(r)); return 0; }
    case 0x0A0B: {  // SDivide(dividend, divisor) -> remainder, quotient (quotient on top)
        f.nin = 4; const int16_t a = static_cast<int16_t>(f.w(2)), b = static_cast<int16_t>(f.w(0));
        if (b == 0) return 0x0B01;
        f.rw(0, static_cast<uint16_t>(a / b)); f.rw(2, static_cast<uint16_t>(a % b)); return 0;
    }
    case 0x0B0B: { f.nin = 4; const uint16_t a = f.w(2), b = f.w(0); if (!b) return 0x0B01; f.rw(0, a / b); f.rw(2, a % b); return 0; }
    case 0x0C0B: { f.nin = 8; const int64_t r = static_cast<int64_t>(static_cast<int32_t>(f.l(4))) * static_cast<int32_t>(f.l(0)); f.rl(0, static_cast<uint32_t>(r)); f.rl(4, static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32)); return 0; }
    case 0x0D0B: { f.nin = 8; const int32_t a = static_cast<int32_t>(f.l(4)), b = static_cast<int32_t>(f.l(0)); if (!b) return 0x0B01; f.rl(0, static_cast<uint32_t>(a / b)); f.rl(4, static_cast<uint32_t>(a % b)); return 0; }
    case 0x180B: f.nin = 4; f.rw(0, static_cast<uint16_t>(f.l(0) >> 16)); return 0;
    case 0x190B: f.nin = 4; f.rw(0, static_cast<uint16_t>(f.l(0))); return 0;
    case 0x220B: { f.nin = 8; char b[8]; std::snprintf(b, sizeof b, "%04X", f.w(6)); put_padded(f.l(2), f.w(0), b); return 0; }        // Int2Hex(int, ptr, len)
    case 0x230B: { f.nin = 10; char b[12]; std::snprintf(b, sizeof b, "%08X", f.l(6)); put_padded(f.l(2), f.w(0), b); return 0; }      // Long2Hex(long, ptr, len)
    case 0x240B: { f.nin = 6; const std::string s = [&] { std::string t; for (uint16_t i = 0; i < f.w(0); ++i) t += static_cast<char>(r8(f.l(2) + i)); return t; }(); f.rw(0, static_cast<uint16_t>(std::strtoul(s.c_str(), nullptr, 16))); return 0; }
    case 0x250B: { f.nin = 6; const std::string s = [&] { std::string t; for (uint16_t i = 0; i < f.w(0); ++i) t += static_cast<char>(r8(f.l(2) + i)); return t; }(); f.rl(0, static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 16))); return 0; }
    case 0x260B: {  // Int2Dec(int, ptr, len, signed)
        f.nin = 10; const bool sgn = f.w(0); const uint16_t len = f.w(2); const uint32_t p = f.l(4); const uint16_t v = f.w(8);
        put_padded(p, len, sgn ? std::to_string(static_cast<int16_t>(v)) : std::to_string(v)); return 0;
    }
    case 0x270B: {  // Long2Dec(long, ptr, len, signed)
        f.nin = 12; const bool sgn = f.w(0); const uint16_t len = f.w(2); const uint32_t p = f.l(4); const uint32_t v = f.l(8);
        put_padded(p, len, sgn ? std::to_string(static_cast<int32_t>(v)) : std::to_string(v)); return 0;
    }
    case 0x280B: case 0x290B: {  // Dec2Int / Dec2Long(ptr, len, signed)
        const bool is_long = (num == 0x290B);
        f.nin = 8; const bool sgn = f.w(0); const uint16_t len = f.w(2); const uint32_t p = f.l(4);
        std::string s; for (uint16_t i = 0; i < len; ++i) s += static_cast<char>(r8(p + i));
        const long long v = std::strtoll(s.c_str(), nullptr, 10);
        (void)sgn;
        if (is_long) f.rl(0, static_cast<uint32_t>(v)); else f.rw(0, static_cast<uint16_t>(v));
        return 0;
    }
    // Text Tools
    case 0x020C: case 0x030C: f.nin = 0; return 0;
    case 0x090C: case 0x0A0C: case 0x0B0C: f.nin = 4; return 0;
    case 0x0C0C: case 0x0D0C: case 0x0E0C: f.nin = 0; f.rw(0, 0x7F); f.rw(2, 0x00); return 0;
    case 0x0F0C: case 0x100C: case 0x110C: f.nin = 6; return 0;
    case 0x150C: f.nin = 2; return 0;
    case 0x160C: f.nin = 4; return 0;
    case 0x180C: case 0x190C: { f.nin = 2; console_out(std::string(1, static_cast<char>(f.w(0) & 0x7F)), num == 0x190C); return 0; }
    case 0x1A0C: case 0x1B0C: { f.nin = 4; console_out(p_string(f.l(0)) + "\r", num == 0x1B0C); return 0; }
    case 0x1C0C: case 0x1D0C: { f.nin = 4; console_out(p_string(f.l(0)), num == 0x1D0C); return 0; }
    case 0x1E0C: case 0x1F0C: {  // TextWriteBlock(ptr, offset, count)
        f.nin = 8; const uint32_t p = f.l(4); const uint16_t off = f.w(2), n = f.w(0);
        std::string s; for (uint16_t i = 0; i < n; ++i) s += static_cast<char>(r8(p + off + i) & 0x7F);
        console_out(s, num == 0x1F0C); return 0;
    }
    case 0x200C: case 0x210C: { f.nin = 4; console_out(c_string(f.l(0)), num == 0x210C); return 0; }
    case 0x220C: { f.nin = 2; const int ch = std::fgetc(stdin); f.rw(0, ch == EOF ? 0x0D : (ch == '\n' ? 0x0D : ch)); return 0; }
    // Loader
    case 0x0411: f.nin = 0; f.rw(0, 0x0400); return 0;
    default: break;
    }
    fatal("Toolbox call %s ($%04X) is not implemented", tool_name(num).c_str(), num);
}

// --- trap dispatch ----------------------------------------------------------------------------
void set_error(uint16_t err) {
    cpu.a = err;          // GS/OS and the Toolbox return the error in a full 16-bit A
    cpu.c = err != 0;
    cpu.z = err == 0;
}

uint16_t gsos_call(uint16_t num, uint32_t pb) {
    if ((num & 0xFF00) == 0x0100) return shell_call(num, pb);
    if (num & 0x2000) return gsos_class1(num, pb);
    return gsos_class0(num, pb);
}

bool OrcaBus::trap(uint32_t pcaddr) {
    if (pcaddr == HALT_ADDR || pcaddr == HALT_ADDR + 1) { finished = true; return true; }
    switch (pcaddr) {
    case TOOL_ENTRY: case TOOL_ENTRY2: {
        ++call_count;
        const uint16_t num = cpu.x;
        ToolFrame f{static_cast<uint32_t>(cpu.sp) + 4, 0};
        const uint16_t err = tool_call(num, f);
        if (trace && (num & 0xFF) != 0x0C && num != 0x0902) tlog("%s -> %04X", tool_name(num).c_str(), err);
        const uint32_t ret = r24(cpu.sp + 1);
        cpu.sp = static_cast<uint16_t>(cpu.sp + 3 + f.nin);
        cpu.pc = static_cast<uint16_t>((ret & 0xFFFF) + 1);
        cpu.pbr = static_cast<uint8_t>(ret >> 16);
        set_error(err);
        return true;
    }
    case GSOS_INLINE: {
        ++call_count;
        const uint32_t ret = r24(cpu.sp + 1);
        const uint16_t num = r16(ret + 1);
        const uint32_t pb = r32(ret + 3);
        const uint16_t err = gsos_call(num, pb);
        cpu.sp += 3;
        cpu.pc = static_cast<uint16_t>((ret & 0xFFFF) + 7);
        cpu.pbr = static_cast<uint8_t>(ret >> 16);
        set_error(err);
        return true;
    }
    case GSOS_STACK: {
        ++call_count;
        const uint32_t ret = r24(cpu.sp + 1);
        const uint16_t num = r16(cpu.sp + 4);
        const uint32_t pb = r32(cpu.sp + 6);
        const uint16_t err = gsos_call(num, pb);
        cpu.sp += 9;
        cpu.pc = static_cast<uint16_t>((ret & 0xFFFF) + 1);
        cpu.pbr = static_cast<uint8_t>(ret >> 16);
        set_error(err);
        return true;
    }
    default: return false;
    }
}

// --- running a program ---------------------------------------------------------------------------
int run_program(const std::string &prog_host, const std::string &cmdline) {
    std::vector<uint8_t> image;
    if (!read_host_file(prog_host, image)) fatal("cannot read %s", prog_host.c_str());
    HostEntry pe; stat_host(prog_host, pe);
    shell_vars["~program"] = pe.name;
    w8(OS_KIND, 1);

    Loaded L = load_omf(image, prog_host, [&](const OmfSegment &s, uint32_t size, bool dpstack) {
        uint16_t attr = 0xC000 | (dpstack ? 0x0005 : 0x0000);   // locked, fixed; the DP/stack in bank 0, page aligned
        if (s.align >= 0x100) attr |= 0x0004;
        if (s.banksize == 0x10000) attr |= 0x0010;
        uint16_t err; const uint32_t h = mm.alloc(size, USER_ID, attr, 0, err);
        if (!h) fatal("no room for segment '%s' (%u bytes)", s.segname.c_str(), size);
        const uint32_t addr = r32(h);
        tlog("segment %2u %-12s kind $%04X %6u bytes at $%06X", s.segnum, s.segname.c_str(), s.kind, size, addr);
        return addr;
    });
    if (L.dp_size == 0) {
        const uint32_t h = alloc_block(stack_default, 0xC005, 0);
        L.dp_addr = r32(h); L.dp_size = stack_default;
    }
    // "BYTEWRKS" + the command line, as the shell passes it in X:Y
    const std::string line = "BYTEWRKS" + cmdline;
    const uint32_t lh = alloc_block(static_cast<uint32_t>(line.size() + 1), 0xC000);
    const uint32_t laddr = r32(lh);
    for (size_t i = 0; i < line.size(); ++i) w8(laddr + static_cast<uint32_t>(i), static_cast<uint8_t>(line[i]));
    w8(laddr + static_cast<uint32_t>(line.size()), 0);

    cpu.reset(0);
    cpu.e = false; cpu.m = false; cpu.xf = false;
    cpu.dbr = 0;
    cpu.dp = static_cast<uint16_t>(L.dp_addr);
    cpu.sp = static_cast<uint16_t>(L.dp_addr + L.dp_size - 1);
    cpu.push8(0); cpu.push16(static_cast<uint16_t>(HALT_ADDR - 1));   // RTL lands on the halt
    cpu.a = USER_ID;
    cpu.x = static_cast<uint16_t>(laddr >> 16);
    cpu.y = static_cast<uint16_t>(laddr & 0xFFFF);
    cpu.pbr = static_cast<uint8_t>(L.entry >> 16);
    cpu.pc = static_cast<uint16_t>(L.entry & 0xFFFF);
    tlog("entry $%06X, DP/stack $%04X+%u, command line at $%06X", L.entry, L.dp_addr, L.dp_size, laddr);

    finished = false;
    while (!finished) cpu.step();

    // close anything left open so writes land
    std::vector<uint16_t> open; for (auto &kv : files) open.push_back(kv.first);
    for (uint16_t rn : open) do_close(rn);

    if (trace) std::fprintf(stderr, "orcarun: %llu instructions, %llu calls; merrf=%u merr=%u\n",
                            static_cast<unsigned long long>(cpu.instructions), static_cast<unsigned long long>(call_count), linfo.merrf, linfo.merr);
    return (linfo.merrf > linfo.merr) ? 1 : 0;
}

// --- omf2bin ---------------------------------------------------------------------------------
// The ORCA linker's `+S` symbol table gives each symbol as (offset, segment
// number); only omf2bin knows where each segment lands. With --syms, turn the
// table into "ADDRESS NAME" lines, sorted, for a2min's profiler
// (A2MIN_SYMBOLS). A page-granular profile could only say "the tail of bank 0";
// this is what names the routine.
void write_symbol_map(const std::string &table, const std::string &out,
                      const std::map<uint16_t, uint32_t> &segbase) {
    std::vector<uint8_t> raw;
    if (!read_host_file(table, raw)) fatal("cannot read %s", table.c_str());
    std::istringstream in(std::string(raw.begin(), raw.end()));
    std::vector<std::pair<uint32_t, std::string>> syms;
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        if (line.find("Global symbol table") != std::string::npos) { inside = true; continue; }
        if (!inside) continue;
        // Two entries per line: OFFSET G|P SEG 00 NAME
        std::istringstream ls(line);
        std::string off, gp, seg, zero, name;
        while (ls >> off >> gp >> seg >> zero >> name) {
            // Only real entries: the table is followed by prose ("There are 2
            // segments, for a length of ...") that would otherwise parse.
            if (off.size() != 8 || off.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos ||
                (gp != "G" && gp != "P")) break;
            const auto it = segbase.find(static_cast<uint16_t>(std::strtoul(seg.c_str(), nullptr, 16)));
            if (it == segbase.end()) continue;              // a segment omf2bin did not place
            syms.emplace_back(it->second + static_cast<uint32_t>(std::strtoul(off.c_str(), nullptr, 16)), name);
        }
    }
    if (syms.empty()) fatal("%s: no symbols found (was the linker given +S?)", table.c_str());
    std::sort(syms.begin(), syms.end());
    std::string text;
    char buf[16];
    for (const auto &s : syms) { std::snprintf(buf, sizeof buf, "%06X ", s.first); text += buf + s.second + "\n"; }
    if (!write_host_file(out, std::vector<uint8_t>(text.begin(), text.end()))) fatal("cannot write %s", out.c_str());
}

int omf2bin(const std::string &in, const std::string &out, uint32_t org,
            const std::string &syms_in = "", const std::string &map_out = "") {
    std::map<uint16_t, uint32_t> segbase;                         // segment number -> load address
    std::vector<uint8_t> image;
    if (!read_host_file(in, image)) fatal("cannot read %s", in.c_str());
    // The stub a2min enters in emulation mode: go native with 16-bit registers,
    // park the 8-bit stack pointer at $03FE, give the C code a stack at $BFFF
    // (the top of bank 0's RAM; the C VM's runs down to $7000, above its
    // program: docs/VM.md "The C stack"), JSL the entry, then put everything
    // back and RTS to a2min's halt.
    //   clc xce rep #$30 tsc sta $03FE lda #$BFFF tcs jsl entry lda $03FE tcs sec xce rts
    const uint32_t stub_len = 23;
    uint32_t cursor = org + stub_len;
    // Bank 0 holds the first segment (code and, in the small model, every global)
    // and any data segment, below $C000 where the IIgs has its I/O space. Further
    // code segments -- ORCA/C's `segment "name";` -- get banks of their own from
    // bank 4 up; ORCA/C calls every function with JSL, and absolute addressing
    // still means bank 0 because nothing changes the data bank register.
    constexpr uint32_t BANK0_LIMIT = 0xC000, FIRST_CODE_BANK = 4;
    uint32_t code_bank = FIRST_CODE_BANK, bank_cursor = 0;
    bool first = true;                                            // the blank segment: code + globals
    std::vector<std::pair<uint32_t, uint32_t>> extra;             // (address, size) outside bank 0
    Loaded L = load_omf(image, in, [&](const OmfSegment &s, uint32_t size, bool dpstack) {
        if (dpstack) return 0x0000u;                              // a2min programs bring their own DP
        const bool code = (s.kind & 0x1F) == 0x00;
        const bool blank = first;                                 // (segment numbers start after ExpressLoad's)
        first = false;
        if (blank || !code) {
            cursor = (cursor + 0xFF) & ~0xFFu;                    // page aligned
            const uint32_t at = cursor;
            if (at + size > BANK0_LIMIT)
                fatal("omf2bin: segment '%s' (%u bytes) does not fit in bank 0 below $C000; "
                      "move code into a `segment` of its own", s.segname.c_str(), size);
            cursor += size;
            segbase[s.segnum] = at;
            return at;
        }
        if (size > 0x10000) fatal("omf2bin: segment '%s' is larger than a bank", s.segname.c_str());
        if (bank_cursor + size > 0x10000) {
            ++code_bank;
            bank_cursor = 0;
            // Bank 5 is the VM's window ring and remembered set, bank 7 the
            // regex engine's NFA and stack (VM.md): code there would be
            // overwritten by the first copy of a mature object, or the first
            // match. scripts/bank-gate.py checks the map for any symbol in them.
            if (code_bank == 5 || code_bank == 7) ++code_bank;
        }
        const uint32_t at = (code_bank << 16) | bank_cursor;
        bank_cursor = (bank_cursor + size + 0xFF) & ~0xFFu;
        extra.emplace_back(at, size);
        segbase[s.segnum] = at;
        return at;
    });
    const uint8_t stub[stub_len] = { 0x18, 0xFB, 0xC2, 0x30, 0x3B, 0x8D, 0xFE, 0x03, 0xA9, 0xFF, 0xBF, 0x1B,
                                     0x22, static_cast<uint8_t>(L.entry), static_cast<uint8_t>(L.entry >> 8), static_cast<uint8_t>(L.entry >> 16),
                                     0xAD, 0xFE, 0x03, 0x1B, 0x38, 0xFB, 0x60 };
    for (uint32_t i = 0; i < stub_len; ++i) w8(org + i, stub[i]);
    std::vector<uint8_t> bin;
    if (extra.empty()) {
        // everything in bank 0: a flat image, loaded at org
        bin.assign(ram.begin() + org, ram.begin() + cursor);
    } else {
        // "GSIM", a chunk count, then (address, length, bytes) per chunk
        auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) bin.push_back(static_cast<uint8_t>(v >> (8 * i))); };
        bin.insert(bin.end(), {'G', 'S', 'I', 'M'});
        const unsigned count = 1 + static_cast<unsigned>(extra.size());
        bin.push_back(count & 0xFF); bin.push_back(count >> 8);
        put32(org); put32(cursor - org);
        bin.insert(bin.end(), ram.begin() + org, ram.begin() + cursor);
        for (const auto &e : extra) {
            put32(e.first); put32(e.second);
            bin.insert(bin.end(), ram.begin() + e.first, ram.begin() + e.first + e.second);
        }
    }
    if (!write_host_file(out, bin)) fatal("cannot write %s", out.c_str());
    std::printf("omf2bin: %s -> %s: %u bytes in bank 0 at $%04X", in.c_str(), out.c_str(), cursor - org, org);
    for (const auto &e : extra) std::printf(", %u at $%06X", e.second, e.first);
    std::printf(", entry $%06X\n", L.entry);
    if (!syms_in.empty()) write_symbol_map(syms_in, map_out, segbase);
    return 0;
}

void usage() {
    std::fprintf(stderr,
        "usage: orcarun [--map N=DIR]... [--orca ROOT] [--var NAME=VALUE]... [--trace] [--stack N]\n"
        "               [--sfile S] [--dfile D] [--parms P] [--istring I] [--minus abc] [--plus xyz]\n"
        "               [--lops N] [--merr N] [--keep] [--org N] <program> [command line...]\n"
        "       orcarun --omf2bin [--org 0x2000] [--syms TABLE OUT.map] <load-file> <out.bin>\n");
    std::exit(2);
}

uint32_t flag_bits(const std::string &letters) {
    // letter 'a' is bit 31, 'b' bit 30, ... (CCommon.pas: flag_d = $10000000)
    uint32_t v = 0;
    for (char c : letters) {
        const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (l < 'a' || l > 'z') fatal("flag letter '%c'", c);
        v |= 1u << (31 - (l - 'a'));
    }
    return v;
}

} // namespace

int main(int argc, char **argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) usage();

    if (args[0] == "--omf2bin") {
        uint32_t org = 0x2000;
        std::string syms_in, map_out;
        std::vector<std::string> files;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--org") {
                if (i + 1 >= args.size()) fatal("--org needs a value");
                org = static_cast<uint32_t>(std::strtoul(args[++i].c_str(), nullptr, 0));
            } else if (args[i] == "--syms") {
                if (i + 2 >= args.size()) fatal("--syms needs TABLE and OUT.map");
                syms_in = args[++i]; map_out = args[++i];
            } else if (args[i].rfind("--", 0) == 0) { std::fprintf(stderr, "unknown option %s\n", args[i].c_str()); usage(); }
            else files.push_back(args[i]);
        }
        if (files.size() != 2) usage();
        return omf2bin(files[0], files[1], org, syms_in, map_out);
    }

    std::string program, cmdline;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        auto need = [&](const char *what) -> std::string { if (i + 1 >= args.size()) fatal("%s needs a value", what); return args[++i]; };
        if (a == "--map") {
            const std::string v = need("--map");
            const size_t eq = v.find('=');
            if (eq == std::string::npos) fatal("--map N=DIR");
            const unsigned n = static_cast<unsigned>(std::strtoul(v.substr(0, eq).c_str(), nullptr, 10));
            if (n >= 32) fatal("--map: prefix %u", n);
            prefixes[n] = v.substr(eq + 1);
        } else if (a == "--orca") {
            const std::string root = need("--orca");
            prefixes[13] = root + "/Libraries";
            prefixes[15] = root;
            prefixes[16] = root + "/Languages";
            prefixes[17] = root + "/Utilities";
            boot_volume = root; home = root;
        } else if (a == "--var") {
            const std::string v = need("--var");
            const size_t eq = v.find('=');
            if (eq == std::string::npos) fatal("--var NAME=VALUE");
            shell_vars[lower(v.substr(0, eq))] = v.substr(eq + 1);
        } else if (a == "--trace") trace = true;
        else if (a == "--stack") stack_default = static_cast<uint32_t>(std::strtoul(need("--stack").c_str(), nullptr, 0));
        else if (a == "--sfile") linfo.sfile = need("--sfile");
        else if (a == "--dfile") linfo.dfile = need("--dfile");
        else if (a == "--parms") linfo.parms = need("--parms");
        else if (a == "--istring") linfo.istring = need("--istring");
        else if (a == "--minus") linfo.mflags = flag_bits(need("--minus"));
        else if (a == "--plus") linfo.pflags = flag_bits(need("--plus"));
        else if (a == "--lops") linfo.lops = static_cast<uint8_t>(std::strtoul(need("--lops").c_str(), nullptr, 0));
        else if (a == "--merr") linfo.merr = static_cast<uint8_t>(std::strtoul(need("--merr").c_str(), nullptr, 0));
        else if (a == "--keep") linfo.kflag = 1;
        else if (a == "--org") linfo.org = static_cast<uint32_t>(std::strtoul(need("--org").c_str(), nullptr, 0));
        else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); }
        else if (program.empty()) program = a;
        else cmdline += (cmdline.empty() ? "" : " ") + a;
    }
    if (program.empty()) usage();
    if (prefixes[0].empty()) {
        char buf[4096];
        if (::getcwd(buf, sizeof buf)) prefixes[0] = buf;
    }
    {
        // prefixes 1 and 9: the program's own directory (the shell sets 9)
        const size_t slash = program.rfind('/');
        const std::string dir = (slash == std::string::npos) ? "." : program.substr(0, slash);
        char rp[4096];
        const std::string abs = ::realpath(dir.c_str(), rp) ? rp : dir;
        if (prefixes[1].empty()) prefixes[1] = abs;
        if (prefixes[9].empty()) prefixes[9] = abs;
    }
    if (linfo.dfile.empty() == false) linfo.kflag = 1;
    cpu_ptr = &cpu;
    return run_program(program, cmdline);
}
