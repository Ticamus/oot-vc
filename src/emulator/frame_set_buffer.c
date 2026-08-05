//! frameSetBuffer, carved out of mm-j's frame.c (0x80015018, 0x38) so that OoT's equipment-subscreen
//! Link portrait can be resolved. The base function is oot-j's to the instruction -- its FBT_DEPTH
//! branch computes the same `nOffsetDepth1 = nOffsetDepth0 + 0x257FC`, and its
//! FBT_COLOR_SHOW / FBT_COLOR_DRAW branch is **empty**, which is what makes it a free hook.
//!
//! Why here rather than in a carve of rdpParseGBI (0x13C8, 1266 instructions): the portrait needs
//! G_SETCIMG time and nothing else. rdpParseGBI's G_SETCIMG case (0x80052D70) fills all five fields of
//! aBuffer[FBT_COLOR_DRAW] -- nSize 0x1A4, nWidth 0x1A8, nFormat 0x1AC, pData 0x1B0 through
//! ramGetBuffer, nAddress 0x1B4 -- and then calls `frameSetBuffer(pFrame, FBT_COLOR_DRAW)` at
//! 0x80052DF0. So by the time this runs, everything the hack reads is already in place.
//!
//! What mm-j has and does not have. It already maintains the equivalent of oot-j's address table,
//! un-gated: `pFrame->anCIMGAddresses[8]` with the count at `pFrame->nNumCIMGAddresses`, filled at
//! 0x80052E4C. Everything after that in the case is guarded by `storageDevice == 5` and keyed on MM's
//! own hardcoded framebuffer addresses, so none of it runs for the combo at COMBO_GAME_MODE_OOT. The
//! portrait branch of oot-j's frameHackCIMG_Zelda has no counterpart at all -- which is why the
//! portrait came out as noise: OoT renders Link into a 64x112 off-screen colour image and reads it
//! straight back as a texture, and with no resolve it reads whatever was left in that RDRAM.

#include "emulator/frame.h"
#include "emulator/system.h"
#include "emulator/xlHeap.h"
#include "macros.h"
#include "revolution/gx.h"
#include "revolution/os.h"

//! frame.c's draw-sync flag, under the name dtk gives it. It MUST be the retail object: the only writer
//! is retail frameDrawSyncCallback (0x80009A6C, which tests the token against 0x7D00 and sets this to
//! 1), so a private copy would spin here forever.
extern volatile bool lbl_80200694; // sCopyFrameSyncReceived

//! Not in the original game. The portrait resolve, ported from oot-j's frameHackCIMG_Zelda. Off means
//! this file is a plain byte-exact carve again, which is also how the base is verified: compile with 0
//! and cmp_fn.py the object against build/mm-j/obj -- 56/56 OK as of writing.
#define COMBO_PAUSE_PORTRAIT 1

#if COMBO_PAUSE_PORTRAIT
//! oot-j's own state for this, with its sizes and its semantics kept rather than tidied, because the
//! trigger *is* the fill pattern: entries go in as pairs (an address and the same address one
//! 64x112x2 buffer later, since the portrait is double-buffered), and the resolve only starts once the
//! table is full -- so the first three distinct colour images a run ever sees are exempt, which is how
//! the main framebuffers are skipped without naming them. The portrait buffers are never among those
//! three, so they always resolve.
//!
//! One deliberate substitution: oot-j keys the table on the raw G_SETCIMG command word, which is not
//! reachable from here, so this keys it on the segment-resolved pBuffer->nAddress. Same identity
//! relation -- one distinct buffer per entry -- since the mapping is deterministic per segment.
//!
//! Both arrays must land in .data, not .bss: this TU's split claims only .text, and .bss from such a TU
//! is inserted into the middle of the pinned layout -- a sixteen-byte array added to a sibling carve
//! once shifted every later .bss object and the console reported corrupted save data at startup.
//!
//! Hence the deliberately non-zero initialisers. `= {0}` is NOT enough: MWCC treats an all-zero
//! initialiser as no initialiser and emitted 0x220 bytes of .bss for exactly that, which is how this
//! nearly repeated the save-corruption episode. The values themselves are meaningless -- the table is
//! only read below sNumAddr, which starts at zero and only grows as entries are written, and tempLine is
//! a scratch buffer that xlHeapCopy fills before anything reads it. sNumAddr is a scalar, so .sbss is
//! fine for it: that lands past the end of the retail image.
static u32 sConstantBufAddr[6] ATTRIBUTE_ALIGN(32) = {1};
static u16 tempLine[ZELDA_PAUSE_EQUIP_PLAYER_WIDTH / 4][4][4] ATTRIBUTE_ALIGN(32) = {{{1}}};
static u32 sNumAddr;
#endif

bool frameSetBuffer(Frame* pFrame, FrameBufferType eType) {
    if (eType == FBT_COLOR_SHOW || eType == FBT_COLOR_DRAW) {
#if COMBO_PAUSE_PORTRAIT
        //! Not in the original game. OoT only: MM's own path for this case is the `storageDevice == 5`
        //! block back in rdpParseGBI, and after the OoTMM switch the mode flips to 5 so this goes quiet
        //! on its own.
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

                    //! The frame this resolve happens in is not meant to be presented -- it holds the
                    //! portrait, not the scene -- so keep it off the screen the way the pause capture
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
