#include "emulator/cpu.h"
#include "emulator/ram.h"
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

    //! Not in the original game, see COMBO_REPAIR_TASK.
    if (COMBO_REPAIR_TASK) {
        Rsp* pRSP = SYSTEM_RSP(pSystem);

        if (pRSP != NULL && pRSP->pDMEM != NULL) {
            u32* pnTask = (u32*)(pRSP->pDMEM + 0xFC0);
            s32 iWord;

            //! Remember where in RDRAM the guest's own OSTask lives. Unconditional on the type, unlike
            //! the snapshot below: this address is wanted even when the copy in DMEM is already trampled.
            //!
            //! The length test is not decoration. rspPut32 fills these three fields from three separate
            //! guest stores -- SP_MEM_ADDR, then SP_DRAM_ADDR, then SP_RD_LEN, which is the one that
            //! performs the copy -- so sampling on the address alone catches half-built transfers whose
            //! nAddressRDRAM still belongs to the previous DMA, and a repair then reads a descriptor out
            //! of unrelated guest memory. A descriptor load is always 0x40 bytes, so requiring RD_LEN
            //! 0x3F pins all three registers to the same transfer.
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
