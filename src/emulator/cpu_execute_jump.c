#include "emulator/cpu.h"
#include "macros.h"

// Defined in cpu.c; marked scope:global in config/mm-j/symbols.txt so this unit can
// reference it across the split.
bool cpuExecuteUpdate(Cpu* pCPU, s32* pnAddressGCN, u64 nTime);

s32 cpuExecuteJump(Cpu* pCPU, s32 nCount, s32 nAddressN64, s32 nAddressGCN) {
    s64 curTime = OSGetTime();

    if (pCPU->nWaitPC != 0) {
        pCPU->nMode |= 8;
    } else {
        pCPU->nMode &= ~8;
    }

    pCPU->nMode |= 4;
    pCPU->nPC = nAddressN64;

    if (!cpuExecuteUpdate(pCPU, &nAddressGCN, curTime)) {
        return false;
    }

    pCPU->nTimeLast = curTime;
    return nAddressGCN;
}
