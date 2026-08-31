#ifndef _COMBO_IPC_H
#define _COMBO_IPC_H

#include "macros.h"
#include "revolution/types.h"

//! Master switch for the OoTMM multiplayer bridge. At 0, the feature compiles to nothing.
#define COMBO_MULTI 1

//! Transport under the register block.
//! NONE     - probe only; stays in single player.
//! LOOPBACK - in-emulator test transport; no network.
//! SOCKET   - TCP to the PC relay through IOS.
#define COMBO_IPC_BACKEND_NONE 0
#define COMBO_IPC_BACKEND_LOOPBACK 1
#define COMBO_IPC_BACKEND_SOCKET 2
#define COMBO_IPC_BACKEND COMBO_IPC_BACKEND_SOCKET

#define COMBO_IPV4(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))
#define COMBO_IPC_HOST COMBO_IPV4(127, 0, 0, 1)
#define COMBO_IPC_PORT 14237

//! Poll interval between socket checks. Serviced from the register handlers, because a guest waiting
//! on an acknowledgement may never reach the frame-end pump.
#define COMBO_IPC_POLL_US 8000

//! Delay before retrying after a failed connection or dropped link.
#define COMBO_IPC_RETRY_US 3000000

#define COMBO_IPC_DEBUG 0

//! State changes only: connect, drop, refused writes, protocol errors.
#define COMBO_IPC_REPORT COMBO_IPC_DEBUG

//! Register accesses, rejected writes and socket transfers, capped so a hot path cannot flood.
#define COMBO_IPC_TRACE COMBO_IPC_DEBUG
#define COMBO_IPC_TRACE_MAX 80

#define COMBO_IPC_OVERLAY COMBO_IPC_DEBUG
#define COMBO_IPC_OVERLAY_HOLD 8

//! Register block. The emulator maps this as a 64 KB device window.
#define COMBO_IPC_ADDRESS_0 0x1FE00000
#define COMBO_IPC_ADDRESS_1 0x1FE0FFFF

//! Maximum message size supported by the OoTMM protocol.
#define COMBO_IPC_MSG_MAX 256

//! Queues behind the register block.
#define COMBO_IPC_BUFFER_SIZE 4096

#define COMBO_IPC_IO_SIZE 512

#if IS_MM && COMBO_MULTI

bool comboIpcCreate(void* pSystem, void* pCPU);
bool comboIpcDestroy(void);

#define COMBO_IPC_CREATE(pSystem, pCPU) comboIpcCreate(pSystem, pCPU)
#define COMBO_IPC_DESTROY() comboIpcDestroy()

//! What the on-screen status needs from the transport.
typedef struct ComboNetInfo {
    /* 0x00 */ char* szState;
    /* 0x04 */ s32 nError;
    /* 0x08 */ u32 nAddressLocal;
    /* 0x0C */ u32 nAddressRemote;
    /* 0x10 */ s32 nPort;
} ComboNetInfo;

void comboNetOpen(void);
void comboNetClose(void);
void comboNetGetInfo(ComboNetInfo* pInfo);
s32 comboNetGetError(void);
s32 comboNetGetStalls(void);
bool comboNetPoll(void);
s32 comboNetSend(const void* pData, s32 nSize);
s32 comboNetRecv(void* pData, s32 nSize);

#else

#define COMBO_IPC_CREATE(pSystem, pCPU) true
#define COMBO_IPC_DESTROY() true

#endif

#endif
