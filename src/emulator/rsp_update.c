#include "emulator/comboPerf.h"
#include "emulator/cpu.h"
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

extern OSAlarm lbl_80180D30;
extern s32 lbl_8020076C;
extern s32 lbl_80200770;
extern char lbl_80151E30[];
void fn_80092BFC(void* pAlarm);
void fn_80054AE4(OSAlarm* pAlarm, OSContext* pContext);
void fn_8000FA74(Frame* pFrame);

// frame.c file-scope flags: gbFrameBegin (cleared by frameBegin, set by frameEnd) and gbFrameValid
// (set by frameEnd, cleared by frameDrawDone; gates frameBeginOK).
extern bool lbl_802006A8;
extern u32 lbl_802006AC;

extern OSThread lbl_801809E0;
extern s32 lbl_80200768;
extern void* lbl_80200794;

extern u32 gComboTaskRamAddr;

#define RSP_TASK_TYPE(pRSP) (*(u32*)((pRSP)->pDMEM + 0xFC0))

static s32 gComboOrphanCount;
static s32 gComboTaskFresh;
static s32 gComboPrevHalt;
static s32 gComboPrevYield;

static void comboTaskTrack(Rsp* pRSP) {
    s32 nHalt = pRSP->nStatus & 1;
    s32 nYield = pRSP->yield.bValid;

    if (gComboPrevHalt != 0 && nHalt == 0) {
        gComboTaskFresh = 1;
    }

    if (nHalt != 0 || (pRSP->nMode & 0x12) != 0 || pRSP->iDL != 0 || (gComboPrevYield != 0 && nYield == 0)) {
        gComboTaskFresh = 0;
    }

    gComboPrevHalt = nHalt;
    gComboPrevYield = nYield;
}

bool rspUpdate(Rsp* pRSP, RspUpdateMode eMode) {
    bool bDone;
    s32 nCount = 0;
    Frame* pFrame = SYSTEM_FRAME(pRSP->pHost);

    comboTaskTrack(pRSP);

    if (!(pRSP->nStatus & 1)) {
        if (pRSP->nMode & 0x20) {
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

            if (pRSP->iDL == 0 && (pRSP->nMode & 0x12) == 0) {
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

                        COMBO_PERF_BUMP(nOrphans);
                        if (gComboTaskFresh == 0) {
                            COMBO_PERF_BUMP(nOrphanBlocked);
                        }

                        COMBO_PERF_LOG_ORPHAN(nType, gComboTaskFresh, pRSP->nStatus, pRSP->nMode, pRSP->iDL,
                                              pRSP->yield.bValid, ((u32*)(pRSP->pDMEM + 0xFC0))[12]);
                        gComboTaskFresh = 0;

                        if (nType == 1) {
                            xlObjectEvent(pRSP->pHost, 0x1000, (void*)10);
                            pRSP->nMode |= 2;
                        } else if (nType == 2) {
                            lbl_80200794 = pRSP->pDMEM + 0xFC0;
                            DCStoreRange(pRSP->pDMEM + 0xFC0, 0x40);
                            lbl_80200768 = 1;
                            OSResumeThread(&lbl_801809E0);
                        } else {
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

                    //! gbFrameBegin set means no frame is open, so this "done" belongs to no graphics
                    //! task. Calling frameEnd here would hit its
                    //! "INTERNAL ERROR: Called when 'gbFrameBegin' is TRUE!" path. Do that path's recovery
                    //! by hand instead (clear gbFrameValid and gNoSwapBuffer), or frameBeginOK refuses
                    //! every later task setup.
                    if (lbl_802006A8) {
                        COMBO_PERF_BUMP(nRetireNoSp);
                        pRSP->nStatus |= 1;
                        lbl_802006AC = 0;
                        gNoSwapBuffer = false;
                    } else {
                        pRSP->nStatus |= 0x201;
                        xlObjectEvent(pRSP->pHost, 0x1000, (void*)5);

                        if (!frameEnd(pFrame)) {
                            return false;
                        }

                        // Only frameEnd call site in the DOL, so the one place a closed guest frame can
                        // be counted.
                        COMBO_PERF_FRAME(SYSTEM_CPU(pRSP->pHost) != NULL &&
                                                 SYSTEM_CPU(pRSP->pHost)->gTree != NULL
                                             ? SYSTEM_CPU(pRSP->pHost)->gTree->total_memory
                                             : 0);
                    }
                }

                *(u32*)pRSP->unk00E0 = OSGetTick();
            }
        }
    }

    return true;
}
