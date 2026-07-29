#include "emulator/vc64_RVL.h"
#include "emulator/codeRVL.h"
#include "emulator/controller.h"
#include "emulator/flash.h"
#include "emulator/frame.h"
#include "emulator/rom.h"
#include "emulator/storeRVL.h"
#include "emulator/system.h"
#include "emulator/xlFileRVL.h"
#include "emulator/xlHeap.h"
#include "emulator/xlObject.h"
#include "macros.h"
#include "revolution/demo.h"
#include "revolution/gx.h"
#include "revolution/vi.h"
#include "string.h"

#if IS_OOT || IS_MT
static char* gaszArgument[12];

System* gpSystem;
#elif IS_MM
static char* gaszArgument[8];

bool gResetBeginFlag = true;

Frame* gpFrame;
System* gpSystem;
Code* gpCode;

bool gbDisplayedError;
bool gbReset;
u32 gnTickReset;

bool gButtonDownToggle = false;
bool gDVDResetToggle = false;

extern u32 lbl_80200654;
extern u32 lbl_801FF7DC;


#endif

static void simulatorDEMOSwapBuffers(void) {
#if IS_OOT
    if ((void*)DemoCurrentBuffer == (void*)DemoFrameBuffer1) {
        DemoCurrentBuffer = DemoFrameBuffer2;
    } else {
        DemoCurrentBuffer = DemoFrameBuffer1;
    }
#elif IS_MM
    lbl_80200654++;

    if (lbl_80200654 >= lbl_801FF7DC) {
        lbl_80200654 = 0;
    }
#endif
}

void simulatorDEMODoneRender(void) {
#if IS_OOT || IS_MT
    SYSTEM_FRAME(gpSystem)->nMode = 0;
    SYSTEM_FRAME(gpSystem)->nModeVtx = -1;
    frameDrawReset(SYSTEM_FRAME(gpSystem), 0x5FFED);

    GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    GXSetColorUpdate(GX_ENABLE);
    GXCopyDisp(DemoCurrentBuffer, GX_TRUE);
    GXDrawDone();
    VISetNextFrameBuffer(DemoCurrentBuffer);
    VIFlush();
    VIWaitForRetrace();
    simulatorDEMOSwapBuffers();
#elif IS_MM
    GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    GXSetColorUpdate(GX_ENABLE);
    GXCopyDisp(lbl_8017B1E0[lbl_80200654 * 2].unk_04, GX_TRUE);
    GXDrawDone();
    VISetNextFrameBuffer(lbl_8017B1E0[lbl_80200654 * 2].unk_04);
    VIFlush();
    VIWaitForRetrace();
    simulatorDEMOSwapBuffers();
#endif
}

#if IS_OOT
bool simulatorDVDShowError(s32 nStatus, void* anData, s32 nSizeRead, u32 nOffset) { return true; }

bool simulatorDVDOpen(char* szNameFile, DVDFileInfo* pFileInfo) { return false; }
#endif

bool simulatorDVDRead(DVDFileInfo* pFileInfo, void* anData, s32 nSizeRead, s32 nOffset, DVDCallback callback) {
#if IS_OOT || IS_MT
    return false;
#elif IS_MM
    __simulatorCNTReadNAND();

    if (callback != NULL) {
        callback(nSizeRead, NULL);
    }

    return true;
#endif
}

#if IS_OOT

bool simulatorShowLoad(s32 unknown, char* szNameFile, f32 rProgress) { return true; }

#elif IS_MM

// bool fn_800070C0(s32 arg2, ? (*arg4)(s32, ?)) {
//     fn_80083848();
//
//     if (arg4 != NULL) {
//         arg4(arg2, 0);
//     }
//
//     return true;
// }

bool fn_80007118(u32* mapData, s32 channel) {
    simulatorSetControllerMap(SYSTEM_CONTROLLER(gpSystem), channel, mapData);
    return true;
}

bool simulatorCopyControllerMap(u32* mapDataOutput, u32* mapDataInput) {
    int i;

    for (i = 0; i < 22; i++) {
        mapDataOutput[i] = mapDataInput[i];
    }

    return true;
}

bool simulatorReadFLASH(s32 address, u8* data, s32 size) {
    if (!fn_80061B88(SYSTEM_FLASH(gpSystem)->pStore, data, address, size)) {
        return false;
    }

    return true;
}

