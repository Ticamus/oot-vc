#ifndef _XL_POST_RVL_H
#define _XL_POST_RVL_H

#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool xlPostSetup(void);
bool xlPostReset(void);
bool xlPostText(const char* fmt, const char* file, s32 line, ...);

#ifdef __cplusplus
}
#endif

#endif
