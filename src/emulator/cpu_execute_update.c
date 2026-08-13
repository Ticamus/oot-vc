#include "emulator/cpu.h"
#include "emulator/ram.h"
#include "revolution/os/OSError.h"
#include "emulator/rom.h"
#include "emulator/rsp.h"
#include "emulator/system.h"
#include "emulator/vc64_RVL.h"
#include "emulator/vi.h"
#include "emulator/xlObject.h"
#include "macros.h"

// Split out of cpu.c so this single function can be built from source and linked while
// the rest of cpu.c still comes from the extracted object. See config/mm-j/splits.txt.

// Defined in cpu.c; dtk exposes every function it recovers with global scope, so these
// resolve across the split without any symbols.txt change.
bool cpuFindAddress(Cpu* pCPU, s32 nAddressN64, s32* pnAddressGCN);
bool treeTimerCheck(Cpu* pCPU);
bool treeCleanUp(Cpu* pCPU, CpuTreeRoot* root);

// Unidentified MM-only helpers living in rsp.c and helpRVL.c, plus an rsp.c flag.
extern s32 lbl_801FFF60;
extern void fn_80054B64(s32 arg0);
extern void fn_80085C84(void* pHelp);

#if IS_MM
//! Debug only. Armed by cpu_execute_jump.c when the OoTMM switch stub is entered. This
//! function is the one hook the whole interpreter funnels through -- cpuExecuteOpcode,
//! cpuExecuteCall and cpuExecuteIdle all call it -- so it sees the interpreted `cache` and
//! MMIO opcodes and the direct `jal`s that never reach pfJump.
//!
//! Consecutive PCs inside one small region collapse into a single line with a hit count, so a
//! polling loop shows up as one entry rather than flooding the log. COMBO_TRACE_BEAT then
//! keeps printing while the guest stays in that region: heartbeats still arriving means it is
//! spinning on an interpreted opcode, silence means it left the interpreter for good and is
//! stuck inside a recompiled block.
#define COMBO_TRACE_REGION 0x40
#define COMBO_TRACE_BEAT 200000
extern s32 gComboTraceLeft;
static s32 gComboTracePC = 0;
static s32 gComboTraceHits = 0;

//! REMOVED, along with the guest heartbeat, the guest-code dump and the __osActiveQueue thread
//! walker. They did their job -- they are what established that every OoT thread parks in osRecvMesg
//! while only libultra's VI manager and the idle thread run, and the frames-closed counter is what
//! finally separated a frozen guest from a healthy instant. Keeping them costs a dozen format strings
//! and several globals in a TU whose split claims only .text, and that footprint is the one thing
//! that grew between a build that boots cleanly and one the console reports as corrupted memory.
//! Reinstate from git history if the guest side needs watching again.

//! Not in the original game. Half of the cure for the OoT pause freeze; the other half is the orphan
//! recovery in rsp_update.c. Full write-up in docs/ootmm_pause_freeze.md -- read that before touching
//! either, because five earlier attempts at this fault were wrong in instructive ways.
//!
//! In short: retail rspParseGBI_F3DEX2 copies display-list data into `pDMEM + ((word >> 3) & 0xFF8)`
//! with a display-list-supplied length and no bound (0x8006F318 and five siblings), and 0xFC0 is where
//! libultra keeps the OSTask descriptor. OoT's pause menu drives that path; MM's own lists never do.
//! rspPut32 then reads OSTask.type from there, finds garbage, and bails through .L_80070F80 with the
//! halt bit already cleared and nothing dispatched, so the guest waits on a task nobody owns.
//!
//! Only `type` is actually lost to the emulator -- rspParseGBI_Setup reads just +0x30 and rspFindUCode
//! +0x10..+0x1C, all of which survive -- but the whole sixty-four bytes are restored anyway, because the
//! correct source is right there: the guest's own OSTask in RDRAM, which the parser never touches.
//!
//! This function is the right home for it. It runs inside the guest on every interpreted opcode, MMIO
//! access and block boundary, thousands of times a frame, so the descriptor is normally back before the
//! guest's next SP_STATUS write reaches rspPut32. Normally, not always: that read happens inside the
//! guest's own store and there is no hook between the two, which is exactly why rsp_update.c has to
//! recover the tasks that slip through.
#define COMBO_REPAIR_TASK 1

//! RDRAM address of the guest's own OSTask, captured from the descriptor load's DMA parameters, and the
//! authoritative source for a repair. Global so rsp_update.c can decide what to dispatch from the same
//! source rather than trusting whatever is left in DMEM -- an earlier version trusted a DMEM snapshot,
//! which was always one task behind, and dispatched the wrong one.
u32 gComboTaskRamAddr;

