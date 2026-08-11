#include "emulator/rom.h"
#include "emulator/code_80083508.h"
#include "emulator/codeRVL.h"
#include "emulator/cpu.h"
#include "emulator/errordisplay.h"
#include "emulator/frame.h"
#include "emulator/helpRVL.h"
#include "emulator/ram.h"
#include "emulator/system.h"
#include "emulator/vc64_RVL.h"
#include "emulator/xlCoreRVL.h"
#include "emulator/xlFile.h"
#include "emulator/xlHeap.h"
#include "macros.h"
#include "revolution/os.h"

static bool romMakeFreeCache(Rom* pROM, s32* piCache, RomCacheType eType);
static bool romSetBlockCache(Rom* pROM, s32 iBlock, RomCacheType eType);
static bool __romLoadBlock_Complete(Rom* pROM) NO_INLINE;

_XL_OBJECTTYPE gClassROM = {
    "ROM",
    sizeof(Rom),
    NULL,
    (EventFunc)romEvent,
};

#if IS_MM
static OSThread sRomThread;

static tXL_FILE* spRomFile;
static Rom* spRom; // saved rom state?
static OSTime snOSTime;
s32 lbl_80200740;

s32 fn_80050AFC(void) { return lbl_80200740; }

// Checks wether the rom is compressed or no. A compressed VC image starts with 
// `(nSize << 2) | eCompression`, so a word matching the N64 magic (big endian or byte swapped)
// can only be a plain ROM.
#define ROM_IS_RAW(nHeader) ((nHeader) == 0x80371240 || (nHeader) == 0x37804012)

#define ROM_CACHE_MAX 0x02000000 // 32 MB

#if IS_MM
//! Debug only, see romCopy(). gComboTraceLeft lives in cpu_execute_jump.c.
extern s32 gComboTraceLeft;
static s32 gComboRomBeat = 0;
#endif

// Helpers to stream ROM from NAND
#define ROM_NAND_INFO(pROM) ((CNTFileInfoNAND*)&(pROM)->fileInfo)
static bool romOpenNAND(Rom* pROM) {
    return simulatorCNTOpenNAND(&gCNTHandle.handleNAND, pROM->acNameFile, ROM_NAND_INFO(pROM)) == 0;
}

// Synchronous replacement for GC simulatorDVDRead().
static bool romReadNAND(Rom* pROM, void* pTarget, s32 nSize, s32 nOffset) {
    CNTFileInfoNAND* pInfo = ROM_NAND_INFO(pROM);

    if (pInfo->handle == NULL || (u32)nOffset >= pInfo->length) {
        return false;
    }

    // contentReadNAND only bounds checks the start of the read. The caller rounds the
    // size up to 32 bytes and the last block of the image is short, so clamp the tail
    // here instead of letting IOS read past the content.
    if ((u32)(nOffset + nSize) > pInfo->length) {
        nSize = pInfo->length - nOffset;
    }

    return simulatorCNTReadNAND(pInfo, pTarget, nSize, nOffset) >= 0;
}
#endif

static bool fn_80042064(void) {
    SYSTEM_ROM(gpSystem)->bLoad = true;
    return true;
}

static bool romFindFreeCache(Rom* pROM, s32* piCache, RomCacheType eType) {
    s32 iBlock;

    if (eType == RCT_RAM) {
        for (iBlock = 0; iBlock < pROM->nCountBlockRAM; iBlock++) {
            if (!(pROM->anBlockCachedRAM[iBlock >> 3] & (1 << (iBlock & 7)))) {
                *piCache = iBlock;
                return true;
            }
        }
    } else if (eType == RCT_ARAM) {
        for (iBlock = 0; iBlock < ARRAY_COUNT(pROM->anBlockCachedARAM); iBlock++) {
            if (!(pROM->anBlockCachedARAM[iBlock >> 3] & (1 << (iBlock & 7)))) {
                *piCache = -(iBlock + 1);
                return true;
            }
        }
    } else {
        return false;
    }

    return false;
}

static bool romFindOldestBlock(Rom* pROM, s32* piBlock, RomCacheType eTypeCache, s32 whichBlock) {
    RomBlock* pBlock;
    s32 iBlock;
    s32 iBlockOldest;
    u32 nTick;
    u32 nTickDelta;
    u32 nTickDeltaOldest;

    nTick = pROM->nTick;
    nTickDeltaOldest = 0;

    for (iBlock = 0; iBlock < ARRAY_COUNT(pROM->aBlock); iBlock++) {
        pBlock = &pROM->aBlock[iBlock];
        if (pBlock->nSize != 0 &&
            ((eTypeCache == RCT_RAM && pBlock->iCache >= 0) || (eTypeCache == RCT_ARAM && pBlock->iCache < 0))) {
            if (pBlock->nTickUsed > nTick) {
                nTickDelta = -1 - (pBlock->nTickUsed - nTick);
            } else {
                nTickDelta = nTick - pBlock->nTickUsed;
            }
            if (whichBlock == 0) {
                if (nTickDelta > nTickDeltaOldest && pBlock->keep == 0) {
                    iBlockOldest = iBlock;
                    nTickDeltaOldest = nTickDelta;
                }
            } else if (whichBlock == 1) {
                if (nTickDelta > nTickDeltaOldest && pBlock->keep == 1) {
                    iBlockOldest = iBlock;
                    nTickDeltaOldest = nTickDelta;
                }
            } else if (nTickDelta > nTickDeltaOldest) {
                iBlockOldest = iBlock;
                nTickDeltaOldest = nTickDelta;
            }
        }
    }

    if (nTickDeltaOldest != 0) {
        *piBlock = iBlockOldest;
        return true;
    }

    return false;
}

