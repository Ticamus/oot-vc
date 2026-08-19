/**
 * @file comboPerf.c
 *
 * Work counters for the emulator's hot paths, on screen and/or in the log. See
 * include/emulator/comboPerf.h for what each counter answers.
 *
 * Everything here lives in the private .crashtext/.crashdata sections crashScreen.c already appends
 * after .sbss2, so no retail section address moves. Nothing may add to the DOL's own .bss -- that
 * once presented as corrupted save data -- so every object below is initialised, hence the `= {1}`
 * sentinels.
 */
#include "emulator/comboPerf.h"
#include "emulator/controller.h"
#include "emulator/cpu.h"
#include "emulator/crashScreen.h"
#include "emulator/soundRVL.h"
#include "emulator/system.h"
#include "emulator/vc64_RVL.h"
#include "emulator/xlCoreRVL.h"
#include "macros.h"
#include "revolution/os.h"
#include "revolution/vi.h"
#include "stdio.h"

#if IS_MM && COMBO_PERF_COUNTERS

//! Only suppressible when the chord can turn it back on.
#if COMBO_PERF_BUTTONS
#define PERF_LOG_ON sLogOn
#else
#define PERF_LOG_ON true
#endif

#pragma section ".crashtext"
#pragma section ".crashdata" ".crashbss"

#define CT DECL_SECTION(".crashtext")
#define CD DECL_SECTION(".crashdata")

//! `1` costs one count in the first period, which is never printed -- see sStamp in comboPerfFrame.
CD ComboPerf gComboPerf = {1};

//! OSGetTime() at the start of the current log period, or 1 before the first period opens.
CD static s64 sStamp = 1;

//! Separate frame count/stamp for the shorter overlay period, so it can differ in length from the log
//! period without disturbing either average.
CD static s32 sDrawFrames = 1;
CD static s64 sDrawStamp = 1;

#if COMBO_PERF_REPORT
CD static const char kFmtRate[] = "perf: %d frames / %d ms = %d.%02d fps | %d boundaries/frame | "
                                 "tree %d kills %d\n";
CD static const char kFmtFind[] = "perf:   find %d calls / %d insn (%d insn/call) retry %d jt %d\n";
CD static const char kFmtCombo[] =
    "perf:   repair %d (mm %d) shadow %d | hostjmp %d slow %d nodes %d | orphan %d\n";
//! The RSP task-lifecycle watchdogs. Printed every period even when zero, unlike kFmtRom: a zero here
//! is the result being looked for, so its absence must not read as "not measured".
CD static const char kFmtTask[] = "perf:   orphan blocked %d | retire-no-sp %d | shadow-used %d\n";
CD static const char kFmtRom[] = "perf:   rom %d blocks (%d/frame) lru %d entries (%d/block)\n";
//! Audio backend health. Printed every period even when clean, like kFmtTask and unlike kFmtRom: a
//! zero here is the result being looked for, so its absence must not read as "not measured".
//! `depth` is the mean ready-queue depth against nDepthTarget; `drained`/`starve`/`poolfull` are
//! counts out of `of` samples taken at 60 Hz. See ComboPerf's nSnd* fields for what each one means.
CD static const char kFmtSnd[] =
    "perf:   snd depth %d.%02d/%d | drained %d starve %d poolfull %d feeds %d (of %d)\n";
//! Printed beside kFmtSnd because it is the candidate explanation for it: a skipped retrace is an
//! audio buffer the guest never generates. Every period, zero included.
CD static const char kFmtRetrace[] = "perf:   retrace host %d guest %d lost %d (%d%%)\n";
//! The decisive one. `need` is nFrequency * 4 B/s, what the AI drains; `fed` is what the guest
//! actually supplied. Below ~95% the guest is structurally underproducing and no amount of host
//! buffering can cover it -- the queue will empty and stay empty.
CD static const char kFmtSndRate[] = "perf:   snd fed %d B/s need %d B/s (%d%%) freq %d\n";
//! The guest cart-DMA path. `inval` is the share of wall clock spent inside fn_8000A504, i.e. the
//! tree/frame/RSP invalidation every DMA completion performs -- see ComboPerf::nDmaTicks. `gate` is
//! how often the retrace gate refused to advance a copy. Printed every period, zero included.
CD static const char kFmtDma[] = "perf:   dma %d (%d KB) inval %d ms (%d%% of wall) gate %d\n";
//! The audio HLE, timed from inside MM's RSP audio thread. `tasks/s` against 60 and `%% of wall`
//! near 100 are the two readings that matter -- see ComboPerf::nAbiTicks. `B/task` is the work the
//! guest asked for, which is what separates "more audio" from "slower audio" across two ROMs.
CD static const char kFmtAbi[] =
    "perf:   abi %d tasks (%d/s) %d ms (%d%% of wall, %d us/task) %d B/task\n";
