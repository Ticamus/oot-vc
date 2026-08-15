#include "emulator/comboPerf.h"
#include "emulator/cpu.h"
#include "emulator/system.h"
#include "macros.h"

// Defined in cpu.c; marked scope:global in config/mm-j/symbols.txt so this unit can
// reference it across the split.
bool cpuExecuteUpdate(Cpu* pCPU, s32* pnAddressGCN, u64 nTime);

#if IS_MM
// Raw entrypoints the OoTMM combo's payload jumps to; see comboEmulatorSwitchFix() in
// system.c for what the switch has to put back together.
#define COMBO_OOT_ENTRY 0x80000400
#define COMBO_MM_ENTRY 0x80080000

// Debug only. Arms a bounded trace of every indirect jump the payload takes, for locating a hang
// without a hardware debugger.
#define COMBO_TRACE_JUMPS 400
s32 gComboTraceLeft = 0;

#if COMBO_DEBUG_HOOKS
#define COMBO_ARM_TRACE() (gComboTraceLeft = COMBO_TRACE_JUMPS)
#else
#define COMBO_ARM_TRACE() ((void)0)
#endif

bool gComboSwitching = false; // see the declaration in system.h

// Defined in cpu_execute_update.c: whether a host address is inside an allocated code block.
bool comboHostIsCode(Cpu* pCPU, u32 nHost);

#if COMBO_DEBUG_HOOKS
s32 gComboHostJumpsLeft = 20; // debug only, bounded
#endif
#endif

s32 cpuExecuteJump(Cpu* pCPU, s32 nCount, s32 nAddressN64, s32 nAddressGCN) {
    s64 curTime = OSGetTime();

    COMBO_PERF_BUMP(nJumps);

#if IS_MM
    if (gIsOotmmCombo && ((u32)nAddressN64 < 0x80000000 || (u32)nAddressN64 >= 0x80800000) &&
        comboHostIsCode(pCPU, (u32)nAddressN64)) {
        COMBO_PERF_BUMP(nHostJumps);
#if COMBO_DEBUG_HOOKS
        if (gComboHostJumpsLeft > 0) {
            gComboHostJumpsLeft--;
            OSReport("combo: pfJump got host address %08X, returning it as the host target\n", nAddressN64);
        }
#endif
        return nAddressN64;
    }

    // comboGameSwitch2 jumps through a KSEG1 alias (comboGameSwitch3 + 0x20000000); fold it back to
    // KSEG0 before using it as a lookup key. No-op for ordinary KSEG0 addresses.
    if (gIsOotmmCombo) {
        nAddressN64 = (nAddressN64 & 0x1FFFFFFF) | 0x80000000;

        // comboGameSwitch2 also parks $sp in KSEG1 (`la sp,0xa0800000`); fold it here too. Both
        // aliases share the same RDRAM buffer, so nothing is lost under emulation.
        if ((pCPU->aGPR[29].u32 & 0xE0000000) == 0xA0000000) {
            OSReport("combo: switch stub entered, target %08X, sp %08X -> %08X\n", nAddressN64,
                     pCPU->aGPR[29].u32, (pCPU->aGPR[29].u32 & 0x1FFFFFFF) | 0x80000000);
            pCPU->aGPR[29].s64 = (s32)((pCPU->aGPR[29].u32 & 0x1FFFFFFF) | 0x80000000);
            COMBO_ARM_TRACE();
            gComboSwitching = true; // an uncached $sp is unique to comboGameSwitch2
        }

#if COMBO_DEBUG_HOOKS
        // aGPR[31] deliberately not printed: the emulator parks host addresses there, not the guest's $ra.
        if (gComboTraceLeft > 0) {
            gComboTraceLeft--;
            OSReport("combo: jump %08X (sp %08X)\n", nAddressN64, pCPU->aGPR[29].u32);
        }
#endif
    }
#endif

    if (pCPU->nWaitPC != 0) {
        pCPU->nMode |= 8;
    } else {
        pCPU->nMode &= ~8;
    }

    pCPU->nMode |= 4;
    pCPU->nPC = nAddressN64;

#if IS_MM
    // comboGameSwitch4 ends on `jr a0` into the other game's raw entrypoint, having abandoned the
    // outgoing game's call stack so the one safe moment to rebuild emulator state.
    if (gIsOotmmCombo &&
        ((nAddressN64 == COMBO_MM_ENTRY && !pCPU->isMM) || (nAddressN64 == COMBO_OOT_ENTRY && pCPU->isMM))) {
        pCPU->isMM = nAddressN64 == COMBO_MM_ENTRY;
        OSReport("combo: game switch, jump to %08X (isMM=%d)\n", nAddressN64, pCPU->isMM);

        if (!comboEmulatorSwitchFix(pCPU)) {
            return 0;
        }

        // gComboSwitching deliberately NOT cleared here: the VI is still the dead one waitSubsystems()
        // left behind, so clearing now would deadlock the incoming game's boot DMA.
        // cpuExecuteUpdate() clears it once the retrace counters agree again.

        // Deliberately not rebuilding the tree or compiling the entrypoint here: gTree is
        // NULL, and cpuExecuteUpdate() below both skips its collector for that reason and
        // then resolves nPC through cpuFindAddress(), which takes the cold boot path.
        // Compiling here instead would expose the fresh function to that collector.
    }
#endif

    if (!cpuExecuteUpdate(pCPU, &nAddressGCN, curTime)) {
        return false;
    }

    pCPU->nTimeLast = curTime;
    return nAddressGCN;
}