static inline void romMarkBlockAsFree(Rom* pROM, s32 iBlock) {
    RomBlock* pBlock;
    s32 iCache;

    pBlock = &pROM->aBlock[iBlock];
    iCache = pBlock->iCache;
    if (iCache < 0) {
        iCache = -(iCache + 1);
        pROM->anBlockCachedARAM[iCache >> 3] &= ~(1 << (iCache & 7));
    } else {
        pROM->anBlockCachedRAM[iCache >> 3] &= ~(1 << (iCache & 7));
    }
    pBlock->nSize = 0;
}

static bool romMakeFreeCache(Rom* pROM, s32* piCache, RomCacheType eType) {
    s32 iCache;
    s32 iBlockOldest;

    if (eType == RCT_RAM) {
        if (!romFindFreeCache(pROM, &iCache, RCT_RAM)) {
            if (romFindOldestBlock(pROM, &iBlockOldest, RCT_RAM, 2)) {
                iCache = pROM->aBlock[iBlockOldest].iCache;
#if IS_MM
                //! The Wii has no ARAM: romSetBlockCache(RCT_ARAM)
                //! only flips the bookkeeping bits here (the AR DMA the GameCube emulator
                //! performed has no equivalent), so a block "moved to ARAM" would be served
                //! back as garbage. Evict by dropping the least recently used RAM block
                //! instead; it is transparently re-read from NAND on the next access.
                romMarkBlockAsFree(pROM, iBlockOldest);
#else
                if (!romSetBlockCache(pROM, iBlockOldest, RCT_ARAM) &&
                    romFindOldestBlock(pROM, &iBlockOldest, RCT_RAM, 0)) {
                    iCache = pROM->aBlock[iBlockOldest].iCache;
                    romMarkBlockAsFree(pROM, iBlockOldest);
                }
#endif
            } else {
                return false;
            }
        }
    } else {
        if (!romFindFreeCache(pROM, &iCache, RCT_ARAM)) {
            if (romFindOldestBlock(pROM, &iBlockOldest, RCT_ARAM, 0)) {
                iCache = pROM->aBlock[iBlockOldest].iCache;
                romMarkBlockAsFree(pROM, iBlockOldest);
            } else {
                return false;
            }
        }
    }

    *piCache = iCache;
    return true;
}

static bool romSetBlockCache(Rom* pROM, s32 iBlock, RomCacheType eType) {
    RomBlock* pBlock;
    s32 iCacheRAM;
    s32 iCacheARAM;
    s32 nOffsetRAM;
    s32 nOffsetARAM;

    pBlock = &pROM->aBlock[iBlock];
    if ((eType == RCT_RAM && pBlock->iCache >= 0) || (eType == RCT_ARAM && pBlock->iCache < 0)) {
        return true;
    }

    if (eType == RCT_RAM) {
        iCacheARAM = -(pBlock->iCache + 1);
        if (!romMakeFreeCache(pROM, &iCacheRAM, RCT_RAM)) {
            return false;
        }

        nOffsetRAM = iCacheRAM * 0x2000;
        nOffsetARAM = iCacheARAM * 0x2000;

        pROM->anBlockCachedARAM[iCacheARAM >> 3] &= ~(1 << (iCacheARAM & 7));
        pROM->anBlockCachedRAM[iCacheRAM >> 3] |= (1 << (iCacheRAM & 7));
        pBlock->iCache = iCacheRAM;
    } else if (eType == RCT_ARAM) {
        iCacheRAM = pBlock->iCache;
        if (!romMakeFreeCache(pROM, &iCacheARAM, RCT_ARAM)) {
            return false;
        }
        iCacheARAM = -(iCacheARAM + 1);

        nOffsetRAM = iCacheRAM * 0x2000;
        nOffsetARAM = iCacheARAM * 0x2000;

        pROM->anBlockCachedRAM[iCacheRAM >> 3] &= ~(1 << (iCacheRAM & 7));
        pROM->anBlockCachedARAM[iCacheARAM >> 3] |= (1 << (iCacheARAM & 7));
        pBlock->iCache = -(iCacheARAM + 1);
    } else {
        return false;
    }

    return true;
}

static inline void romByteSwap(Rom* pROM) {
    u32 i;
    u32* anData = (u32*)pROM->load.anData;

    for (i = 0; i < ((pROM->load.nSize + 3) >> 2); i++) {
        *anData++ = ((*anData >> 8) & 0x00FF00FF) | ((*anData << 8) & 0xFF00FF00);
    }
}

static bool __romLoadBlock_Complete(Rom* pROM) {
    s32 iBlock;

    if (pROM->bFlip) {
        romByteSwap(pROM);
    }

    iBlock = pROM->load.iBlock;
    pROM->aBlock[iBlock].nSize = pROM->load.nSize;
    pROM->aBlock[iBlock].iCache = pROM->load.iCache;
    pROM->aBlock[iBlock].keep = 0;

    pROM->anBlockCachedRAM[pROM->load.iCache >> 3] |= (1 << (pROM->load.iCache & 7));

    if ((pROM->load.pCallback != NULL) && !pROM->load.pCallback()) {
        return false;
    }

    return true;
}

#if !IS_MM
static void __romLoadBlock_CompleteGCN(long nResult, DVDFileInfo* fileInfo) {
    Rom* pROM = SYSTEM_ROM(gpSystem);

    pROM->load.nResult = nResult;
    pROM->load.bDone = true;
}
#endif