//! The graphics display-list parser. With this line the frame budget finally closes: gbi + abi +
//! inval + the remainder is the whole of the wall clock. `calls` are 0x400-command time slices,
//! `tasks` are display lists set up -- the two differ because MM slices on an alarm, not on the
//! count. `B/task` is the work asked for, read like the audio line's.
CD static const char kFmtGbi[] =
    "perf:   gbi %d tasks (%d/frame) %d ms (%d%% of wall, %d us/task) %d B/task | %d slices\n";
CD static const char kFmtSplit[] = "perf:   boundaries/frame jump %d idle %d other %d\n";
#endif

#if COMBO_PERF_OVERLAY

//! Two font pixels per screen pixel, as the crash screen draws: an 8x8 glyph at 1x is unreadable on
//! most displays.
#define PERF_SCALE 2
#define PERF_CHAR_W (8 * PERF_SCALE)
#define PERF_LINE_H (8 * PERF_SCALE + 2)
#define PERF_COLS 30
#define PERF_ROWS 4
#define PERF_ORIGIN_X 24
#define PERF_ORIGIN_Y 24
#define PERF_PAD 4

//! Comfortably past PERF_COLS. sprintf writes here unbounded, and this sits in .crashdata next to the
//! counters, so an overflow would corrupt the displayed numbers.
#define PERF_LINE_MAX 48

//! YUYV, what the external framebuffer holds: Y high byte, neutral 0x80 chroma low byte. 0xFF is
//! white, 0x10 near-black.
#define PERF_FG 0xFF80
#define PERF_BG 0x1080

//! Sanity filter on the emulator's own buffer pointers. MEM2 matters here: the XFBs live around
//! 0x90000000 (xlCoreInitRVL's heap-1 derivation), not in MEM1 with the code heaps -- omitting it is
//! why the first version of this overlay drew nothing.
#define PERF_MEM1_LO 0x80000000
#define PERF_MEM1_HI 0x81800000
#define PERF_MEM2_LO 0x90000000
#define PERF_MEM2_HI 0x94000000

//! The emulator's two external framebuffers (lbl_8017B1E0[0]/[1].unk_04, alternated per
//! lbl_80200654/lbl_801FF7DC).
#define PERF_FB_COUNT 2

//! The overlay text, rebuilt on the main thread every COMBO_PERF_DRAW_PERIOD frames and blitted from
//! the retrace interrupt. Sentinel-initialised (a space, not "") so the block is PROGBITS and nothing
//! draws before the first period closes.
CD static char sLine[PERF_ROWS][PERF_LINE_MAX] = {" ", " ", " ", " "};

//! Page 0: throughput (fps, work/frame, cache pressure). Page 1: the boundary scanner and combo fixes.
#define PERF_PAGES 2

CD static const char kFmtP0L0[] = "FPS %d.%02d";
CD static const char kFmtP0L1[] = "bnd/f %d tree %dk";
CD static const char kFmtP0L2[] = "kill %d rep %d hj %d";

CD static const char kFmtP1L0[] = "find %d / %d insn";
//! orph is dispatched/blocked -- the second number is the Sched_HandleRDPDone watchdog, on screen so a
//! session can be judged without the log.
CD static const char kFmtP1L1[] = "retry %d jt %d orph %d/%d";
//! ROM block cache rather than repair/mm/shadow: those are correctness watchdogs already in the
//! OSReport, while blocks-per-frame separates "paging from NAND" from CPU-bound slowdowns. Stays 0
//! for any image that fits nSizeCacheRAM.
CD static const char kFmtP1L2[] = "rom %d/f lru %d";

//! Fourth row, every page: the raw button word the chord is matched against, so a controller problem
//! and a log problem don't look identical from the sofa. Drop COMBO_PERF_SHOW_BTN to 0 once the chord
//! is known good.
#define COMBO_PERF_SHOW_BTN 0
CD static const char kFmtBtnRow[] = "btn %08X";
CD static s32 sBtnWord = -1;
#define PERF_PUBLISH_BTN(n) (sBtnWord = (n))

//! Runtime state the chord drives. sPage is never reset, so a page selected once stays selected.
//!
//! Initialised to PERF_PAGES, not 0: a zero initializer here lands in NOBITS .crashbss, which grows
//! `_bss_init_info` in .init and shifts the whole image (`PERF_PAGES % PERF_PAGES == 0` keeps the
//! boot page the overview).
//! Two non-zero states rather than a bool, for the same reason sPage starts at PERF_PAGES: a zero
//! initializer lands in NOBITS .crashbss, and `bool sShow = false` did exactly that -- one four-byte
//! section is enough to add an entry to _bss_init_info in .init and shift the whole image. Encoded so
//! it stays PROGBITS whichever way COMBO_PERF_OVERLAY_DEFAULT is set.
#define PERF_SHOW_OFF 1
#define PERF_SHOW_ON 2
CD static s32 sShow = COMBO_PERF_OVERLAY_DEFAULT ? PERF_SHOW_ON : PERF_SHOW_OFF;
#define PERF_SHOWN (sShow == PERF_SHOW_ON)

CD static s32 sPage = PERF_PAGES;
#define PERF_PAGE (sPage % PERF_PAGES)

