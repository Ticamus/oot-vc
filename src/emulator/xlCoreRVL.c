#include "emulator/xlCoreRVL.h"
#include "emulator/errordisplay.h"
#include "emulator/vc64_RVL.h"
#include "emulator/xlHeap.h"
#include "emulator/xlList.h"
#include "emulator/xlPostRVL.h"
#include "macros.h"
#include "revolution/demo.h"
#include "revolution/sc.h"
#include "revolution/vi.h"

static GXRenderModeObj rmodeobj;
static s32 gnCountArgument;
static char** gaszArgument;
static void* DefaultFifo;
static GXFifoObj* DefaultFifoObj;

GXRenderModeObj* rmode;

#if IS_MM
// Read by helpRVL.c; still unnamed.
void* lbl_80200860;

static void* gArenaHi;
static void* gArenaLo;
#endif

#if IS_MM
static inline u32 getFBTotalSize(f32 aspectRatio, u32 multiplier) {
    u16 lineCount = GXGetNumXfbLines(rmode->efbHeight, aspectRatio);
    u16 fbWidth = ROUND_UP(rmode->fbWidth, 16);
    // The (u32) cast keeps MWCC from reassociating this into (fbWidth * lineCount) * multiplier.
    return fbWidth * (u32)(lineCount * multiplier);
}
#else
static inline u32 getFBTotalSize(f32 aspectRatio) {
    u16 lineCount = GXGetNumXfbLines(rmode->efbHeight, aspectRatio);
    u16 fbWidth = ROUND_UP(rmode->fbWidth, 16);
    return fbWidth * lineCount;
}
#endif


#if IS_MM
void xlCoreInitRenderMode(GXRenderModeObj* mode) {
    u32 nTickLast;
    VITvFormat nFormat;

    SCInit();

    nTickLast = OSGetTick();
    while (SCCheckStatus() == 1 && OS_TICKS_TO_MSEC(OSGetTick() - nTickLast) < 3000) {}

    if (mode != NULL) {
        rmode = mode;
        return;
    }

    nFormat = VIGetTvFormat();
    switch (nFormat) {
        case VI_NTSC:
            rmode = &GXNtsc480IntDf;
            if (VIGetDTVStatus() && SCGetProgressiveMode() == 1) {
                rmode = &GXNtsc480Prog;
            }
            rmode->viXOrigin -= 32;
            rmode->viWidth += 64;
            break;
        case VI_PAL:
        case VI_MPAL:
        case VI_EURGB60:
            rmode = &GXPal528IntDf;
            rmode->viXOrigin -= 32;
            rmode->viWidth += 64;
            rmode->xfbHeight = rmode->viHeight = 574;
            rmode->viYOrigin = (s32)(574 - rmode->viHeight) / 2;
            rmode->efbHeight = 480;
            break;
        default: {
            extern char lbl_801562A0[];
            extern char lbl_801562B8[];
            extern char lbl_801562C4[];
            OSReport(lbl_801562A0, nFormat);
            OSPanic(lbl_801562B8, 209, lbl_801562C4);
            break;
        }
    }

    GXAdjustForOverscan(rmode, &rmodeobj, 0, 0);
    rmode = &rmodeobj;
}

#else
static void xlCoreInitRenderMode(GXRenderModeObj* mode) {
    u32 nTickLast;

    SCInit();

    nTickLast = OSGetTick();
    while (SCCheckStatus() == 1 && OS_TICKS_TO_MSEC(OSGetTick() - nTickLast) < 3000) {}

    if (mode != NULL) {
        rmode = mode;
        return;
    }

    switch (VIGetTvFormat()) {
        case VI_NTSC:
            rmode = VIGetDTVStatus() && SCGetProgressiveMode() == 1 ? &GXNtsc480Prog : &GXNtsc480IntDf;
            rmode->viXOrigin -= 32;
            rmode->viWidth += 64;
            break;
        case VI_PAL:
        case VI_MPAL:
        case VI_EURGB60:
            rmode = &GXPal528IntDf;
            rmode->viXOrigin -= 32;
            rmode->viWidth += 64;
            rmode->xfbHeight = rmode->viHeight = 574;
            rmode->viYOrigin = (s32)(574 - rmode->viHeight) / 2;
            break;
        default:
            OSPanic("xlCoreRVL.c", 138, "DEMOInit: invalid TV format\n");
            break;
    }

    rmode->efbHeight = 480;
    GXAdjustForOverscan(rmode, &rmodeobj, 0, 0);
    rmode = &rmodeobj;
}
#endif

