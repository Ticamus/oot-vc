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
#define COMBO_PERF_COUNTERS 0

//! 1 = the bounded `combo:` OSReport probes in the hottest paths (cpuExecuteUpdate, pfJump, romCopy,
//! romUpdate), each the instrument for one intermittent bug. A shared budget (gComboTraceLeft) is
//! only bounded if its consumers and decrementers are compiled in together -- see COMBO_ARM_TRACE.
#define COMBO_DEBUG_HOOKS 0

//! Freeze the game before title screen (returns without resuming the audio thread AND without raising SP)
#define COMBO_MUTE_AUDIO_HLE 0

//! 1 = the fault handler that names the guest function behind a host DSI/alignment/program fault,
//! installed from cpuReset. Free until something faults. With it off, the fault goes straight to
//! __OSUnhandledException.
#define COMBO_FAULT_NAMER 1

//! Runtime chord (Wii Remote A + D-pad, or L+R+D-pad on a GameCube pad), the pad it is read from,
//! and the state the overlay boots in. Only reachable when the counters are compiled in.
#define COMBO_PERF_BUTTONS 0
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
    //! Audio-backend health, sampled from the VI post-retrace callback at a fixed 60 Hz -- see
    //! comboPerfSampleSound. Sampling from comboPerfFrame instead would run at the *rendered* frame
    //! rate, which in the slow zones is 15-20 Hz against starvation events lasting a few
    //! milliseconds: it would undercount hardest exactly where the answer matters.
    //!
    //! All five are additive, never a running minimum, because comboPerfReset zeroes the whole
    //! struct each period and a min field would read 0 forever.
    /* 0x54 */ s32 nSndSamples; // retraces sampled; the denominator for the four below
    /* 0x58 */ s32 nSndDepth;   // sum of Sound::nReadyCount -> mean queue depth
    /* 0x5C */ s32 nSndDrained; // samples with an empty ready queue
    /* 0x60 */ s32 nSndStarve;  // samples with eMode == SOUND_MM_MODE_STARVE, i.e. audibly silent
    //! Samples with an empty free list. MM's soundMakeBuffer returns false when it cannot take a
    //! descriptor, soundSetLength propagates it, and MM's aiPut32 discards the result -- so the
    //! guest's buffer is dropped silently and you hear a click. Non-zero means nDepthTarget is set
    //! too close to SOUND_MM_BUF_COUNT.
    /* 0x64 */ s32 nSndPoolFull;
    //! Samples at which Sound::pSrcData had moved since the previous one, i.e. the guest had written
    //! a new AI_DRAM_ADDR. This is the discriminator the first four counters lack on their own:
    //! eMode == STARVE and an empty queue look identical whether the host failed to keep up or the
    //! game simply had nothing to play.
    //!
    //!   drained high, feeds high -> real underrun; the host is behind. Raising COMBO_SND_DEPTH helps.
    //!   drained high, feeds ~0   -> the guest submitted nothing at all. Not an underrun, and the
    //!                               depth knob cannot touch it: look at the guest audio thread.
    //!
    //! Undercounts if the guest reuses one AI buffer address, which OoT and MM do not -- they
    //! alternate between two.
    /* 0x68 */ s32 nSndFeeds;
    //! How far Cpu::nRetrace (host VI fields, a fixed 60 Hz) and Cpu::nRetraceUsed (VI interrupts
    //! actually delivered to the guest) each advanced over the period. Their difference is the number
    //! of retraces destroyed by the catch-up in cpuExecuteUpdate, which on a delta of 4 or more does
    //! `nRetraceUsed = nRetrace` and throws away everything in between.
    //!
    //! This is the suspect for the post-switch audio underrun: MM's audio thread generates one buffer
    //! per retrace while the AI drains at a fixed 32 kHz, so each lost retrace is a buffer that is
    //! never produced. AudioThread_Update can only trim +/-0x10 samples around samplesPerFrameTarget,
    //! about +/-3%, so it cannot close a deficit of this size and the queue never refills.
    //!
    //! Sampled from comboPerfRetrace rather than counted at the catch-up site on purpose: that site
    //! is in cpuExecuteUpdate, i.e. in .text, and the two extra bumps grew the carved TU past its
    //! split and shifted .data/.bss/.sdata by 0x40. Nothing may move the retail image.
    //!
    //! MEASURED 2026-08-16: lost is 0 in every period, on both halves, healthy or starving. The
    //! catch-up snap is NOT the cause of the underrun. Kept as a regression watchdog.
    /* 0x6C */ s32 nRetraceHost;
    /* 0x70 */ s32 nRetraceGuest;
    //! Sum of Sound::nSndLen over the feed edges counted in nSndFeeds, i.e. how many bytes of audio
    //! the guest actually handed to the AI this period. This is what turns the `feeds` shortfall into
    //! a number that can be checked against a requirement instead of a suspicion.
    //!
    //! The AI drains a fixed nFrequency * 4 bytes per second (16-bit stereo). Divide nSndBytes by the
    //! period and compare: at 100% the guest is keeping up and any starvation is jitter, at ~80% it
    //! is structurally underproducing and no host-side buffering can ever cover it.
    /* 0x74 */ s32 nSndBytes;
    //! The guest cart-DMA path, which is a different thing from the ROM block cache and was never
    //! measured. `nRomBlocks` covers NAND paging only; everything below is paid on *every* guest cart
    //! DMA, cache hit included, and costs the same on Dolphin as on hardware because none of it is I/O.
    //!
    //! nDmaTicks is the one that decides. fn_8000A504 runs at every DMA completion and calls
    //! cpuInvalidateCache -> cpuDMAUpdateFunction -> treeKillRange, whose loop does a full BST descent
    //! per FOUR BYTES of the destination range whenever that range holds no compiled function
    //! (cpu.c:13541-13545). Audio sample data lands in a code-free heap, so a 0x1000-byte sample DMA
    //! is 1024 descents. That scales with streamed audio volume, which is exactly the shape of
    //! "custom music makes it worse".
    //!
    //! Raw ticks, not microseconds: the 64-bit conversion happens once at report time instead of at
    //! every DMA. At 60.75 MHz an s32 holds ~70 s of accumulated time, far past a log period.
    //! Bytes, not KB: audio sample DMAs are often smaller than 1 KB and would all round to zero. The
    //! division happens once at report time.
    /* 0x78 */ s32 nDmaCount;
    /* 0x7C */ s32 nDmaBytes;
    /* 0x80 */ s32 nDmaTicks;
    //! romCopyUpdate passes refused by the retrace gate at rom.c:438. That test sits *before* the
    //! cache-hit check, so a fully resident copy stalls too, and it stays closed for as long as the
    //! guest defers its VI interrupt. The `retrace ... lost 0%` line does NOT cover this: it measures
    //! net divergence over a period, not how long the gate was shut.
    /* 0x84 */ s32 nGateStall;
    //! The HLE'd audio microcode, measured from inside MM's RSP audio thread (rsp_audio.c's
    //! fn_80054C34). The last hypothesis standing after NAND paging, cart-DMA invalidation, the
    //! retrace gate, retrace loss and queue depth were all measured out.
    //!
    //! nAbiTicks is WALL time around rspParseABI, not CPU time: fn_80054B1C suspends and resumes the
    //! thread off a 1 ms alarm for the whole call, so roughly half of what is counted is the thread
    //! sitting suspended. That is the right thing to measure for the question being asked -- whether
    //! a task can finish inside a 16.7 ms frame -- but it means "% of wall" near 100 says the thread
    //! is permanently backlogged, not that it is burning 100% of the CPU.
    //!
    //! Read tasks/s against 60: if it lands near the ~48 Hz the guest feeds at, this thread is the
    //! pacer and the duty cycle in fn_80054B1C is the lever.
    /* 0x88 */ s32 nAbiTasks;
    /* 0x8C */ s32 nAbiTicks;
    //! Bytes of audio display list the guest actually submitted, summed over nAbiTasks. `nLengthMBI`
    //! is the OSTask data_size and it is exactly rspParseABI2's loop bound (_aspMain.c:2921-2926), so
    //! this is the work request, separated from the time it took to serve it.
    //!
    //! Why it matters: nAbiTicks alone cannot tell "the combo asks for more audio work" apart from
    //! "the same work costs more here". Against a retail capture in the same place, bytes/task
    //! answers the first question on its own, and it does so without regenerating a ROM.
    /* 0x90 */ s32 nAbiBytes;
    //! The graphics display-list parser, the last unmeasured block of the frame. rspParseGBI is the
    //! main thread's largest consumer and nothing counted it, so every "% of wall" figure so far was
    //! measured against an unknown denominator.
    //!
    //! nGbiCalls counts re-entries into the 0x400-command time slice (rsp_update.c:284), not display
    //! lists: a high calls/frame with a low share of wall means the slicing costs more than it buys.
    //! Raw ticks like nDmaTicks, converted once at report time.
    /* 0x94 */ s32 nGbiCalls;
    /* 0x98 */ s32 nGbiTicks;
    //! Graphics tasks set up, and the display-list bytes they declared -- the exact analogue of
    //! nAbiTasks/nAbiBytes, read from the same OSTask field at rspParseGBI_Setup.
    //!
    //! This replaces a first attempt that sampled Rsp::nCountVertex. That field is NOT a per-frame
    //! vertex count: it is the microcode's vertex-cache capacity, assigned once from
    //! pUCode->nCountVertex (rsp.c:211), so it printed a constant 64 in every period of the first
    //! retail capture and measured nothing at all.
    //!
    //! If B/task reads 0, this game's graphics tasks leave data_size at zero (the display list is
    //! self-terminating) and the scene-size question has to be answered from gbi ms/frame alone --
    //! which is still valid, because rspParseGBI is identical code for both ROMs and has no combo
    //! gating, so more time on the same room can only mean more display list.
    /* 0x9C */ s32 nGbiTasks;
    /* 0xA0 */ s32 nGbiBytes;
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

