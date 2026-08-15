#include "emulator/cpu.h"
#include "emulator/comboPerf.h"
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

#if IS_MM && COMBO_DEBUG_HOOKS
// Debug only. Armed by cpu_execute_jump.c on OoTMM switch entry; every interpreter hook funnels
// through this function.
#define COMBO_TRACE_REGION 0x40
#define COMBO_TRACE_BEAT 200000
extern s32 gComboTraceLeft;
static s32 gComboTracePC = 0;
static s32 gComboTraceHits = 0;
#endif

#if IS_MM
// Fixes the OoT pause freeze together with the orphan recovery in rsp_update.c. 
// retail rspParseGBI_F3DEX2 copies display-list data into
// pDMEM+0xFC0 with an unbounded length, and 0xFC0 is where libultra keeps the OSTask descriptor.
// OoT's pause menu drives that path and can trample it. The fix restores the descriptor from the
// guest's own OSTask in RDRAM, which the parser never touches.
#define COMBO_REPAIR_TASK 1

// Only OoT's display lists trample the descriptor, so retail mm-j should not get a repair.
#define COMBO_REPAIR_ARMED(pCPU) gIsOotmmCombo

// Shadow is only read once the authoritative RDRAM source is unknown.
#define COMBO_SHADOW_WANTED(pRSP) (gComboTaskRamAddr == 0 && (pRSP)->nAddressSP == 0xFC0)

// RDRAM address of the guest's own OSTask, captured from the descriptor load's DMA parameters.
// Global so rsp_update.c can dispatch from the same source instead of a DMEM snapshot.
u32 gComboTaskRamAddr;

// Fallback for the window before any descriptor load has been seen. Initialised, so it lands in
// .data rather than .bss: this TU's split claims only .text.
u32 gComboShadowTask[16] = {1};
bool gComboShadowValid;
#endif

#define CPU_SYSTEM(pCPU) ((System*)(pCPU)->pSystem)

#if IS_MM
// A fault in recompiled code reports a host code-cache PC, which says nothing about the guest.
// comboFaultNamer names the guest function instead, and uninstalls itself before returning so the
// default handler still prints its usual dump on the re-fault.
Cpu* gComboCpu = (Cpu*)-1;

#if COMBO_DEBUG_HOOKS
s32 gComboBadStackLeft = 4;   // debug only, bounded; see the stack check in cpuExecuteUpdate
s32 gComboRaProbeLeft = 12;   // debug only, bounded; see the saved-$ra slot probe in cpuExecuteUpdate
s32 gComboMergedLeft = 2;     // debug only, bounded; see the merged-node alarm in cpuExecuteUpdate
#endif

#if COMBO_FAULT_NAMER
static CpuFunction* comboFindByHostNode(CpuFunction* pNode, u32 nHost, s32 nDepth) {
    CpuFunction* pFound;

    // Bounded so a corrupt tree can't take the exception handler with it. 400 not 64: the tree is
    // a BST keyed on nAddress0 and inserts roughly in address order, so it's skewed well past a
    // balanced log2(N).
    if (pNode == NULL || nDepth > 400) {
        return NULL;
    }

    COMBO_PERF_BUMP(nHostNodes);

    if (pNode->pfCode != NULL && nHost >= (u32)pNode->pfCode &&
        nHost < (u32)pNode->pfCode + (u32)pNode->memory_size) {
        return pNode;
    }

    if ((pFound = comboFindByHostNode(pNode->left, nHost, nDepth + 1)) != NULL) {
        return pFound;
    }
    return comboFindByHostNode(pNode->right, nHost, nDepth + 1);
}

// The compiled function whose host code starts closest below nHost, for when exact containment fails.
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