//! The callback this one chains in front of (cpu.c's __cpuRetraceCallback, which must be preserved).
//! -1 sentinel rather than NULL so it lands in .crashdata and keeps the install one-shot.
CD static VIRetraceCallback sPrevRetrace = (VIRetraceCallback)-1;

//! Stamps performed, and a small self-diagnosis budget: reports whether the counter is running, the
//! callback installed, the video mode, and the framebuffer addresses -- this is what would have caught
//! the MEM2-vs-MEM1 bug in the first overlay version immediately.
CD static s32 sDrawn = 1;
CD static s32 sDiagLeft = 3;
CD static const char kFmtDiag[] = "perf: overlay %dx%d cb %08X fb %08X/%08X drawn %d\n";

CT static bool comboPerfIsRam(u32 nAddress) {
    return (nAddress >= PERF_MEM1_LO && nAddress < PERF_MEM1_HI) ||
           (nAddress >= PERF_MEM2_LO && nAddress < PERF_MEM2_HI);
}

//! Blit one glyph, PERF_SCALE-scaled. x stays even in every caller so each pair lands on a proper YUYV
//! [Y0 U][Y1 V] boundary.
CT static void comboPerfChar(u16* pFB, s32 nStride, s32 x, s32 y, char ch) {
    const u8* pGlyph;
    s32 iRow;
    s32 iCol;
    s32 iX;
    s32 iY;

    if ((u8)ch < 0x20 || (u8)ch > 0x7E) {
        return;
    }
    pGlyph = gCrashFont[(u8)ch - 0x20];

    for (iRow = 0; iRow < 8; iRow++) {
        u8 nBits = pGlyph[iRow];

        for (iCol = 0; iCol < 8; iCol++) {
            if (nBits & (1 << iCol)) {
                for (iY = 0; iY < PERF_SCALE; iY++) {
                    u16* p = pFB + (y + iRow * PERF_SCALE + iY) * nStride + (x + iCol * PERF_SCALE);

                    for (iX = 0; iX < PERF_SCALE; iX++) {
                        p[iX] = PERF_FG;
                    }
                }
            }
        }
    }
}

CT static void comboPerfFillRect(u16* pFB, s32 nStride, s32 x, s32 y, s32 w, s32 h, u16 nColor) {
    s32 iX;
    s32 iY;

    for (iY = 0; iY < h; iY++) {
        u16* p = pFB + (y + iY) * nStride + x;

        for (iX = 0; iX < w; iX++) {
            p[iX] = nColor;
        }
    }
}

//! Stamp the overlay into one external framebuffer.
CT static void comboPerfStamp(u16* pFB, s32 nStride) {
    s32 iLine;

    for (iLine = 0; iLine < PERF_ROWS; iLine++) {
        s32 y = PERF_ORIGIN_Y + iLine * PERF_LINE_H;
        s32 x = PERF_ORIGIN_X;
        const char* pText = sLine[iLine];
        s32 nLen = 0;

        while (pText[nLen] != '\0' && nLen < PERF_COLS) {
            nLen++;
        }

        //! Only as wide as the text: this runs inside the retrace interrupt, and a full-width fill
        //! would be forty kilobytes of stores a field.
        comboPerfFillRect(pFB, nStride, x - PERF_PAD, y - 1, nLen * PERF_CHAR_W + 2 * PERF_PAD,
                          PERF_LINE_H, PERF_BG);

        while (nLen-- > 0) {
            comboPerfChar(pFB, nStride, x, y, *pText++);
            x += PERF_CHAR_W;
        }
    }

    //! VI reads the framebuffer straight out of RAM, so the stores must leave the data cache before
    //! the field starts. Whole rows in one call: DCStoreRange works in cache lines anyway, so a
    //! partial row flushes neighbouring pixels regardless.
    DCStoreRange(pFB + (PERF_ORIGIN_Y - 1) * nStride,
                 PERF_ROWS * PERF_LINE_H * nStride * (s32)sizeof(u16));
}

//! Snapshot of MM's audio backend, five loads. Runs in the VI interrupt, so it must stay this short
//! and must not take a lock; torn reads across the five fields are irrelevant to a statistic.
//!
//! The offsets come from include/emulator/soundRVL.h's IS_MM branch, which is the retail layout read
//! out of the disassembly -- soundRVL.c is not linked for mm-j, so these are the only names for it.
//! Previous Sound::pSrcData, for the nSndFeeds edge count. -1 rather than NULL so it lands in
//! .crashdata: a zero initialiser would create .crashbss and with it a _bss_init_info entry, which
//! grows .init and shifts the whole image.
CD static void* sSndLastSrc = (void*)-1;

