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
#endif

s32 cpuExecuteJump(Cpu* pCPU, s32 nCount, s32 nAddressN64, s32 nAddressGCN) {
    s64 curTime = OSGetTime();

#if IS_MM
    //! Not in the original game. comboGameSwitch2 in the combo's payload jumps through a
    //! KSEG1 alias (comboGameSwitch3 + 0x20000000). The recompiler passes the raw register
    //! value straight here, so fold any segment alias back to KSEG0 before it is used as a
    //! lookup key. The GameCube version instead emits the same masking into the compiled
    //! code, ahead of the branch to pfJump; doing it here needs no change to cpuGetPPC,
    //! which is not linked from source in this build. No-op for the KSEG0 addresses
    //! everything else jumps to.
    if (gIsOotmmCombo) {
        nAddressN64 = (nAddressN64 & 0x1FFFFFFF) | 0x80000000;
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
    //! Not in the original game. comboGameSwitch4 ends on `jr a0` into the foreign game's
    //! raw entrypoint, having abandoned the outgoing game's call stack. That is the one safe
    //! moment to rebuild the emulator's state, so recognise the jump here.
    if (gIsOotmmCombo &&
        ((nAddressN64 == COMBO_MM_ENTRY && !pCPU->isMM) || (nAddressN64 == COMBO_OOT_ENTRY && pCPU->isMM))) {
        pCPU->isMM = nAddressN64 == COMBO_MM_ENTRY;
        OSReport("combo: game switch, jump to %08X (isMM=%d)\n", nAddressN64, pCPU->isMM);

        if (!comboEmulatorSwitchFix(pCPU)) {
            return 0;
        }

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