//! Fallback for the window before any descriptor load has been seen. Initialised, so it lands in .data
//! rather than .bss: a TU whose split claims only .text must not add .bss, that inserts into the pinned
//! layout and corrupts save data at startup.
u32 gComboShadowTask[16] = {1};
bool gComboShadowValid;
#endif

#define CPU_SYSTEM(pCPU) ((System*)(pCPU)->pSystem)

#if IS_MM
//! Debug only. A fault inside recompiled code reports a host PC in the code cache, which says nothing
//! about the guest. This names the guest function instead, by scanning the function tree for the node
//! whose compiled code contains SRR0, then dumping the guest words around both ends of its bounds. It
//! **uninstalls itself** before returning: the OS reloads the faulting context afterwards, so the same
//! instruction faults again immediately and the default handler prints the usual full dump the second
//! time. All storage initialised, so none of it lands in .bss (this TU's split claims only .text).
Cpu* gComboCpu = (Cpu*)-1;
s32 gComboFaultArmed = -1;

//! Debug only. Bounded, initialised so it lands in .data. See the stack check in cpuExecuteUpdate.
s32 gComboBadStackLeft = 4;

static CpuFunction* comboFindByHostNode(CpuFunction* pNode, u32 nHost, s32 nDepth) {
    CpuFunction* pFound;

    // Bounded: this runs inside an exception, and a corrupt tree must not take the handler with it.
    // 400 not 64: the function tree is a BST keyed on nAddress0 and inserts largely in address order, so
    // it is badly skewed and a legitimate node can sit far deeper than a balanced log2(N) -- a too-small
    // cap made comboFindByHost miss real nodes and report "not in any compiled function" wrongly.
    if (pNode == NULL || nDepth > 400) {
        return NULL;
    }

    if (pNode->pfCode != NULL && nHost >= (u32)pNode->pfCode &&
        nHost < (u32)pNode->pfCode + (u32)pNode->memory_size) {
        return pNode;
    }

    if ((pFound = comboFindByHostNode(pNode->left, nHost, nDepth + 1)) != NULL) {
        return pFound;
    }
    return comboFindByHostNode(pNode->right, nHost, nDepth + 1);
}

//! Debug only. The compiled function whose host code starts closest below nHost, for when exact
//! containment fails -- it names what the fault is just past, which a bare "not found" cannot.
static void comboFindNearestBelowNode(CpuFunction* pNode, u32 nHost, s32 nDepth, CpuFunction** ppBest) {
    if (pNode == NULL || nDepth > 400) {
        return;
    }
    if (pNode->pfCode != NULL && (u32)pNode->pfCode <= nHost &&
        (*ppBest == NULL || (u32)pNode->pfCode > (u32)(*ppBest)->pfCode)) {
        *ppBest = pNode;
    }
    comboFindNearestBelowNode(pNode->left, nHost, nDepth + 1, ppBest);
    comboFindNearestBelowNode(pNode->right, nHost, nDepth + 1, ppBest);
}

//! Which compiled function, if any, owns a host address.
CpuFunction* comboFindByHost(Cpu* pCPU, u32 nHost) {
    CpuFunction* pFound;

    if (pCPU == NULL || pCPU == (Cpu*)-1 || pCPU->gTree == NULL) {
        return NULL;
    }

    if ((pFound = comboFindByHostNode(pCPU->gTree->left, nHost, 0)) != NULL) {
        return pFound;
    }
    return comboFindByHostNode(pCPU->gTree->right, nHost, 0);
}

static void comboDumpGuest(s32 nGuest, s32 nFromWord, s32 nToWord, Ram* pRAM) {
    u32 nOffset = (u32)nGuest & 0x1FFFFFFF;
    s32 iWord;

    if (pRAM == NULL || pRAM->pBuffer == NULL || (s32)(nOffset + nFromWord * 4) < 0 ||
        nOffset + (nToWord + 8) * 4 > (u32)pRAM->nSize) {
        return;
    }

    for (iWord = nFromWord; iWord < nToWord; iWord += 8) {
        u32* pnCode = (u32*)(pRAM->pBuffer + nOffset);
        OSReport("combo:   rdram %08X: %08X %08X %08X %08X %08X %08X %08X %08X\n", nGuest + iWord * 4,
                 pnCode[iWord + 0], pnCode[iWord + 1], pnCode[iWord + 2], pnCode[iWord + 3], pnCode[iWord + 4],
                 pnCode[iWord + 5], pnCode[iWord + 6], pnCode[iWord + 7]);
    }
}

