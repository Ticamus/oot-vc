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

//! Debug only. Entering the switch stub arms a bounded trace of every indirect jump the
//! payload takes, so a hang can be located without a hardware debugger. A tight MMIO poll
//! (`waitForPi`) lives inside a single recompiled block and never reaches pfJump, so the
//! last line printed is the function the guest is stuck in. Set to 0 to disable.
#define COMBO_TRACE_JUMPS 400
s32 gComboTraceLeft = 0;

//! See the declaration in system.h.
bool gComboSwitching = false;
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

        //! Not in the original game. comboGameSwitch2 parks the stack in KSEG1 as well
        //! (`la sp,0xa0800000`) right before that jump. cpuGetPPC's $sp fast path -- nFlagRAM
        //! bit 29, seeded in cpuMakeFunction -- compiles a stack access to
        //! `add r7,$sp,rRamOffset` + `stw rX,imm(r7)` without folding bit 29 away, so
        //! comboGameSwitch3's `sw ra,0x10(sp)` resolved to 0xA1732848 and took a DSI. Only the
        //! generic pfRam path masks the address with 0xDFFFFFFF, and cpuGetPPC is not linked
        //! from source in this build, so fold the stack pointer here instead. The payload uses
        //! it purely as scratch stack, and the emulator backs both aliases with the same RDRAM
        //! buffer, so the uncached view buys nothing under emulation.
        if ((pCPU->aGPR[29].u32 & 0xE0000000) == 0xA0000000) {
            OSReport("combo: switch stub entered, target %08X, sp %08X -> %08X\n", nAddressN64,
                     pCPU->aGPR[29].u32, (pCPU->aGPR[29].u32 & 0x1FFFFFFF) | 0x80000000);
            pCPU->aGPR[29].s64 = (s32)((pCPU->aGPR[29].u32 & 0x1FFFFFFF) | 0x80000000);
            gComboTraceLeft = COMBO_TRACE_JUMPS;

            //! An uncached stack pointer is unique to comboGameSwitch2, so this is the point
            //! where the payload takes over. See gComboSwitching in system.h.
            gComboSwitching = true;
        }

        //! Debug only, see COMBO_TRACE_JUMPS. aGPR[31] is deliberately not printed: the
        //! emulator parks host addresses there (cpuExecuteCall stores nAddressGCN, and
        //! cpuExecuteOpcode swaps in nReturnAddrLast), so it is not the guest's $ra.
        if (gComboTraceLeft > 0) {
            gComboTraceLeft--;
            OSReport("combo: jump %08X (sp %08X)\n", nAddressN64, pCPU->aGPR[29].u32);
        }
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

        //! gComboSwitching is deliberately NOT cleared here. The payload is done, but the VI is
        //! still the dead one waitSubsystems() left behind: viForceRetrace() needs
        //! pVI->nStatus & 3 and returns false until the incoming game reprograms VI_CONTROL_REG,
        //! so cpuExecuteUpdate() cannot close the retrace gap that built up during the switch,
        //! and every callback-driven romCopy() stays deferred. The incoming game's boot DMAs its
        //! own code, so clearing the flag here deadlocks it: it cannot reach the VI setup that
        //! would let the copy complete. cpuExecuteUpdate() clears the flag once the counters
        //! agree again.

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
