#include "emulator/rsp.h"
#include "emulator/comboPerf.h"
#include "emulator/cpu.h"
#include "emulator/frame.h"
#include "emulator/ram.h"
#include "emulator/system.h"
#include "emulator/vc64_RVL.h"
#include "emulator/xlHeap.h"
#include "emulator/xlObject.h"
#include "revolution/os/OSAlarm.h"
#include "revolution/os/OSCache.h"
#include "revolution/os/OSThread.h"

#if IS_MM

#define RSP_HOST(pRSP) ((pRSP)->pHost)

//! MM time-slices display-list parsing: an OSAlarm handler clears this flag to make rspParseGBI hand
//! control back, in place of OoT's explicit command count.
extern s32 lbl_801FFF60;

extern s32 lbl_801FFF5C;
extern OSAlarm lbl_801809B0;
extern OSThread lbl_801809E0;
extern u8 lbl_80180660[];
extern s32 lbl_8020076C;
extern void* lbl_80200794;
extern volatile s32 lbl_80200798;
extern s32 lbl_8020080C;

extern void fn_800988E8(void* pArg0, s32 nArg1, s32 nArg2);
extern void fn_80092BFC(OSAlarm* pAlarm);
extern void fn_80092B78(OSAlarm* pAlarm, s64 nTick, s32 nArg2, s32 nArg3, void (*pHandler)(void));
extern void fn_8009301C(OSAlarm* pAlarm, void* pArg1);
extern OSThread* fn_80093024(void);
extern void fn_80098888(void* pArg0, void* pArg1, s32 nArg2);

bool rspParseABI(Rsp* pRSP, RspTask* pTask);

// marks the RSP audio thread as having been started.
void fn_80054AE4(void) {
    lbl_8020076C = 1;
}

// global (not per-RSP) alarm handler that clears the RSP time-slice
// flag and drives the audio/video sync update; the RSP-local counterpart is
// fn_80054B10 below.
void fn_80054AF0(OSAlarm* pAlarm, OSContext* pContext) {
    lbl_801FFF60 = 0;
    fn_800988E8(lbl_80180660 + 0x31C, 1, 0);
}

// per-RSP alarm handler; clears the "alarm armed" flag so
// fn_80054B64 can re-arm it next time it is needed.
void fn_80054B10(OSAlarm* pAlarm, OSContext* pContext) {
    lbl_801FFF5C = 0;
}

//! Ticks the audio thread stays running for each one it stays suspended. Retail is 1, a flat 50%
//! duty; 3 gives 75%.
#define COMBO_AUDIO_DUTY_RUN 3

// toggles the RSP audio thread between suspended and running.
void fn_80054B1C(void) {
    OSThread* pThread = fn_80093024();

    lbl_80200798--;
    if (lbl_80200798 == 0) {
        OSSuspendThread(pThread);
    } else if (lbl_80200798 < 0) {
        lbl_80200798 = COMBO_AUDIO_DUTY_RUN;
        OSResumeThread(pThread);
    }
}

// no-op stub used by soundRVL's not yet decompiled audio driver code.
void fn_80054B60(void) {
}

// (re)arms the RSP time-slice alarm and drives one RSP update.
s32 fn_80054B64(s32 nArg0) {
    OSAlarm sAlarm;
    Rsp* pRSP;
    Rsp* pNext;

    OSCreateAlarm(&sAlarm);
    pRSP = SYSTEM_RSP(gpSystem);
    if (lbl_801FFF60 != 0) {
        return 0;
    }

    lbl_801FFF5C = 1;
    OSSetAlarm(&sAlarm, ((s64)pRSP->unk_59E0 << 32) | (u32)pRSP->unk_59E4, fn_80054B10);
    rspUpdate(pRSP, 1);
    fn_80092BFC(&sAlarm);
    lbl_801FFF60 = 1;

    if (pRSP->nMode & 0x12) {
        pNext = *(Rsp**)&pRSP->unk_59DC;
        lbl_801FFF60 = 1;
        if (lbl_8020080C == 0) {
            fn_80092BFC(&lbl_801809B0);
            OSSetAlarm(&lbl_801809B0, (s64)(u32)pNext, fn_80054AF0);
        }
    }

    return 0;
}

// RSP audio-processing thread body (the `OSThreadFunc` passed to
// `OSCreateThread` in `rspEvent`); the thread's own `OSThread*` is passed back
// in as its argument so it can suspend itself each iteration.
void* fn_80054C34(void* pArg) {
    u8* pBuf = (u8*)pArg;
    Rsp* pRSP = SYSTEM_RSP(gpSystem);
    Cpu* pCPU = SYSTEM_CPU(gpSystem);
    OSAlarm sAlarm;
    u32 nTickStart;

    fn_80098888(pBuf + 0x31C, pBuf + 0x33C, 4);

#ifdef __MWERKS__
    // clang-format off
    asm {
        li      r3, 4
        oris    r3, r3, 4
        mtspr   914, r3
        li      r3, 5
        oris    r3, r3, 5
        mtspr   915, r3
        li      r3, 6
        oris    r3, r3, 6
        mtspr   916, r3
        li      r3, 7
        oris    r3, r3, 7
        mtspr   917, r3
    }
    // clang-format on
#endif

    OSCreateAlarm(&sAlarm);

    for (;;) {
        OSSuspendThread((OSThread*)pBuf);
        DCFlushRange(pRSP, sizeof(Rsp));
        DCFlushRange(pCPU, 0x121C0);
        DCFlushRange(lbl_80200794, 0x40);
        fn_8009301C(&sAlarm, pBuf);
        lbl_80200798 = 1;
        fn_80092B78(&sAlarm, OSGetTime(), pRSP->unk_59F0, pRSP->unk_59F4, fn_80054B1C);

        COMBO_PERF_ADD(nAbiBytes, ((RspTask*)lbl_80200794)->nLengthMBI & 0x7FFFFF);

        nTickStart = COMBO_PERF_TICK();
        rspParseABI(pRSP, lbl_80200794);
        COMBO_PERF_ADD(nAbiTicks, COMBO_PERF_TICK() - nTickStart);
        COMBO_PERF_BUMP(nAbiTasks);

        COMBO_PERF_LOG_UCODE(pRSP->eTypeAudioUCodeMM);
        fn_80092BFC(&sAlarm);
        pRSP->nStatus |= 0x201;
        xlObjectEvent(RSP_HOST(pRSP), 0x1000, (void*)5);
        DCStoreRange(pRSP, sizeof(Rsp));
        DCStoreRange(pCPU, 0x121C0);
    }
}

#endif
