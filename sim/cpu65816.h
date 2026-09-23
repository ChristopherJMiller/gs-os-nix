// cpu65816.h -- the 65816 interpreter shared by the harnesses in sim/.
//
// One core, two worlds. a2min.cpp wires it to RAM, a slot decoder and the
// Verilated a2nix card (M2, the VM scoreboard); orcarun.cpp wires it to a flat
// 16 MB address space and a host implementation of the GS/OS, ORCA-shell and
// Toolbox entry points, so that ORCA/C runs on Linux (docs/TOOLCHAIN.md, D18).
// The instruction semantics and the data-sheet cycle costs are identical in
// both; only the bus differs, which is the point.
//
// The discipline from a2min.cpp is kept verbatim: an unknown opcode ABORTS
// rather than being skipped, decimal mode aborts, and so does anything
// interrupt-related (BRK, COP, RTI, WAI) -- none of that is modelled, and a
// program that reaches it is outside what the harness can vouch for.
//
// CYCLE COUNTS are the WDC data-sheet base costs: 2 per instruction, plus the
// addressing-mode cost, plus one per extra byte moved when a register is 16 bits
// wide, plus one for direct-page modes when the D register's low byte is
// non-zero. Not modelled: the page-crossing penalty on indexed reads, and the
// 6502 direct-page wrap in emulation mode.
//
// The Bus policy supplies:
//   uint8_t rd8(uint32_t addr);            24-bit address, already masked
//   void    wr8(uint32_t addr, uint8_t v);
//   bool    trap(uint32_t pcaddr);         called before each fetch; true = the
//                                          bus handled this address (a halt, a
//                                          tool-call vector) and the CPU must not
//                                          execute what is there
//   void    cycles(int n);                 the instruction's cycle count, for
//                                          whatever the bus does with time
//   [[noreturn]] void die(const char *what, uint32_t where);
#pragma once

#include <cstdint>
#include <cstdio>

template <class Bus>
struct Cpu65816 {
    Bus &bus;

    uint16_t a = 0;         // C. In 8-bit mode the high byte (B) is preserved.
    uint16_t x = 0, y = 0;
    uint16_t sp = 0x01FF;
    uint16_t dp = 0;        // direct page register D
    uint8_t  dbr = 0, pbr = 0;
    uint16_t pc = 0;
    bool c = false, z = false, n = false, v = false;
    bool dec = false, irq = false;
    bool m = true, xf = true;   // M and X width flags: 1 = 8-bit
    bool e = true;              // emulation mode
    uint64_t instructions = 0;
    uint64_t cycles = 0;        // CPU cycles, data-sheet costs

    // The last few instruction addresses, printed when a run ends in an error.
    static constexpr int TRACE_LEN = 24;
    uint32_t trace_pc[TRACE_LEN] = {};
    uint64_t trace_n = 0;

    explicit Cpu65816(Bus &b) : bus(b) {}

    void reset(uint16_t start_pc) {
        a = x = y = 0; sp = 0x01FF; dp = 0; dbr = pbr = 0; pc = start_pc;
        c = z = n = v = dec = irq = false; m = xf = e = true;
        instructions = 0; cycles = 0; trace_n = 0;
    }

    // --- memory ------------------------------------------------------------------
    uint8_t  rd8(uint32_t a_) { return bus.rd8(a_ & 0xFFFFFF); }
    void     wr8(uint32_t a_, uint8_t v_) { bus.wr8(a_ & 0xFFFFFF, v_); }
    uint16_t rd16(uint32_t a_) { return static_cast<uint16_t>(rd8(a_) | (rd8(a_ + 1) << 8)); }
    void     wr16(uint32_t a_, uint16_t v_) { wr8(a_, v_ & 0xFF); wr8(a_ + 1, v_ >> 8); }
    uint32_t rd24(uint32_t a_) { return rd16(a_) | (static_cast<uint32_t>(rd8(a_ + 2)) << 16); }

    bool m8() const { return e || m; }    // accumulator is 8 bits wide
    bool x8() const { return e || xf; }   // index registers are 8 bits wide

    // Cycle-cost modifiers: one extra cycle per extra byte moved, and one for a
    // direct page that is not page-aligned.
    int wA() const { return m8() ? 0 : 1; }
    int wX() const { return x8() ? 0 : 1; }
    int dl() const { return (dp & 0xFF) ? 1 : 0; }