CT static void comboPerfSampleSound(void) {
    Sound* pSound;

    if (gpSystem == NULL) {
        return;
    }

    //! apObject[] is NULLed by systemCreateStorageDevice, so this is a real test and not a
    //! garbage-pointer read. In practice the callback is not installed until ~30 guest frames in,
    //! long after SOT_AUDIO exists; the test is here because the callback outlives any teardown.
    pSound = SYSTEM_SOUND(gpSystem);
    if (pSound == NULL) {
        return;
    }

    gComboPerf.nSndSamples++;
    gComboPerf.nSndDepth += pSound->nReadyCount;

    if (pSound->nReadyCount == 0) {
        gComboPerf.nSndDrained++;
    }
    if (pSound->eMode == SOUND_MM_MODE_STARVE) {
        gComboPerf.nSndStarve++;
    }
    if (pSound->nFreeCount == 0) {
        gComboPerf.nSndPoolFull++;
    }
    if (pSound->pSrcData != sSndLastSrc) {
        sSndLastSrc = pSound->pSrcData;
        gComboPerf.nSndFeeds++;
        gComboPerf.nSndBytes += pSound->nSndLen;
    }
}

//! Previous Cpu::nRetrace / Cpu::nRetraceUsed, for the advance deltas. -1 marks "no previous sample":
//! the first one after a reset only seeds them, so a period boundary cannot invent a huge delta.
CD static s32 sRetraceHostPrev = -1;
CD static s32 sRetraceGuestPrev = -1;

//! Counts how many VI fields the guest was actually given against how many the host produced. Both
//! are plain counters that only ever rise, so the difference of their advances is exactly what the
//! nDelta >= 4 snap in cpuExecuteUpdate threw away.
CT static void comboPerfSampleRetrace(void) {
    Cpu* pCPU;
    s32 nHost;
    s32 nGuest;

    if (gpSystem == NULL || (pCPU = SYSTEM_CPU(gpSystem)) == NULL) {
        return;
    }

    nHost = (s32)pCPU->nRetrace;
    nGuest = (s32)pCPU->nRetraceUsed;

    //! Skip the seeding sample, and any sample where either counter went backwards -- the OoT<->MM
    //! switch resets the CPU, and a negative delta would poison the whole period.
    if (sRetraceHostPrev >= 0 && nHost >= sRetraceHostPrev && nGuest >= sRetraceGuestPrev) {
        gComboPerf.nRetraceHost += nHost - sRetraceHostPrev;
        gComboPerf.nRetraceGuest += nGuest - sRetraceGuestPrev;
    }

    sRetraceHostPrev = nHost;
    sRetraceGuestPrev = nGuest;
}

//! Echo of Sound::nDepthTarget so the report line can show what the measured depth is being compared
//! against. Refreshed from the main thread rather than re-read in the interrupt.
CD static s32 sSndTarget = -1;

//! Echo of Sound::nFrequency. The AI drains nFrequency * 4 bytes per second, so this is the
//! denominator the supplied byte rate is judged against.
CD static s32 sSndFreq = -1;

//! One-shot budget for the layout self-check below.
CD static s32 sSndDiagLeft = 1;
CD static const char kFmtSndInit[] =
    "perf: snd play %04X zero %04X target %d flow %d free %d  (play must read 2000)\n";

/**
 * @brief Refresh sSndTarget, and once per boot prove the Sound layout is the right one.
 *
 * Everything the nSnd* counters report is read at hardcoded offsets into a retail object mm-j does
 * not build, so a wrong base pointer or a wrong map would produce plausible-looking numbers rather
 * than an error. `play` is the sentinel: MM's soundEvent hardcodes soundSetBufferSize(pSound,
 * 0x2000) and nothing else in the struct holds that value, so `play 2000` together with a `target`
 * matching what systemSetupGameALL wrote is the proof. If either is wrong, ignore the snd line.
 *
 * Main thread, unlike comboPerfSampleSound -- OSReport does not belong in the VI interrupt.
 */
CT static void comboPerfCheckSound(void) {
    Sound* pSound;

    if (gpSystem == NULL || (pSound = SYSTEM_SOUND(gpSystem)) == NULL) {
        return;
    }

    sSndTarget = pSound->nDepthTarget;
    sSndFreq = pSound->nFrequency;

    if (sSndDiagLeft > 0) {
        sSndDiagLeft--;
        OSReport(kFmtSndInit, pSound->nSizePlay, pSound->nSizeZero, pSound->nDepthTarget,
                 pSound->bFlowControl, pSound->nFreeCount);
    }
}

