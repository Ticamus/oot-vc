#ifndef _LIBRARY_H
#define _LIBRARY_H

#include "emulator/cpu.h"
#include "emulator/xlObject.h"
#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*LibraryFuncImpl)(Cpu*);

typedef struct LibraryFunc {
    /* 0x0 */ char* szName;
    /* 0x4 */ LibraryFuncImpl pfLibrary;
    /* 0x8 */ u32 anData[17];
} LibraryFunc; // size = 0x4C

#if IS_MM
// MM keeps a back-pointer to the owning System at 0x04 and shifts everything after it down
// by four bytes. Offsets read off libraryEvent()'s case 2 initialiser (stores to
// 0x00/0x04/0x08/0x0C/0x10/0x14), libraryFindException()'s `stw r0, 0x10(r27)` and the
// 0x68 sizeof in gClassLibrary.
typedef struct Library {
    /* 0x00 */ s32 nFlag;
    /* 0x04 */ void* pHost;
    /* 0x08 */ s32 nAddStackSwap;
    /* 0x0C */ s32 nCountFunction;
    /* 0x10 */ s32 nAddressException;
    /* 0x14 */ LibraryFunc* aFunction;
    /* 0x18 */ void* apData[10];
    /* 0x40 */ s32 anAddress[10];
} Library; // size = 0x68
#else
typedef struct Library {
    /* 0x00 */ s32 nFlag;
    /* 0x04 */ s32 nAddStackSwap;
    /* 0x08 */ s32 nCountFunction;
    /* 0x0C */ s32 nAddressException;
    /* 0x10 */ LibraryFunc* aFunction;
    /* 0x14 */ void* apData[10];
    /* 0x3C */ s32 anAddress[10];
} Library; // size = 0x64
#endif

extern _XL_OBJECTTYPE gClassLibrary;

bool libraryTestFunction(Library* pLibrary, CpuFunction* pFunction);
bool libraryFunctionReplaced(Library* pLibrary, s32 iFunction);
bool libraryCall(Library* pLibrary, Cpu* pCPU, s32 iFunction);
bool libraryEvent(Library* pLibrary, s32 nEvent, void* pArgument);

#ifdef __cplusplus
}
#endif

#endif
