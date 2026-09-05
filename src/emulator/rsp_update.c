#include "emulator/comboPerf.h"
#include "emulator/comboSave.h"
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

#if IS_MM && COMBO_FRAME_BUDGET

#if COMBO_BUDGET_REPORT
#define COMBO_BUDGET_LOG_PERIOD 60

//! MM's OS_CYCLES_TO_NSEC (1000000000/15625 over OS_CPU_COUNTER/15625), so the report prints the
//! number func_80173B48() would have returned.
#define COMBO_CYCLES_TO_NSEC(nCycles) (((u64)(nCycles) * 64000) / 3000)

//! framerateDivisor while a scene is running (z_play.c).
#define COMBO_BUDGET_DIVISOR 3

static s32 gComboBudgetLeft;
#endif

#if COMBO_FIX_RETRACE_TIME
static s32 gComboRetraceFixed;
#endif

#if COMBO_FIX_RDP_TIME
static s32 gComboRdpClamps;
#endif

static u32* comboBudgetGuest(Ram* pRAM, u32 nAddress) {
    u32 nOffset = nAddress & 0x1FFFFFFF;

    if (pRAM == NULL || pRAM->pBuffer == NULL || nOffset + sizeof(OSTime) > pRAM->nSize) {
        return NULL;
    }

    return (u32*)(pRAM->pBuffer + nOffset);
}

//! These are OSTimes, big-endian like the host, so word 0 is the high half. Anything that does not
//! fit in the low word is already far outside the band, so saturate instead. It reports as -1.
static u32 comboBudgetRead(const u32* pnTime) { return pnTime[0] != 0 ? 0xFFFFFFFF : pnTime[1]; }

static void comboBudgetWrite(u32* pnTime, u32 nValue) {
    pnTime[0] = 0;
    pnTime[1] = nValue;
}

//! Corrects the frame-budget inputs MM latched at boot. See COMBO_FRAME_BUDGET in system.h.
static void comboFrameBudget(Rsp* pRSP) {
    System* pSystem = pRSP->pHost;
    Cpu* pCPU = SYSTEM_CPU(pSystem);
    Ram* pRAM = SYSTEM_RAM(pSystem);
    u32* pnRetrace;
    u32 nRetrace;
#if COMBO_FIX_RDP_TIME || COMBO_BUDGET_REPORT
    u32* pnRDP;
    u32 nRDP;
#endif
#if COMBO_BUDGET_REPORT
    u32* pnPeriod;
    u32 nRDPRaw;
    s32 nSpare;
#endif

    if (!COMBO_BUDGET_ARMED(pSystem, pCPU)) {
        return;
    }

    pnRetrace = comboBudgetGuest(pRAM, COMBO_MM_IRQ_RETRACE_TIME);
    if (pnRetrace == NULL) {
        return;
    }
    nRetrace = comboBudgetRead(pnRetrace);

#if COMBO_FIX_RDP_TIME || COMBO_BUDGET_REPORT
    pnRDP = comboBudgetGuest(pRAM, COMBO_MM_RDP_TIME_TOTAL);
    if (pnRDP == NULL) {
        return;
    }
    nRDP = comboBudgetRead(pnRDP);
#endif
#if COMBO_BUDGET_REPORT
    pnPeriod = comboBudgetGuest(pRAM, COMBO_MM_GRAPH_PERIOD);
    if (pnPeriod == NULL) {
        return;
    }
    nRDPRaw = nRDP;
#endif

#if COMBO_FIX_RETRACE_TIME
    if (nRetrace == 0) {
        gComboRetraceFixed = 0;
    } else if (!gComboRetraceFixed && gIsOotmmCombo &&
               (nRetrace < COMBO_RETRACE_MIN || nRetrace > COMBO_RETRACE_MAX)) {
        gComboRetraceFixed = 1;
        OSReport("combo: gIrqMgrRetraceTime %d outside [%d,%d], forcing %d\n", nRetrace, COMBO_RETRACE_MIN,
                 COMBO_RETRACE_MAX, COMBO_RETRACE_REF);
        comboBudgetWrite(pnRetrace, COMBO_RETRACE_REF);
        nRetrace = COMBO_RETRACE_REF;
    }
#endif

#if COMBO_FIX_RDP_TIME
    //! Every frame, because Graph_ExecuteAndDraw recomputes it every frame.
    if (gIsOotmmCombo && nRDP > COMBO_RDP_TIME_CAP) {
        if (gComboRdpClamps++ == 0) {
            OSReport("combo: gRDPTimeTotal %d over cap %d, clamping from here on\n", nRDP, COMBO_RDP_TIME_CAP);
        }
        comboBudgetWrite(pnRDP, COMBO_RDP_TIME_CAP);
        nRDP = COMBO_RDP_TIME_CAP;
    }
#endif

#if COMBO_BUDGET_REPORT
    if (--gComboBudgetLeft > 0) {
        return;
    }
    gComboBudgetLeft = COMBO_BUDGET_LOG_PERIOD;

    nSpare = (s32)(COMBO_CYCLES_TO_NSEC(COMBO_BUDGET_DIVISOR * (u64)nRetrace) - COMBO_CYCLES_TO_NSEC(nRDP));

    OSReport("combo: budget retrace %d rdp %d (raw %d) period %d -> spare %d ns, roll dust %d, step dust %d\n",
             nRetrace, nRDP, nRDPRaw, comboBudgetRead(pnPeriod), nSpare, nSpare / 20000000, nSpare / 12000000);
#endif
}

#define COMBO_FRAME_BUDGET_TICK(pRSP) comboFrameBudget(pRSP)
#else
#define COMBO_FRAME_BUDGET_TICK(pRSP) ((void)0)
#endif

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

#if COMBO_MUTE_AUDIO_HLE
    //! Guest reloads its audio microcode on scene and
    //! game changes, and rspParseABI would re-run its checksum pass the moment anything put -1 back.
    pRSP->eTypeAudioUCodeMM = RSP_MM_AUDIO_NONE;
#endif

    comboTaskTrack(pRSP);

    // Flushes the save image once it has been dirty past its deadline
    COMBO_SAVE_TICK();

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

                    COMBO_PERF_BUMP(nGbiTasks);
                    COMBO_PERF_ADD(nGbiBytes, ((RspTask*)(pRSP->pDMEM + 0xFC0))->nLengthMBI & 0x7FFFFF);

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
                u32 nGbiTick = COMBO_PERF_TICK();

                rspParseGBI(pRSP, &bDone, nCount);

                COMBO_PERF_ADD(nGbiTicks, COMBO_PERF_TICK() - nGbiTick);
                COMBO_PERF_BUMP(nGbiCalls);

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

                        COMBO_FRAME_BUDGET_TICK(pRSP);
                    }
                }

                *(u32*)pRSP->unk00E0 = OSGetTick();
            }
        }
    }

    return true;
}