    uint32_t pcaddr() const { return (static_cast<uint32_t>(pbr) << 16) | pc; }
    uint8_t  fetch8() { const uint8_t v_ = rd8(pcaddr()); ++pc; return v_; }
    uint16_t fetch16() { const uint8_t lo = fetch8(); const uint8_t hi = fetch8(); return static_cast<uint16_t>(lo | (hi << 8)); }
    uint32_t fetch24() { const uint16_t lo = fetch16(); const uint8_t hi = fetch8(); return lo | (static_cast<uint32_t>(hi) << 16); }
    uint16_t fetchA() { return m8() ? fetch8() : fetch16(); }
    uint16_t fetchX() { return x8() ? fetch8() : fetch16(); }

    // Width-aware register access.
    uint16_t getA() const { return m8() ? (a & 0xFF) : a; }
    void setA(uint16_t v_) { if (m8()) a = static_cast<uint16_t>((a & 0xFF00) | (v_ & 0xFF)); else a = v_; }
    uint16_t maskA() const { return m8() ? 0xFF : 0xFFFF; }
    uint16_t signA() const { return m8() ? 0x80 : 0x8000; }
    uint16_t maskX() const { return x8() ? 0xFF : 0xFFFF; }
    uint16_t signX() const { return x8() ? 0x80 : 0x8000; }
    void setX(uint16_t v_) { x = v_ & maskX(); }
    void setY(uint16_t v_) { y = v_ & maskX(); }

    void nz_w(uint16_t v_, uint16_t mask, uint16_t sign) { v_ &= mask; z = (v_ == 0); n = (v_ & sign) != 0; }
    void nzA(uint16_t v_) { nz_w(v_, maskA(), signA()); }
    void nzX(uint16_t v_) { nz_w(v_, maskX(), signX()); }

    uint16_t rdA(uint32_t ea) { return m8() ? rd8(ea) : rd16(ea); }
    void     wrA(uint32_t ea, uint16_t v_) { if (m8()) wr8(ea, v_ & 0xFF); else wr16(ea, v_); }
    uint16_t rdX(uint32_t ea) { return x8() ? rd8(ea) : rd16(ea); }
    void     wrX(uint32_t ea, uint16_t v_) { if (x8()) wr8(ea, v_ & 0xFF); else wr16(ea, v_); }

    // Stack, always bank 0. In emulation mode the pointer lives in page 1.
    void fix_sp() { if (e) sp = static_cast<uint16_t>(0x0100 | (sp & 0xFF)); }
    void push8(uint8_t v_) { wr8(sp, v_); --sp; fix_sp(); }
    uint8_t pop8() { ++sp; fix_sp(); return rd8(sp); }
    void push16(uint16_t v_) { push8(v_ >> 8); push8(v_ & 0xFF); }
    uint16_t pop16() { const uint8_t lo = pop8(); const uint8_t hi = pop8(); return static_cast<uint16_t>(lo | (hi << 8)); }

    // Processor status. Native: N V M X D I Z C. Emulation: bits 5 and 4 read as 1.
    uint8_t packP() const {
        return static_cast<uint8_t>((n << 7) | (v << 6) |
                                    ((e ? 1 : m) << 5) | ((e ? 1 : xf) << 4) |
                                    (dec << 3) | (irq << 2) | (z << 1) | (c ? 1 : 0));
    }
    void unpackP(uint8_t p) {
        n = p & 0x80; v = p & 0x40;
        if (!e) {
            m = p & 0x20;
            xf = p & 0x10;
            if (xf) { x &= 0xFF; y &= 0xFF; }   // narrowing clears the high bytes
        }
        dec = p & 0x08; irq = p & 0x04; z = p & 0x02; c = p & 0x01;
    }

    // --- effective addresses (24-bit) ----------------------------------------------
    static uint32_t bank(uint8_t b, uint16_t off) { return (static_cast<uint32_t>(b) << 16) | off; }
    uint32_t dbr_(uint16_t off) const { return bank(dbr, off); }