// Which compiled function, if any, owns a host address. Only the fault namer uses this; see
// comboHostIsCode below for the hot-path predicate that replaced it elsewhere.
#pragma dont_inline on
static CpuFunction* comboFindByHost(Cpu* pCPU, u32 nHost) {
    CpuFunction* pFound;

    if (pCPU == NULL || pCPU == (Cpu*)-1 || pCPU->gTree == NULL) {
        return NULL;
    }

    if ((pFound = comboFindByHostNode(pCPU->gTree->left, nHost, 0)) != NULL) {
        return pFound;
    }
    return comboFindByHostNode(pCPU->gTree->right, nHost, 0);
}
#pragma dont_inline off
#endif

// Sizes of the two block heaps cpuHeapTake carves code from, and their block strides.
#define COMBO_HEAP1_BLOCK 0x200
#define COMBO_HEAP2_BLOCK 0xA00
#define COMBO_HEAP1_SPAN (ARRAY_COUNT(((Cpu*)0)->aHeap1Flag) * 32 * COMBO_HEAP1_BLOCK)
#define COMBO_HEAP2_SPAN (ARRAY_COUNT(((Cpu*)0)->aHeap2Flag) * 32 * COMBO_HEAP2_BLOCK)

// Replaces the O(N) tree walk cpu_execute_jump.c used to do on every out-of-RDRAM indirect jump.
// pfCode is a block-heap base and aHeap1Flag/aHeap2Flag say which blocks are allocated, so "inside
// an allocated code block" is a couple of compares and a bit test. A heapID-3 chunk (too large for
// either block heap) falls back to the walk.
bool comboHostIsCode(Cpu* pCPU, u32 nHost) {
    u32 nOffset;
    u32 iBlock;

    if (pCPU == NULL || pCPU == (Cpu*)-1) {
        return false;
    }

    if (pCPU->gHeap1 != NULL && (nOffset = nHost - (u32)pCPU->gHeap1) < COMBO_HEAP1_SPAN) {
        iBlock = nOffset / COMBO_HEAP1_BLOCK;
        return (pCPU->aHeap1Flag[iBlock >> 5] & (1 << (iBlock & 31))) != 0;
    }

    if (pCPU->gHeap2 != NULL && (nOffset = nHost - (u32)pCPU->gHeap2) < COMBO_HEAP2_SPAN) {
        iBlock = nOffset / COMBO_HEAP2_BLOCK;
        return (pCPU->aHeap2Flag[iBlock >> 5] & (1 << (iBlock & 31))) != 0;
    }

    if (((nHost ^ (u32)pCPU->gHeap1) & 0xFF000000) == 0 || ((nHost ^ (u32)pCPU->gHeap2) & 0xFF000000) == 0) {
        COMBO_PERF_BUMP(nHostSlow);
#if COMBO_FAULT_NAMER
        return comboFindByHost(pCPU, nHost) != NULL;
#else
        return false;
#endif
    }

    return false;
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

#if COMBO_FAULT_NAMER
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
            comboDumpGuest(pCPU->aGPR[31].u32, -8, 8, pRAM);
        }
    }

    comboDumpGPR(pCPU);
    comboDumpGuest(pCPU->nPC, -8, 16, pRAM);
    comboDumpGuest(pCPU->aGPR[29].u32, 0, 24, pRAM);
}

// Installs comboFaultNamer and publishes the Cpu it reports on. Called from cpuReset.
void comboFaultArm(Cpu* pCPU) {
    gComboCpu = pCPU;
    OSSetErrorHandler(OS_ERR_DSI, (OSErrorHandler)comboFaultNamer);
    OSSetErrorHandler(OS_ERR_ALIGNMENT, (OSErrorHandler)comboFaultNamer);
    OSSetErrorHandler(OS_ERR_PROGRAM, (OSErrorHandler)comboFaultNamer);
}
#endif

#if COMBO_DEBUG_HOOKS
s32 gComboRaZeroLeft = 8; // debug only, bounded; see comboProbeRaZero
#endif

extern s32 gComboFindAddr;
extern s32 gComboFindReason;
extern s32 gComboFindStart;
extern s32 gComboFindEnd;
extern s32 gComboFindStop;

