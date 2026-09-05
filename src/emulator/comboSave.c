#include "emulator/comboSave.h"
#include "emulator/flash.h"
#include "emulator/pak.h"
#include "emulator/sram.h"
#include "emulator/storeRVL.h"
#include "emulator/system.h"
#include "emulator/vc64_RVL.h"
#include "macros.h"
#include "revolution/os.h"

#if IS_MM && COMBO_SAVE_COMMIT

#pragma section ".crashtext"
#pragma section ".crashdata" \
                ".crashbss"

#define CT DECL_SECTION(".crashtext")
#define CD DECL_SECTION(".crashdata")

//! MM's per-frame commit
bool fn_8007E540(Store* pStore);

#define COMBO_SAVE_NO_STAMP 1
CD static s64 sDirtyStamp = COMBO_SAVE_NO_STAMP;

#if COMBO_SAVE_REPORT
CD static const char kFmtCommit[] = "combo: save image dirty, committing %d bytes to NAND\n";
#endif

CT static Store* comboSaveStore(void) {
    Flash* pFlash;
    Sram* pSram;
    Pak* pPak;

    if (gpSystem == NULL) {
        return NULL;
    }

    pFlash = SYSTEM_FLASH(gpSystem);
    if (pFlash != NULL) {
        return pFlash->pStore;
    }

    pSram = SYSTEM_SRAM(gpSystem);
    if (pSram != NULL) {
        return pSram->pStore;
    }

    pPak = SYSTEM_PAK(gpSystem);
    if (pPak != NULL) {
        return pPak->pStore;
    }

    return NULL;
}

CT void comboSaveTick(void) {
    Store* pStore = comboSaveStore();
    OSTime nNow;

    if (pStore == NULL) {
        return;
    }

    // unk_B8 is the dirty flag fn_80061BC0 sets on every guest save write, and fn_80061CAC clears the
    // moment it hands the buffer to NANDWriteAsync. Clean means either nothing to do or a commit
    // already in flight, both want the stamp rearmed for the next burst.
    if (pStore->unk_B8 == 0) {
        sDirtyStamp = COMBO_SAVE_NO_STAMP;
        return;
    }

    if (sDirtyStamp == COMBO_SAVE_NO_STAMP) {
        sDirtyStamp = OSGetTime();
        return;
    }

    // Compared in ticks rather than converted to milliseconds so this costs a multiply instead of a
    // 64-bit divide
    nNow = OSGetTime();
    if (nNow - sDirtyStamp < OSMillisecondsToTicks(COMBO_SAVE_DEADLINE_MS)) {
        return;
    }

    // unk_B9 is 0 from fn_80061CAC until the close callback fn_80061C08 runs. fn_8007E540 checks it
    // too, but bail here so the deadline is measured from the write that is still outstanding rather
    // than from a tick that did nothing.
    if (pStore->unk_B9 != 1) {
        return;
    }

#if COMBO_SAVE_REPORT
    OSReport(kFmtCommit, pStore->nFileSize);
#endif
    pStore->time = 0;
    fn_8007E540(pStore);

    sDirtyStamp = COMBO_SAVE_NO_STAMP;
}

#endif
