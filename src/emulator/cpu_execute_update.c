#include "emulator/cpu.h"
#include "emulator/rom.h"
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
#endif

#define CPU_SYSTEM(pCPU) ((System*)(pCPU)->pSystem)

// Both helpers are inlined into cpuExecuteUpdate by MWCC and are used nowhere else, so
// they travel with it rather than staying behind in cpu.c.
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
    //! Debug only, see COMBO_TRACE_REGION.
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

    if (!romUpdate(SYSTEM_ROM(pSystem))) {
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
    //! Not in the original game. An OoTMM combo game switch has just called treeKill(), so
    //! the tree is gone. Skip the collector this once: there is nothing to collect, and
    //! letting treeCleanUp() run would risk killing the incoming game's entry function
    //! between the cpuFindAddress() below compiling it and the caller using it.
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
    //! Not in the original game. This is the point where the incoming game's VI has come back
    //! to life: the block above only advances nRetraceUsed once viForceRetrace() succeeds, which
    //! needs the guest to have reprogrammed VI_CONTROL_REG after waitSubsystems() zeroed it.
    //! Until then the retrace gap cannot close, and romCopyUpdate() defers every callback-driven
    //! copy -- which is why the switch window has to stay open across the incoming game's early
    //! boot rather than ending at comboGameSwitch4's jump. See gComboSwitching in system.h.
    if (gComboSwitching && pCPU->nRetrace == pCPU->nRetraceUsed) {
        gComboSwitching = false;
        OSReport("combo: retrace counters back in sync at %d, switch window closed\n", pCPU->nRetrace);
    }
#endif

    if (pCPU->nMode & 1) {
        nCounter = pCPU->anCP0[9];
        nCompare = pCPU->anCP0[11];
        // MM compares the delta itself rather than nCounter + delta, and reaches the
        // System through pCPU rather than the cached local.
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

    // MM assigns the delta rather than accumulating into the counter.
    pCPU->anCP0[9] = nCounterDelta;

    // Like the RSP block above, this one goes through the gpSystem global rather than
    // the cached local.
    if ((pCPU->nMode & 8) && !(pCPU->nMode & 4) && gpSystem->bException) {
        if (!systemCheckInterrupts(gpSystem)) {
            return false;
        }
    }

    if (pCPU->nMode & 4) {
        pCPU->nMode &= ~0x84;
        if (!cpuFindAddress(pCPU, pCPU->nPC, pnAddressGCN)) {
            return false;
        }
    }
    return true;
}