//! Chained VI post-retrace callback -- lets the overlay work without touching the emulator's present
//! path. Post-retrace is the one instant a framebuffer write lands after the emulator's EFB copy and
//! before the field scans it out (VISetRegs latches `CurrBufAddr = NextBufAddr` between PreCB/PostCB).
//!
//! Both buffers are stamped, not just the current one: VIGetCurrentFrameBuffer isn't linked here, and
//! stamping both is free anyway -- the buffer that just became current keeps the text for its field,
//! the other one is about to be copied into and loses it regardless, then gets stamped again once it
//! becomes current. Redrawn every field since the copy erases it each time.
//!
//! It is also the audio sampling clock: a fixed 60 Hz tick, which is what comboPerfSampleSound needs.
CT static void comboPerfRetrace(u32 nCount) {
    s32 nStride;
    s32 nHeight;
    s32 iFB;

    if (sPrevRetrace != NULL && sPrevRetrace != (VIRetraceCallback)-1) {
        sPrevRetrace(nCount);
    }

    //! Before the PERF_SHOWN gate: the audio sample is a measurement, not part of the overlay, and
    //! must keep accumulating with the overlay switched off. The callback itself is installed from
    //! comboPerfBuild, which runs every COMBO_PERF_DRAW_PERIOD frames regardless of PERF_SHOWN.
    comboPerfSampleSound();
    comboPerfSampleRetrace();

    if (!PERF_SHOWN || rmode == NULL) {
        return;
    }

    nStride = rmode->fbWidth;
    nHeight = rmode->xfbHeight;

    //! Refuses to draw rather than clipping -- a mode this small would put the overlay over something
    //! worth seeing anyway.
    if (nStride < PERF_ORIGIN_X + PERF_COLS * PERF_CHAR_W + 2 * PERF_PAD ||
        nHeight < PERF_ORIGIN_Y + PERF_ROWS * PERF_LINE_H + 2 * PERF_PAD) {
        return;
    }

    for (iFB = 0; iFB < PERF_FB_COUNT; iFB++) {
        u16* pFB = (u16*)lbl_8017B1E0[iFB].unk_04;

        if (comboPerfIsRam((u32)pFB)) {
            comboPerfStamp(pFB, nStride);
            sDrawn++;
        }
    }
}

//! Rebuild the overlay text. Main thread, so sprintf is fine here; the retrace callback only reads.
CT static void comboPerfBuild(s32 nRate, s32 nTreeMemory) {
    s32 nFrames = gComboPerf.nFrames;

    if (PERF_PAGE == 1) {
        sprintf(sLine[0], kFmtP1L0, gComboPerf.nFinds, gComboPerf.nFindInsns);
        sprintf(sLine[1], kFmtP1L1, gComboPerf.nFindRetries, gComboPerf.nJtProbes, gComboPerf.nOrphans,
                gComboPerf.nOrphanBlocked);
        sprintf(sLine[2], kFmtP1L2, gComboPerf.nRomBlocks / nFrames,
                gComboPerf.nRomBlocks != 0 ? gComboPerf.nRomScan / gComboPerf.nRomBlocks : 0);
    } else {
        sprintf(sLine[0], kFmtP0L0, nRate / 100, nRate % 100);
        //! nFrames cannot be zero here: comboPerfFrame increments it before it can reach this.
        sprintf(sLine[1], kFmtP0L1, gComboPerf.nBoundaries / nFrames, nTreeMemory / 1024);
        sprintf(sLine[2], kFmtP0L2, gComboPerf.nKills, gComboPerf.nRepairs, gComboPerf.nHostJumps);
    }

#if COMBO_PERF_SHOW_BTN
    sprintf(sLine[3], kFmtBtnRow, sBtnWord);
#else
    sLine[3][0] = ' ';
    sLine[3][1] = '\0';
#endif

    //! One-shot, interrupts off: installing twice would recurse into our own callback, and a retrace
    //! landing mid-swap would drop cpu.c's callback for that field.
    if (sPrevRetrace == (VIRetraceCallback)-1) {
        bool bEnabled = OSDisableInterrupts();

        sPrevRetrace = VISetPostRetraceCallback(comboPerfRetrace);
        OSRestoreInterrupts(bEnabled);
    }

    //! Bounded self-diagnosis, spread over three periods so `drawn` can be seen rising rather than
    //! printed once as a fixed value.
    if (sDiagLeft > 0) {
        sDiagLeft--;
        OSReport(kFmtDiag, rmode != NULL ? rmode->fbWidth : -1, rmode != NULL ? rmode->xfbHeight : -1,
                 sPrevRetrace, lbl_8017B1E0[0].unk_04, lbl_8017B1E0[1].unk_04, sDrawn);
    }
}
#endif

//! Bounded so a dispatch storm cannot flood the log, and initialised so the budget stays PROGBITS.
//! 40 lines is several pauses' worth: steady-state play dispatches none at all.
CD static s32 sOrphanLogLeft = 40;
CD static const char kFmtOrphan[] =
    "combo: orphan dispatch type %d fresh %d | status %08X mode %08X iDL %d yield %d | dl %08X\n";

CT void comboPerfLogOrphan(s32 nType, s32 nFresh, s32 nStatus, s32 nMode, s32 iDL, s32 nYield, u32 nDL) {
    if (sOrphanLogLeft <= 0) {
        return;
    }

    sOrphanLogLeft--;
    OSReport(kFmtOrphan, nType, nFresh, nStatus, nMode, iDL, nYield, nDL);
}

//! -2 rather than -1 as the "nothing seen yet" value: -1 is RSP_MM_AUDIO_DETECT, a state the guest
//! genuinely returns to whenever it reloads its microcode, and that transition is worth a line.
//! Non-zero so the word stays PROGBITS in .crashdata and contributes no NOBITS to .init.
CD static s32 sUCodeReported = -2;
CD static const char kFmtUCode[] = "perf: audio ucode arm %d (MM numbering: -1 detect, 1 ABI2, 6 none)\n";

