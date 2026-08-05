//! Not in the original game. Carved out of mm-j's frame.c so that OoT's pause-menu capture can
//! be added to it, for the OoTMM combined ROM.
//!
//! The base function is byte-identical to oot-j's frameHackTIMG_Zelda, checked with
//! tools/m2c_helpers/cmp_fn.py -- 96 instructions, 0 differing bytes -- which is why this is the
//! function to extend rather than rdpParseGBI: the rest of frame.c is NotLinked everywhere and
//! carries no such guarantee.
//!
//! mm-j already reaches it for the combo. jumptable_80151414 entry 0x3D (command 0xFD,
//! G_SETTIMG) points at rdpParseGBI+0x448 = 0x80053110, whose very first action is to test
//! pSystem->storageDevice against SOT_RAM/SOT_ROM and then call in here -- so it runs at the top
//! of the case, before anything touches aBuffer[FBT_IMAGE], and systemSetupGameALL() sets
//! storageDevice = SOT_ROM for the combo. The grey-scale trigger data is bit-for-bit oot-j's
//! sCommandCodes_1679, and ZeldaGreyScaleConvert exists as MM's own 0x400-byte version, so
//! OoT's Lens of Truth already worked through this path untouched.
//!
//! What did not work is the pause-menu background. The texture-address redirect at the bottom of
//! this function is the *consumer* of sSrcBuffer/sDestinationBuffer, and in retail mm-j those two
//! labels are only ever read -- nothing in the whole DOL writes them, so the redirect is dormant
//! and MM uses its own inline CIMG hack instead. The missing half is the *producer*: resolve the
//! GC EFB into the buffer the game is about to read back, and record the swap.
//!
//! oot-j does that in frameHackCIMG_Zelda, at G_SETCIMG time, by peeking pnGBI[1] for a
//! 0xFD10013F G_SETTIMG. That peeked-at command is exactly the one this function is handed, so
//! the same test is done here on the current command instead of one ahead:
//!
//!     oot-j, at CIMG                        here, at TIMG
//!     high2 == 0xFD10013F (from pnGBI[1])   *pnCommandHi == 0xFD10013F
//!     low2  = destination (from pnGBI[1])   *pnCommandLo
//!     pBuffer->nAddress / nWidth            pFrame->aBuffer[FBT_COLOR_DRAW], still intact
//!
//! pBuffer in rdpParseGBI's G_SETCIMG case *is* &pFrame->aBuffer[FBT_COLOR_DRAW], so the two
//! read the same fields; the preceding G_SETCIMG filled them and nothing has overwritten them
//! yet. The producer runs before the redirect, mirroring oot-j's ordering across the two
//! commands. It cannot redirect its own command by accident: it leaves sSrcBuffer holding the
//! colour image address while *pnCommandLo names the destination.
//!
//! Not covered: the second half of oot-j's frameHackCIMG_Zelda, the 64x112
//! ZELDA_PAUSE_EQUIP_PLAYER branch that resolves the EFB into a 64-wide buffer and untiles it to
//! RGB5A3. That one genuinely belongs at G_SETCIMG time, but it needs neither pnGBI nor the
//! command words -- only pFrame and the buffer -- so if the equipment subscreen turns out to
//! need it, the place for it is a carve of frameSetBuffer (0x80015018, 0x38 bytes).

#include "emulator/frame.h"
#include "emulator/ram.h"
#include "emulator/rsp.h"
#include "emulator/system.h"
#include "macros.h"
#include "revolution/os.h"

void ZeldaGreyScaleConvert(Frame* pFrame);
void CopyAndConvertCFB(u16* pData);

//! frame.c file-scope objects reached across the splits, under the names dtk gives them. All
//! three have to stay MM's own storage: sSpecialZeldaHackON has two readers left behind in the
//! retail half of frame.c, at 0x8000E928 and 0x8000EE34 (the frameDrawRectTexture{,_Setup}
//! equivalents), and they are what stops the game painting over the grey-scaled frame. Defining
//! any of these afresh here would leave those readers watching a variable nobody writes.
extern u8 lbl_8020064C; // sSpecialZeldaHackON
extern u32 lbl_8020068C; // sSrcBuffer
extern u32 lbl_80200690; // sDestinationBuffer