static bool romLoadBlock(Rom* pROM, s32 iBlock, s32 iCache, UnknownCallbackFunc pCallback) {
    u8* anData;
    s32 nSizeRead;
    u32 nSize;
    u32 nOffset;

    nOffset = iBlock * 0x2000;
    if ((nSize = pROM->nSize - nOffset) > 0x2000) {
        nSize = 0x2000;
    }
    anData = &pROM->pCacheRAM[iCache * 0x2000];
    nSizeRead = (nSize + 0x1F) & 0xFFFFFFE0;

    pROM->load.nSize = nSize;
    pROM->load.iBlock = iBlock;
    pROM->load.iCache = iCache;
    pROM->load.anData = anData;
    pROM->load.pCallback = pCallback;

    if (pCallback == NULL) {
#if IS_MM
        if (!romReadNAND(pROM, anData, nSizeRead, nOffset + pROM->offsetToRom)) {
#else
        if (!simulatorDVDRead(&pROM->fileInfo, anData, nSizeRead, nOffset + pROM->offsetToRom, NULL)) {
#endif
            return false;
        }
    } else {
        pROM->load.nOffset = nOffset;
        pROM->load.nSizeRead = nSizeRead;
#if IS_MM
        //! NAND content reads are synchronous, unlike the
        //! asynchronous disc reads this wrapped. Perform the read now and hand the async
        //! state machine its completion straight away, so romUpdate() finishes the block
        //! on its next pass exactly as it would have after a DVD callback.
        if (!romReadNAND(pROM, anData, nSizeRead, nOffset + pROM->offsetToRom)) {
            return false;
        }

        pROM->load.nResult = nSizeRead;
        pROM->load.bDone = true;
#else
        if (!simulatorDVDRead(&pROM->fileInfo, anData, nSizeRead, nOffset + pROM->offsetToRom,
                              &__romLoadBlock_CompleteGCN)) {
            return false;
        }
#endif
        return true;
    }

    if (!__romLoadBlock_Complete(pROM)) {
        return false;
    }
    return true;
}

static bool __romLoadUpdate_Complete(void) {
    Rom* pROM = SYSTEM_ROM(gpSystem);

    pROM->load.bWait = false;
    return true;
}

static bool romLoadUpdate(Rom* pROM) {
    s32 iCache;
    RomBlock* pBlock;
    u32 iBlock0;
    u32 iBlock1;
    Cpu* pCPU;

    pCPU = SYSTEM_CPU(gpSystem);
    if ((pROM->load.nOffset0 == 0 && pROM->load.nOffset1 == 0) || pROM->load.bWait) {
        return true;
    }

    iBlock0 = pROM->load.nOffset0 >> 13;
    iBlock1 = pROM->load.nOffset1 >> 13;

    while (iBlock0 <= iBlock1) {
        if (pCPU->nRetrace != pCPU->nRetraceUsed) {
            return true;
        }

        pBlock = &pROM->aBlock[iBlock0];
        pBlock->nTickUsed = ++pROM->nTick;
        if (pBlock->nSize == 0) {
            if (!romMakeFreeCache(pROM, &iCache, 0)) {
                return false;
            }

            pROM->load.bWait = true;
            if (!romLoadBlock(pROM, iBlock0, iCache, &__romLoadUpdate_Complete)) {
                return false;
            }

            return true;
        }

        pROM->load.nOffset0 = ++iBlock0 * 0x2000;
    }

    pROM->load.nOffset1 = 0;
    pROM->load.nOffset0 = 0;
    return true;
}

static bool __romCopyUpdate_Complete(void) {
    Rom* pROM = SYSTEM_ROM(gpSystem);

    pROM->copy.bWait = false;
    return true;
}

static inline bool romAreOffsetsZero(Rom* pROM) { return pROM->load.nOffset0 == 0 && pROM->load.nOffset1 == 0; }

static inline bool romCheckOffsets(Rom* pROM) {
    if (romAreOffsetsZero(pROM)) {
        return false;
    }

    return true;
}

static inline void romSetEndianness(Rom* pROM) { pROM->bFlip = pROM->acHeader[0] == 0x37 && pROM->acHeader[1] == 0x80; }

static bool romCopyUpdate(Rom* pROM) {
    RomBlock* pBlock;
    s32 iCache;
    s32 nTickLast;
    u8* anData;
    u32 iBlock;
    u32 nSize;
    u32 nOffsetBlock;
    Cpu* pCPU;

    s32 var_r0;
    s32 var_r5;

    pCPU = SYSTEM_CPU(gpSystem);

    if (romCheckOffsets(pROM) || pROM->copy.nSize == 0 || pROM->copy.bWait) {
        return true;
    }

    while (pROM->copy.nSize != 0) {
        if (pROM->copy.pCallback != NULL && pCPU->nRetrace != pCPU->nRetraceUsed) {
            return true;
        }

        iBlock = pROM->copy.nOffset / 0x2000;
        pBlock = &pROM->aBlock[iBlock];
        nTickLast = pBlock->nTickUsed;
        pBlock->nTickUsed = ++pROM->nTick;

        if (pBlock->nSize != 0) {
            if (pBlock->iCache < 0 && !romSetBlockCache(pROM, iBlock, 0)) {
                return false;
            }
        } else {
            if (!romMakeFreeCache(pROM, &iCache, 0)) {
                return false;
            }

            if (pROM->copy.pCallback == NULL) {
                if (!romLoadBlock(pROM, iBlock, iCache, NULL)) {
                    return false;
                }
            } else {
                pBlock->nTickUsed = nTickLast;
                pROM->nTick--;
                pROM->copy.bWait = true;
                if (!romLoadBlock(pROM, iBlock, iCache, &__romCopyUpdate_Complete)) {
                    return false;
                } else {
                    return true;
                }
            }
        }

        nOffsetBlock = pROM->copy.nOffset & 0x1FFF;
        if ((nSize = pBlock->nSize - nOffsetBlock) > pROM->copy.nSize) {
            nSize = pROM->copy.nSize;
        }

        anData = &pROM->pCacheRAM[pBlock->iCache * 0x2000];
        if (!xlHeapCopy(pROM->copy.pTarget, anData + nOffsetBlock, nSize)) {
            return false;
        }

        pROM->copy.pTarget = (u8*)pROM->copy.pTarget + nSize;
        pROM->copy.nSize -= nSize;
        pROM->copy.nOffset += nSize;
    }

    if (pROM->copy.pCallback != NULL && !pROM->copy.pCallback()) {
        return false;
    }
    return true;
}

#if IS_MM
s32 fn_80051738(void* pBuffer, u32 nOffset, u32 nSize) {
    s32 nSizeBytes = nSize;
    Rom* pROM = (Rom*)pBuffer;

    if (nOffset + nSize >= spRom->unk_218) {
        nSizeBytes = spRom->unk_218 - nOffset;

        if (nSizeBytes <= 0) {
            return 0;
        }
    }

    xlFileSetPosition(spRomFile, nOffset);
    xlFileGet(spRomFile, pROM, nSizeBytes);
    return nSizeBytes;
}
#endif

static bool fn_80042C98(Rom* pROM) {
    tXL_FILE* pFile;

    s32 iBuffer;
    u32 nSizeBytes;
    s32 nSize;
    u8* pCacheRAM;
    u32* pBuffer;
    u32 nBuffer;

#if IS_OOT || IS_MT
#define FILE_PTR (pFile)
#elif IS_MM
#define FILE_PTR (spRomFile)
#endif

#if IS_MM
    fn_80007844();
#endif

    pCacheRAM = pROM->pCacheRAM;
    pROM->pBuffer = pCacheRAM;

#if IS_MM
    spRom = pROM;
#endif

    if (!xlFileOpen(&FILE_PTR, XLFT_BINARY, pROM->acNameFile)) {
        return false;
    }

#if IS_OOT
    nSize = pROM->nSize;

    while (nSize > 0) {
        nSizeBytes = nSize;
        if (nSize > 0x80000) {
            nSizeBytes = 0x80000;
        }

        if (!xlFileGet(FILE_PTR, pCacheRAM, nSizeBytes)) {
            return false;
        }

        nSize -= nSizeBytes;
        pCacheRAM = &pCacheRAM[nSizeBytes];
        simulatorShowLoad(1, pROM->acNameFile, (f32)(pROM->nSize - nSize) / (f32)pROM->nSize);
    }
#elif IS_MM
    fn_8008338C(pCacheRAM, SYSTEM_FRAME(gpSystem)->unk_20B0, 0x80000, fn_80051738, 0);
#endif

    if (!xlFileClose(&FILE_PTR)) {
        return false;
    }

#if IS_MM
    xlHeapCopy(pROM->acHeader, pROM->pBuffer, 0x40);

    romSetEndianness(pROM);
    frameSetupCache();
#endif

    pROM->eModeLoad = RLM_FULL;

    if (pROM->bFlip) {
        pBuffer = pROM->pBuffer;

        for (iBuffer = 0; iBuffer < ((s32)(pROM->nSize + 3) >> 2); iBuffer++) {
            nBuffer = pBuffer[iBuffer];
            pBuffer[iBuffer] = (((nBuffer >> 8) & 0xFF0000) | ((nBuffer >> 8) & 0xFF) |
                                (((nBuffer << 8) & 0xFF000000) | ((nBuffer << 8) & 0xFF00)));
        }
    }

#if IS_MM
    fn_8000784C();
#endif

    return true;
}

#if IS_MM
bool fn_800518EC(void) { return !OSIsThreadTerminated(&sRomThread); }
#endif

static void* __ROMEntry(void* arg) {
    fn_80042C98(SYSTEM_ROM(gpSystem));
    return NULL;
}

s32 fn_80042E30(EDString* pSTString) {
    s32 ret;
    bool bThread;

#if IS_MM
    if (SYSTEM_ROM(gpSystem)->unk_C != 0) {
        if (OSGetTime() - snOSTime < OSSecondsToTicks(4)) {
            return 0;
        }
    }
#endif

    bThread = OSIsThreadTerminated(&ROM_THREAD);
    ret = 0;

    if (bThread) {
        ret = 2;
    }

    return ret;
}

static inline bool romLoadFullOrPartLoop(Rom* pROM) {
    s32 i;
    s32 iCache;
    u32 temp_r27;
    u32 temp_r30;

    temp_r27 = (u32)(pROM->nSize - 1) / 0x2000;
    temp_r30 = pROM->nTick = temp_r27 + 1;

    for (i = 0; i < temp_r30; i++) {
        pROM->aBlock[i].nTickUsed = temp_r27 - i;

        if (!romMakeFreeCache(pROM, &iCache, RCT_RAM)) {
            return false;
        }

        if (!romLoadBlock(pROM, i, iCache, NULL)) {
            return false;
        }
    }

    return true;
}

static bool romLoadFullOrPart(Rom* pROM) {
    if ((s32)pROM->nSize <= pROM->nSizeCacheRAM) {
        void* pBuffer;

        if (!xlHeapTake(&pBuffer, ROM_THREAD_SIZE | 0x30000000)) {
            return false;
        }

#if IS_MM
        snOSTime = OSGetTime();
#endif
        if (OSCreateThread(&ROM_THREAD, (OSThreadFunc)__ROMEntry, pROM, (void*)((u8*)pBuffer + ROM_THREAD_SIZE),
                           ROM_THREAD_SIZE, OS_PRIORITY_MAX, 1)) {
#if IS_OOT
            OSResumeThread(&ROM_THREAD);
            errorDisplayShow(pROM->unk_C ? ERROR_NO_CONTROLLER : ERROR_BLANK_2);
#elif IS_MM
            int var_r28 = pROM->unk_C ? 8 : 16;

            if (pROM->unk_C != 0) {
                if (!fn_8007F440(16)) {
                    return false;
                }

                if (!fn_8007F440(9)) {
                    return false;
                }
            } else if (!fn_8007F440(16)) {
                return false;
            }

            OSResumeThread(&ROM_THREAD);
            errorDisplayShow(var_r28);

            if (pROM->unk_C != 0) {
                if (!fn_8007F474(16)) {
                    return false;
                }

                if (!fn_8007F474(9)) {
                    return false;
                }
            } else if (!fn_8007F474(16)) {
                return false;
            }
#endif
            pROM->unk_C = 0;
        }

        if (!xlHeapFree(&pBuffer)) {
            return false;
        }
    } else {
        s32 i;

        pROM->nTick = 0;
        pROM->eModeLoad = RLM_PART;

        for (i = 0; i < ARRAY_COUNT(pROM->aBlock); i++) {
            pROM->aBlock[i].nSize = 0;
            pROM->aBlock[i].iCache = 0;
            pROM->aBlock[i].nTickUsed = 0;
        }

        for (i = 0; i < ARRAY_COUNTU(pROM->anBlockCachedRAM); i++) {
            pROM->anBlockCachedRAM[i] = 0;
        }

        for (i = 0; i < ARRAY_COUNTU(pROM->anBlockCachedARAM); i++) {
            pROM->anBlockCachedARAM[i] = 0;
        }

        if ((s32)pROM->nSize < (pROM->nSizeCacheRAM + 0xFFA000) && !romLoadFullOrPartLoop(pROM)) {
            return false;
        }
    }

    return true;
}

bool romGetPC(Rom* pROM, u64* pnPC) {
    s32 nOffset;
    u32 nData;
    u32 iData;
    u32 anData[0x400];

    if (romCopy(pROM, &anData, 0, sizeof(anData), NULL)) {
        nData = 0;
        for (iData = 0; iData < 0x400 - 0x10; iData++) {
            nData += anData[iData + 0x10];
        }

        switch (nData) {
            case 0x49F60E96:
            case 0xFB631223:
            case 0x2ADFE50A:
            case 0x57C85244:
                nOffset = 0;
                break;
            case 0x027FDF31:
                nOffset = -0x80;
                break;
            case 0x497E414B:
            case 0xE6DECB4B:
            case 0x27C4ED44:
                nOffset = 0x100000;
                break;
            case 0xD5BE5580:
                nOffset = 0x200000;
                break;
            default:
                return false;
        }

        *pnPC = anData[2] - nOffset;
        return true;
    } else {
        return false;
    }
}

#if IS_OOT
bool romGetCode(Rom* pROM, s32* acCode) {
    *acCode = (pROM->acHeader[0x3B] << 0x18) | (pROM->acHeader[0x3C] << 0x10) | (pROM->acHeader[0x3D] << 0x8) |
              pROM->acHeader[0x3E];
    return true;
}
#elif IS_MM
bool romGetCode(Rom* pROM, char* acCode) {
    if (pROM->acHeader[0x3B] == '\0') {
        acCode[0] = acCode[1] = acCode[2] = acCode[3] = ' ';
    } else {
        acCode[0] = pROM->acHeader[0x3B];
        acCode[1] = pROM->acHeader[0x3C];
        acCode[2] = pROM->acHeader[0x3D];
        acCode[3] = pROM->acHeader[0x3E];
    }

    acCode[4] = '\0';
    return true;
}

bool romTestCode(Rom* pROM, char* acCode) {
    s32 iCode;
    char acCodeCurrent[5];

    romGetCode(pROM, acCodeCurrent);

    for (iCode = 0; iCode < 4; iCode++) {
        if (acCode[iCode] != acCodeCurrent[iCode]) {
            return false;
        }
    }

    return true;
}
#endif

static bool romPut8(Rom* pROM, u32 nAddress, s8* pData) { return true; }
static bool romPut16(Rom* pROM, u32 nAddress, s16* pData) { return true; }
static bool romPut32(Rom* pROM, u32 nAddress, s32* pData) { return true; }
static bool romPut64(Rom* pROM, u32 nAddress, s64* pData) { return true; }

static bool romGet8(Rom* pROM, u32 nAddress, s8* pData) {
    u8 nData;

    nAddress &= 0x07FFFFFF;
    if (nAddress < pROM->nSize && romCopy(pROM, &nData, nAddress, 1, 0)) {
        *pData = nData;
        return true;
    }

    return true;
}

static bool romGet16(Rom* pROM, u32 nAddress, s16* pData) {
    u16 nData;

    nAddress &= 0x07FFFFFF;
    if (nAddress < pROM->nSize && romCopy(pROM, &nData, nAddress, 2, 0)) {
        *pData = nData;
        return true;
    }

    return true;
}

static bool romGet32(Rom* pROM, u32 nAddress, s32* pData) {
    u32 nData;

    nAddress &= 0x07FFFFFF;
    if (nAddress < pROM->nSize && romCopy(pROM, &nData, nAddress, 4, 0)) {
        *pData = nData;
        return true;
    }

    return true;
}

static bool romGet64(Rom* pROM, u32 nAddress, s64* pData) {
    u64 nData;

    nAddress &= 0x07FFFFFF;
    if (nAddress < pROM->nSize && romCopy(pROM, &nData, nAddress, 8, 0)) {
        *pData = nData;
        return true;
    }

    return true;
}

#if IS_OOT
static bool romGetBlock(Rom* pROM, CpuBlock* pBlock) {
    void* buf;

    if (pBlock->nAddress1 < 0x04000000) {
        if (!ramGetBuffer(SYSTEM_RAM(gpSystem), &buf, pBlock->nAddress1, &pBlock->nSize)) {
            return false;
        }

        if (!romCopy(pROM, buf, pBlock->nAddress0, pBlock->nSize, NULL)) {
            return false;
        }
    }

    if (!pBlock->pfUnknown(pBlock, 1)) {
        return false;
    }

    return true;
}
#endif

static bool romPutDebug8(Rom* pROM, u32 nAddress, s8* pData) { return true; }
static bool romPutDebug16(Rom* pROM, u32 nAddress, s16* pData) { return true; }
static bool romPutDebug32(Rom* pROM, u32 nAddress, s32* pData) { return true; }
static bool romPutDebug64(Rom* pROM, u32 nAddress, s64* pData) { return true; }

static bool romGetDebug8(Rom* pROM, u32 nAddress, s8* pData) {
    *pData = 0;
    return true;
}

static bool romGetDebug16(Rom* pROM, u32 nAddress, s16* pData) {
    *pData = 0;
    return true;
}

static bool romGetDebug32(Rom* pROM, u32 nAddress, s32* pData) {
    *pData = 0;
    return true;
}

static bool romGetDebug64(Rom* pROM, u32 nAddress, s64* pData) {
    *pData = 0;
    return true;
}

static inline bool romCopyLoad(Rom* pROM) {
    if (!romLoadFullOrPart(pROM)) {
        return false;
    }

    pROM->bLoad = false;
    return true;
}

static inline bool romCopyLoop(Rom* pROM, u8* pTarget, u32 nOffset, u32 nSize, UnknownCallbackFunc* pCallback) {
    int i;

    pROM->copy.bWait = false;
    pROM->copy.nSize = nSize;
    pROM->copy.pTarget = pTarget;
    pROM->copy.nOffset = nOffset;
    pROM->copy.pCallback = pCallback;

    for (i = 0; i < pROM->nCountOffsetBlocks; i += 2) {
        if ((pROM->anOffsetBlock[i] <= nOffset) && (nOffset <= pROM->anOffsetBlock[i + 1])) {
            pROM->load.nOffset0 = pROM->anOffsetBlock[i];
            pROM->load.nOffset1 = pROM->anOffsetBlock[i + 1];
            return true;
        }
    }

    return false;
}

bool romCopy(Rom* pROM, void* pTarget, s32 nOffset, s32 nSize, UnknownCallbackFunc* pCallback) {
    tXL_FILE* pFile;

    nOffset &= 0x07FFFFFF;

#if IS_MM
    //! Debug only. Armed by cpu_execute_jump.c on entry to the OoTMM switch stub. The
    //! payload's raw PI DMA reaches romCopy through piPut32 -> fn_800166D0, and the PI busy
    //! bit only clears once pCallback runs, so any path that returns without calling it wedges
    //! the payload in waitForPi().
    if (gComboTraceLeft > 0) {
        OSReport("combo: romCopy off %08X size %08X cb %08X (romSize %08X cache %08X mode %d "
                 "bLoad %d)\n",
                 nOffset, nSize, (u32)pCallback, pROM->nSize, pROM->nSizeCacheRAM, pROM->eModeLoad,
                 pROM->bLoad);
    }
#endif

    if (pROM->nSizeCacheRAM == 0) {
        if (!xlFileOpen(&pFile, XLFT_BINARY, pROM->acNameFile)) {
            return false;
        }

        if (!xlFileSetPosition(pFile, nOffset + pROM->offsetToRom)) {
            return false;
        }

        if (!xlFileGet(pFile, pTarget, (s32)nSize)) {
            return false;
        }

        if (!xlFileClose(&pFile)) {
            return false;
        }

        if ((pCallback != NULL) && !pCallback()) {
            return false;
        }

        return true;
    }

    if (pROM->bLoad && !romCopyLoad(pROM)) {
        return false;
    }

    if (((nOffset + nSize) > pROM->nSize) && ((nSize = pROM->nSize - nOffset) < 0)) {
#if IS_MM
        //! Debug only, see above: this is the one exit that drops pCallback silently.
        if (gComboTraceLeft > 0) {
            OSReport("combo: romCopy past end of image, callback dropped\n");
        }
#endif
        return true;
    }

    if (pROM->eModeLoad == RLM_PART) {
        if (romCopyLoop(pROM, pTarget, nOffset, nSize, pCallback) && !romLoadUpdate(pROM)) {
            return false;
        }

        if (!romCopyUpdate(pROM)) {
            return false;
        }

        return true;
    }

    if (pROM->eModeLoad == RLM_FULL) {
        if (!xlHeapCopy(pTarget, (void*)((u32)pROM->pBuffer + nOffset), nSize)) {
            return false;
        }

        if ((pCallback != NULL) && !pCallback()) {
            return false;
        }

        return true;
    }

    return false;
}

bool romCopyImmediate(Rom* pROM, void* pTarget, s32 nOffsetROM, s32 nSize) {
    void* pSource;
    RomBlock* pBlock;
    s32 nOffsetARAM;
    s32 nSizeCopy;
    s32 nOffsetBlock;
    s32 nSizeCopyARAM;
    s32 nSizeDMA;
    s32 nOffset;
    s32 nOffsetTarget;
    s32 pad;
    u8* pBuffer;
    u8 anBuffer[608];
    s32 iCache;

    if (pROM->nSizeCacheRAM == 0) {
        return false;
    }

    if (pROM->bLoad && !romCopyLoad(pROM)) {
        return false;
    }

    nOffsetROM = nOffsetROM & 0x07FFFFFF;
    pBuffer = (u8*)(((s32)anBuffer + 0x1F) & 0xFFFFFFE0);

    if (nOffsetROM + nSize > pROM->nSize && (nSize = pROM->nSize - nOffsetROM) < 0) {
        return true;
    }

    if (pROM->eModeLoad == RLM_PART) {
        while (nSize != 0U) {
            pBlock = &pROM->aBlock[nOffsetROM / 0x2000];
            if (pBlock->nSize == 0) {
                if (!romMakeFreeCache(pROM, &iCache, RCT_RAM)) {
                    return false;
                }

                if (!romLoadBlock(pROM, nOffsetROM / 0x2000, iCache, NULL)) {
                    return false;
                }
            }

            nOffsetBlock = nOffsetROM % 0x2000;
            if ((nSizeCopy = pBlock->nSize - nOffsetBlock) > nSize) {
                nSizeCopy = nSize;
            }

            if (pBlock->iCache >= 0) {
                pSource = &pROM->pCacheRAM[(pBlock->iCache * 0x2000)] + nOffsetBlock;
                if (!xlHeapCopy(pTarget, pSource, nSizeCopy)) {
                    return false;
                }
            }

            pTarget = (u8*)pTarget + nSizeCopy;
            nOffsetROM += nSizeCopy;
            nSize -= nSizeCopy;
        }
        return true;
    } else if (pROM->eModeLoad == RLM_FULL) {
        if (!xlHeapCopy(pTarget, (u8*)pROM->pBuffer + nOffsetROM, nSize)) {
            return false;
        }
        return true;
    }

    return false;
}

bool romUpdate(Rom* pROM) {
    s32 nStatus;

#if IS_MM
    //! Debug only, see romCopy(). Rate limited: cpuExecuteUpdate calls this on every
    //! interpreted opcode, so the payload's poll loop reaches it millions of times. Shows
    //! whether a pending copy/load is simply never completing.
    if (gComboTraceLeft > 0 && ++gComboRomBeat % 400000 == 0) {
        OSReport("combo: romUpdate copy(bWait %d off %08X size %08X) load(bWait %d bDone %d "
                 "result %08X read %08X iBlock %d)\n",
                 pROM->copy.bWait, pROM->copy.nOffset, pROM->copy.nSize, pROM->load.bWait,
                 pROM->load.bDone, pROM->load.nResult, pROM->load.nSizeRead, pROM->load.iBlock);
    }
#endif

    if (pROM->copy.bWait || pROM->load.bWait) {
        if (pROM->load.bDone && pROM->load.nResult == pROM->load.nSizeRead) {
            pROM->load.bDone = false;
            if (!__romLoadBlock_Complete(pROM)) {
                return false;
            }
        }

#if IS_OOT
        nStatus = DVDGetCommandBlockStatus(&pROM->fileInfo.block);
        if (nStatus != 1) {
            if (!simulatorDVDShowError(nStatus, pROM->load.anData, pROM->load.nSizeRead,
                                       pROM->offsetToRom + pROM->load.nOffset)) {
                return false;
            }

            if ((nStatus == 0xB) || (nStatus == -1)) {
                if (!simulatorDVDRead(&pROM->fileInfo, pROM->load.anData, pROM->load.nSizeRead,
                                      pROM->offsetToRom + pROM->load.nOffset, &__romLoadBlock_CompleteGCN)) {
                    return false;
                }
            }
        }
#endif
    }

    if (!romLoadUpdate(pROM)) {
        return false;
    }

    if (!romCopyUpdate(pROM)) {
        return false;
    }

    return true;
}

#if IS_OOT
bool romSetImage(Rom* pROM, char* szNameFile) {
    tXL_FILE* pFile;
    s32 iName;
    s32 nSize;
    s32 nHeapSize;
    s32 iCode;
    u32 anData[256];

    for (iName = 0; (szNameFile[iName] != '\0') && (iName < 0x200); iName++) {
        pROM->acNameFile[iName] = szNameFile[iName];
    }
    pROM->acNameFile[iName] = '\0';

    if (xlFileGetSize(&nSize, pROM->acNameFile)) {
        s32 fret;

        pROM->nSize = (u32)(nSize - pROM->offsetToRom);
        nHeapSize = (nSize + 0x1FFF) & ~0x1FFF;
        pROM->nCountBlockRAM = nHeapSize / 0x2000;
        pROM->nSizeCacheRAM = nHeapSize;

        if (!xlHeapTake(&pROM->pBuffer, nHeapSize | 0x70000000) &&
            !helpMenuAllocate(SYSTEM_HELP(gpSystem), pROM, nHeapSize | 0x70000000, (void*)fn_80042064)) {
            fret = 0;
        } else {
            fret = 1;
            pROM->pCacheRAM = pROM->pBuffer;
        }

        if (!fret) {
            return false;
        }
    } else {
        return false;
    }

    if (!xlFileOpen(&pFile, XLFT_BINARY, szNameFile)) {
        return false;
    }

    if (!xlFileSetPosition(pFile, pROM->offsetToRom)) {
        return false;
    }

    if (!xlFileGet(pFile, &pROM->acHeader, sizeof(pROM->acHeader))) {
        return false;
    }

    if (!xlFileSetPosition(pFile, pROM->offsetToRom + 0x1000)) {
        return false;
    }

    if (!xlFileGet(pFile, &anData, sizeof(anData))) {
        return false;
    }

    if (!xlFileClose(&pFile)) {
        return false;
    }

    for (pROM->nChecksum = 0, iCode = 0; iCode < ARRAY_COUNT(anData); iCode++) {
        pROM->nChecksum += anData[iCode];
    }

    romSetEndianness(pROM);
    return true;
}
#elif IS_MM
static inline bool romSetImage_Inline(Rom* pROM, s32 nSize, bool bRaw) {
    s32 nHeapSize;

    pROM->nSize = nSize;
    nHeapSize = ROUND_UP(pROM->nSize, 0x2000);

    //! Cap the resident image so it fits in MEM2; the block cache then
    //! pages the rest on demand. romLoadFullOrPart() picks RLM_PART as soon
    //! as nSize exceeds nSizeCacheRAM. See ROM_CACHE_MAX for why raw images only.
    if (bRaw && nHeapSize > ROM_CACHE_MAX) {
        nHeapSize = ROM_CACHE_MAX;
    }

    pROM->nSizeCacheRAM = nHeapSize;
    pROM->nCountBlockRAM = nHeapSize / 0x2000;

    if (nHeapSize + 0x01B00000 < 0x03600000) {
        if (!xlHeapTake(&pROM->pBuffer, nHeapSize | 0x70000000)) {
            return false;
        }

        lbl_80200740 = 0;
    } else if (!fn_8005F5F4(SYSTEM_HELP(gpSystem), &pROM->pBuffer, nHeapSize | 0x70000000, fn_80042064)) {
        return false;
    } else {
        lbl_80200740 = 1;
    }

    pROM->pCacheRAM = pROM->pBuffer;
    return true;
}

bool romSetImage(Rom* pROM, char* szNameFile) {
    tXL_FILE* pFile;
    s32 iName;
    u32 nSize;
    bool bRaw;

    for (iName = 0; (szNameFile[iName] != '\0') && (iName < 0x200); iName++) {
        pROM->acNameFile[iName] = szNameFile[iName];
    }
    pROM->acNameFile[iName] = '\0';

    xlFileGetSize(&pROM->unk_218, pROM->acNameFile);

    if (!xlFileOpen(&pFile, XLFT_BINARY, szNameFile)) {
        return false;
    }

    if (!xlFileGet(pFile, &nSize, 4)) {
        return false;
    }

    if (!xlFileClose(&pFile)) {
        return false;
    }

    // A plain ROM has no header: take its size from the file itself.
    bRaw = ROM_IS_RAW(nSize);

    if (!romSetImage_Inline(pROM, bRaw ? pROM->unk_218 : nSize >> 2, bRaw)) {
        return false;
    }

    //! When the image does not fit in the cache it is served block by block,
    //! so open the NAND handle romLoadBlock() reads through. This has to happen
    //! before the romCopy() below: that call already goes through the block cache.
    if ((s32)pROM->nSize > pROM->nSizeCacheRAM && !romOpenNAND(pROM)) {
        return false;
    }

    if (!romCopy(SYSTEM_ROM(gpSystem), pROM->acHeader, 0, 0x40, NULL)) {
        return false;
    }

    return true;
}

#endif

bool romGetImage(Rom* pROM, char* acNameFile) {
    if (pROM->acNameFile[0] == '\0') {
        return false;
    }

    if (acNameFile != NULL) {
        s32 iName;

        for (iName = 0; pROM->acNameFile[iName] != '\0'; iName++) {
            acNameFile[iName] = pROM->acNameFile[iName];
        }
    }

    return true;
}

#if IS_OOT
bool romGetBuffer(Rom* pROM, void** pBuffer, u32 nAddress, s32* pData) {
    if (pROM->eModeLoad == RLM_FULL) {
        nAddress &= 0x07FFFFFF;

        if (pData != NULL) {
            u32 nSize = pROM->nSize;

            if (nAddress >= nSize) {
                return false;
            }

            if (nAddress + *pData > nSize) {
                *pData -= (nSize - nAddress);
            }
        }

        *pBuffer = (void*)((u32)pROM->pBuffer + nAddress);
        return true;
    }

    return false;
}
#endif

#if IS_MM
s32 fn_80052A6C(Rom* pROM) {
    if (pROM->bLoad) {
        //! TODO: romCopyLoad?
        if (!romLoadFullOrPart(pROM)) {
            return false;
        }

        pROM->bLoad = false;
        return true;
    }

    return true;
}
#endif

bool romEvent(Rom* pROM, s32 nEvent, void* pArgument) {
    switch (nEvent) {
        case 2:
            pROM->nSize = 0;
            pROM->nTick = 0;
            pROM->bLoad = true;
            pROM->bFlip = false;
            pROM->acNameFile[0] = '\0';
            pROM->eModeLoad = RLM_NONE;
            pROM->unk_C = 1;
            pROM->pBuffer = NULL;
            pROM->offsetToRom = 0;
            pROM->anOffsetBlock = NULL;
            pROM->nCountOffsetBlocks = 0;
            pROM->copy.nSize = 0;
            pROM->copy.bWait = false;
            pROM->load.bWait = false;
            pROM->load.nOffset1 = 0;
            pROM->load.nOffset0 = 0;
            pROM->load.bDone = false;
            pROM->nSizeCacheRAM = 0;
            pROM->nCountBlockRAM = 0;
            pROM->pCacheRAM = NULL;
            break;
        case 3:
            if (pROM->pBuffer != NULL) {
                OSSetMEM2ArenaLo(pROM->pBuffer);
            }
            break;
        case 0x1002:
            switch (((CpuDevice*)pArgument)->nType) {
                case 0:
#if IS_OOT
                    if (!cpuSetGetBlock(SYSTEM_CPU(gpSystem), pArgument, (GetBlockFunc)romGetBlock)) {
                        return false;
                    }
#endif

                    if (!cpuSetDevicePut(SYSTEM_CPU(gpSystem), pArgument, (Put8Func)romPut8, (Put16Func)romPut16,
                                         (Put32Func)romPut32, (Put64Func)romPut64)) {
                        return false;
                    }
                    if (!cpuSetDeviceGet(SYSTEM_CPU(gpSystem), pArgument, (Get8Func)romGet8, (Get16Func)romGet16,
                                         (Get32Func)romGet32, (Get64Func)romGet64)) {
                        return false;
                    }
                    break;
                case 1:
                    if (!cpuSetDevicePut(SYSTEM_CPU(gpSystem), pArgument, (Put8Func)romPutDebug8,
                                         (Put16Func)romPutDebug16, (Put32Func)romPutDebug32,
                                         (Put64Func)romPutDebug64)) {
                        return false;
                    }
                    if (!cpuSetDeviceGet(SYSTEM_CPU(gpSystem), pArgument, (Get8Func)romGetDebug8,
                                         (Get16Func)romGetDebug16, (Get32Func)romGetDebug32,
                                         (Get64Func)romGetDebug64)) {
                        return false;
                    }
                    break;
            }
            break;
        case 0:
        case 1:
            break;
        case 0x1003:
        case 0x1004:
#if IS_OOT
        case 0x1007:
#elif IS_MM
        case 0x1005:
#endif
            break;
        default:
            return false;
    }

    return true;
}