#if IS_MM
static inline void xlCoreInitMem(void) {
    void* arenaLo;
    void* arenaHi;
    u32 fbSize;
    u32 fbSize2;

    gArenaLo = arenaLo = OSGetArenaLo();
    gArenaHi = arenaHi = OSGetArenaHi();

    if (fn_8007FC84()) {
        fbSize2 = 0xBB800;
    } else {
        fbSize2 = 0x87600;
    }

    fbSize = (((u16)(rmode->fbWidth + 0xF) & 0xFFF0) * (fn_800AB2A0(rmode->xfbHeight, rmode->viTVmode) * 2) & 0x1FFFE);

    if (fbSize < fbSize2) {
        fbSize = fbSize2;
    }

    // arenaLo = OSGetArenaLo();
    // arenaHi = OSGetArenaHi();

    // arenaLo = OSInitAlloc(arenaLo, arenaHi, 1);
    OSSetArenaLo(arenaLo);
    xlHeapSetup();

    // arenaLo = (void*)(((s32)arenaLo + 0x1F) & ~0x1F);
    // arenaHi = (void*)((s32)arenaHi & ~0x1F);
    // OSSetCurrentHeap(OSCreateHeap(arenaLo, arenaHi));
    // OSSetArenaLo(arenaHi);
}
#endif

extern void* lbl_80200858;
extern void* lbl_8020085C;
extern u32 lbl_80200654;
extern u32 lbl_801FF7DC;

static inline void __xlCoreInitGX(void) {
    GXSetViewport(0.0f, 0.0f, rmode->fbWidth, rmode->efbHeight, 0.0f, 1.0f);
    GXSetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GXSetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GXSetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
    GXSetDispCopyYScale((f32)rmode->xfbHeight / (f32)rmode->efbHeight);
    GXSetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);

    if (rmode->aa != 0) {
        GXSetPixelFmt(GX_PF_RGBA565_Z16, GX_ZC_LINEAR);
    } else {
        GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    }

    GXSetDispCopyGamma(GX_GM_1_0);

    VISetNextFrameBuffer(DemoFrameBuffer1);
    DemoCurrentBuffer = DemoFrameBuffer2;
    VIFlush();
    VIWaitForRetrace();

    if (rmode->viTVmode & 1) {
        VIWaitForRetrace();
    }
}

bool xlCoreInitGX(void) {
#if IS_MM
    u8  vfilter[7];
    int i;
    f32 filterScale = 1.0f;

    GXSetViewport(0.0f, 0.0f, rmode->fbWidth, rmode->efbHeight, 0.0f, 1.0f);
    GXSetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GXSetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GXSetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
    GXSetDispCopyYScale((f32)rmode->xfbHeight / (f32)rmode->efbHeight);

    filterScale = (filterScale < 0.0f) ? 0.0f : filterScale;
    filterScale = (filterScale > 2.0f) ? 2.0f : filterScale;
    for (i = 0; i < 7; i++)
        vfilter[i] = (u8)(filterScale * (f32)rmode->vfilter[i]);

    GXSetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, vfilter);

    if (rmode->aa != 0) {
        GXSetPixelFmt(GX_PF_RGBA565_Z16, GX_ZC_LINEAR);
    } else {
        GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    }

    GXCopyDisp(lbl_8017B1E0[lbl_80200654].unk_04, GX_TRUE);

    GXSetDispCopyGamma(GX_GM_1_0);