// One guest word out of RDRAM, or 0 when the address is not backed by it.
static u32 comboGuestWord(Ram* pRAM, u32 nGuest) {
    u32 nOffset = nGuest & 0x1FFFFFFF;

    if (pRAM == NULL || pRAM->pBuffer == NULL || (nGuest & 3) != 0 || nOffset + 4 > (u32)pRAM->nSize) {
        return 0;
    }
    return *(u32*)(pRAM->pBuffer + nOffset);
}

// Bounded probe for the OoTMM "Farore's Wind" crash: `jr $ra` executed with aGPR[31] == 0, which
// becomes a host branch to address 0 with no trail back to the branching site. A return address of
// 0 is never legitimate, so report the first boundaries where it's already 0, with the executing
// function named.
#if COMBO_DEBUG_HOOKS
#pragma dont_inline on
static void comboProbeRaZero(Cpu* pCPU, Ram* pRAM) {
    CpuFunction* pFunction = pCPU->pFunctionLast;
    u32 nEntry = pCPU->nPC - 4;

    gComboRaZeroLeft--;
    OSReport("combo: ra==0 at pc %08X retlast %08X calllast %08X sp %08X mode %08X fn %08X..%08X\n", pCPU->nPC,
             pCPU->nReturnAddrLast, pCPU->nCallLast, pCPU->aGPR[29].u32, pCPU->nMode,
             pFunction != NULL ? pFunction->nAddress0 : 0, pFunction != NULL ? pFunction->nAddress1 : 0);

    OSReport("combo:   entry %08X: %08X %08X %08X\n", nEntry, comboGuestWord(pRAM, nEntry),
             comboGuestWord(pRAM, nEntry + 4), comboGuestWord(pRAM, nEntry + 8));

    comboDumpGuest(pCPU->aGPR[29].u32, 0, 16, pRAM);
}
#pragma dont_inline off
#endif

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

// Kept out of line so `-inline auto` doesn't pull these back into cpuExecuteUpdate and cost its
// register budget.
#pragma dont_inline on

// Fallback snapshot of a valid in-DMEM descriptor, for the window before gComboTaskRamAddr is known.
static void comboShadowTask(Rsp* pRSP) {
    u32* pnTask = (u32*)(pRSP->pDMEM + 0xFC0);
    s32 iWord;

    for (iWord = 0; iWord < 16; iWord++) {
        gComboShadowTask[iWord] = pnTask[iWord];
    }

    gComboShadowValid = true;
    COMBO_PERF_BUMP(nShadow);
}

// Put the descriptor back from the guest's own OSTask in RDRAM, or from the snapshot if that is not
// available yet. Reached when OSTask.type in DMEM is outside 1..7, i.e. the parser overwrote it.
static void comboRepairTask(Cpu* pCPU, Rsp* pRSP) {
    u32* pnTask = (u32*)(pRSP->pDMEM + 0xFC0);
    u32* pnSource = NULL;
    Ram* pRAM = SYSTEM_RAM((System*)pCPU->pSystem);
    s32 iWord;

    if (pRAM != NULL && pRAM->pBuffer != NULL && gComboTaskRamAddr != 0 &&
        gComboTaskRamAddr + 0x40 <= pRAM->nSize) {
        u32* pnGuest = (u32*)(pRAM->pBuffer + gComboTaskRamAddr);

        if ((u32)(pnGuest[0] - 1) <= 6) {
            pnSource = pnGuest;
        }
    }

    if (pnSource == NULL && gComboShadowValid) {
        COMBO_PERF_BUMP(nShadowUsed);
        pnSource = gComboShadowTask;
    }

    if (pnSource != NULL) {
        for (iWord = 0; iWord < 16; iWord++) {
            pnTask[iWord] = pnSource[iWord];
        }

        COMBO_PERF_BUMP(nRepairs);

        if (pCPU->isMM) {
            COMBO_PERF_BUMP(nRepairsMM);
        }
    }
}