CT void comboPerfLogUCode(s32 nType) {
    if (nType == sUCodeReported) {
        return;
    }

    sUCodeReported = nType;
    OSReport(kFmtUCode, nType);
}

CT static void comboPerfReset(void) {
    u32* pnWord = (u32*)&gComboPerf;
    s32 iWord;

    for (iWord = 0; iWord < (s32)(sizeof(ComboPerf) / sizeof(u32)); iWord++) {
        pnWord[iWord] = 0;
    }
}

#if COMBO_PERF_BUTTONS

//! No-op when the overlay is compiled out: the fourth row it feeds doesn't exist, but the chord and
//! its bounded log trace still do.
#ifndef PERF_PUBLISH_BTN
#define PERF_PUBLISH_BTN(n) ((void)0)
#endif

//! Bits of the emulator's normalised button word, Controller::unk_BC (built by controller.c's
//! `fn_800623F4` from a GameCube pad, a Classic Controller, and the Wii Remote's own buttons).
//!
//! Low bits (GameCube pad + Classic Controller):
//!   0x01 A, 0x02 B, 0x04 X, 0x08 Y, 0x10 HOME, 0x20 START, 0x80 L, 0x100 R, 0x600 Z,
//!   0x80000/0x100000/0x200000/0x400000 D-pad left/right/up/down.
//!
//! High bits (Wii Remote only, KPAD `hold`):
//!   0x02000000 LEFT, 0x04000000 RIGHT, 0x08000000 UP, 0x10000000 DOWN, 0x20000000 A.
//!
//! The high bits are used here because the N64-controller mapper (`fn_80062D0C`) never reads them, so
//! a Wii Remote chord built from them reaches nothing in the game.
#define PERF_WM_LEFT 0x02000000
#define PERF_WM_RIGHT 0x04000000
#define PERF_WM_UP 0x08000000
#define PERF_WM_DOWN 0x10000000
#define PERF_WM_A 0x20000000

#define PERF_BTN_L 0x00000080
#define PERF_BTN_R 0x00000100
#define PERF_BTN_DPAD_LEFT 0x00080000
#define PERF_BTN_DPAD_RIGHT 0x00100000
#define PERF_BTN_DPAD_UP 0x00200000
#define PERF_BTN_DPAD_DOWN 0x00400000

//! Two chords, both accepted: Wii Remote (hold A, tap D-pad) is primary; L+R is the GameCube-pad
//! fallback.
#define PERF_MOD_LR (PERF_BTN_L | PERF_BTN_R)

#define PERF_ACT_SHOW 1
#define PERF_ACT_PAGE 2
#define PERF_ACT_LOG 4
#define PERF_ACT_REARM 8

//! -1 so a button already held when the counter starts can't read as a press edge.
CD static s32 sButtonsLast = -1;
CD static bool sLogOn = true;
CD static const char kFmtToggle[] = "perf: overlay %d page %d log %d\n";
CD static const char kFmtRearm[] = "perf: debug probes re-armed\n";

//! Bounded trace of the button word, printed on every change -- separates "not running" (no line),
//! "wrong offsets" (word never changes), and "wrong bits chosen" (changes but never the chord bits).
//! Also on the overlay's fourth row.
CD static s32 sBtnTraceLeft = 40;
CD static const char kFmtBtn[] = "perf: btn %08X (cont %d)\n";

#if COMBO_DEBUG_HOOKS
//! The bounded probes' budgets, so the chord can refill them instead of requiring a reboot. Keep in
//! step with the initialisers in cpu_execute_update.c/cpu_execute_jump.c.
extern s32 gComboBadStackLeft;
extern s32 gComboRaProbeLeft;
extern s32 gComboMergedLeft;
extern s32 gComboHostJumpsLeft;
#endif

//! Read the chord and act on press edges. Called once per closed guest frame, keeping input handling
//! out of the retrace interrupt.
//!
//! Nothing but loads: the emulator's own accessor fn_80062C18 was rejected because its MM branch sets
//! `pController->unk_220 = -1` and never restores it, risking game input for no benefit. Own
//! previous-state copy rather than Controller::unk_CC, which advances at the controller thread's rate
//! and would miss or double-see edges.
//!
//! Which actions the current word and its press edges call for. Both chords map onto the same four
//! actions; a chord whose modifier isn't held contributes nothing.
CT static s32 comboPerfChord(s32 nNow, s32 nTrig) {
    s32 nAct = 0;

    if (nNow & PERF_WM_A) {
        if (nTrig & PERF_WM_LEFT) {
            nAct |= PERF_ACT_SHOW;
        }
        if (nTrig & PERF_WM_RIGHT) {
            nAct |= PERF_ACT_PAGE;
        }
        if (nTrig & PERF_WM_UP) {
            nAct |= PERF_ACT_LOG;
        }
        if (nTrig & PERF_WM_DOWN) {
            nAct |= PERF_ACT_REARM;
        }
    }

    if ((nNow & PERF_MOD_LR) == PERF_MOD_LR) {
        if (nTrig & PERF_BTN_DPAD_LEFT) {
            nAct |= PERF_ACT_SHOW;
        }
        if (nTrig & PERF_BTN_DPAD_RIGHT) {
            nAct |= PERF_ACT_PAGE;
        }
        if (nTrig & PERF_BTN_DPAD_UP) {
            nAct |= PERF_ACT_LOG;
        }
        if (nTrig & PERF_BTN_DPAD_DOWN) {
            nAct |= PERF_ACT_REARM;
        }
    }

    return nAct;
}

