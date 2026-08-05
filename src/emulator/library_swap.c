//! osViSwapBuffer_Entry, carved out of mm-j's library.c during the OoT pause-freeze investigation and
//! kept afterwards. The instrumentation it existed for is gone; what remains is a byte-exact
//! reimplementation (verified 104/104), so it is equivalent to leaving the function in the extracted
//! object, and it can be folded back into library.c in config/mm-j/splits.txt if the split count ever
//! matters. Both hypotheses it was written to test were cleared -- see docs/ootmm_pause_freeze.md.
//!
//! Worth knowing anyway: this is an HLE hook bound by libraryTestFunction to the game's osViSwapBuffer,
//! and it is the only thing in the whole DOL that calls rspFrameComplete. It signals a completed frame
//! **only when the framebuffer address the game passes differs from the previous call**, remembered in a
//! function-static.
//!
//! The body is oot-j's, written against CPU_SYSTEM(pCPU) rather than gpSystem because that is what
//! mm-j's does -- it reaches the System through pCPU+0x18, then apObject[SOT_LIBRARY] at +0x68 and
//! apObject[SOT_RSP] at +0x38. Offsets confirmed against the corrected headers: Cpu.aGPR at 0x58
//! puts aGPR[4].u32 at 0x7C and aGPR[29].s32 at 0x144, and Library.nAddStackSwap is at 0x08, all of
//! which match the retail disassembly.

#include "emulator/cpu.h"
#include "emulator/library.h"
#include "emulator/rsp.h"
#include "emulator/system.h"
#include "macros.h"
#include "revolution/os.h"

bool rspFrameComplete(Rsp* pRSP);

// Same definition cpu.c uses for MM: the owning System is cached in the Cpu at 0x18.
#define CPU_SYSTEM(pCPU) ((System*)(pCPU)->pSystem)

bool osViSwapBuffer_Entry(Cpu* pCPU) {
    static u32 nAddress = 0xFFFFFFFF;

    pCPU->aGPR[29].s32 += SYSTEM_LIBRARY(CPU_SYSTEM(pCPU))->nAddStackSwap;

    if (nAddress != pCPU->aGPR[4].u32) {
        nAddress = pCPU->aGPR[4].u32;
        if (!rspFrameComplete(SYSTEM_RSP(CPU_SYSTEM(pCPU)))) {
            return false;
        }
    }

    return true;
}