#pragma dont_inline off
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
    COMBO_PERF_BUMP(nBoundaries);
#endif

#if IS_MM && COMBO_DEBUG_HOOKS
    // Debug only, bounded. Validates the COMBO_TAIL_CALL_TARGET fix in cpu_find_function.c: the
    // payload helper at 0x8074EC14 must be the start of its own node, not merely contained by a
    // merged one.
    if (gComboMergedLeft > 0 && pCPU->pFunctionLast != NULL && pCPU->pFunctionLast->nAddress0 < 0x8074EC14 &&
        pCPU->pFunctionLast->nAddress1 > 0x8074EC14) {
        gComboMergedLeft--;
        OSReport("combo: node %08X..%08X still swallows 8074EC14, boundary fix did not take\n",
                 pCPU->pFunctionLast->nAddress0, pCPU->pFunctionLast->nAddress1);
    }

    // Debug only, bounded. Tracks when the saved-$ra slot of the payload helper at 0x8074EC14 gets
    // corrupted, by walking guest sp+0x44 forward across the helper's four `jal`s.
    if (gComboRaProbeLeft > 0 &&
        (pCPU->nReturnAddrLast == 0x8074EC38 || pCPU->nReturnAddrLast == 0x8074EC64 ||
         pCPU->nReturnAddrLast == 0x8074EC80 || pCPU->nReturnAddrLast == 0x8074ECA0)) {
        Ram* pRAM = SYSTEM_RAM(pSystem);
        u32 nSlotAddr = (pCPU->aGPR[29].u32 + 0x44) & 0x1FFFFFFF;
        u32 nSlot = 0;

        if (pRAM != NULL && pRAM->pBuffer != NULL && nSlotAddr + 4 <= (u32)pRAM->nSize) {
            nSlot = *(u32*)(pRAM->pBuffer + nSlotAddr);
        }

        if (gComboRaProbeLeft == 12 || nSlot < 0x80800000) {
            gComboRaProbeLeft--;
            OSReport("combo: ra probe slot %08X retlast %08X ra %08X sp %08X fn %08X\n", nSlot,
                     pCPU->nReturnAddrLast, pCPU->aGPR[31].u32, pCPU->aGPR[29].u32,
                     pCPU->pFunctionLast != NULL ? pCPU->pFunctionLast->nAddress0 : 0);
        }
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
#endif

#if IS_MM
    // `nReturnAddrLast != 0` excludes cold boot, where $ra == 0 is correct.
#if COMBO_DEBUG_HOOKS
    if (gComboRaZeroLeft > 0 && pCPU->aGPR[31].u32 == 0 && pCPU->nReturnAddrLast != 0 && gIsOotmmCombo) {
        comboProbeRaZero(pCPU, SYSTEM_RAM(pSystem));
    }
#endif

    if (COMBO_REPAIR_TASK && COMBO_REPAIR_ARMED(pCPU)) {
        Rsp* pRSP = SYSTEM_RSP(pSystem);

        if (pRSP != NULL && pRSP->pDMEM != NULL) {
            u32 nType = *(u32*)(pRSP->pDMEM + 0xFC0);

            // Must stay on the boundary path: nAddressSP/nSizeGet/nAddressRDRAM are sticky, so
            // sampling them later would pick up whatever DMA came next instead.
            if (pRSP->nAddressSP == 0xFC0 && (pRSP->nSizeGet & 0xFFF) == 0x3F) {
                gComboTaskRamAddr = (u32)pRSP->nAddressRDRAM;
            }

            if ((u32)(nType - 1) > 6) {
                comboRepairTask(pCPU, pRSP);
            } else if (COMBO_SHADOW_WANTED(pRSP)) {
                comboShadowTask(pRSP);
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
                COMBO_PERF_BUMP(nKills);
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
