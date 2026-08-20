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
#define COMBO_IPC_PORT 14230

//! Poll interval between socket checks. The socket is serviced from the register
//! handlers because the guest may wait for an acknowledgement and never reach
//! the frame-end pump.
#define COMBO_IPC_POLL_US 8000

//! Delay before retrying after a failed connection or dropped link.
#define COMBO_IPC_RETRY_US 3000000

//! Log state changes only (connect, drop, refused, protocol errors).
#define COMBO_IPC_REPORT 1

//! Debug tracing. Logs register accesses, rejected writes, and socket transfers,
//! capped to avoid flooding the log. Disable once the link is working.
#define COMBO_IPC_TRACE 1
#define COMBO_IPC_TRACE_MAX 80

//! Register block. The emulator maps this as a 64 KB device window.
#define COMBO_IPC_ADDRESS_0 0x1FE00000
#define COMBO_IPC_ADDRESS_1 0x1FE0FFFF

//! Maximum message size supported by the OoTMM protocol.
#define COMBO_IPC_MSG_MAX 256

//! Queues behind the register block. Sized to hold a full 16-entry WAL burst.
#define COMBO_IPC_BUFFER_SIZE 1024

//! Temporary buffer used for socket transfers.
#define COMBO_IPC_IO_SIZE 512

#if IS_MM && COMBO_MULTI

bool comboIpcCreate(void* pSystem, void* pCPU);
bool comboIpcDestroy(void);

#define COMBO_IPC_CREATE(pSystem, pCPU) comboIpcCreate(pSystem, pCPU)
#define COMBO_IPC_DESTROY() comboIpcDestroy()

void comboNetOpen(void);
void comboNetClose(void);
bool comboNetPoll(void);
s32 comboNetSend(const void* pData, s32 nSize);
s32 comboNetRecv(void* pData, s32 nSize);

#else

#define COMBO_IPC_CREATE(pSystem, pCPU) true
#define COMBO_IPC_DESTROY() true

#endif