    uint32_t ea_dp()        { return static_cast<uint16_t>(dp + fetch8()); }
    uint32_t ea_dp_x()      { return static_cast<uint16_t>(dp + fetch8() + x); }
    uint32_t ea_dp_y()      { return static_cast<uint16_t>(dp + fetch8() + y); }
    uint32_t ea_dp_ind()    { return dbr_(rd16(static_cast<uint16_t>(dp + fetch8()))); }
    uint32_t ea_dp_x_ind()  { return dbr_(rd16(static_cast<uint16_t>(dp + fetch8() + x))); }
    uint32_t ea_dp_ind_y()  { return (dbr_(rd16(static_cast<uint16_t>(dp + fetch8()))) + y) & 0xFFFFFF; }
    uint32_t ea_dp_long()   { return rd24(static_cast<uint16_t>(dp + fetch8())); }
    uint32_t ea_dp_long_y() { return (rd24(static_cast<uint16_t>(dp + fetch8())) + y) & 0xFFFFFF; }
    uint32_t ea_abs()       { return dbr_(fetch16()); }
    uint32_t ea_abs_x()     { return (dbr_(fetch16()) + x) & 0xFFFFFF; }
    uint32_t ea_abs_y()     { return (dbr_(fetch16()) + y) & 0xFFFFFF; }
    uint32_t ea_long()      { return fetch24(); }
    uint32_t ea_long_x()    { return (fetch24() + x) & 0xFFFFFF; }
    uint32_t ea_sr()        { return static_cast<uint16_t>(sp + fetch8()); }
    uint32_t ea_sr_ind_y()  { return (dbr_(rd16(static_cast<uint16_t>(sp + fetch8()))) + y) & 0xFFFFFF; }

    // --- ALU helpers, width-generic --------------------------------------------------
    void do_adc(uint16_t mval) {
        if (dec) bus.die("decimal-mode ADC is not modelled", pcaddr());
        const uint32_t a_ = getA(), s = a_ + mval + (c ? 1u : 0u);
        v = (~(a_ ^ mval) & (a_ ^ s) & signA()) != 0;
        c = s > maskA();
        setA(static_cast<uint16_t>(s & maskA()));
        nzA(static_cast<uint16_t>(s));
    }

    void do_sbc(uint16_t mval) {
        if (dec) bus.die("decimal-mode SBC is not modelled", pcaddr());
        const uint32_t a_ = getA(), s = a_ - mval - (c ? 0u : 1u);
        v = ((a_ ^ mval) & (a_ ^ s) & signA()) != 0;
        c = !(s & (static_cast<uint32_t>(maskA()) + 1));
        setA(static_cast<uint16_t>(s & maskA()));
        nzA(static_cast<uint16_t>(s));
    }

    void do_cmp(uint16_t r, uint16_t mval, uint16_t mask, uint16_t sign) {
        r &= mask; mval &= mask;
        c = r >= mval;
        nz_w(static_cast<uint16_t>(r - mval), mask, sign);
    }

    // Shifts and rotates on an A-width value.
    uint16_t sh_asl(uint16_t v_) { c = (v_ & signA()) != 0; const uint16_t r = static_cast<uint16_t>((v_ << 1) & maskA()); nzA(r); return r; }
    uint16_t sh_lsr(uint16_t v_) { v_ &= maskA(); c = v_ & 1; const uint16_t r = v_ >> 1; nzA(r); return r; }
    uint16_t sh_rol(uint16_t v_) { const bool cin = c; c = (v_ & signA()) != 0; const uint16_t r = static_cast<uint16_t>(((v_ << 1) | (cin ? 1 : 0)) & maskA()); nzA(r); return r; }
    uint16_t sh_ror(uint16_t v_) { v_ &= maskA(); const bool cin = c; c = v_ & 1; const uint16_t r = static_cast<uint16_t>((v_ >> 1) | (cin ? signA() : 0)); nzA(r); return r; }
    uint16_t sh_inc(uint16_t v_) { const uint16_t r = static_cast<uint16_t>((v_ + 1) & maskA()); nzA(r); return r; }
    uint16_t sh_dec(uint16_t v_) { const uint16_t r = static_cast<uint16_t>((v_ - 1) & maskA()); nzA(r); return r; }

    template <uint16_t (Cpu65816::*F)(uint16_t)>
    void rmw(uint32_t ea) { wrA(ea, (this->*F)(rdA(ea))); }