#define sSpecialZeldaHackON lbl_8020064C
#define sSrcBuffer lbl_8020068C
#define sDestinationBuffer lbl_80200690

//! Not in the original game. frameEnd's prologue reports "frameEnd: INTERNAL ERROR: Called when
//! 'gbFrameBegin' is TRUE!" and bails when this is set, i.e. when no frame is open: frameBegin
//! clears it, frameEnd sets it. The capture below resolves the EFB and then blocks on a draw-sync
//! token, which is only safe while a frame is actually open and frameBegin has installed
//! GXSetDrawSyncCallback -- outside that window it perturbs the frame pairing, which is what the
//! pause was reporting.
extern bool lbl_802006A8; // gbFrameBegin
#define gbFrameBegin lbl_802006A8

//! DIAGNOSTIC SWITCHES, not features. What has been established about the freeze so far:
//!
//!  - It is not the draw-sync spin inside CopyAndConvertCFB: the ENTER/DONE bracket below lands in
//!    the same millisecond, every time, right up to the freeze.
//!  - It is not presentation. The guest heartbeat shows nRetrace never diverging from nRetraceUsed,
//!    so viForceRetrace keeps succeeding and gNoSwapBuffer/frameDrawDone are not involved.
//!  - It is not a corrupted display list starving frameEnd: the pump film shows three healthy frame
//!    closes (parse finished, no DOUBLE-END, so frameEnd ran) in the nine milliseconds after the
//!    capture, and only then does the guest stop queuing tasks.
//!  - It is not rspUpdate's "nothing queued" branch. All three things that branch has been made to
//!    do -- retail's re-parse, a plain skip, and an acknowledgement -- froze identically, and the
//!    film puts that branch 143 ms *after* the last healthy frame. `rearm 0` there is normal: retail
//!    leaves the pump dormant after every frame and the next GFX task re-arms it.
//!
//! What is left is that OoT stops submitting tasks a few frames after a capture, and then everything
//! else follows: the pump has nothing to re-arm it, and the guest parks in Idle_ThreadEntry waiting
//! for a completion. The graph thread's last sampled PC is inside an overlay, on sGraphStack --
//! ovl_kaleido_scope, the pause menu itself.
//!
//! So the trigger is in this hook, and these split it into its two independent halves. Detection and
//! probe arming stay on in every combination, so the log still works with both off.
//!
//!   COMBO_PAUSE_RESOLVE   the EFB resolve into guest RAM (CopyAndConvertCFB) and gNoSwapBuffer
//!   COMBO_PAUSE_REDIRECT  arming sSrcBuffer/sDestinationBuffer, hence the `*pnGBI += 8` redirect
//!
//! Run 0/0 first. It costs the pause background but must not freeze; if it still does, the trigger is
//! not here and the capture only coincides with it in time. Then 1/0 and 0/1 name the half.
//!
//! The redirect is the half I would bet on, and it is the one oot-j's own comment history flags: it
//! advances the display-list pointer by eight commands on any G_SETTIMG resolving to sSrcBuffer, and
//! sSrcBuffer is never cleared once armed -- not across pauses, not across an OoTMM game switch.
//! Set to 0/0 for the first bisect run, which is the state this is committed in. Everything the
//! thread dump added since narrows the picture without naming the culprit, so it is worth writing
//! down here rather than losing it:
//!
//!  - Every game thread is OS_STATE_WAITING inside osRecvMesg, and every wait is on a normal queue:
//!    the scheduler on gScheduler.interruptQ, the DMA manager on sDmaMgrMsgQueue, the PI manager on
//!    gPiMgrCmdQueue, pad/audio/irq/fault/main on their own. Only libultra's VI manager and the idle
//!    thread run. Nothing is in flight -- both DMA-side threads are idle, waiting for work.
//!  - The graph thread waits on a queue 0x78 above its own sp, so a local declared by whatever called
//!    osRecvMesg: the shape of DmaMgr_RequestSync, which puts its OSMesgQueue on the stack. And the
//!    address moves between dumps (sGraphStack+0x1554, then +0x12E8), so it is not deadlocked on one
//!    object -- it wakes, runs, and blocks again, cycling without ever submitting a graphics task.
//!  - Its last PCs before parking are 8038EFF4 and 8039513C, which OoTMM's own doc/OoT-MMap.txt puts
//!    in vanilla space rather than the payload at 0x80400000+, so they are a relocated overlay --
//!    ovl_kaleido_scope, the pause menu -- plus PreRender_AntiAliasFilter in vanilla code.
//!  - The saved ra is useless here: it reads 809C1968 for every thread, past the end of RAM even with
//!    the expansion pak, so libultra does not maintain it for these. pc and sp do resolve correctly.
//! Both back on: the 0/0 run froze exactly as before, so this hook is not the trigger and the
//! bisect is spent. The freeze was cpu_execute_idle.c's aliased pROM->copy.nSize -- see the
//! comment there. Keep the switches, they are the cheapest way to take this file out of the
//! picture again if something else here is ever suspected.
#define COMBO_PAUSE_CAPTURE 1
#define COMBO_PAUSE_RESOLVE 1
#define COMBO_PAUSE_REDIRECT 1

