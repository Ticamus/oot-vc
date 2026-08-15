#include "emulator/comboPerf.h"
#include "emulator/cpu.h"
#include "emulator/xlHeap.h"
#include "macros.h"

bool cpuSetCP0_Status(Cpu* pCPU, u64 nStatus, u32 unknown);
bool cpuHackHandler(Cpu* pCPU);
bool treeKill(Cpu* pCPU);

extern u32 gaHeapTreeFlag[125];
extern void* gHeapTree;

#if IS_MM && COMBO_FAULT_NAMER
void comboFaultArm(Cpu* pCPU);
#endif

// Inlined into cpuReset
static inline bool cpuHeapReset(u32* array, s32 count) {
    s32 i;

    for (i = 0; i < count; i++) {
        array[i] = 0;
    }

    return true;
}

bool cpuReset(Cpu* pCPU) {
    s32 iRegister;
    s32 iTLB;

#if IS_MM && COMBO_FAULT_NAMER
    comboFaultArm(pCPU); // first, so nothing can fault before the handler is in place
#endif

    // MM's cpuReset does not clear nTick.
    pCPU->nCountCodeHack = 0;
    pCPU->nMode = 0x40;
    pCPU->pfStep = NULL;

    for (iTLB = 0; iTLB < ARRAY_COUNT(pCPU->aTLB); iTLB++) {
        for (iRegister = 0; iRegister < 5; iRegister++) {
            pCPU->aTLB[iTLB][iRegister] = 0;
        }
        pCPU->aTLB[iTLB][4] = -1;
    }

    pCPU->nLo = 0;
    pCPU->nHi = 0;
    pCPU->nPC = 0x80000400;
    pCPU->nWaitPC = -1;

    for (iRegister = 0; iRegister < ARRAY_COUNT(pCPU->aGPR); iRegister++) {
        pCPU->aGPR[iRegister].u64 = 0;
    }

    for (iRegister = 0; iRegister < ARRAY_COUNT(pCPU->aFPR); iRegister++) {
        pCPU->aFPR[iRegister].f64 = 0.0;
    }

    for (iRegister = 0; iRegister < ARRAY_COUNT(pCPU->anFCR); iRegister++) {
        pCPU->anFCR[iRegister] = 0;
    }

    pCPU->aGPR[20].u64 = 1;
    pCPU->aGPR[22].u64 = 0x3F;
    pCPU->aGPR[29].u64 = 0xA4001FF0;

    for (iRegister = 0; iRegister < ARRAY_COUNT(pCPU->anCP0); iRegister++) {
        pCPU->anCP0[iRegister] = 0;
    }

    pCPU->anCP0[15] = 0xB00;
    pCPU->anCP0[9] = 0x10000000;
    cpuSetCP0_Status(pCPU, 0x2000FF01, 1);
    pCPU->anCP0[16] = 0x6E463;

    pCPU->nCountAddress = 0;
    if (cpuHackHandler(pCPU)) {
        pCPU->nMode |= 0x10;
    }

    if (!cpuHeapReset(pCPU->aHeap1Flag, ARRAY_COUNT(pCPU->aHeap1Flag))) {
        return false;
    }
    // Sized to match aHeap1Flag: 256 blocks in MM.
    if (pCPU->gHeap1 == NULL && !xlHeapTake(&pCPU->gHeap1, 0x400000 | 0x30000000)) {
        return false;
    }

    if (!cpuHeapReset(pCPU->aHeap2Flag, ARRAY_COUNT(pCPU->aHeap2Flag))) {
        return false;
    }
    if (pCPU->gHeap2 == NULL && !xlHeapTake(&pCPU->gHeap2, 0x104000 | 0x30000000)) {
        return false;
    }

    if (!cpuHeapReset(gaHeapTreeFlag, ARRAY_COUNT(gaHeapTreeFlag))) {
        return false;
    }

    // MM allocates the tree heap here, alongside gHeap1/gHeap2; OoT defers it to cpuEvent.
    if (gHeapTree == NULL && !xlHeapTake((void**)&gHeapTree, 0x46500 | 0x30000000)) {
        return false;
    }

    if (pCPU->gTree != NULL) {
        treeKill(pCPU);
    }

    pCPU->nCompileFlag = 1;
    return true;
}
