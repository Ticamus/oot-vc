#ifndef _VC64_RVL_H
#define _VC64_RVL_H

#include "emulator/system.h"
#include "revolution/dvd.h"
#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SimulatorArgumentType {
    SAT_NONE = -1,
    SAT_NAME = 0,
    SAT_PROGRESSIVE = 1,
    SAT_VIBRATION = 2,
    SAT_RESET = 3,
    SAT_CONTROLLER = 4,
    SAT_XTRA = 5,
    SAT_MEMORYCARD = 6,
    SAT_MOVIE = 7,
#if IS_OOT || IS_MT
    SAT_UNK8 = 8,
    SAT_UNK9 = 9,
    SAT_UNK10 = 10,
#endif
    SAT_COUNT,
} SimulatorArgumentType;

//! TODO: document these
typedef struct struct_8017B1E0 {
    /* 0x00 */ u32 unk_00;
    /* 0x04 */ void* unk_04;
} struct_8017B1E0; // size = 0x8

extern struct_8017B1E0 lbl_8017B1E0[];




void simulatorDEMODoneRender(void);
bool simulatorDVDShowError(s32 nStatus, void* anData, s32 nSizeRead, u32 nOffset);
bool simulatorDVDOpen(char* szNameFile, DVDFileInfo* pFileInfo);
bool simulatorDVDRead(DVDFileInfo* pFileInfo, void* anData, s32 nSizeRead, s32 nOffset, DVDCallback callback);
bool simulatorShowLoad(s32 unknown, char* szNameFile, f32 rProgress);
bool simulatorGetArgument(SimulatorArgumentType eType, char** pszArgument);
bool xlMain(void);

extern System* gpSystem;

#ifdef __cplusplus
}
#endif

#endif
