#ifndef _ROM_H
#define _ROM_H

#include "emulator/errordisplay.h"
#include "emulator/xlFileRVL.h"
#include "emulator/xlObject.h"
#include "revolution/types.h"
#include "macros.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROM_THREAD_SIZE 0x2000

#if IS_OOT || IS_MT
#define ROM_THREAD (DefaultThread)
#elif IS_MM
#define ROM_THREAD (sRomThread)
#endif

#if IS_MM
//! Not in the original game: 4096 blocks of 0x2000 only cover a 32 MB image, and
//! romLoadBlock()/romCopyImmediate() index aBlock[nOffset / 0x2000] with no bound
//! check -- a larger ROM writes past the array into nTick / pCacheRAM / the cache
//! bitfields. 8192 covers 64 MB (the OoTMM combo image).
//! WARNING: this grows Rom by 0x10000 and shifts every field after aBlock. rom.c is
//! the only object linked from source for mm-j that touches those fields; the sole
//! remaining reader in the original binary is cpuExecuteIdle's `pROM->copy.nSize`
//! test, which now lands on aBlock[4289].nSize -- always 0 for a 64 MB image, i.e.
//! the same result as an idle ROM, so the behaviour is unchanged.
#define ROM_BLOCK_COUNT 8192
#else
#define ROM_BLOCK_COUNT 6144
#endif

typedef bool UnknownCallbackFunc(void);
typedef bool ProgressCallbackFunc(f32 progressPercent);

typedef enum RomModeLoad {
    RLM_NONE = -1,
    RLM_PART = 0,
    RLM_FULL = 1,
    RLM_COUNT = 2,
} RomModeLoad;

typedef enum RomCacheType {
    RCT_NONE = -1,
    RCT_RAM = 0,
    RCT_ARAM = 1,
} RomCacheType;

typedef struct RomBlock {
    /* 0x00 */ s32 iCache; // Stores cache index `i` if the block is in RAM, or `-(i + 1)` if the block is in ARAM
    /* 0x04 */ u32 nSize;
    /* 0x08 */ u32 nTickUsed;
    /* 0x0C */ s8 keep;
} RomBlock; // size = 0x10

typedef struct RomCopyState {
    /* 0x00 */ bool bWait;
    /* 0x04 */ UnknownCallbackFunc* pCallback;
    /* 0x08 */ u8* pTarget;
    /* 0x0C */ u32 nSize;
    /* 0x10 */ u32 nOffset;
} RomCopyState; // size = 0x14

typedef struct RomLoadState {
    /* 0x00 */ bool bWait;
    /* 0x04 */ bool bDone;
    /* 0x08 */ s32 nResult;
    /* 0x0C */ u8* anData;
    /* 0x10 */ UnknownCallbackFunc* pCallback;
    /* 0x14 */ s32 iCache;
    /* 0x18 */ s32 iBlock;
    /* 0x1C */ s32 nOffset;
    /* 0x20 */ u32 nOffset0;
    /* 0x24 */ u32 nOffset1;
    /* 0x28 */ u32 nSize;
    /* 0x2C */ u32 nSizeRead;
} RomLoadState; // size = 0x30

typedef struct Rom {
    /*   OoT      MM   */
#if IS_MM
    /* 0x00000 0x00000 */ void* pHost;
#endif
    /* 0x00000 0x00004 */ void* pBuffer;
    /* 0x00004 0x00008 */ bool bFlip;
    /* 0x00008 0x0000C */ bool bLoad;
#if IS_OOT || IS_MT
    /* 0x0000C   N/A   */ s32 unk_C;
#endif
    /* 0x00010 0x00010 */ char acNameFile[513];
    /* 0x00214 0x00214 */ u32 nSize;
    /* 0x00218 0x00218 */ s32 unk_218;
    /* 0x0021C 0x0021C */ RomModeLoad eModeLoad;
    /* 0x00220 0x00220 */ RomBlock aBlock[ROM_BLOCK_COUNT]; // MM: was 4096, see ROM_BLOCK_COUNT
    /* 0x18220 0x10220 */ u32 nTick;
    /* 0x18224 0x10224 */ u8* pCacheRAM;
    /* 0x18228 0x10228 */ u8 anBlockCachedRAM[IS_MM ? 1024 : 4096]; // Bitfield, one bit per block
    /* 0x19228 0x10628 */ u8 anBlockCachedARAM[2046]; // Bitfield, one bit per block
    /* 0x19A28 0x10E28 */ RomCopyState copy;
    /* 0x19A3C 0x10E3C */ RomLoadState load;
#if IS_MM
    /*   N/A   0x10E6C */ s32 unk_C;
    /*   N/A   0x10E70 */ s32 unk_10E70;
#endif
    /* 0x19A6C 0x10E74 */ s32 nCountBlockRAM;
    /* 0x19A70 0x10E78 */ s32 nSizeCacheRAM;
    /* 0x19A74 0x10E7C */ u8 acHeader[64];
    /* 0x19AB4 0x10EBC */ u32* anOffsetBlock;
    /* 0x19AB8 0x10EC0 */ s32 nCountOffsetBlocks;
#if IS_OOT || IS_MT
    /* 0x19ABC 0x10EC4 */ u32 nChecksum;
#endif
    /* 0x19AC0 0x10EC8 */ DVDFileInfo fileInfo;
#if IS_MM
    /*   N/A   0x10F00 */ s32 unk_10F00;
    /*   N/A   0x10F04 */ s32 unk_10F04;
#endif
    /* 0x19AFC 0x10F08 */ s32 offsetToRom;
} Rom; // size = 0x19B00 ; 0x10F0C

s32 fn_80042E30(EDString* pSTString);
bool romGetPC(Rom* pROM, u64* pnPC);

#if IS_OOT || IS_MT
bool romGetCode(Rom* pROM, s32* acCode);
#elif IS_MM
bool romGetCode(Rom* pROM, char* acCode);
bool romTestCode(Rom* pROM, char* acCode);
bool fn_800518EC(void);
#endif

//! NOTE: The debug informations indicates that `nSize` is unsigned, but the
//! generated code seems to treat it as signed.
bool romCopy(Rom* pROM, void* pTarget, s32 nOffset, s32 nSize, UnknownCallbackFunc* pCallback);

bool romUpdate(Rom* pROM);
bool romSetImage(Rom* pROM, char* szNameFile);
bool romGetImage(Rom* pROM, char* acNameFile);
bool romGetBuffer(Rom* pROM, void** pBuffer, u32 nAddress, s32* pData);
bool romEvent(Rom* pROM, s32 nEvent, void* pArgument);

extern _XL_OBJECTTYPE gClassROM;

#ifdef __cplusplus
}
#endif

#endif