CT static void comboPerfInput(void) {
    Controller* pController;
    s32 nCont;
    s32 nNow;
    s32 nTrig;
    s32 nAct;

    if (gpSystem == NULL) {
        return;
    }

    pController = SYSTEM_CONTROLLER(gpSystem);
    if (pController == NULL) {
        return;
    }

    //! Read, but no longer used to bail out: an earlier version returned early on unk_4C == 0, but its
    //! meaning across Wii Remote/Classic/GameCube isn't established here and a wrong reading would
    //! silently disable every button. Printed instead.
    nCont = pController->unk_4C[COMBO_PERF_PAD_CHAN];

    nNow = pController->unk_BC[COMBO_PERF_PAD_CHAN];
    nTrig = nNow & ~sButtonsLast;

    if (nNow != sButtonsLast && sBtnTraceLeft > 0) {
        sBtnTraceLeft--;
        OSReport(kFmtBtn, nNow, nCont);
    }

    sButtonsLast = nNow;
    PERF_PUBLISH_BTN(nNow);

    if ((nAct = comboPerfChord(nNow, nTrig)) == 0) {
        return;
    }

#if COMBO_DEBUG_HOOKS
    if (nAct & PERF_ACT_REARM) {
        gComboBadStackLeft = 4;
        gComboRaProbeLeft = 12;
        gComboMergedLeft = 2;
        gComboHostJumpsLeft = 20;
        OSReport(kFmtRearm);
    }
#endif

#if COMBO_PERF_OVERLAY
    if (nAct & PERF_ACT_SHOW) {
        sShow = PERF_SHOWN ? PERF_SHOW_OFF : PERF_SHOW_ON;
    }
    if (nAct & PERF_ACT_PAGE) {
        sPage++;
    }
#endif
    if (nAct & PERF_ACT_LOG) {
        sLogOn = !sLogOn;
    }

    //! Always acknowledged in the log: a chord press with no visible change is indistinguishable from
    //! the chord not being read at all.
#if COMBO_PERF_OVERLAY
    OSReport(kFmtToggle, PERF_SHOWN, PERF_PAGE, sLogOn);
#else
    OSReport(kFmtToggle, 0, 0, sLogOn);
#endif
}
#endif

//! Frames per second in hundredths, without floating point.
CT static s32 comboPerfRate(s32 nFrames, s64 nTicks) {
    s32 nMillis = (s32)OSTicksToMilliseconds(nTicks);

    //! Guards the division; a divide-by-zero inside a diagnostic is the least useful crash there is.
    if (nMillis < 1) {
        nMillis = 1;
    }

    return (nFrames * 100000) / nMillis;
}

