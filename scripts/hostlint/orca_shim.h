/* orca_shim.h -- force-included (-include) when a host compiler parses the VM.
 * The ORCA/C pragmas are left for -Wno-unknown-pragmas; `segment` and asm{}
 * are rewritten by orca2host.py. */
#ifndef ORCA_SHIM_H
#define ORCA_SHIM_H
/* An asm{} block, as far as the host can tell: it may read or write every C
 * object it names. */
void orca_asm(int, ...);
#endif
