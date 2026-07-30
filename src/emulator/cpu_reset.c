#include "emulator/cpu.h"
#include "emulator/xlHeap.h"
#include "macros.h"

// Split out of cpu.c so this single function can be built from source and linked while
// the rest of cpu.c still comes from the extracted object. See config/mm-j/splits.txt.

// Defined in cpu.c; dtk gives every function it recovers global scope, so these resolve
// across the split without any symbols.txt change.
bool cpuSetCP0_Status(Cpu* pCPU, u64 nStatus, u32 unknown);
bool cpuHackHandler(Cpu* pCPU);
bool treeKill(Cpu* pCPU);

// MM keeps the tree heap and its allocation bitmap as file-scope objects in cpu.c rather
// than as Cpu members; they are named in config/mm-j/symbols.txt so this unit can reach
// cpu.c's definitions instead of emitting its own.
extern u32 gaHeapTreeFlag[125];
extern void* gHeapTree;

// Inlined into cpuReset by MWCC; travels with it.
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