CT void comboPerfFrame(s32 nTreeMemory) {
    s64 nNow;

    gComboPerf.nFrames++;
    comboPerfCheckSound();

#if COMBO_PERF_BUTTONS
    comboPerfInput();
#endif

#if COMBO_PERF_OVERLAY
    if (++sDrawFrames >= COMBO_PERF_DRAW_PERIOD) {
        nNow = OSGetTime();

        //! First period only: counters have been accumulating since boot over an untimed interval.
        //! Open the period and start clean.
        if (sDrawStamp != 1) {
            comboPerfBuild(comboPerfRate(sDrawFrames, nNow - sDrawStamp), nTreeMemory);
        }

        sDrawStamp = nNow;
        sDrawFrames = 0;
    }
#endif

    if (gComboPerf.nFrames < COMBO_PERF_LOG_PERIOD) {
        return;
    }

    nNow = OSGetTime();

    if (sStamp == 1) {
        sStamp = nNow;
        comboPerfReset();
        return;
    }

#if COMBO_PERF_REPORT
    if (PERF_LOG_ON) {
        s32 nFrames = gComboPerf.nFrames;
        s32 nRate = comboPerfRate(nFrames, nNow - sStamp);

        OSReport(kFmtRate, nFrames, (s32)OSTicksToMilliseconds(nNow - sStamp), nRate / 100, nRate % 100,
                 gComboPerf.nBoundaries / nFrames, nTreeMemory, gComboPerf.nKills);
        OSReport(kFmtFind, gComboPerf.nFinds, gComboPerf.nFindInsns,
                 gComboPerf.nFinds != 0 ? gComboPerf.nFindInsns / gComboPerf.nFinds : 0,
                 gComboPerf.nFindRetries, gComboPerf.nJtProbes);
        OSReport(kFmtCombo, gComboPerf.nRepairs, gComboPerf.nRepairsMM, gComboPerf.nShadow,
                 gComboPerf.nHostJumps, gComboPerf.nHostSlow, gComboPerf.nHostNodes,
                 gComboPerf.nOrphans);
        OSReport(kFmtTask, gComboPerf.nOrphanBlocked, gComboPerf.nRetireNoSp, gComboPerf.nShadowUsed);
        {
            //! nSndSamples is zero only before the retrace callback is installed, i.e. for the first
            //! couple of draw periods.
            s32 nSnd = gComboPerf.nSndSamples;
            s32 nDepth100 = nSnd != 0 ? (gComboPerf.nSndDepth * 100) / nSnd : 0;

            OSReport(kFmtSnd, nDepth100 / 100, nDepth100 % 100, sSndTarget, gComboPerf.nSndDrained,
                     gComboPerf.nSndStarve, gComboPerf.nSndPoolFull, gComboPerf.nSndFeeds, nSnd);
        }
        {
            s32 nHost = gComboPerf.nRetraceHost;
            s32 nLost = nHost - gComboPerf.nRetraceGuest;

            OSReport(kFmtRetrace, nHost, gComboPerf.nRetraceGuest, nLost,
                     nHost != 0 ? (nLost * 100) / nHost : 0);
        }
        {
            s32 nMs = (s32)OSTicksToMilliseconds(nNow - sStamp);
            //! Split as (bytes * 25) / (ms * kHz) rather than the obvious
            //! bytes * 100000 / (ms * freq * 4): the numerator of that form reaches ~1e10 over a
            //! three-second period and does not fit in an s32.
            s32 nKHz = sSndFreq / 1000;
            s32 nFed = nMs > 0 ? (gComboPerf.nSndBytes / nMs) * 1000 : 0;

            OSReport(kFmtSndRate, nFed, sSndFreq > 0 ? sSndFreq * 4 : 0,
                     (nMs > 0 && nKHz > 0) ? (gComboPerf.nSndBytes * 25) / (nMs * nKHz) : 0, sSndFreq);
        }
        {
            //! nDmaTicks is raw timebase, converted here rather than at each DMA. The percentage is
            //! against wall clock, not frame count: it answers "how much of the elapsed time went
            //! into DMA invalidation", which is directly comparable to the audio shortfall.
            s32 nMs = (s32)OSTicksToMilliseconds(nNow - sStamp);
            s32 nInvalMs = (s32)OSTicksToMilliseconds((s64)(u32)gComboPerf.nDmaTicks);

            OSReport(kFmtDma, gComboPerf.nDmaCount, gComboPerf.nDmaBytes >> 10, nInvalMs,
                     nMs > 0 ? (nInvalMs * 100) / nMs : 0, gComboPerf.nGateStall);
        }
        {
            s32 nMs = (s32)OSTicksToMilliseconds(nNow - sStamp);
            s32 nTasks = gComboPerf.nAbiTasks;
            s32 nAbiMs = (s32)OSTicksToMilliseconds((s64)(u32)gComboPerf.nAbiTicks);

            OSReport(kFmtAbi, nTasks, nMs > 0 ? (nTasks * 1000) / nMs : 0, nAbiMs,
                     nMs > 0 ? (nAbiMs * 100) / nMs : 0, nTasks > 0 ? (nAbiMs * 1000) / nTasks : 0,
                     nTasks > 0 ? gComboPerf.nAbiBytes / nTasks : 0);
        }
        {
            //! Same shape as the dma line: raw ticks converted once here, share taken against wall
            //! clock rather than frame count so it is directly comparable with `abi` and `inval`.
            s32 nMs = (s32)OSTicksToMilliseconds(nNow - sStamp);
            s32 nGbiMs = (s32)OSTicksToMilliseconds((s64)(u32)gComboPerf.nGbiTicks);
            s32 nTasks = gComboPerf.nGbiTasks;

            OSReport(kFmtGbi, nTasks, nTasks / nFrames, nGbiMs, nMs > 0 ? (nGbiMs * 100) / nMs : 0,
                     nTasks > 0 ? (nGbiMs * 1000) / nTasks : 0,
                     nTasks > 0 ? gComboPerf.nGbiBytes / nTasks : 0, gComboPerf.nGbiCalls);
        }
        //! "other" is cpuExecuteOpcode plus cpuExecuteCall, still in the extracted cpu.c.
        OSReport(kFmtSplit, gComboPerf.nJumps / nFrames, gComboPerf.nIdles / nFrames,
                 (gComboPerf.nBoundaries - gComboPerf.nJumps - gComboPerf.nIdles) / nFrames);
        //! Only printed when the block cache is actually paging -- a line of zeroes every sixty
        //! frames is noise otherwise.
        if (gComboPerf.nRomBlocks != 0) {
            OSReport(kFmtRom, gComboPerf.nRomBlocks, gComboPerf.nRomBlocks / nFrames, gComboPerf.nRomScan,
                     gComboPerf.nRomScan / gComboPerf.nRomBlocks);
        }
    }
#endif

    sStamp = nNow;
    comboPerfReset();
}

#endif
