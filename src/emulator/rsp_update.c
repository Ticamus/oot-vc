//! rspUpdate, carved out of mm-j's rsp.c (0x80071630, 0x1FC) so the OoT pause freeze could be fixed
//! here. Full write-up, including everything that was eliminated along the way and the five wrong
//! attempts at this fault, in **docs/ootmm_pause_freeze.md** -- read that before changing anything
//! below, because two of those attempts looked obviously right and hung the guest.
//!
//! The retail shape, which is NOT oot-j's -- there is no `(nMode & 4) && (nMode & 8)` gate:
//!
//!     if (nStatus & 1) return true;
//!     if (nMode & 0x20) { nMode &= ~0x30; nStatus |= 0x201; event SP; return true; }
//!     if (nMode & 2) { if (frameBeginOK() && eMode == RUM_IDLE) { nMode = (nMode & ~2) | 0x10;
//!                        rspParseGBI_Setup(pRSP, pDMEM + 0xFC0); } else { <watchdog>; return true; } }
//!     if (eMode == RUM_IDLE) nCount = 0x400;
//!     if (nCount) { rspParseGBI(...); if (bDone) { nMode &= ~0x10; nStatus |= 0x201; event SP;
//!                                                 frameEnd(pFrame); } nTickLast = OSGetTick(); }
//!
//! Three additions, all marked below: the orphaned-task recovery (the fix), a skip when there is nothing
//! to parse, and the frame protection on a re-parse. Everything else is faithful.
//!
//! One rule for this file and its siblings: **no new .bss.** Its split claims only .text, so a .bss
//! object is inserted into the middle of the pinned layout and shifts every later one -- a sixteen-byte
//! array here once made the console report corrupted save data at startup. Scalars land in .sbss past
//! the end of the retail image and are fine; an array must be initialised so it lands in .data.

#include "emulator/frame.h"
#include "emulator/ram.h"
#include "emulator/rsp.h"
#include "emulator/system.h"
#include "emulator/xlObject.h"
#include "macros.h"
#include "revolution/os.h"

bool frameBeginOK(Frame* pFrame);
bool frameEnd(Frame* pFrame);
bool rspParseGBI_Setup(Rsp* pRSP, void* pTask);
bool rspParseGBI(Rsp* pRSP, bool* pbDone, s32 nCount);

//! rsp.c file-scope objects, under the names dtk gives them. lbl_80180D30 is the OSAlarm behind the
//! ten-second RSP watchdog; lbl_80200770 says the alarm is armed, lbl_8020076C that it fired.
extern OSAlarm lbl_80180D30;
extern s32 lbl_8020076C;
extern s32 lbl_80200770;
extern char lbl_80151E30[];
void fn_80092BFC(void* pAlarm);
void fn_80054AE4(OSAlarm* pAlarm, OSContext* pContext);
void fn_8000FA74(Frame* pFrame);

//! frame.c file-scope flags. lbl_802006A8 is gbFrameBegin: cleared by frameBegin, set by frameEnd.
//! lbl_802006AC is gbFrameValid: set by frameEnd, cleared by frameDrawDone once the frame has actually
//! been presented, and what frameBeginOK gates the next task setup on.
extern bool lbl_802006A8;
extern u32 lbl_802006AC;

//! rspPut32's audio hand-off, replayed by the recovery below. lbl_801809E0 is the audio thread, which
//! suspends itself at the top of its loop; lbl_80200794 is the descriptor pointer it works from, which
//! is DMEM+0xFC0 itself rather than a copy; lbl_80200768 says a task has been handed over.
extern OSThread lbl_801809E0;
extern s32 lbl_80200768;
extern void* lbl_80200794;

//! cpu_execute_update.c's capture of where the guest's own OSTask lives in RDRAM. Authoritative, unlike
//! anything in DMEM: the display-list parser tramples DMEM, never this.
extern u32 gComboTaskRamAddr;