    int branch(bool take) {
        const int8_t off = static_cast<int8_t>(fetch8());
        if (take) { pc = static_cast<uint16_t>(pc + off); return 3; }
        return 2;
    }

    // The eight "group 1" operations share fifteen addressing modes selected by the
    // low five opcode bits. Decoding them in one place covers 119 opcodes without 119
    // cases. Returns the addressing-mode cycle cost (data-sheet cost minus the 2-cycle
    // base), or -1 if the low bits are not a group-1 mode. `imm` is set for immediate.
    int group1_ea(uint8_t op, uint32_t &ea, bool &imm) {
        imm = false;
        switch (op & 0x1F) {
            case 0x01: ea = ea_dp_x_ind();  return 4 + dl();
            case 0x03: ea = ea_sr();        return 2;
            case 0x05: ea = ea_dp();        return 1 + dl();
            case 0x07: ea = ea_dp_long();   return 4 + dl();
            case 0x09: imm = true;          return 0;
            case 0x0D: ea = ea_abs();       return 2;
            case 0x0F: ea = ea_long();      return 3;
            case 0x11: ea = ea_dp_ind_y();  return 3 + dl();
            case 0x12: ea = ea_dp_ind();    return 3 + dl();
            case 0x13: ea = ea_sr_ind_y();  return 5;
            case 0x15: ea = ea_dp_x();      return 2 + dl();
            case 0x17: ea = ea_dp_long_y(); return 4 + dl();
            case 0x19: ea = ea_abs_y();     return 2;
            case 0x1D: ea = ea_abs_x();     return 2;
            case 0x1F: ea = ea_long_x();    return 3;
            default: return -1;
        }
    }

    void group1_op(uint8_t op, uint32_t ea, bool imm) {
        const uint8_t which = op >> 5;      // ORA AND EOR ADC STA LDA CMP SBC
        if (which == 4) {                   // STA
            wrA(ea, getA());
            return;
        }
        const uint16_t mval = imm ? fetchA() : rdA(ea);
        switch (which) {
            case 0: setA(getA() | mval); nzA(getA()); break;
            case 1: setA(getA() & mval); nzA(getA()); break;
            case 2: setA(getA() ^ mval); nzA(getA()); break;
            case 3: do_adc(mval); break;
            case 5: setA(mval); nzA(mval); break;
            case 6: do_cmp(getA(), mval, maskA(), signA()); break;
            case 7: do_sbc(mval); break;
            default: break;
        }
    }

