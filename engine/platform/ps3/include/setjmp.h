/*
 * setjmp.h -- PS3 shim header for correct PPC64 jmp_buf sizing.
 *
 * PSL1GHT/devkitPro's prebuilt libc.a ships a setjmp() compiled WITH
 * __ALTIVEC__ that writes 448 bytes into jmp_buf, but the stock
 * machine/setjmp.h defines jmp_buf as double[32] (256 bytes) when
 * __ALTIVEC__ is not defined project-wide (this project stays
 * -mno-altivec globally, like every other PS3 homebrew here) -- a
 * 192-byte .bss/stack overflow on every single setjmp() call. Confirmed
 * on real hardware in this project (AGENTS.md goal-stack #3: FI.GameInfo
 * observed clobbered with unrelated string bytes immediately after
 * Host_Main's setjmp/Host_InitCommon's error-recovery setjmp ran) and
 * previously root-caused and fixed the same way in the sibling
 * ioQuake3-PS3 port on the identical toolchain (see
 * E:\...\quake3\Ioquake3-PS3\ioQuake3-PS3\code\sys\ps3_setjmp.S).
 *
 * ps3_setjmp.S (linked ahead of libc.a, see xcompile.py's PS3 class)
 * replaces setjmp/longjmp with a correct 64-bit std/ld implementation
 * (the stock one also truncates 64-bit GPRs via 32-bit stw/lwz -- a
 * second, independent bug that would corrupt registers across any real
 * longjmp(), not just overflow at setjmp() time) writing 328 bytes. We
 * define jmp_buf as double[64] (512 bytes) to fit it with margin.
 *
 * This header is found first via -I ordering in xcompile.py's PS3
 * cflags() (this directory is added before the toolchain's own
 * ppu/include) -- don't reorder that.
 */
#ifndef PS3_SETJMP_H
#define PS3_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* PPC64 jmp_buf: 512 bytes (double[64]) -- large enough for our
 * replacement setjmp which writes 328 bytes */
typedef double jmp_buf[64];

extern void longjmp(jmp_buf, int);
extern int  setjmp(jmp_buf);

#ifdef __cplusplus
}
#endif

#endif /* PS3_SETJMP_H */
