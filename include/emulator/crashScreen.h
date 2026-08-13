#ifndef _CRASH_SCREEN_H
#define _CRASH_SCREEN_H

#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

//! 1 = __OSUnhandledException shows the visual crash debugger instead of calling PPCHalt (OSError.c).
//! Left on where the other diagnostics are off, because it executes only on a build that has already
//! crashed and it is the only report available without a hardware debugger. Set to 0 for retail
//! behaviour -- a halt, i.e. a black screen.
#define COMBO_CRASH_SCREEN 1

/**
 * @brief 8x8 glyphs for printable ASCII, indexed by `ch - 0x20`. Row-major, one byte per row, bit N
 * of a row being the pixel at x = N. Lives in .crashdata; shared with comboPerf.c's on-screen counter.
 */
extern const u8 gCrashFont[0x5F][8];

/**
 * @brief Visual crash debugger. Builds a paginated report of the crashed Wii CPU state and the N64 CPU state.
 */
void CrashScreenShow(void);

/**
 * @brief Re-entrancy counter for __OSUnhandledException (see OSError.c)* : returns 0 on the first exception, 
 * 1 if another exception arrives before that one 
 * finished, 2+ if CrashScreenShow itself is what's faulting.
 */
s32 CrashScreenEnter(void);

#ifdef __cplusplus
}
#endif

#endif