    void step() {
        const uint32_t op_pc = pcaddr();
        if (bus.trap(op_pc)) return;

        trace_pc[trace_n++ % TRACE_LEN] = op_pc;
        const uint8_t op = fetch8();
        int cyc = 2;
        ++instructions;

        switch (op) {
        // --- loads and stores outside group 1 ---
        case 0xA2: setX(fetchX());              nzX(x); cyc += wX(); break;                // LDX #
        case 0xA6: setX(rdX(ea_dp()));          nzX(x); cyc += 1 + dl() + wX(); break;     // LDX dp
        case 0xB6: setX(rdX(ea_dp_y()));        nzX(x); cyc += 2 + dl() + wX(); break;     // LDX dp,Y
        case 0xAE: setX(rdX(ea_abs()));         nzX(x); cyc += 2 + wX(); break;            // LDX abs
        case 0xBE: setX(rdX(ea_abs_y()));       nzX(x); cyc += 2 + wX(); break;            // LDX abs,Y
        case 0xA0: setY(fetchX());              nzX(y); cyc += wX(); break;                // LDY #
        case 0xA4: setY(rdX(ea_dp()));          nzX(y); cyc += 1 + dl() + wX(); break;     // LDY dp
        case 0xB4: setY(rdX(ea_dp_x()));        nzX(y); cyc += 2 + dl() + wX(); break;     // LDY dp,X
        case 0xAC: setY(rdX(ea_abs()));         nzX(y); cyc += 2 + wX(); break;            // LDY abs
        case 0xBC: setY(rdX(ea_abs_x()));       nzX(y); cyc += 2 + wX(); break;            // LDY abs,X
        case 0x86: wrX(ea_dp(),    x); cyc += 1 + dl() + wX(); break;                      // STX dp
        case 0x96: wrX(ea_dp_y(),  x); cyc += 2 + dl() + wX(); break;                      // STX dp,Y
        case 0x8E: wrX(ea_abs(),   x); cyc += 2 + wX(); break;                             // STX abs
        case 0x84: wrX(ea_dp(),    y); cyc += 1 + dl() + wX(); break;                      // STY dp
        case 0x94: wrX(ea_dp_x(),  y); cyc += 2 + dl() + wX(); break;                      // STY dp,X
        case 0x8C: wrX(ea_abs(),   y); cyc += 2 + wX(); break;                             // STY abs
        case 0x64: wrA(ea_dp(),    0); cyc += 1 + dl() + wA(); break;                      // STZ dp
        case 0x74: wrA(ea_dp_x(),  0); cyc += 2 + dl() + wA(); break;                      // STZ dp,X
        case 0x9C: wrA(ea_abs(),   0); cyc += 2 + wA(); break;                             // STZ abs
        case 0x9E: wrA(ea_abs_x(), 0); cyc += 3 + wA(); break;                             // STZ abs,X

        // --- BIT ---
        case 0x89: { const uint16_t mval = fetchA(); z = ((getA() & mval) & maskA()) == 0; cyc += wA(); } break;  // BIT # (Z only)
        case 0x24: case 0x34: case 0x2C: case 0x3C: {
            const uint32_t ea = (op == 0x24) ? ea_dp() : (op == 0x34) ? ea_dp_x() : (op == 0x2C) ? ea_abs() : ea_abs_x();
            const uint16_t mval = rdA(ea);
            z = ((getA() & mval) & maskA()) == 0;
            n = (mval & signA()) != 0;
            v = (mval & (signA() >> 1)) != 0;
            cyc += ((op == 0x24) ? 1 + dl() : (op == 0x34) ? 2 + dl() : 2) + wA();
        } break;

        // --- TSB / TRB: test and set/reset bits (dp 5, abs 6; +2 when 16-bit) ---
        case 0x04: { const uint32_t ea = ea_dp();  const uint16_t mv = rdA(ea); z = ((getA() & mv) & maskA()) == 0; wrA(ea, (mv | getA()) & maskA());  cyc += 3 + dl() + 2 * wA(); } break;
        case 0x0C: { const uint32_t ea = ea_abs(); const uint16_t mv = rdA(ea); z = ((getA() & mv) & maskA()) == 0; wrA(ea, (mv | getA()) & maskA());  cyc += 4 + 2 * wA(); } break;
        case 0x14: { const uint32_t ea = ea_dp();  const uint16_t mv = rdA(ea); z = ((getA() & mv) & maskA()) == 0; wrA(ea, (mv & ~getA()) & maskA()); cyc += 3 + dl() + 2 * wA(); } break;
        case 0x1C: { const uint32_t ea = ea_abs(); const uint16_t mv = rdA(ea); z = ((getA() & mv) & maskA()) == 0; wrA(ea, (mv & ~getA()) & maskA()); cyc += 4 + 2 * wA(); } break;

        // --- read-modify-write on memory: dp 5, dp,X 6, abs 6, abs,X 7; +2 when 16-bit ---
        case 0x06: rmw<&Cpu65816::sh_asl>(ea_dp());    cyc += 3 + dl() + 2 * wA(); break;
        case 0x16: rmw<&Cpu65816::sh_asl>(ea_dp_x());  cyc += 4 + dl() + 2 * wA(); break;
        case 0x0E: rmw<&Cpu65816::sh_asl>(ea_abs());   cyc += 4 + 2 * wA(); break;
        case 0x1E: rmw<&Cpu65816::sh_asl>(ea_abs_x()); cyc += 5 + 2 * wA(); break;
        case 0x26: rmw<&Cpu65816::sh_rol>(ea_dp());    cyc += 3 + dl() + 2 * wA(); break;
        case 0x36: rmw<&Cpu65816::sh_rol>(ea_dp_x());  cyc += 4 + dl() + 2 * wA(); break;
        case 0x2E: rmw<&Cpu65816::sh_rol>(ea_abs());   cyc += 4 + 2 * wA(); break;
        case 0x3E: rmw<&Cpu65816::sh_rol>(ea_abs_x()); cyc += 5 + 2 * wA(); break;
        case 0x46: rmw<&Cpu65816::sh_lsr>(ea_dp());    cyc += 3 + dl() + 2 * wA(); break;
        case 0x56: rmw<&Cpu65816::sh_lsr>(ea_dp_x());  cyc += 4 + dl() + 2 * wA(); break;
        case 0x4E: rmw<&Cpu65816::sh_lsr>(ea_abs());   cyc += 4 + 2 * wA(); break;
        case 0x5E: rmw<&Cpu65816::sh_lsr>(ea_abs_x()); cyc += 5 + 2 * wA(); break;
        case 0x66: rmw<&Cpu65816::sh_ror>(ea_dp());    cyc += 3 + dl() + 2 * wA(); break;
        case 0x76: rmw<&Cpu65816::sh_ror>(ea_dp_x());  cyc += 4 + dl() + 2 * wA(); break;
        case 0x6E: rmw<&Cpu65816::sh_ror>(ea_abs());   cyc += 4 + 2 * wA(); break;
        case 0x7E: rmw<&Cpu65816::sh_ror>(ea_abs_x()); cyc += 5 + 2 * wA(); break;
        case 0xE6: rmw<&Cpu65816::sh_inc>(ea_dp());    cyc += 3 + dl() + 2 * wA(); break;
        case 0xF6: rmw<&Cpu65816::sh_inc>(ea_dp_x());  cyc += 4 + dl() + 2 * wA(); break;
        case 0xEE: rmw<&Cpu65816::sh_inc>(ea_abs());   cyc += 4 + 2 * wA(); break;
        case 0xFE: rmw<&Cpu65816::sh_inc>(ea_abs_x()); cyc += 5 + 2 * wA(); break;
        case 0xC6: rmw<&Cpu65816::sh_dec>(ea_dp());    cyc += 3 + dl() + 2 * wA(); break;
        case 0xD6: rmw<&Cpu65816::sh_dec>(ea_dp_x());  cyc += 4 + dl() + 2 * wA(); break;
        case 0xCE: rmw<&Cpu65816::sh_dec>(ea_abs());   cyc += 4 + 2 * wA(); break;
        case 0xDE: rmw<&Cpu65816::sh_dec>(ea_abs_x()); cyc += 5 + 2 * wA(); break;

        // --- accumulator shifts ---
        case 0x0A: setA(sh_asl(getA())); break;
        case 0x2A: setA(sh_rol(getA())); break;
        case 0x4A: setA(sh_lsr(getA())); break;
        case 0x6A: setA(sh_ror(getA())); break;
        case 0x1A: setA(sh_inc(getA())); break;                                       // INC A
        case 0x3A: setA(sh_dec(getA())); break;                                       // DEC A

        // --- index arithmetic and compares ---
        case 0xE8: setX(x + 1); nzX(x); break;                                        // INX
        case 0xCA: setX(x - 1); nzX(x); break;                                        // DEX
        case 0xC8: setY(y + 1); nzX(y); break;                                        // INY
        case 0x88: setY(y - 1); nzX(y); break;                                        // DEY
        case 0xE0: do_cmp(x, fetchX(),        maskX(), signX()); cyc += wX(); break;             // CPX #
        case 0xE4: do_cmp(x, rdX(ea_dp()),    maskX(), signX()); cyc += 1 + dl() + wX(); break;  // CPX dp
        case 0xEC: do_cmp(x, rdX(ea_abs()),   maskX(), signX()); cyc += 2 + wX(); break;         // CPX abs
        case 0xC0: do_cmp(y, fetchX(),        maskX(), signX()); cyc += wX(); break;             // CPY #
        case 0xC4: do_cmp(y, rdX(ea_dp()),    maskX(), signX()); cyc += 1 + dl() + wX(); break;  // CPY dp
        case 0xCC: do_cmp(y, rdX(ea_abs()),   maskX(), signX()); cyc += 2 + wX(); break;         // CPY abs

        // --- transfers ---
        case 0xAA: setX(getA());  nzX(x); break;                                      // TAX
        case 0xA8: setY(getA());  nzX(y); break;                                      // TAY
        case 0x8A: setA(x);       nzA(getA()); break;                                 // TXA
        case 0x98: setA(y);       nzA(getA()); break;                                 // TYA
        case 0x9B: setY(x);       nzX(y); break;                                      // TXY
        case 0xBB: setX(y);       nzX(x); break;                                      // TYX
        case 0xBA: setX(sp);      nzX(x); break;                                      // TSX
        case 0x9A: sp = x8() ? static_cast<uint16_t>(x & 0xFF) : x; fix_sp(); break;  // TXS
        case 0x5B: dp = a; nz_w(a, 0xFFFF, 0x8000); break;                            // TCD (always 16)
        case 0x7B: a  = dp; nz_w(a, 0xFFFF, 0x8000); break;                           // TDC
        case 0x1B: sp = a; fix_sp(); break;                                           // TCS
        case 0x3B: a  = sp; nz_w(a, 0xFFFF, 0x8000); break;                           // TSC
        case 0xEB: a  = static_cast<uint16_t>((a << 8) | (a >> 8));                   // XBA
                   nz_w(a, 0xFF, 0x80); cyc = 3; break;

        // --- stack ---
        case 0x48: if (m8()) push8(getA() & 0xFF); else push16(a); cyc += 1 + wA(); break;               // PHA
        case 0x68: { const uint16_t v_ = m8() ? pop8() : pop16(); setA(v_); nzA(v_); cyc += 2 + wA(); } break;  // PLA
        case 0xDA: if (x8()) push8(x & 0xFF); else push16(x); cyc += 1 + wX(); break;                    // PHX
        case 0xFA: { const uint16_t v_ = x8() ? pop8() : pop16(); setX(v_); nzX(v_); cyc += 2 + wX(); } break;  // PLX
        case 0x5A: if (x8()) push8(y & 0xFF); else push16(y); cyc += 1 + wX(); break;                    // PHY
        case 0x7A: { const uint16_t v_ = x8() ? pop8() : pop16(); setY(v_); nzX(v_); cyc += 2 + wX(); } break;  // PLY
        case 0x08: push8(packP()); cyc += 1; break;                                   // PHP
        case 0x28: unpackP(pop8()); cyc += 2; break;                                  // PLP
        case 0x8B: push8(dbr); cyc += 1; break;                                       // PHB
        case 0xAB: dbr = pop8(); nz_w(dbr, 0xFF, 0x80); cyc += 2; break;              // PLB
        case 0x0B: push16(dp); cyc += 2; break;                                       // PHD
        case 0x2B: dp = pop16(); nz_w(dp, 0xFFFF, 0x8000); cyc += 3; break;           // PLD
        case 0x4B: push8(pbr); cyc += 1; break;                                       // PHK
        case 0xF4: push16(fetch16()); cyc = 5; break;                                 // PEA
        case 0xD4: push16(rd16(static_cast<uint16_t>(dp + fetch8()))); cyc = 6 + dl(); break;  // PEI
        case 0x62: { const int16_t rel = static_cast<int16_t>(fetch16());             // PER
                     push16(static_cast<uint16_t>(pc + rel)); cyc = 6; } break;

        // --- flags ---
        case 0x18: c = false; break;                                                  // CLC
        case 0x38: c = true;  break;                                                  // SEC
        case 0x58: irq = false; break;                                                // CLI
        case 0x78: irq = true;  break;                                                // SEI
        case 0xD8: dec = false; break;                                                // CLD
        case 0xF8: dec = true;  break;                                                // SED
        case 0xB8: v = false; break;                                                  // CLV
        case 0xC2: unpackP(static_cast<uint8_t>(packP() & ~fetch8())); cyc = 3; break;// REP #
        case 0xE2: unpackP(static_cast<uint8_t>(packP() |  fetch8())); cyc = 3; break;// SEP #
        case 0xFB: {                                                                  // XCE
            const bool old_e = e;
            e = c;
            c = old_e;
            // Either direction lands in 8-bit widths; entering emulation also narrows
            // the index registers and parks the stack in page 1.
            m = true; xf = true;
            if (e) { x &= 0xFF; y &= 0xFF; fix_sp(); }
        } break;

        // --- branches ---
        case 0x90: cyc = branch(!c); break;  case 0xB0: cyc = branch(c);  break;
        case 0xF0: cyc = branch(z);  break;  case 0xD0: cyc = branch(!z); break;
        case 0x10: cyc = branch(!n); break;  case 0x30: cyc = branch(n);  break;
        case 0x50: cyc = branch(!v); break;  case 0x70: cyc = branch(v);  break;
        case 0x80: cyc = branch(true);   break;                                       // BRA
        case 0x82: { const int16_t rel = static_cast<int16_t>(fetch16());             // BRL
                     pc = static_cast<uint16_t>(pc + rel); cyc = 4; } break;

        // --- jumps and calls ---
        case 0x4C: pc = fetch16(); cyc = 3; break;                                    // JMP abs
        case 0x6C: pc = rd16(fetch16()); cyc = 5; break;                              // JMP (abs)   bank 0 pointer
        case 0x7C: pc = rd16(bank(pbr, static_cast<uint16_t>(fetch16() + x))); cyc = 6; break; // JMP (abs,X)
        case 0x5C: { const uint32_t t = fetch24(); pbr = t >> 16; pc = t & 0xFFFF; cyc = 4; } break; // JML long
        case 0xDC: { const uint32_t t = rd24(fetch16()); pbr = t >> 16; pc = t & 0xFFFF; cyc = 6; } break; // JML [abs]
        case 0x20: { const uint16_t t = fetch16(); push16(static_cast<uint16_t>(pc - 1)); pc = t; cyc = 6; } break; // JSR abs
        case 0xFC: { const uint16_t t = rd16(bank(pbr, static_cast<uint16_t>(fetch16() + x)));   // JSR (abs,X)
                     push16(static_cast<uint16_t>(pc - 1)); pc = t; cyc = 8; } break;
        case 0x22: { const uint32_t t = fetch24();                                    // JSL long
                     push8(pbr); push16(static_cast<uint16_t>(pc - 1));
                     pbr = t >> 16; pc = t & 0xFFFF; cyc = 8; } break;
        case 0x60: pc = static_cast<uint16_t>(pop16() + 1); cyc = 6; break;          // RTS
        case 0x6B: pc = static_cast<uint16_t>(pop16() + 1); pbr = pop8(); cyc = 6; break; // RTL

        // --- block moves: MVN increments, MVP decrements; A+1 bytes; A ends at $FFFF ---
        case 0x54: case 0x44: {
            const uint8_t dst = fetch8(), src = fetch8();   // encoded destination first
            dbr = dst;
            const int dir = (op == 0x54) ? 1 : -1;
            // With 16-bit index registers this copies up to 64K; with 8-bit ones the
            // pointers wrap within 256 bytes, as on the real chip.
            for (;;) {
                wr8(bank(dst, y), rd8(bank(src, x)));
                setX(static_cast<uint16_t>(x + dir));
                setY(static_cast<uint16_t>(y + dir));
                cycles += 7;
                if (a == 0) { a = 0xFFFF; break; }
                --a;
            }
            cyc = 0;
        } break;

        case 0xEA: break;                                                             // NOP

        // --- explicitly refused: none of this is modelled, and vouching for a program
        // that reaches it would be dishonest ---
        case 0x00: bus.die("BRK (no interrupt model)", op_pc);
        case 0x02: bus.die("COP (no interrupt model)", op_pc);
        case 0x40: bus.die("RTI (no interrupt model)", op_pc);
        case 0x42: bus.die("WDM", op_pc);
        case 0xCB: bus.die("WAI (no interrupt model)", op_pc);
        case 0xDB: bus.die("STP", op_pc);

        default: {
            uint32_t ea = 0; bool imm = false;
            const int mode_cost = group1_ea(op, ea, imm);
            if (mode_cost >= 0) {
                group1_op(op, ea, imm);
                cyc += mode_cost + wA();
            } else {
                std::fprintf(stderr,
                    "65816: unimplemented opcode $%02X at $%06X after %llu instructions.\n"
                    "Add it rather than ignoring it -- a skipped instruction yields a\n"
                    "wrong-but-plausible result.\n",
                    op, op_pc, static_cast<unsigned long long>(instructions));
                bus.die("unimplemented opcode", op_pc);
            }
        }
        }

        cycles += cyc;
        bus.cycles(cyc);
    }

    void print_trace(std::FILE *f) const {
        std::fprintf(f, "  last PCs:");
        for (uint64_t i = (trace_n > TRACE_LEN ? trace_n - TRACE_LEN : 0); i < trace_n; ++i)
            std::fprintf(f, " %06X", trace_pc[i % TRACE_LEN]);
        std::fprintf(f, "\n");
    }
};