//! Not in the original game, and the whole reason this needed a latch. oot-j captures from
//! frameHackCIMG_Zelda, so it fires once per G_SETCIMG -- once when the game points the colour
//! image at the depth buffer to build its pre-render. Doing it from the G_SETTIMG side instead
//! fires once per *read* of that pre-render, and OoT's pause screen reads it back in several
//! strips per frame: measured at 40+ captures in a single pause, each one a blocking EFB resolve
//! plus a draw-sync wait. That is what was desynchronising the RSP task flow into arming frameEnd
//! twice ("gbFrameBegin is TRUE"), and it also explains the softness -- every capture after the
//! first resolves an EFB that already has some strips drawn into it, so the image degrades with
//! each one.
//!
//! So: capture on the first qualifying read after normal rendering, and re-arm as soon as the
//! colour image is something other than the depth buffer, which is how a later pause is caught.
static u32 sCaptureArmedFor;

//! Private to this function in oot-j too, and its retail counterpart lbl_8014DD98 is referenced
//! from nowhere else, so a local copy is exact rather than merely equivalent.
static s32 sCommandCodes_1679[] = {
    0xF5500000, 0x07080200, 0xE6000000, 0x00000000, 0xF3000000, 0x073BF01A, 0xE7000000, 0x00000000,
};

// The G_SETTIMG that OoT emits straight after pointing the colour image at the depth buffer.
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

    //! Not in the original game. The producer described at the top of this file: OoT is about to
    //! read back as a texture what it just rendered into the depth buffer, so resolve the EFB
    //! there for real and arm the redirect below for the frames that follow. gNoSwapBuffer keeps
    //! the capture frame off the screen.
    //!
    //! Two deliberate departures from oot-j's version. It also sets gnCountMapHack = -1, which is
    //! written in two places in that DOL and read in none, so it is dropped. And it returns false
    //! when ramGetBuffer fails, which makes rdpParseGBI abandon the display list; a bogus address
    //! here only means this heuristic misfired, so skipping the capture is preferable to killing
    //! the frame.
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

    //! Note for anyone auditing this branch: it advances the display-list pointer by eight commands, so
    //! a misfire on an unrelated G_SETTIMG naming the depth buffer would eat eight real commands.
    //! Measured innocent of the pause freeze -- a build with the whole hook inert froze identically -- but
    //! it is still the sharpest edge in this file. sSrcBuffer is never cleared once armed, in oot-j either.
    if (sSrcBuffer == SEGMENT_ADDRESS(SYSTEM_RSP(gpSystem), *pnCommandLo)) {
        *pnCommandLo = sDestinationBuffer;
        *pnGBI += 8;
    }

    return true;
}
