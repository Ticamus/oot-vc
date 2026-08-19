#ifndef _SOUNDRVL_H
#define _SOUNDRVL_H

#include "emulator/xlObject.h"
#include "macros.h"
#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VI_NTSC_CLOCK 48681812

typedef enum SoundRamp {
    SR_NONE = -1,
    SR_DECREASE = 0,
    SR_INCREASE = 1,
} SoundRamp;

typedef enum SoundBeep {
    SOUND_BEEP_ACCEPT = 0,
    SOUND_BEEP_DECLINE = 1,
    SOUND_BEEP_SELECT = 2,
    SOUND_BEEP_COUNT = 3,
} SoundBeep;

typedef enum SoundPlayMode {
    SPM_NONE = -1,
    SPM_PLAY = 0,
    SPM_RAMPQUEUED = 1,
    SPM_RAMPPLAYED = 2,
} SoundPlayMode;

#if IS_MM
typedef struct SoundBuf {
    /* 0x0 */ u32 nSize;
    /* 0x4 */ void* pData;
} SoundBuf; // size = 0x8

//! Value MM stores in Sound::eMode when soundPlayBuffer found the ready queue empty and fell back
//! to pBufferZero. Distinct from the SoundPlayMode enum above, which is OoT's.
#define SOUND_MM_MODE_STARVE 3

//! Number of descriptors in the pool. `cmpwi r26, 0x10` in fn_80073C78, and the free-list push in
//! fn_80073208 refuses to grow past it. Bounds how large nDepthTarget may safely be set.
#define SOUND_MM_BUF_COUNT 16

//! Value systemSetupGameALL writes to Sound::nDepthTarget, i.e. how many finished audio buffers the
//! backend keeps queued before it tells the guest "AI busy" and the guest stops producing. Retail
//! MM ships 6.
#define COMBO_SND_DEPTH 6

typedef struct Sound {
    /* 0x000 */ s32 bMute;
    /* 0x004 */ void* pSrcData;
    /* 0x008 */ s32 nClockVI;
    /* 0x00C */ s32 nFrequency;
    /* 0x010 */ s32 nDacrate;
    /* 0x014 */ s32 nSndLen;
    /* 0x018 */ SoundBuf aBuf[SOUND_MM_BUF_COUNT];
    /* 0x098 */ s32 nVolume;
    /* 0x09C */ s32 nVolumeCurve[257];
    /* 0x4A0 */ s32 nFreeCount;
    /* 0x4A4 */ SoundBuf* apFree[SOUND_MM_BUF_COUNT];
    /* 0x4E4 */ s32 nReadyCount;
    /* 0x4E8 */ SoundBuf* apReady[SOUND_MM_BUF_COUNT];
    /* 0x528 */ SoundBuf* pFill;
    /* 0x52C */ SoundBuf* pPlaying;
    /* 0x530 */ f32 rMasterVolume;
    /* 0x534 */ s32 nDepthTarget;
    /* 0x538 */ s32 bDMAMatched;
    //! 0 selects a volume-derived answer in fn_80073B20 instead of the queue-depth one. MM sets 1.
    /* 0x53C */ s32 bFlowControl;
    /* 0x540 */ s32 nFakeAILen;
    /* 0x544 */ s32 nMuteBuffers;
    /* 0x548 */ f32 rVolumeCur;
    /* 0x54C */ f32 rVolumeTarget;
    /* 0x550 */ f32 rVolumeStep;
    /* 0x554 */ volatile s32 eMode;
    /* 0x558 */ void* pBufferZero;
    /* 0x55C */ void* pBufferB;
    /* 0x560 */ void* pBufferC;
    /* 0x564 */ void* pBufferD;
    /* 0x568 */ s32 nSizePlay;
    /* 0x56C */ s32 nSizeZero;
    /* 0x570 */ s32 nSizeB;
    /* 0x574 */ s32 nSizeCD;
    /* 0x578 */ s16 bInterpolate;
    /* 0x57A */ u8 pad_57A[2];
    /* 0x57C */ u8 anScratch[0x10];
} Sound; // size >= 0x58C

#else

typedef struct Sound {
    /* 0x000 */ s32 unk_00;
    /* 0x004 */ void* pSrcData;
    /* 0x008 */ s32 nFrequency;
    /* 0x00C */ s32 nDacrate;
    /* 0x010 */ s32 nSndLen;
    /* 0x014 */ void* apBuffer[16];
    /* 0x054 */ s32 anSizeBuffer[16];
    /* 0x094 */ s32 unk_94;
    /* 0x098 */ s32 nVolumeCurve[257];
    /* 0x49C */ s32 iBufferPlay;
    /* 0x4A0 */ s32 iBufferMake;
    /* 0x4A4 */ volatile SoundPlayMode eMode;
    /* 0x4A8 */ void* pBufferZero;
    /* 0x4AC */ void* pBufferHold;
    /* 0x4B0 */ void* pBufferRampUp;
    /* 0x4B4 */ void* pBufferRampDown;
    /* 0x4B8 */ s32 nSizePlay;
    /* 0x4BC */ s32 nSizeZero;
    /* 0x4C0 */ s32 nSizeHold;
    /* 0x4C4 */ s32 nSizeRamp;
} Sound; // size = 0x4C8

#endif

bool soundWipeBuffers(Sound* pSound);
bool soundSetLength(Sound* pSound, s32 nSize);
bool soundSetDACRate(Sound* pSound, s32 nDacRate);
bool soundSetAddress(Sound* pSound, void* pData);
bool soundGetDMABuffer(Sound* pSound, u32* pnSize);
bool soundSetBufferSize(Sound* pSound, s32 nSize);
bool soundLoadBeep(Sound* pSound, SoundBeep iBeep, char* szNameFile);
bool soundPlayBeep(Sound* pSound, SoundBeep iBeep);
bool soundEvent(Sound* pSound, s32 nEvent, void* pArgument);

extern _XL_OBJECTTYPE gClassAudio;

#ifdef __cplusplus
}
#endif

#endif
