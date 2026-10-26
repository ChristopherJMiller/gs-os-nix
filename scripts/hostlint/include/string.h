/* the four string.h routines the VM links (ORCALib), for host parsing only */
#ifndef SHIM_STRING_H
#define SHIM_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);
#endif
