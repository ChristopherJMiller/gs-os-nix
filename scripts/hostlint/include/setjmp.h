/* setjmp.h for host parsing only. ORCA/C's jmp_buf is an array too. */
#ifndef SHIM_SETJMP_H
#define SHIM_SETJMP_H
typedef unsigned char jmp_buf[16];
int  setjmp(jmp_buf);
void longjmp(jmp_buf, int) __attribute__((noreturn));
#endif
