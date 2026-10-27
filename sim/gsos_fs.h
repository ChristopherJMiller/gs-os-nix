// gsos_fs.h -- a2min's GS/OS: the class-1 file calls and the ORCA shell's
// ReadVariable, answered from host directories, read-only (docs/GSOS.md).
//
// It is what the VM's gsos.c will meet on the IIgs, as far as the VM can
// tell: GS/OS full pathnames with `/` separators, a directory's file type
// $0F, and our FST's symlink -- a file of type $F1, aux type "NSL", holding
// its target (DECISIONS.md D30). So no host symlink is ever followed, in the
// last component or any other: the VM resolves them itself, as Nix does, and
// that is what this tests. The volumes are exactly the mounts.
//
// Not orcarun's GS/OS, which has one volume (the host root), follows host
// symlinks, and writes: that one serves the compiler.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct GsosFs {
    std::function<uint8_t(uint32_t)> rd;
    std::function<void(uint32_t, uint8_t)> wr;
    std::string prefix, dir;                    // A2MIN_MOUNT=<prefix>=<dir>

    static constexpr uint16_t badSystemCall = 0x01, badPathSyntax = 0x40, invalidRefNum = 0x43,
                              pathNotFound = 0x44, volNotFound = 0x45, fileNotFound = 0x46,
                              eofEncountered = 0x4C, buffTooSmall = 0x4F, endOfDir = 0x61;
    static constexpr uint16_t FT_DIR = 0x0F, FT_FILE = 0x00, FT_LINK = 0xF1;
    static constexpr uint32_t AUX_LINK = 0x004C534E;   // "NSL"

    struct Node { uint16_t ftype = FT_FILE; uint32_t aux = 0; bool dir = false; };
    struct Entry { std::string name; Node node; };
    struct Open { std::string bytes; std::vector<Entry> entries; bool dir = false; uint32_t mark = 0; size_t next = 0; };
    std::map<uint16_t, Open> open;
    uint16_t next_ref = 1;

    bool mount(const char *spec) {
        const char *eq = std::strchr(spec, '=');
        if (!eq || spec[0] != '/') return false;
        prefix.assign(spec, eq);
        dir = eq + 1;
        while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
        return !dir.empty();
    }

    uint16_t r16(uint32_t a) { return static_cast<uint16_t>(rd(a) | (rd(a + 1) << 8)); }
    uint32_t r32(uint32_t a) { return r16(a) | (static_cast<uint32_t>(r16(a + 2)) << 16); }
    void w16(uint32_t a, uint16_t v) { wr(a, v & 0xFF); wr(a + 1, v >> 8); }
    void w32(uint32_t a, uint32_t v) { w16(a, v & 0xFFFF); w16(a + 2, v >> 16); }
    std::string gs_string(uint32_t a) {
        std::string s;
        for (uint16_t i = 0, n = r16(a); i < n; ++i) s += static_cast<char>(rd(a + 2 + i));
        return s;
    }
    // A ResultBuf: its size, then a GSString's length and bytes
    uint16_t put_result(uint32_t a, const std::string &s) {
        const uint16_t size = r16(a);
        w16(a + 2, static_cast<uint16_t>(s.size()));
        if (s.size() + 4 > size) return buffTooSmall;
        for (size_t i = 0; i < s.size(); ++i) wr(a + 4 + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
        return 0;
    }

    static Node node_of(const std::filesystem::file_status &st) {
        Node n;
        if (std::filesystem::is_symlink(st)) { n.ftype = FT_LINK; n.aux = AUX_LINK; }
        else if (std::filesystem::is_directory(st)) { n.ftype = FT_DIR; n.dir = true; }
        return n;
    }

    // A GS/OS full pathname to the host path under the mount, every component
    // looked at without following it: a symlink is a file, so nothing is
    // under it. 0, or the GS/OS error.
    uint16_t lookup(const std::string &path, std::filesystem::path &host, Node &node) {
        namespace fs = std::filesystem;
        if (path.empty() || path[0] != '/' || path.find(':') != std::string::npos) return badPathSyntax;
        if (prefix.empty() || path.compare(0, prefix.size(), prefix) != 0 ||
            (path.size() > prefix.size() && path[prefix.size()] != '/'))
            return volNotFound;
        host = dir;
        node = Node{FT_DIR, 0, true};
        std::string rest = path.substr(prefix.size()), c;
        for (size_t i = 0; i <= rest.size(); ++i) {
            if (i < rest.size() && rest[i] != '/') { c += rest[i]; continue; }
            if (c.empty()) continue;
            if (!node.dir) return pathNotFound;         // a file where a directory must be
            host /= c;
            std::error_code ec;
            const fs::file_status st = fs::symlink_status(host, ec);
            if (ec || !fs::exists(st)) {
                const bool last = rest.find_first_not_of('/', i) == std::string::npos;
                return last ? fileNotFound : pathNotFound;
            }
            node = node_of(st);
            c.clear();
        }
        return 0;
    }

    uint16_t call(uint16_t num, uint32_t pb) {
        namespace fs = std::filesystem;
        const uint16_t pcount = r16(pb);
        switch (num) {
        case 0x2006: {                                  // GetFileInfo
            fs::path host; Node n;
            if (const uint16_t e = lookup(gs_string(r32(pb + 2)), host, n)) return e;
            if (pcount >= 2) w16(pb + 6, 0xC3);                     // access
            if (pcount >= 3) w16(pb + 8, n.ftype);
            if (pcount >= 4) w32(pb + 10, n.aux);
            if (pcount >= 5) w16(pb + 14, n.dir ? 0x0D : 0x01);     // storage type
            return 0;
        }
        case 0x2010: {                                  // Open, for reading
            fs::path host; Node n;
            if (const uint16_t e = lookup(gs_string(r32(pb + 4)), host, n)) return e;
            Open o;
            o.dir = n.dir;
            if (n.ftype == FT_LINK) o.bytes = fs::read_symlink(host).string();
            else if (n.dir) {
                for (const auto &de : fs::directory_iterator(host))
                    o.entries.push_back({de.path().filename().string(), node_of(de.symlink_status())});
                std::sort(o.entries.begin(), o.entries.end(),
                          [](const Entry &a, const Entry &b) { return a.name < b.name; });
            } else {
                std::ifstream f(host, std::ios::binary);
                o.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
            const uint16_t ref = next_ref++;
            open[ref] = std::move(o);
            w16(pb + 2, ref);
            return 0;
        }
        case 0x2012: {                                  // Read
            auto it = open.find(r16(pb + 2));
            if (it == open.end()) return invalidRefNum;
            Open &o = it->second;
            const uint32_t buf = r32(pb + 4) & 0xFFFFFF, req = r32(pb + 8);
            uint32_t n = 0;
            while (n < req && o.mark < o.bytes.size()) wr(buf + n++, static_cast<uint8_t>(o.bytes[o.mark++]));
            w32(pb + 12, n);
            return n == 0 && req != 0 ? eofEncountered : 0;
        }
        case 0x2014: return open.erase(r16(pb + 2)) ? 0 : invalidRefNum;   // Close
        case 0x2019: {                                  // GetEOF
            auto it = open.find(r16(pb + 2));
            if (it == open.end()) return invalidRefNum;
            w32(pb + 4, static_cast<uint32_t>(it->second.bytes.size()));
            return 0;
        }
        case 0x201C: {                                  // GetDirEntry: base 1, the next entry
            auto it = open.find(r16(pb + 2));
            if (it == open.end() || !it->second.dir) return invalidRefNum;
            Open &o = it->second;
            if (r16(pb + 6) != 1 || r16(pb + 8) != 1) return badSystemCall;   // the VM asks for nothing else
            if (o.next >= o.entries.size()) return endOfDir;
            const Entry &e = o.entries[o.next++];
            w16(pb + 4, 0);                             // flags
            if (const uint16_t err = put_result(r32(pb + 10), e.name)) return err;
            if (pcount >= 6) w16(pb + 14, static_cast<uint16_t>(o.next));
            if (pcount >= 7) w16(pb + 16, e.node.ftype);
            if (pcount >= 13) w32(pb + 44, e.node.aux);
            return 0;
        }
        case 0x010B: {                                  // ReadVariable: A2MIN_VAR_<NAME>, its case folded
            std::string name = gs_string(r32(pb + 2));
            for (char &ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            const char *v = std::getenv(("A2MIN_VAR_" + name).c_str());
            if (pcount > 2) w16(pb + 10, 0);
            return put_result(r32(pb + 6), v ? v : "");
        }
        default: return badSystemCall;
        }
    }
};