bool simulatorWriteFLASH(s32 address, u8* data, s32 size) {
    if (!fn_80061BC0(SYSTEM_FLASH(gpSystem)->pStore, data, address, size)) {
        return false;
    }

    return true;
}

bool fn_80007280() {
    OSGetTick();
    return true;
}

#endif

static bool simulatorParseArguments(void) {
    char* szText;
    char* szValue;
    s32 iArgument;

    for (iArgument = 0; iArgument < SAT_COUNT; iArgument++) {
        gaszArgument[iArgument] = NULL;
    }

#if IS_OOT || IS_MT
    iArgument = 1;
#elif IS_MM
    iArgument = 0;
#endif

    while (iArgument < xlCoreGetArgumentCount()) {
        xlCoreGetArgument(iArgument, &szText);
        iArgument++;
        if (szText[0] == '-' || szText[0] == '/' || szText[0] == '\\') {
            if (szText[2] == '\0') {
                xlCoreGetArgument(iArgument, &szValue);
                iArgument++;
            } else {
                szValue = &szText[2];
            }

            switch (szText[1]) {
#if IS_OOT || IS_MT
                case 'L':
                case 'l':
                    gaszArgument[SAT_UNK9] = szValue;
                    break;
                case 'H':
                case 'h':
                    gaszArgument[SAT_UNK10] = szValue;
                    break;
                case 'V':
                case 'v':
                    gaszArgument[SAT_VIBRATION] = szValue;
                    break;
                case 'R':
                case 'r':
                    gaszArgument[SAT_RESET] = szValue;
                    break;
                case 'P':
                case 'p':
                    gaszArgument[SAT_PROGRESSIVE] = szValue;
                    break;
                case 'G':
                case 'g':
                    gaszArgument[SAT_CONTROLLER] = szValue;
                    break;
                case 'C':
                case 'c':
                    gaszArgument[SAT_MEMORYCARD] = szValue;
                    break;
                case 'M':
                case 'm':
                    gaszArgument[SAT_MOVIE] = szValue;
                    break;
                case 'X':
                case 'x':
                    gaszArgument[SAT_XTRA] = szValue;
                    break;
#elif IS_MM
                case 'V':
                case 'v':
                    gaszArgument[SAT_VIBRATION] = szValue;
                    break;
                case 'P':
                case 'p':
                    gaszArgument[SAT_PROGRESSIVE] = szValue;
                    break;
                case 'R':
                case 'r':
                    gaszArgument[SAT_RESET] = szValue;
                    break;
                case 'X':
                case 'x':
                    gaszArgument[SAT_XTRA] = szValue;
                    break;
                case 'C':
                case 'c':
                    gaszArgument[SAT_MEMORYCARD] = szValue;
                    break;
                case 'M':
                case 'm':
                    gaszArgument[SAT_MOVIE] = szValue;
                    break;
                case 'G':
                case 'g':
                    gaszArgument[SAT_CONTROLLER] = szValue;
                    break;
#endif
            }
        } else {
            gaszArgument[SAT_NAME] = szText;
        }
    }
    return true;
}

bool simulatorGetArgument(SimulatorArgumentType eType, char** pszArgument) {
    if (eType != SAT_NONE && pszArgument != NULL && gaszArgument[eType] != NULL) {
        *pszArgument = gaszArgument[eType];
        return true;
    }

    return false;
}

static inline bool simulatorRun(SystemMode* peMode) {
    s32 nResult;

    while (systemGetMode(gpSystem, peMode) && *peMode == SM_RUNNING) {
        nResult = systemExecute(gpSystem, 100000);
        if (!nResult) {
            return nResult;
        }
    }

    return true;
}

#if IS_MM
#endif

#if IS_OOT || IS_MT
#define FRAME_PTR (SYSTEM_FRAME(gpSystem))
#elif IS_MM
#define FRAME_PTR (gpFrame)
#endif

