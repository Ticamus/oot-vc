#include "emulator/comboPerf.h"
#include "emulator/cpu.h"
#include "emulator/rom.h"
#include "emulator/system.h"
#include "emulator/vc64_RVL.h"
#include "emulator/vi.h"
#include "macros.h"

// Must be linked: raising ROM_BLOCK_COUNT to 8192 for the 64 MB combo image pushes every Rom field
// after aBlock down by 0x10000, and this function's `pROM->copy.nSize` test is a fixed load from
// Rom+0x10E34 in the extracted object.

bool cpuExecuteUpdate(Cpu* pCPU, s32* pnAddressGCN, u64 nTime);

// MM reaches its System through a back-pointer stored in Cpu, where OoT uses the
// gpSystem global. Only usable where a Cpu* is in scope.
#if IS_MM
#define CPU_SYSTEM(pCPU) ((System*)(pCPU)->pSystem)
#else
#define CPU_SYSTEM(pCPU) (gpSystem)
#endif

#if IS_MM
#define VI_FORCE_RETRACE(pVI, nUnknown) viForceRetrace((pVI), (nUnknown))
#else
#define VI_FORCE_RETRACE(pVI, nUnknown) viForceRetrace(pVI)
#endif

s32 cpuExecuteIdle(Cpu* pCPU, s32 nCount, s32 nAddressN64, s32 nAddressGCN) {
#if IS_MM
    u64 idleTime;
#endif
    Rom* pROM;

    COMBO_PERF_BUMP(nIdles); // how much of a frame the guest spends idle-waiting

    pROM = SYSTEM_ROM(CPU_SYSTEM(pCPU));

#if IS_MM
    if (!fn_80007280(0, 0, 0, 1)) {
        return false;
    }

    idleTime = OSGetTime();
#else
    nCount = OSGetTick();
#endif
    if (pCPU->nWaitPC != 0) {
        pCPU->nMode |= 8;
    } else {
        pCPU->nMode &= ~8;
    }

    pCPU->nMode |= 0x80;
    pCPU->nPC = nAddressN64;
    if (!(pCPU->nMode & 0x40) && pROM->copy.nSize == 0) {
        VI_FORCE_RETRACE(SYSTEM_VI(CPU_SYSTEM(pCPU)), 0);
    }

#if IS_MM
    if (!cpuExecuteUpdate(pCPU, &nAddressGCN, idleTime)) {
        return false;
    }

    pCPU->nTimeLast = idleTime;
#else
    if (!cpuExecuteUpdate(pCPU, &nAddressGCN, nCount)) {
        return false;
    }

    pCPU->nTickLast = OSGetTick();
#endif
    return nAddressGCN;
}