#define RSP_TASK_TYPE(pRSP) (*(u32*)((pRSP)->pDMEM + 0xFC0))

//! Not in the original game. Consecutive sightings of the orphaned-task state, so a transient cannot
//! trigger a dispatch. A scalar, so it lands in .sbss -- see the header.
static s32 gComboOrphanCount;

bool rspUpdate(Rsp* pRSP, RspUpdateMode eMode) {
    bool bDone;
    s32 nCount = 0;
    Frame* pFrame = SYSTEM_FRAME(pRSP->pHost);

    //! One nested if with a single return at the end rather than early returns: retail shares one
    //! epilogue, and this is what makes the carve byte-exact when the additions are compiled out.
    if (!(pRSP->nStatus & 1)) {
        if (pRSP->nMode & 0x20) {
            //! nMode before nStatus, as retail does.
            pRSP->nMode &= ~0x30;
            pRSP->nStatus |= 0x201;
            xlObjectEvent(pRSP->pHost, 0x1000, (void*)5);
        } else {
            if (pRSP->nMode & 2) {
                if (frameBeginOK(pFrame) && eMode == RUM_IDLE) {
                    if (lbl_80200770 == 0) {
                        fn_80092BFC(&lbl_80180D30);
                        lbl_8020076C = 0;
                        lbl_80200770 = 1;
                    }

                    pRSP->nMode = (pRSP->nMode & ~2) | 0x10;

                    if (!rspParseGBI_Setup(pRSP, pRSP->pDMEM + 0xFC0)) {
                        return false;
                    }
                } else {
                    if (lbl_80200770 != 0) {
                        lbl_80200770 = 0;
                        OSSetAlarm(&lbl_80180D30, OSMillisecondsToTicks(10000), fn_80054AE4);
                    } else if (lbl_8020076C == 1) {
                        OSReport(lbl_80151E30);
                        fn_8000FA74(pFrame);
                    }

                    return true;
                }
            }

            //! NOT IN THE ORIGINAL GAME, and this is the fix for the OoT pause freeze. Two jobs in one
            //! block, because they are the same state.
            //!
            //! First: there is nothing to parse when the display-list stack is empty with nothing queued
            //! and nothing in flight. rspParseGBI would walk nothing and report done anyway, and that
            //! unearned "done" is what produced every stale completion this file used to chase.
            //!
            //! Second: that same state is where a dropped task ends up. rspPut32 reads OSTask.type out of
            //! DMEM+0xFC0 *after* clearing the halt bit, and for a type outside 1..7 it bails through
            //! .L_80070F80 -- `li r3, 0; b end` -- with nothing queued and nothing to complete the task.
            //! The descriptor repair in cpu_execute_update.c cannot fully prevent that, because the read
            //! happens inside the guest's own store to SP_STATUS and there is no hook between the two.
            //! So recognise the state it leaves behind and dispatch the task retail would have.
            //!
            //! The gate is narrow by construction: a graphics submission sets 0x2, an audio submission
            //! leaves its thread busy, a completed task sets the halt bit, a list rspLoadYield restored
            //! comes back with iDL >= 1 and must still be parsed, and a virgin descriptor reads type 0.
            //! Four consecutive sightings, ~12 ms, so a transient cannot trigger it.
            //!
            //! iDL is the discriminator here, not the nMode bits. An earlier build tested the mode bits
            //! instead, blocked the parse of a resumed display list, and hung the graph thread.
            if (pRSP->iDL == 0 && (pRSP->nMode & 0x12) == 0) {
                //! The type comes from the guest's copy in RDRAM, and DMEM is refreshed from it, because
                //! rspParseGBI_Setup reads the display-list pointer out of DMEM and the audio thread reads
                //! the whole descriptor from there. An earlier build trusted a DMEM snapshot that was
                //! always one task behind and woke the audio thread for a finished audio pass while OoT
                //! waited for its frame.
                u32 nType = RSP_TASK_TYPE(pRSP);
                Ram* pRAM = SYSTEM_RAM(pRSP->pHost);

                if (pRAM != NULL && pRAM->pBuffer != NULL && gComboTaskRamAddr != 0 &&
                    gComboTaskRamAddr + 0x40 <= pRAM->nSize) {
                    u32* pnGuest = (u32*)(pRAM->pBuffer + gComboTaskRamAddr);

                    if ((u32)(pnGuest[0] - 1) <= 6) {
                        u32* pnTask = (u32*)(pRSP->pDMEM + 0xFC0);
                        s32 iWord;

                        for (iWord = 0; iWord < 16; iWord++) {
                            pnTask[iWord] = pnGuest[iWord];
                        }

                        nType = pnGuest[0];
                    }
                }

                if ((u32)(nType - 1) <= 6 && OSIsThreadSuspended(&lbl_801809E0) &&
                    !systemExceptionPending(pRSP->pHost, SIT_SP)) {
                    if (++gComboOrphanCount >= 4) {
                        gComboOrphanCount = 0;

                        if (nType == 1) {
                            //! Retail's case 1 at 0x80070D84, minus the yield branch that cannot apply
                            //! here: DP is raised at submit time by 0x80070D8C, and the queued bit makes
                            //! the next pump call run a real rspParseGBI_Setup.
                            xlObjectEvent(pRSP->pHost, 0x1000, (void*)10);
                            pRSP->nMode |= 2;
                        } else if (nType == 2) {
                            //! Retail's case 2 at 0x80070E0C, in its own order: publish the descriptor
                            //! pointer the audio thread works from, flush those sixty-four bytes, then wake
                            //! it. Safe to issue from here only because the gate above established that the
                            //! thread is suspended.
                            lbl_80200794 = pRSP->pDMEM + 0xFC0;
                            DCStoreRange(pRSP->pDMEM + 0xFC0, 0x40);
                            lbl_80200768 = 1;
                            OSResumeThread(&lbl_801809E0);
                        } else {
                            //! The self-completing types, exactly as cases 3, 6 and 7 of
                            //! jumptable_80151D00 retire them.
                            pRSP->nStatus |= 0x201;
                            xlObjectEvent(pRSP->pHost, 0x1000, (void*)5);
                        }
                    }
                } else {
                    gComboOrphanCount = 0;
                }

                return true;
            }

            if (eMode == RUM_IDLE) {
                nCount = 0x400;
            }

            if (nCount != 0) {
                rspParseGBI(pRSP, &bDone, nCount);

                if (bDone) {
                    pRSP->nMode &= ~0x10;

                    //! NOT IN THE ORIGINAL GAME. gbFrameBegin already set means no frame is open, so this
                    //! "done" belongs to no graphics task and calling frameEnd is what produced the
                    //! "frameEnd: INTERNAL ERROR: Called when 'gbFrameBegin' is TRUE!" report. Skip it,
                    //! and do by hand the recovery its own error path does -- clear gbFrameValid -- plus
                    //! the gNoSwapBuffer clear it is missing. Without that pair frameDrawDone never
                    //! presents the frame, gbFrameValid stays set, and frameBeginOK then refuses every
                    //! later task setup.
                    //!
                    //! No completion is claimed here, and gbFrameBegin rather than nMode 0x10 is what
                    //! decides that. A list rspLoadYield put back still has its frame open, so it takes
                    //! the else branch and gets its full completion; a stale re-parse has no frame and its
                    //! "completion" would retire someone else's task. Both mistakes were made once.
                    if (lbl_802006A8) {
                        pRSP->nStatus |= 1;
                        lbl_802006AC = 0;
                        gNoSwapBuffer = false;
                    } else {
                        pRSP->nStatus |= 0x201;
                        xlObjectEvent(pRSP->pHost, 0x1000, (void*)5);

                        if (!frameEnd(pFrame)) {
                            return false;
                        }
                    }
                }

                *(u32*)pRSP->unk00E0 = OSGetTick();
            }
        }
    }

    return true;
}
