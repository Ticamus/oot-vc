#ifndef _COMBO_PERF_H
#define _COMBO_PERF_H

#include "macros.h"
#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

//! Master switch: at 0 this whole unit compiles to nothing and every call site reduces to `(void)0`.
//! At 1 it brings the counters, log summary, overlay and runtime chord. IS_MM-gated below rather than
//! at each call site so sites need no `#if`: comboPerf.c is linked for mm-j alone and oot-* objects
//! must stay byte-exact.
//! Currently 1 to gather evidence on the Sched_HandleRDPDone crash (see nOrphanBlocked). Put it back
//! to 0 once that is settled -- the overlay and the chord ride on this switch too.
#define COMBO_PERF_COUNTERS 1

//! 1 = the bounded `combo:` OSReport probes in the hottest paths (cpuExecuteUpdate, pfJump, romCopy,
//! romUpdate), each the instrument for one intermittent bug. A shared budget (gComboTraceLeft) is
//! only bounded if its consumers and decrementers are compiled in together -- see COMBO_ARM_TRACE.
#define COMBO_DEBUG_HOOKS 0

//! 1 = the fault handler that names the guest function behind a host DSI/alignment/program fault,
//! installed from cpuReset. Free until something faults. With it off, the fault goes straight to
//! __OSUnhandledException.
#define COMBO_FAULT_NAMER 1

//! Runtime chord (Wii Remote A + D-pad, or L+R+D-pad on a GameCube pad), the pad it is read from,
//! and the state the overlay boots in. Only reachable when the counters are compiled in.
#define COMBO_PERF_BUTTONS 1
#define COMBO_PERF_PAD_CHAN 0
#define COMBO_PERF_OVERLAY_DEFAULT false

#define COMBO_PERF_OVERLAY 1

#define COMBO_PERF_REPORT 1

#define COMBO_PERF_LOG_PERIOD 60
#define COMBO_PERF_DRAW_PERIOD 15

#if IS_MM && COMBO_PERF_COUNTERS

typedef struct ComboPerf {
    /* 0x00 */ s32 nBoundaries;
    /* 0x04 */ s32 nFrames;
    /* 0x08 */ s32 nShadow;
    /* 0x0C */ s32 nRepairs;
    /* 0x10 */ s32 nRepairsMM;
    /* 0x14 */ s32 nHostJumps;
    /* 0x18 */ s32 nHostSlow;
    /* 0x1C */ s32 nHostNodes;
    /* 0x20 */ s32 nFinds;
    /* 0x24 */ s32 nFindInsns;
    /* 0x28 */ s32 nFindRetries;
    /* 0x2C */ s32 nJtProbes;
    /* 0x30 */ s32 nKills;
    /* 0x34 */ s32 nOrphans;
    /* 0x38 */ s32 nRomBlocks;
    /* 0x3C */ s32 nRomScan;
    /* 0x40 */ s32 nJumps;
    /* 0x44 */ s32 nIdles;
    //! Orphan dispatches refused because the descriptor had already been consumed -- the one the
    //! Sched_HandleRDPDone crash turns on. Each one is a DP that would have reached OoT's scheduler
    //! with no RDP task outstanding.
    /* 0x48 */ s32 nOrphanBlocked;
    //! Parses retired through rspUpdate's gbFrameBegin branch, which halts without raising SP. Each is
    //! a task that got a DP and never a matching SP.
    /* 0x4C */ s32 nRetireNoSp;
    //! Descriptor repairs that fell back to the shadow snapshot instead of the guest's RDRAM OSTask.
    //! The shadow is frozen at the first descriptor ever seen and is initialised to type 1, so every
    //! hit is a chance to relabel an audio submission as graphics.
    /* 0x50 */ s32 nShadowUsed;
} ComboPerf;

extern ComboPerf gComboPerf;

/**
 * @brief Counts one closed guest frame and, every sixtieth, prints the summary and starts a fresh
 * period. nTreeMemory is the compiled-function tree's current size, or 0 if unknown.
 */
void comboPerfFrame(s32 nTreeMemory);

/**
 * @brief One bounded line per orphaned-task dispatch, with the RSP state that led to it.
 *
 * Separate from the frame-driven report because the interesting dispatches are the ones the guest
 * does not survive: a freeze stops frameEnd, so the periodic summary never prints. Lives here rather
 * than in rsp_update.c so its format string lands in .crashdata instead of growing that carved TU's
 * .data.
 */
void comboPerfLogOrphan(s32 nType, s32 nFresh, s32 nStatus, s32 nMode, s32 iDL, s32 nYield, u32 nDL);

#define COMBO_PERF_BUMP(field) (++gComboPerf.field)
#define COMBO_PERF_ADD(field, n) (gComboPerf.field += (n))
#define COMBO_PERF_FRAME(nTreeMemory) comboPerfFrame(nTreeMemory)
#define COMBO_PERF_LOG_ORPHAN(t, f, s, m, d, y, dl) comboPerfLogOrphan(t, f, s, m, d, y, dl)
#else
#define COMBO_PERF_BUMP(field) ((void)0)
#define COMBO_PERF_ADD(field, n) ((void)0)
#define COMBO_PERF_FRAME(nTreeMemory) ((void)0)
#define COMBO_PERF_LOG_ORPHAN(t, f, s, m, d, y, dl) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
