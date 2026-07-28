#include "emulator/xlPostRVL.h"
#include "emulator/xlCoreRVL.h"
#include "emulator/frame.h"
#include "emulator/system.h"
#include "emulator/vc64_RVL.h"
#include "revolution/vi.h"
#include "macros.h"

#if IS_MM

extern u32 lbl_80200654;
u32 lbl_801FF7DC = 2;

void fn_8008745C(void) {
    SYSTEM_FRAME(gpSystem)->nMode = 0;
    SYSTEM_FRAME(gpSystem)->nModeVtx = -1;
    frameDrawReset(SYSTEM_FRAME(gpSystem), 0x5FFED);

    GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    GXSetColorUpdate(GX_ENABLE);
    GXCopyDisp(lbl_8017B1E0[lbl_80200654 * 2].unk_04, GX_TRUE);
    GXDrawDone();
    VIConfigure(rmode);
    VISetNextFrameBuffer(lbl_8017B1E0[lbl_80200654 * 2].unk_04);
    VIFlush();
    VIWaitForRetrace();

    lbl_80200654 = (lbl_80200654 + 1) % lbl_801FF7DC;
}

bool fn_80087534(void) {
    xlCoreInitGX();
    VISetNextFrameBuffer(lbl_8017B1E0[lbl_80200654 * 2].unk_04);

    lbl_80200654++;

    if (lbl_80200654 >= lbl_801FF7DC) {
        lbl_80200654 = 0;
    }

    VIFlush();
    VIWaitForRetrace();

    if (rmode->viTVmode & 1) {
        VIWaitForRetrace();
    }

    VIConfigure(rmode);
    return true;
}

bool xlPostText(const char* fmt, const char* file, s32 line, ...) { return true; }

#endif

bool xlPostSetup(void) { return true; }

bool xlPostReset(void) { return true; }
