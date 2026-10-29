/* repl.c -- M3 (docs/COMPILER.md "Stage 4"): a prompt, a line typed on
 * standard input, compiled with no file -- no directory, no positions --
 * then run and printed by the VM, and the next, until the input ends.
 *
 * Each line is a run of its own (vm_once), the VM's state set up afresh.
 * What earlier lines left on the card is garbage, which the next mature
 * collection takes: nothing roots it. The window cache stays valid across
 * lines -- card memory is reused only by a compaction, which turns the ring
 * over itself -- but is dropped before each line all the same, so that each
 * starts with the whole ring. Without FRONT_REPL this is one run, as before.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"
#include "front.h"

segment "compile";

#define PROMPT      "nix> "
#define LINE_MAX    0xFF00U                 /* the source bank, under SRC_NAME */

struct gs_std { uint16_t pcount, prefix, ref; };
struct gs_io  { uint16_t pcount, ref; void *buf; uint32_t req, got; };

/* GetStdRefNum: 10 standard input, 11 output */
static uint16_t std_ref(uint16_t prefix)
{
    struct gs_std s;
    s.pcount = 2;
    s.prefix = prefix;
    s.ref = 0;
    gsos_call(0x2037, &s);
    return s.ref;
}

static uint16_t rw(uint16_t num, uint16_t ref, void *buf, uint16_t n, uint32_t *got)
{
    struct gs_io io;
    uint16_t err;
    io.pcount = 4;
    io.ref = ref;
    io.buf = buf;
    io.req = n;
    io.got = 0;
    err = gsos_call(num, &io);
    *got = io.got;
    return err;
}

void front_entry(void)
{
    uint8_t *src = (uint8_t *)SRC_BASE, c;
    uint16_t in, out, n;
    uint32_t got;
    int more = 1;
    if (*(volatile uint8_t *)FRONT_MODE_ADDR != FRONT_REPL) {
        vm_once();
        return;
    }
    in = std_ref(10);
    out = std_ref(11);
    *(uint8_t *)SRC_NAME = 0;                   /* a line has no file */
    while (more) {
        rw(0x2013, out, (void *)PROMPT, sizeof(PROMPT) - 1, &got);     /* Write */
        n = 0;
        for (;;) {                              /* Read, a byte at a time, to a CR or LF */
            if (rw(0x2012, in, &c, 1, &got) || got == 0) { more = 0; break; }
            if (c == '\r' || c == '\n') break;
            if (n < LINE_MAX) src[n] = c;
            if (n <= LINE_MAX) ++n;             /* past it: too long, front_run says so */
        }
        if (n == 0) continue;                   /* an empty line, or the input's end */
        REPL_LEN = n;
        windows_turnover();
        vm_once();
    }
}
