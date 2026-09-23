/* smoke.c -- the first C on a2min: compiled by ORCA/C under orcarun, linked by
 * the ORCA linker, flattened by omf2bin (docs/TOOLCHAIN.md, D18).
 *
 * No ORCALib start-up and no OS: `noroot` keeps the compiler from emitting the
 * root segment that would call ~_BWStartUp, the entry function is first in the
 * file so it is the load file's entry point, and it returns with RTL to
 * omf2bin's stub. Answers land where a2min --run-bin reads them: the result area
 * at $0300 and the status byte at $0320. checks.orcac-smoke asserts the bytes.
 */
#pragma noroot
#pragma optimize 1

static int add(int a, int b);
static long mul32(long a, long b);

void entry(void)
{
    *(volatile int *)0x0300 = add(40, 2);              /* 42       = $002A     */
    *(volatile long *)0x0302 = mul32(12345L, 6789L);   /* 83810205 = $04FED79D */
    *(volatile unsigned char *)0x0320 = 1;
}

static int add(int a, int b)
{
    return a + b;
}

static long mul32(long a, long b)
{
    return a * b;
}