#else
    __xlCoreInitGX();

    VIConfigure(rmode);

    return true;
#endif

}

#if IS_OOT || IS_MT
bool xlCoreBeforeRender(void) {
    if (rmode->field_rendering != 0) {
        GXSetViewportJitter(0.0f, 0.0f, rmode->fbWidth, rmode->efbHeight, 0.0f, 1.0f, VIGetNextField());
    } else {
        GXSetViewport(0.0f, 0.0f, rmode->fbWidth, rmode->efbHeight, 0.0f, 1.0f);
    }

    GXInvalidateVtxCache();
    GXInvalidateTexAll();
    GXSetDrawSync(0);

    return true;
}
#endif

s32 xlCoreGetArgumentCount(void) { return gnCountArgument; }

bool xlCoreGetArgument(s32 iArgument, char** pszArgument) {
    if ((iArgument >= 0) && (iArgument < gnCountArgument)) {
        *pszArgument = *(gaszArgument + iArgument);
        return true;
    }

    return false;
}

bool xlCoreHiResolution(void) { return true; }

bool fn_8007FC84(void) {
#if IS_OOT
    switch (VIGetTvFormat()) {
        case VI_PAL:
        case VI_MPAL:
        case VI_EURGB60:
            return true;
        default:
            break;
    }
    return false;
#elif IS_MM
    u32 nFormat = VIGetTvFormat();

    if (nFormat == VI_PAL) {
        return true;
    }

    return false;
#elif IS_MT
    return VIGetTvFormat() == VI_PAL;
#endif
}

#if IS_OOT || IS_MT
void xlExit(void) { OSPanic("xlCoreRVL.c", 524, "xlExit"); }
#endif

#if IS_MM
#pragma global_optimizer off
#endif

