//! frameBeginOK, carved out of mm-j's frame.c during the OoT pause-freeze investigation and kept
//! afterwards. The instrumentation it existed for is gone; what remains is a byte-exact reimplementation
//! (verified 68/68), so it is equivalent to leaving the function in the extracted object. It can be
//! folded back into frame.c in config/mm-j/splits.txt if the split count ever matters.
//!
//! Worth knowing, since oot-j reasoning does not carry over: mm-j's body is not `return !gbFrameValid`.
//! It reports "SUPER TROUBLE" on one flag and returns whether a second one is clear. That second flag is
//! gbFrameValid, and a refusal here is what makes rspUpdate skip a task setup -- see
//! docs/ootmm_pause_freeze.md, which explains why leaving it set strands every later setup.

#include "emulator/frame.h"
#include "macros.h"
#include "revolution/os.h"

//! frame.c file-scope flags, under the names dtk gives them. lbl_802006AC is gbFrameValid, what this
//! function gates the task setup on; lbl_8020067C drives the retail "SUPER TROUBLE" report.
extern u32 lbl_8020067C;
extern u32 lbl_802006AC;

bool frameBeginOK(Frame* pFrame) {
    if (lbl_8020067C) {
        OSReport("SUPER TROUBLE\n");
    }

    return lbl_802006AC == 0;
}
