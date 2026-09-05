#ifndef _COMBO_SAVE_H
#define _COMBO_SAVE_H

#include "macros.h"
#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMBO_SAVE_COMMIT 1

//! How long the image may stay dirty before we force the commit. This replaces MM's six minutes, it
//! does not stack with it.
#define COMBO_SAVE_DEADLINE_MS 2000

#define COMBO_SAVE_REPORT 1

#if IS_MM && COMBO_SAVE_COMMIT

/**
 * @brief Commits the save image to NAND once it has been dirty for COMBO_SAVE_DEADLINE_MS.
 */
void comboSaveTick(void);

#define COMBO_SAVE_TICK() comboSaveTick()
#else
#define COMBO_SAVE_TICK() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
