#include "emulator/frame.h"
#include "emulator/ram.h"
#include "emulator/rsp.h"
#include "emulator/system.h"
#include "macros.h"
#include "revolution/os.h"

void ZeldaGreyScaleConvert(Frame* pFrame);
void CopyAndConvertCFB(u16* pData);

extern u8 lbl_8020064C; // sSpecialZeldaHackON
extern u32 lbl_8020068C; // sSrcBuffer
extern u32 lbl_80200690; // sDestinationBuffer

#define sSpecialZeldaHackON lbl_8020064C
#define sSrcBuffer lbl_8020068C
#define sDestinationBuffer lbl_80200690

//! gbFrameBegin: set by frameEnd, cleared by frameBegin
extern bool lbl_802006A8; // gbFrameBegin
#define gbFrameBegin lbl_802006A8

//!   COMBO_PAUSE_RESOLVE   the EFB resolve into guest RAM (CopyAndConvertCFB) and gNoSwapBuffer
//!   COMBO_PAUSE_REDIRECT  arming sSrcBuffer/sDestinationBuffer, hence the `*pnGBI += 8` redirect
#define COMBO_PAUSE_CAPTURE 1
#define COMBO_PAUSE_RESOLVE 1
#define COMBO_PAUSE_REDIRECT 1

static u32 sCaptureArmedFor;

static s32 sCommandCodes_1679[] = {
    0xF5500000, 0x07080200, 0xE6000000, 0x00000000, 0xF3000000, 0x073BF01A, 0xE7000000, 0x00000000,
};

// The G_SETTIMG that OoT emits after pointing the colour image at the depth buffer.
#define ZELDA_PAUSE_CAPTURE_COMMAND 0xFD10013F

bool frameHackTIMG_Zelda(Frame* pFrame, u64** pnGBI, u32* pnCommandLo, u32* pnCommandHi) {
    u32 i;

    if ((*pnCommandLo == 0x0F000000) && (*pnCommandHi == 0xFD500000)) {
        u32* tmp = (u32*)*pnGBI;
        for (i = 0; i < ARRAY_COUNT(sCommandCodes_1679); i++) {
            if (tmp[i] != sCommandCodes_1679[i]) {
                break;
            }
        }

        if (i == ARRAY_COUNT(sCommandCodes_1679)) {
            ZeldaGreyScaleConvert(pFrame);
            sSpecialZeldaHackON = 1;
        }
    }

    if ((sSpecialZeldaHackON != 0) && ((*pnCommandLo & 0xFF000000) != 0x0F000000)) {
        sSpecialZeldaHackON = 0;
    }

#if COMBO_PAUSE_CAPTURE
    if (pFrame->aBuffer[FBT_COLOR_DRAW].nAddress != pFrame->aBuffer[FBT_DEPTH].nAddress) {
        // Normal rendering: re-arm for the next pre-render build.
        sCaptureArmedFor = 0;
    } else if (!gbFrameBegin && *pnCommandHi == ZELDA_PAUSE_CAPTURE_COMMAND &&
               pFrame->aBuffer[FBT_COLOR_DRAW].nWidth == N64_FRAME_WIDTH &&
               sCaptureArmedFor != pFrame->aBuffer[FBT_COLOR_DRAW].nAddress) {
        u32 nAddress = SEGMENT_ADDRESS(SYSTEM_RSP(gpSystem), *pnCommandLo);
        u16* pnSource;

        sCaptureArmedFor = pFrame->aBuffer[FBT_COLOR_DRAW].nAddress;

        if (ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pnSource, nAddress, NULL)) {
#if COMBO_PAUSE_REDIRECT
            sDestinationBuffer = nAddress;
            sSrcBuffer = pFrame->aBuffer[FBT_COLOR_DRAW].nAddress;
#endif

#if COMBO_PAUSE_RESOLVE
            CopyAndConvertCFB(pnSource);

            gNoSwapBuffer = true;
#endif

        }
    }
#endif

    //! Advances the display-list pointer by eight commands, so a misfire on an unrelated G_SETTIMG
    //! naming the depth buffer would eat eight real commands. sSrcBuffer is never cleared once
    //! armed, in oot-j either.
    if (sSrcBuffer == SEGMENT_ADDRESS(SYSTEM_RSP(gpSystem), *pnCommandLo)) {
        *pnCommandLo = sDestinationBuffer;
        *pnGBI += 8;
    }

    return true;
}
