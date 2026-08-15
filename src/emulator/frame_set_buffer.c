
#include "emulator/frame.h"
#include "emulator/system.h"
#include "emulator/xlHeap.h"
#include "macros.h"
#include "revolution/gx.h"
#include "revolution/os.h"

extern volatile bool lbl_80200694; // sCopyFrameSyncReceived

//! The portrait resolve, ported from oot-j's frameHackCIMG_Zelda
#define COMBO_PAUSE_PORTRAIT 1

#if COMBO_PAUSE_PORTRAIT
static u32 sConstantBufAddr[6] ATTRIBUTE_ALIGN(32) = {1};
static u16 tempLine[ZELDA_PAUSE_EQUIP_PLAYER_WIDTH / 4][4][4] ATTRIBUTE_ALIGN(32) = {{{1}}};
static u32 sNumAddr;
#endif

bool frameSetBuffer(Frame* pFrame, FrameBufferType eType) {
    if (eType == FBT_COLOR_SHOW || eType == FBT_COLOR_DRAW) {
#if COMBO_PAUSE_PORTRAIT
        //! OoT only: MM's own path for this case is the `storageDevice == 5` block in rdpParseGBI, so
        //! this goes quiet on its own after an OoTMM switch flips the mode to 5.
        if (eType == FBT_COLOR_DRAW && gpSystem->storageDevice == COMBO_GAME_MODE_OOT) {
            FrameBuffer* pBuffer = &pFrame->aBuffer[FBT_COLOR_DRAW];
            u32 i;

            for (i = 0; i < sNumAddr; i++) {
                if (pBuffer->nAddress == sConstantBufAddr[i]) {
                    break;
                }
            }

            if (i >= sNumAddr) {
                if (sNumAddr < ARRAY_COUNT(sConstantBufAddr)) {
                    sConstantBufAddr[sNumAddr++] = pBuffer->nAddress;
                    sConstantBufAddr[sNumAddr++] =
                        pBuffer->nAddress + ZELDA_PAUSE_EQUIP_PLAYER_WIDTH * ZELDA_PAUSE_EQUIP_PLAYER_HEIGHT * 2;
                } else if (pBuffer->nWidth == ZELDA_PAUSE_EQUIP_PLAYER_WIDTH && pBuffer->nSize == 2 &&
                           pBuffer->pData != NULL) {
                    u16* val = pBuffer->pData;
                    u16* valEnd = val + ZELDA_PAUSE_EQUIP_PLAYER_WIDTH * ZELDA_PAUSE_EQUIP_PLAYER_HEIGHT;
                    s32 tile;
                    s32 y;
                    s32 x;

                    //! The frame this resolve happens in is not meant to be presented (it holds the
                    //! portrait, not the scene) so keep it off the screen the way the pause capture
                    //! does. frameDrawDone clears the flag again once the next real frame is presented.
                    gNoSwapBuffer = true;

                    //! Resolve the EFB into the buffer the game is about to read back: 128x224 of EFB
                    //! box-filtered down to the 64x112 the N64 rendered, as RGB5A3.
                    GXSetTexCopySrc(0, 0, ZELDA_PAUSE_EQUIP_PLAYER_WIDTH * 2, ZELDA_PAUSE_EQUIP_PLAYER_HEIGHT * 2);
                    GXSetTexCopyDst(ZELDA_PAUSE_EQUIP_PLAYER_WIDTH, ZELDA_PAUSE_EQUIP_PLAYER_HEIGHT, GX_TF_RGB5A3,
                                    GX_TRUE);
                    DCInvalidateRange(pBuffer->pData,
                                      ZELDA_PAUSE_EQUIP_PLAYER_WIDTH * ZELDA_PAUSE_EQUIP_PLAYER_HEIGHT * sizeof(u16));
                    GXCopyTex(pBuffer->pData, GX_FALSE);

                    lbl_80200694 = false;
                    GXSetDrawSync(FRAME_SYNC_TOKEN);
                    while (!lbl_80200694) {}

                    //! GXCopyTex writes 4x4 tiles, and the N64 wants a linear RGBA5551 image, so undo
                    //! the tiling and shift the pixel format in the same pass: RGB5A3's 5:5:5 sits one
                    //! bit high for RGBA5551, and the alpha bit is forced on. Sixteen tiles per band of
                    //! four lines, 64 pixels wide.
                    while (val < valEnd) {
                        xlHeapCopy(tempLine, val, sizeof(tempLine));

                        for (y = 0; y < 4; y++) {
                            for (tile = 0; tile < ZELDA_PAUSE_EQUIP_PLAYER_WIDTH / 4; tile++) {
                                for (x = 0; x < 4; x++, val++) {
                                    *val = (tempLine[tile][y][x] << 1) | 1;
                                }
                            }
                        }
                    }
                }
            }
        }
#endif
    } else if (eType == FBT_DEPTH) {
        pFrame->nOffsetDepth0 = pFrame->aBuffer[FBT_DEPTH].nAddress & 0x03FFFFFF;
        pFrame->nOffsetDepth1 = pFrame->nOffsetDepth0 + 0x257FC;
    }

    return true;
}
