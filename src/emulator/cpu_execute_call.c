#include "emulator/comboPerf.h"
#include "emulator/cpu.h"
#include "emulator/system.h"
#include "macros.h"

// Split out to fix the VC Crash

bool cpuExecuteUpdate(Cpu* pCPU, s32* pnAddressGCN, u64 nTime);

extern s32 ganMapGPR[];

#if IS_MM
//! Fix for the "VC Crash".
//!
//! See https://pastebin.com/V6ANmXt8
#define COMBO_FIX_VC_CRASH 1
#else
#define COMBO_FIX_VC_CRASH 1
#endif

#if !COMBO_FIX_VC_CRASH
#define nAddressReturn nAddressGCN
#endif

/**
 * @brief Executes a call from the dynamic recompiler environment
 *
 * @param pCPU The emulated VR4300.
 * @param nCount Latest tick count
 * @param nAddressN64 The N64 address of the call.
 * @param nAddressGCN The GameCube address after the call has completed.
 * @return s32 The address of the recompiled called function.
 */
s32 cpuExecuteCall(Cpu* pCPU, s32 nCount, s32 nAddressN64, s32 nAddressGCN) {
    s32 pad;
    s32 nReg;
    s32 count;
    s32* anCode;
    s32 saveGCN;
    CpuFunction* node;
    CpuCallerID* block;
    s32 nDeltaAddress;
#if COMBO_FIX_VC_CRASH
    s32 nAddressReturn;
#endif
#if IS_MM
    s64 callTime = OSGetTime();
#endif

#if !IS_MM
    nCount = OSGetTick();
#endif
    if (pCPU->nWaitPC != 0) {
        pCPU->nMode |= 8;
    } else {
        pCPU->nMode &= ~8;
    }

    pCPU->nMode |= 4;
    pCPU->nPC = nAddressN64;

    pCPU->aGPR[31].s32 = nAddressGCN;

    pCPU->survivalTimer++;

#if COMBO_FIX_VC_CRASH
    // Resolve (and possibly compile) the callee first. nAddressGCN is in/out: it comes in as the
    // host return address and comes back as the callee's compiled entry, so keep the return
    // address before handing it over.
    nAddressReturn = nAddressGCN;

    if (!cpuExecuteUpdate(pCPU, &nAddressGCN, callTime)) {
        return false;
    }

    // Deliberately after cpuExecuteUpdate: the collector may have deleted and re-created the node
    // that contains this call site, and we want the current one.
    saveGCN = nAddressReturn - 4;
#else
    saveGCN = nAddressGCN - 4;
#endif

    cpuFindFunction(pCPU, pCPU->nReturnAddrLast - 8, &node);

    block = node->block;
    for (count = 0; count < node->callerID_total; count++) {
        if (block[count].N64address == nAddressN64 && block[count].GCNaddress == 0) {
            block[count].GCNaddress = saveGCN;
            break;
        }
    }

    saveGCN = (ganMapGPR[31] & 0x100) ? true : false;
    anCode = (s32*)nAddressReturn - (saveGCN ? 4 : 3);
    if (saveGCN) {
        anCode[0] = 0x3CA00000 | ((u32)nAddressReturn >> 16); // lis r5,nAddressReturn@h
        anCode[1] = 0x60A50000 | ((u32)nAddressReturn & 0xFFFF); // ori r5,r5,nAddressReturn@l
        DCStoreRange(anCode, 8);
        ICInvalidateRange(anCode, 8);
    } else {
        nReg = ganMapGPR[31];
        anCode[0] = 0x3C000000 | ((u32)nAddressReturn >> 16) | (nReg << 21); // lis ri,nAddressReturn@h
        anCode[1] = 0x60000000 | ((u32)nAddressReturn & 0xFFFF) | (nReg << 21) | (nReg << 16); // ori ri,ri,...@l
        DCStoreRange(anCode, 8);
        ICInvalidateRange(anCode, 8);
    }

#if !COMBO_FIX_VC_CRASH
#if IS_MM
    if (!cpuExecuteUpdate(pCPU, &nAddressGCN, callTime)) {
        return false;
    }
#else
    if (!cpuExecuteUpdate(pCPU, &nAddressGCN, nCount)) {
        return false;
    }
#endif
#endif

    nDeltaAddress = (u8*)nAddressGCN - (u8*)&anCode[3];
    if (saveGCN) {
        anCode[3] = 0x48000000 | (nDeltaAddress & 0x03FFFFFC); // b nDeltaAddress
        DCStoreRange(anCode, 16);
        ICInvalidateRange(anCode, 16);
    } else {
        anCode[2] = 0x48000000 | (nDeltaAddress & 0x03FFFFFC); // b nDeltaAddress
        DCStoreRange(anCode, 12);
        ICInvalidateRange(anCode, 12);
    }

#if IS_MM
    pCPU->nTimeLast = callTime;
#else
    pCPU->nTickLast = OSGetTick();
#endif

    return nAddressGCN;
}
