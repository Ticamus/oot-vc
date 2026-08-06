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

// Defined in cpu_execute_update.c: which compiled function owns a host address, or NULL.
CpuFunction* comboFindByHost(Cpu* pCPU, u32 nHost);

//! Debug only, bounded. If this fires at all the `jr $ra` convention was broken somewhere, so the count
//! is worth seeing even though the recovery is sound.
s32 gComboHostJumpsLeft = 20;
#endif

s32 cpuExecuteJump(Cpu* pCPU, s32 nCount, s32 nAddressN64, s32 nAddressGCN) {
    s64 curTime = OSGetTime();

#if IS_MM
    //! Not in the original game, and it fixes a whole class rather than one symptom. Read cpuGetPPC's
    //! `jr` case to see why -- the GameCube version of this port has it in source (`_cpuGCN.c`, the
    //! `case 0x08` of the special switch), and mm-j's asm does the same thing at cpuGetPPC+0x1438,
    //! testing `nFlagCODE & 2` with `rlwinm. r3,r0,0,30,30`:
    //!
    //!     if (MIPS_RS(nOpcode) == 31 && !(pCPU->nFlagCODE & 2)) {
    //!         mtlr rA; blr;                       // a HOST return, straight to compiled code
    //!     } else {
    //!         rlwinm r5,r5,0,3,31; oris r5,r5,0x8000; bl pfJump;   // a GUEST jump
    //!     }
    //!
    //! So `aGPR[31]` holds a **host** address by convention -- `jalr` stores `&anCode[iCode] + 20` into
    //! it, and `sw ra`/`lw ra` round-trip that host value through the guest's own stack. Whenever a
    //! `jr $ra` is compiled through the pfJump side -- because `nFlagCODE & 2` was set for that pass, or
    //! because the register was reached by a `jalr` -- a host address arrives here as `nAddressN64`, and
    //! every lookup downstream treats it as guest. Measured earlier at `pc 80EE2858 outside RDRAM`.
    //!
    //! The recovery is exact rather than defensive: if the address is not in RDRAM but *is* inside a
    //! compiled function's code, it is a host return point, and jumping there is precisely what
    //! `mtlr` + `blr` would have done -- returning it unchanged does that, since this function's return
    //! value IS the host address the link stub jumps to. Requiring it to land inside a known code block
    //! keeps a genuinely corrupt target from being waved through. Comes before the KSEG fold below so a
    //! host address is never mistaken for a guest segment alias.
    if (gIsOotmmCombo && ((u32)nAddressN64 < 0x80000000 || (u32)nAddressN64 >= 0x80800000) &&
        comboFindByHost(pCPU, (u32)nAddressN64) != NULL) {
        if (gComboHostJumpsLeft > 0) {
            gComboHostJumpsLeft--;
            OSReport("combo: pfJump got host address %08X, returning it as the host target\n", nAddressN64);
        }
        return nAddressN64;
    }

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