//! Debug only. The guest register file at the last block boundary. A near-null DAR (a small offset off a
//! zeroed base) is a bad pointer in some GPR, and this is what names it. The values are as of the last
//! boundary, not the faulting instruction -- the recompiler runs many guest ops between boundaries -- but
//! for a fault a few instructions into a block that is close enough to spot the offender.
static void comboDumpGPR(Cpu* pCPU) {
    static const char* asName[32] = {"r0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                     "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                     "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    s32 i;

    for (i = 0; i < 32; i += 4) {
        OSReport("combo:   %s %08X  %s %08X  %s %08X  %s %08X\n", asName[i], pCPU->aGPR[i].u32, asName[i + 1],
                 pCPU->aGPR[i + 1].u32, asName[i + 2], pCPU->aGPR[i + 2].u32, asName[i + 3],
                 pCPU->aGPR[i + 3].u32);
    }
}

static void comboFaultNamer(u8 nError, OSContext* pContext, u32 nDSISR, u32 nDAR) {
    Cpu* pCPU;
    CpuFunction* pFunction;
    Ram* pRAM;

    OSSetErrorHandler(OS_ERR_DSI, NULL);
    OSSetErrorHandler(OS_ERR_ALIGNMENT, NULL);
    OSSetErrorHandler(OS_ERR_PROGRAM, NULL);
    pCPU = gComboCpu;
    if (pCPU == NULL || pCPU == (Cpu*)-1) {
        return;
    }
    pRAM = SYSTEM_RAM((System*)pCPU->pSystem);

    pFunction = comboFindByHost(pCPU, pContext->srr0);

    if (pFunction != NULL) {
        OSReport("combo: fault %d at host %08X is guest %08X..%08X (code %08X, +%X), dar %08X, guest pc %08X, "
                 "mode %08X, jumps %d\n",
                 nError, pContext->srr0, pFunction->nAddress0, pFunction->nAddress1, pFunction->pfCode,
                 pContext->srr0 - (u32)pFunction->pfCode, nDAR, pCPU->nPC, pCPU->nMode, pFunction->nCountJump);

        comboDumpGuest(pFunction->nAddress0, -8, 24, pRAM);
        comboDumpGuest(pFunction->nAddress1, -8, 16, pRAM);
    } else {
        CpuFunction* pBelow = NULL;

        comboFindNearestBelowNode(pCPU->gTree != NULL ? pCPU->gTree->left : NULL, pContext->srr0, 0, &pBelow);
        comboFindNearestBelowNode(pCPU->gTree != NULL ? pCPU->gTree->right : NULL, pContext->srr0, 0, &pBelow);

        if (pBelow != NULL) {
            OSReport("combo: fault %d at host %08X, nearest fn below is guest %08X..%08X (code %08X, host +%X "
                     "of size %X), dar %08X, guest pc %08X, mode %08X\n",
                     nError, pContext->srr0, pBelow->nAddress0, pBelow->nAddress1, pBelow->pfCode,
                     pContext->srr0 - (u32)pBelow->pfCode, pBelow->memory_size, nDAR, pCPU->nPC, pCPU->nMode);
        } else {
            OSReport("combo: fault %d at host %08X not in any compiled function, dar %08X, guest pc %08X, "
                     "mode %08X\n",
                     nError, pContext->srr0, nDAR, pCPU->nPC, pCPU->nMode);
        }
    }

    {
        CpuFunction* pCaller = comboFindByHost(pCPU, pCPU->aGPR[31].u32);

        if (pCaller != NULL) {
            OSReport("combo:   ra %08X is guest %08X..%08X (+%X)\n", pCPU->aGPR[31].u32, pCaller->nAddress0,
                     pCaller->nAddress1, pCPU->aGPR[31].u32 - (u32)pCaller->pfCode);
        } else {
            OSReport("combo:   ra %08X not resolved\n", pCPU->aGPR[31].u32);
        }
    }

    comboDumpGPR(pCPU);
    comboDumpGuest(pCPU->nPC, -8, 16, pRAM);
}

extern s32 gComboFindAddr;
extern s32 gComboFindReason;
extern s32 gComboFindStart;
extern s32 gComboFindEnd;
extern s32 gComboFindStop;

static void comboLogUpdateFail(Cpu* pCPU, s32 nSite) {
    OSReport("combo: cpuExecuteUpdate FAILED site %d, pc %08X mode %08X isMM %d retrace %d/%d treeMem %d\n",
             nSite, pCPU->nPC, pCPU->nMode, pCPU->isMM, pCPU->nRetrace, pCPU->nRetraceUsed,
             pCPU->gTree != NULL ? pCPU->gTree->total_memory : -1);

    if (nSite == 2) {
        CpuFunction* pFunction = pCPU->pFunctionLast;

        if (pFunction != NULL) {
            OSReport("combo:   pFunctionLast %08X range %08X..%08X code %08X\n", pFunction, pFunction->nAddress0,
                     pFunction->nAddress1, pFunction->pfCode);
        } else {
            OSReport("combo:   pFunctionLast NULL\n");
        }

        OSReport("combo:   find: addr %08X reason %d range %08X..%08X stop %08X\n", gComboFindAddr,
                 gComboFindReason, gComboFindStart, gComboFindEnd, gComboFindStop);

        comboDumpGuest(pCPU->nPC, -16, 32, SYSTEM_RAM((System*)pCPU->pSystem));
    }
}
#endif

static inline s32 treeMemory(Cpu* pCPU) {
    if (pCPU->gTree != NULL) {
        return pCPU->gTree->total_memory;
    } else {
        return 0;
    }
}

static inline bool treeKillReason(Cpu* pCPU, s32* value) {
    if (pCPU->survivalTimer < 300) {
        return false;
    }
    if (pCPU->survivalTimer == 300) {
        *value = 1;
        return true;
    }
    if (pCPU->survivalTimer % 400 == 0 && treeMemory(pCPU) > 4200000) {
        *value = pCPU->survivalTimer - 200;
        return true;
    }

    return false;
}

// MM passes a 64-bit OSGetTime() stamp rather than OoT's 32-bit OSGetTick() count.
bool cpuExecuteUpdate(Cpu* pCPU, s32* pnAddressGCN, u64 nTime) {
    s32 nDelta;
    u32 nCounter;
    u32 nCompare;
    u32 nCounterDelta;
    System* pSystem;
    CpuTreeRoot* root;

    pSystem = CPU_SYSTEM(pCPU);

#if IS_MM
    gComboCpu = pCPU;
    if (gComboFaultArmed < 0) {
        gComboFaultArmed = 1;
        OSSetErrorHandler(OS_ERR_DSI, (OSErrorHandler)comboFaultNamer);
        OSSetErrorHandler(OS_ERR_ALIGNMENT, (OSErrorHandler)comboFaultNamer);
        OSSetErrorHandler(OS_ERR_PROGRAM, (OSErrorHandler)comboFaultNamer);
    }

    if (gComboBadStackLeft > 0 && (pCPU->aGPR[29].u32 < 0x80000000 || pCPU->aGPR[29].u32 >= 0x80800000)) {
        gComboBadStackLeft--;
        OSReport("combo: guest sp %08X is outside RDRAM, pc %08X, ra %08X, isMM %d, mode %08X\n",
                 pCPU->aGPR[29].u32, pCPU->nPC, pCPU->aGPR[31].u32, pCPU->isMM, pCPU->nMode);
    }

    if (gComboTraceLeft > 0) {
        s32 nDelta = pCPU->nPC - gComboTracePC;

        if (nDelta >= COMBO_TRACE_REGION || nDelta <= -COMBO_TRACE_REGION) {
            OSReport("combo: pc %08X (previous region x%d)\n", pCPU->nPC, gComboTraceHits);
            gComboTracePC = pCPU->nPC;
            gComboTraceHits = 1;
            gComboTraceLeft--;
        } else if (++gComboTraceHits % COMBO_TRACE_BEAT == 0) {
            OSReport("combo: still around %08X, x%d\n", pCPU->nPC, gComboTraceHits);
        }
    }

    if (COMBO_REPAIR_TASK) {
        Rsp* pRSP = SYSTEM_RSP(pSystem);

        if (pRSP != NULL && pRSP->pDMEM != NULL) {
            u32* pnTask = (u32*)(pRSP->pDMEM + 0xFC0);
            s32 iWord;

            if (pRSP->nAddressSP == 0xFC0 && (pRSP->nSizeGet & 0xFFF) == 0x3F) {
                gComboTaskRamAddr = (u32)pRSP->nAddressRDRAM;
            }

            if ((u32)(pnTask[0] - 1) <= 6) {
                if (pRSP->nAddressSP == 0xFC0) {
                    for (iWord = 0; iWord < 16; iWord++) {
                        gComboShadowTask[iWord] = pnTask[iWord];
                    }

                    gComboShadowValid = true;
                }
            } else {
                u32* pnSource = NULL;
                Ram* pRAM = SYSTEM_RAM(pSystem);

                if (pRAM != NULL && pRAM->pBuffer != NULL && gComboTaskRamAddr != 0 &&
                    gComboTaskRamAddr + 0x40 <= pRAM->nSize) {
                    u32* pnGuest = (u32*)(pRAM->pBuffer + gComboTaskRamAddr);

                    if ((u32)(pnGuest[0] - 1) <= 6) {
                        pnSource = pnGuest;
                    }
                }

                if (pnSource == NULL && gComboShadowValid) {
                    pnSource = gComboShadowTask;
                }

                if (pnSource != NULL) {
                    for (iWord = 0; iWord < 16; iWord++) {
                        pnTask[iWord] = pnSource[iWord];
                    }
                }
            }
        }
    }

#endif

    if (!romUpdate(SYSTEM_ROM(pSystem))) {
#if IS_MM
        comboLogUpdateFail(pCPU, 0);
#endif
        return false;
    }

    // MM drives the RSP through two helpers in rsp.c/helpRVL.c rather than rspUpdate,
    // and ignores their results. Note it reads gpSystem directly here.
    if (!pSystem->bException) {
        if (lbl_801FFF60 == 0) {
            fn_80054B64(0);
        }
        fn_80085C84(gpSystem->apObject[SOT_HELP]);
    }

    root = pCPU->gTree;
    if (root != NULL) {
        treeTimerCheck(pCPU);
        if (pCPU->nRetrace == pCPU->nRetraceUsed && root->kill_number < 12) {
            if (treeKillReason(pCPU, &root->kill_limit)) {
                pCPU->survivalTimer++;
            }
            if (root->kill_limit != 0) {
                treeCleanUp(pCPU, root);
            }
        }
    }

    // Accumulated back into nTime rather than a separate delta local: MWCC then keeps the
    // difference in nTime's callee-saved pair, as the target does. The one remaining
    // difference is the operand order of the commutative addc below.
    if (nTime > pCPU->nTimeLast) {
        nTime = nTime - pCPU->nTimeLast;
    } else {
        nTime = (-1 - pCPU->nTimeLast) + nTime;
    }

    pCPU->nTimeTotal += nTime;
    nCounterDelta = (pCPU->nTimeTotal * 77) / 100;

    if ((pCPU->nMode & 0x40) && pCPU->nRetraceUsed != pCPU->nRetrace) {
        if (viForceRetrace(SYSTEM_VI(pSystem), 1)) {
            nDelta = pCPU->nRetrace - pCPU->nRetraceUsed;
            if (nDelta < 0) {
                nDelta = -nDelta;
            }

            if (nDelta < 4) {
                pCPU->nRetraceUsed++;
            } else {
                pCPU->nRetraceUsed = pCPU->nRetrace;
            }
        }
    }

#if IS_MM
    if (gComboSwitching && pCPU->nRetrace == pCPU->nRetraceUsed) {
        gComboSwitching = false;
        OSReport("combo: retrace counters back in sync at %d, switch window closed\n", pCPU->nRetrace);
    }
#endif

    if (pCPU->nMode & 1) {
        nCounter = pCPU->anCP0[9];
        nCompare = pCPU->anCP0[11];
        if (nCounterDelta >= nCounter) {
            if (nCounter < nCompare && nCounterDelta >= nCompare) {
                pCPU->nMode &= ~1;
                xlObjectEvent(CPU_SYSTEM(pCPU), 0x1000, (void*)3);
            }
        } else if (nCounter < nCompare) {
            pCPU->nMode &= ~1;
            xlObjectEvent(CPU_SYSTEM(pCPU), 0x1000, (void*)3);
        }
    }

    pCPU->anCP0[9] = nCounterDelta;

    if ((pCPU->nMode & 8) && !(pCPU->nMode & 4) && gpSystem->bException) {
        if (!systemCheckInterrupts(gpSystem)) {
#if IS_MM
            comboLogUpdateFail(pCPU, 1);
#endif
            return false;
        }
    }

    if (pCPU->nMode & 4) {
        pCPU->nMode &= ~0x84;
        if (!cpuFindAddress(pCPU, pCPU->nPC, pnAddressGCN)) {
#if IS_MM
            comboLogUpdateFail(pCPU, 2);
#endif
            return false;
        }
    }
    return true;
}