int main(int nCount, char** aszArgument) {
    void* arenaLo;
    void* arenaHi;
    f32 aspectRatio;
#if IS_MM
    u32 nSize;
    u32 nSizeHeap;
#else
    s32 nSize;
    s32 nSizeHeap;
#endif

    gnCountArgument = nCount;
    gaszArgument = aszArgument;

#if IS_OOT || IS_MT
    OSInit();
    DVDInit();

    if (NANDInit() != 0) {
        xlExit();
    }

    contentInit();
    VIInit();
    xlCoreInitRenderMode(NULL);
    VIConfigure(rmode);
    OSInitFastCast();
#elif IS_MM
    __PADDisableRecalibration(true);
    OSInitFastCast();
    fn_8007DBDC(0, 0);
    OSInit();
    DVDInit();
    DVDSetAutoFatalMessaging(false);
    NANDInit();
    fn_8011007C();
    fn_800A37DC();
    xlCoreInitRenderMode(NULL);

    gArenaLo = arenaLo = OSGetArenaLo();
    gArenaHi = arenaHi = OSGetArenaHi();

    aspectRatio = (f32)rmode->xfbHeight / (f32)rmode->efbHeight;

    if (fn_8007FC84()) {
        nSizeHeap = 0xBB800;
    } else {
        nSizeHeap = 0x87600;
    }

    nSize = getFBTotalSize(aspectRatio, 2);

    if (nSize < nSizeHeap) {
        nSize = nSizeHeap;
    }

    OSSetArenaLo(arenaLo);

    xlHeapSetup();

    xlHeapTake(&lbl_8017B1E0[0].unk_04, nSize | 0x70000000);
    xlHeapTake(&lbl_8017B1E0[1].unk_04, nSize | 0x70000000);
    VIConfigure(rmode);

    xlHeapTake(&DefaultFifo, 0x40000 | 0x30000000);
    DefaultFifoObj = GXInit(DefaultFifo, 0x40000);
    xlCoreInitGX();

    VISetNextFrameBuffer(lbl_8017B1E0[lbl_80200654].unk_04);

    lbl_80200654++;
    if (lbl_80200654 >= lbl_801FF7DC) {
        lbl_80200654 = 0;
    }

    VIFlush();
    VIWaitForRetrace();

    if (rmode->viTVmode & 1) {
        VIWaitForRetrace();
    }

    GXSetDispCopyGamma(0);
#endif

    if (!xlPostSetup()) {
        SAFE_FAILED("xlCoreRVL.c", 624);
        return false;
    }

#if IS_OOT || IS_MT
    if (!xlHeapSetup()) {
        SAFE_FAILED("xlCoreRVL.c", 625);
        return false;
    }
#endif

    if (!xlListSetup()) {
        SAFE_FAILED("xlCoreRVL.c", 626);
        return false;
    }

    if (!xlObjectSetup()) {
        SAFE_FAILED("xlCoreRVL.c", 627);
        return false;
    }

#if IS_OOT || IS_MT
    aspectRatio = (f32)rmode->xfbHeight / (f32)rmode->efbHeight;
    nSizeHeap = fn_8007FC84() ? 0xBB800 : 0x87600;
    nSize = getFBTotalSize(aspectRatio) * 2;

    if (nSize < nSizeHeap) {
        nSize = nSizeHeap;
    }

#if IS_OOT
    xlHeapTake(&DemoFrameBuffer1, nSize | 0x70000000);
    xlHeapTake(&DemoFrameBuffer2, nSize | 0x70000000);
#endif

    xlHeapFill32(DemoFrameBuffer1, nSize, 0);
    xlHeapFill32(DemoFrameBuffer2, nSize, 0);
    DCStoreRange(DemoFrameBuffer1, nSize);
    DCStoreRange(DemoFrameBuffer2, nSize);

#if IS_OOT
    xlHeapTake(&DefaultFifo, 0x40000 | 0x30000000);
    DefaultFifoObj = GXInit(DefaultFifo, 0x40000);
#else
    xlHeapTake(&DefaultFifo, 0x80000 | 0x30000000);
    DefaultFifoObj = GXInit(DefaultFifo, 0x80000);
#endif

    __xlCoreInitGX();
    errorDisplayInit();
#elif IS_MM
    __PADDisableRecalibration(false);
#endif

    xlMain();

    if (!xlObjectReset()) {
        SAFE_FAILED("xlCoreRVL.c", 641);
        return false;
    }

    if (!xlListReset()) {
        SAFE_FAILED("xlCoreRVL.c", 642);
        return false;
    }

    if (!xlHeapReset()) {
        SAFE_FAILED("xlCoreRVL.c", 643);
        return false;
    }

    if (!xlPostReset()) {
        SAFE_FAILED("xlCoreRVL.c", 644);
        return false;
    }

#if IS_OOT
    OSPanic("xlCoreRVL.c", 603, "CORE DONE!");
#elif IS_MM
    {
        extern char lbl_801562B8[];
        extern char lbl_801562E0[];
        OSPanic(lbl_801562B8, 653, lbl_801562E0);
    }
#elif IS_MT
    OSPanic("xlCoreRVL.c", 646, "CORE DONE!");
#endif

    return false;
}

#if IS_MM
void xlCoreBeforeRender(void) {
    GXRenderModeObj* mode = rmode;

    if (mode->field_rendering != 0) {
        GXSetViewportJitter(0.0f, 0.0f, mode->fbWidth, mode->efbHeight, 0.0f, 1.0f, VIGetNextField());
    } else {
        GXSetViewport(0.0f, 0.0f, mode->fbWidth, mode->efbHeight, 0.0f, 1.0f);
    }

    GXInvalidateVtxCache();
    GXInvalidateTexAll();
    GXSetDrawSync(0);
}
#endif