/**
 * @brief One line each time MM's detected audio microcode changes; nothing while it holds steady.
 *
 * Called per audio task, so the edge test lives here rather than at the call site: rsp_audio.c is a
 * carved TU with a .text-only split, where a format string grows .data and a static risks landing in
 * .bss and shifting the pinned layout. Values are the RSP_MM_AUDIO_* numbering in rsp.h, which is
 * MM's own and not RspAudioUCodeType.
 */
void comboPerfLogUCode(s32 nType);

#define COMBO_PERF_BUMP(field) (++gComboPerf.field)
#define COMBO_PERF_ADD(field, n) (gComboPerf.field += (n))
#define COMBO_PERF_FRAME(nTreeMemory) comboPerfFrame(nTreeMemory)
#define COMBO_PERF_LOG_ORPHAN(t, f, s, m, d, y, dl) comboPerfLogOrphan(t, f, s, m, d, y, dl)
#define COMBO_PERF_LOG_UCODE(nType) comboPerfLogUCode(nType)
//! Raw timebase read for the accumulating timers. u32 rather than OSGetTime's s64 so a delta is one
//! subtraction with no 64-bit arithmetic on the hot path; the counter wraps every ~70 s at 60.75 MHz,
//! and modulo-2^32 subtraction stays correct for any interval shorter than that.
#define COMBO_PERF_TICK() OSGetTick()
#else
#define COMBO_PERF_BUMP(field) ((void)0)
#define COMBO_PERF_ADD(field, n) ((void)0)
#define COMBO_PERF_FRAME(nTreeMemory) ((void)0)
#define COMBO_PERF_LOG_ORPHAN(t, f, s, m, d, y, dl) ((void)0)
#define COMBO_PERF_LOG_UCODE(nType) ((void)0)
//! Folds to a constant so `u32 nStart = COMBO_PERF_TICK();` is a dead local the optimiser drops.
#define COMBO_PERF_TICK() ((u32)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