bool xlMain(void) {
#if IS_OOT || IS_MT
    SystemMode eMode;
    s32 nSize0;
    s32 nSize1;
    GXColor color;
    char acNameROM[32];
#elif IS_MM
    // char acNameROM[45];
    // char* spC;
    // s32 iName;
    // char cName;
    char acNameROM[45];
    GXColor color;
    s32 sp2C;
    s32 nSize1;
    s32 sp10;
    s32 nSize0;
    SystemMode eMode;
    s32 iName;
    char *szNameROM;
#endif

    simulatorParseArguments();

#if IS_OOT
    if (!xlHeapGetHeap1Free(&nSize0)) {
        return false;
    }

    if (nSize0 > 0x01800000) {
        OSReport("\n\nERROR: This program MUST be run on a system with 24MB (or less) memory!\n");
        OSPanic("vc64_RVL.c", 1352, "       Please reduce memory-size to 24MB (using 'setsmemsize 0x1800000')\n\n");
    }
#elif IS_MM
    gDVDResetToggle = 0;
#endif

#ifdef __MWERKS__
    asm {
        li      r3, 0x706
        oris    r3, r3, 0x706
        mtspr   GQR6, r3
        li      r3, 0x507
        oris    r3, r3, 0x507
        mtspr   GQR7, r3
    }
#endif

#if IS_OOT
    VISetBlack(true);
    VIFlush();
    VIWaitForRetrace();

    color.r = color.g = color.b = 0;
    color.a = 255;

    GXSetCopyClear(color, 0xFFFFFF);
#elif IS_MM
    color.r = color.g = color.b = 0;
    color.a = 255;

    gbDisplayedError = false;
    gButtonDownToggle = false;
    gResetBeginFlag = true;

    GXSetCopyClear(color, 0xFFFFFF);
    VISetBlack(true);
    VIFlush();
    VIWaitForRetrace();

    xlCoreBeforeRender();
    simulatorDEMODoneRender();

    xlCoreBeforeRender();
    simulatorDEMODoneRender();

    VIWaitForRetrace();
    VISetBlack(false);
    VIFlush();

    gbReset = false;
    gnTickReset = OSGetTick();
#endif

#if IS_MM
    if (!xlHeapGetHeap1Free(&nSize1)) { //1c
        return false;
    }

    if (!xlHeapGetHeap2Free(&nSize0)) { //14
        return false;
    }

    gpSystem = NULL;

    if (!xlObjectMake((void**)&gpCode, NULL, &gClassCode)) {
        return false;
    }
#else
    if (!xlHeapGetHeap1Free(&nSize0)) { //1c
        return false;
    }
#endif

    if (!xlObjectMake((void**)&gpSystem, NULL, &gClassSystem)) {
        return false;
    }

#if IS_OOT
    if (!xlFileSetOpen((DVDOpenCallback)simulatorDVDOpen)) {
        return false;
    }

    if (!xlFileSetRead((DVDReadCallback)simulatorDVDRead)) {
        return false;
    }

    strcpy(acNameROM, "rom");
#elif IS_MM
    errorDisplayInit();

    if (simulatorGetArgument(SAT_NAME, &szNameROM)) {
        strcpy(acNameROM, szNameROM);
    } else {
        strcpy(acNameROM, "romc");
    }

    iName = strlen(acNameROM) - 1;
    while (iName >= 0 && acNameROM[iName] != '.') {
        iName--;
    }
#endif

    if (!romSetImage(SYSTEM_ROM(gpSystem), acNameROM)) {
        return false;
    }

    if (!systemReset(gpSystem)) {
        return false;
    }

    if (!frameShow(FRAME_PTR)) {
        return false;
    }

    

#if IS_MM
    if (!xlHeapGetHeap1Free(&nSize0)) {
        return false;
    }

    if (!xlHeapGetHeap2Free(&sp10)) {
        return false;
    }

    xlHeapGetHeap1Free(&nSize1);
    xlHeapGetHeap2Free(&sp2C);
    OSReport("%ld - %ld memory used\nmemory free: %ld - %ld\n", gnSizeHeapOS[0], gnSizeHeapOS[1], nSize1, sp2C);
#else
    if (!xlHeapGetHeap1Free(&nSize1)) {
        return false;
    }
#endif

    if (!systemSetMode(gpSystem, SM_RUNNING)) {
        return false;
    }

    simulatorRun(&eMode);

    if (!xlObjectFree((void**)&gpSystem)) {
        return false;
    }

#if IS_MM
    if (!xlObjectFree((void**)&gpFrame)) {
        return false;
    }

    if (!xlObjectFree((void**)&gpCode)) {
        return false;
    }
#endif

    return true;
}
