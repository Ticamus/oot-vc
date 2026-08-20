/**
 * Project64-EM compatible IPC register block, allowing OoTMM's multiplayer client.
 *
 * The guest accesses this block at physical 0x1FE00000 through osEPiReadIo/osEPiWriteIo.
 * The first register provides a magic handshake to detect an IPC-capable emulator.
 *
 * Registers:
 *
 *   0x00 KEY        write IPC_MAGIC_IN to arm, read back IPC_MAGIC_OUT
 *   0x04 STATUS     bit0 connected, bit1 message waiting, bit2 last write failed
 *   0x08 RAM_ADDR   guest address for the next transfer
 *   0x0C WRITE_LEN  send this many bytes from RAM_ADDR as one message
 *   0x10 READ_LEN   write: max bytes to receive; read: bytes actually received
 *
 * Messages use the same framing as the client's Unix socket:
 * a 4-byte little-endian length followed by the payload. Payload fields are
 * big-endian and pass through unchanged.
 */
#include "emulator/comboIpc.h"

#if IS_MM && COMBO_MULTI

#include "emulator/cpu.h"
#include "emulator/ram.h"
#include "emulator/system.h"
#include "emulator/xlObject.h"
#include "revolution/os.h"
#include "string.h"

#pragma section ".crashtext"
#pragma section ".crashdata" \
                ".crashbss"

#define CT DECL_SECTION(".crashtext")
#define CD DECL_SECTION(".crashdata")

#define IPC_REG_KEY 0x00
#define IPC_REG_STATUS 0x04
#define IPC_REG_RAM_ADDR 0x08
#define IPC_REG_WRITE_LEN 0x0C
#define IPC_REG_READ_LEN 0x10

#define IPC_MAGIC_IN 0xAE67E45B
#define IPC_MAGIC_OUT 0x64738358

#define IPC_STATUS_CONNECTED 0x01
#define IPC_STATUS_READ_READY 0x02
#define IPC_STATUS_ERROR 0x04

#define IPC_POLL_TICKS OSMicrosecondsToTicks(COMBO_IPC_POLL_US)

#define IPC_OP_HELLO 0x01
#define IPC_OP_WAL 0x02
#define IPC_OP_WAL_QUERY 0x03
#define IPC_OP_WAL_ACK 0x04
#define IPC_OP_POSITION 0x05

typedef struct ComboIpc {
    /* 0x000 */ void* pSystem;
    /* 0x004 */ u32 nKey;
    /* 0x008 */ u32 nAddressRAM;
    /* 0x00C */ u32 nLengthRead;
    /* 0x010 */ u32 nStatus;
    /* 0x014 */ u32 nSizeRx;
    /* 0x018 */ u32 nSizeTx;
    /* 0x01C */ u32 nTickPoll;
    /* 0x020 */ u32 nSeqOut;
    /* 0x024 */ u8 aRx[COMBO_IPC_BUFFER_SIZE];
    /* 0x424 */ u8 aTx[COMBO_IPC_BUFFER_SIZE];
} ComboIpc;

CD static char kClassName[] = "ComboIPC";

#if COMBO_IPC_TRACE
CD static char kTraceReg[] = "comboIpc: reg %02x %s %08x (status %x tx %d rx %d)\n";
CD static char kTraceWrite[] = "comboIpc: write %d rejected: %s\n";
CD static char kTraceQueued[] = "comboIpc: queued %d, tx now %d\n";
CD static char kTraceSend[] = "comboIpc: sent %d of %d\n";
CD static char kTraceRecv[] = "comboIpc: got %d bytes, rx now %d\n";
CD static char kTraceDeliver[] = "comboIpc: delivered %d to the guest\n";
CD static char kTraceGet[] = "get";
CD static char kTracePut[] = "put";
CD static char kWhyDown[] = "link is down";
CD static char kWhySize[] = "bad size";
CD static char kWhyFull[] = "queue full";
CD static char kWhyRam[] = "guest ram read failed";
CD static s32 sTraceLeft = COMBO_IPC_TRACE_MAX;

#define IPC_TRACE(fn)         \
    do {                      \
        if (sTraceLeft > 0) { \
            sTraceLeft--;     \
            fn;               \
        }                     \
    } while (0)
#else
#define IPC_TRACE(fn) ((void)0)
#endif

#if COMBO_IPC_REPORT
CD static char kMsgConnected[] = "comboIpc: connected\n";
CD static char kMsgDropped[] = "comboIpc: link dropped\n";
CD static char kMsgOverrun[] = "comboIpc: message too big (%d), dropping link\n";
CD static char kMsgArmed[] = "comboIpc: armed by the guest\n";
#define IPC_REPORT0(s) OSReport(s)
#define IPC_REPORT1(s, a) OSReport(s, a)
#else
#define IPC_REPORT0(s) ((void)0)
#define IPC_REPORT1(s, a) ((void)0)
#endif

/**
 * The live object. Not one of the System's apObject[] slots on purpose: 
 * a new SOT_ would grow the System struct.
 */
typedef struct ComboIpcSlot {
    /* 0x0 */ ComboIpc* pObject;
    /* 0x4 */ u32 nMagic;
} ComboIpcSlot;

CD static ComboIpcSlot sSlot = {NULL, 0x49504331};

CT static u32 comboIpcGetLE32(const u8* pData) {
    return (u32)pData[0] | ((u32)pData[1] << 8) | ((u32)pData[2] << 16) | ((u32)pData[3] << 24);
}

CT static void comboIpcPutLE32(u8* pData, u32 nValue) {
    pData[0] = (u8)(nValue >> 0);
    pData[1] = (u8)(nValue >> 8);
    pData[2] = (u8)(nValue >> 16);
    pData[3] = (u8)(nValue >> 24);
}

/**
 * @brief Copies between the guest's RDRAM and a host buffer.
 *
 * The guest hands us a physical RDRAM address (it masks with 0x1FFFFFFF itself). Passing the size to
 * ramGetBuffer makes it clamp against the installed RAM size, which is the only bounds check we get
 * on a value the guest chose.
 */
CT static bool comboIpcRamCopy(ComboIpc* pIpc, u32 nAddress, void* pHost, u32 nSize, bool bToGuest) {
    Ram* pRAM;
    void* pBuffer;
    u32 nClamp;

    pRAM = SYSTEM_RAM(pIpc->pSystem);
    nClamp = nSize;

    if (pRAM == NULL || nSize == 0) {
        return false;
    }

    if (!ramGetBuffer(pRAM, &pBuffer, nAddress, &nClamp) || nClamp < nSize) {
        return false;
    }

    if (bToGuest) {
        memcpy(pBuffer, pHost, nSize);
    } else {
        memcpy(pHost, pBuffer, nSize);
    }

    return true;
}

/** @brief Drops both queues. Called whenever the link goes down, so a reconnect starts clean. */
CT static void comboIpcFlush(ComboIpc* pIpc) {
    pIpc->nSizeRx = 0;
    pIpc->nSizeTx = 0;
    pIpc->nSeqOut = 0;
    pIpc->nStatus &= ~(IPC_STATUS_READ_READY | IPC_STATUS_CONNECTED);
}

/** @brief Length of the complete message at the head of a queue, or 0 if there is not one yet. */
CT static u32 comboIpcPeek(const u8* pQueue, u32 nSize) {
    u32 nLength;

    if (nSize < 4) {
        return 0;
    }

    nLength = comboIpcGetLE32(pQueue);

    if (nLength == 0 || nLength > COMBO_IPC_MSG_MAX || nSize < nLength + 4) {
        return 0;
    }

    return nLength;
}

/** @brief Consumes nCount bytes from the front of a queue. */
CT static void comboIpcDrop(u8* pQueue, u32* pnSize, u32 nCount) {
    if (nCount >= *pnSize) {
        *pnSize = 0;
        return;
    }

    memmove(pQueue, pQueue + nCount, *pnSize - nCount);
    *pnSize -= nCount;
}

/** @brief Appends one framed message to a queue. false when it would not fit. */
CT static bool comboIpcPush(u8* pQueue, u32* pnSize, const void* pPayload, u32 nSize) {
    if (nSize == 0 || nSize > COMBO_IPC_MSG_MAX || *pnSize + nSize + 4 > COMBO_IPC_BUFFER_SIZE) {
        return false;
    }

    comboIpcPutLE32(pQueue + *pnSize, nSize);
    memcpy(pQueue + *pnSize + 4, pPayload, nSize);
    *pnSize += nSize + 4;
    return true;
}

#if COMBO_IPC_BACKEND == COMBO_IPC_BACKEND_LOOPBACK

/**
 * @brief In-emulator stand-in for the OoTMM client, for testing without a network.
 *
 * Completes the HELLO handshake and acks every WAL write, which is enough for the game to report
 * itself connected and to stop blocking in Multi_EnsureSendBufferEmpty.
 */
CT static void comboIpcLoopPump(ComboIpc* pIpc) {
    u8 aReply[24];
    const u8* pMessage;
    u32 nLength;
    u32 nOp;

    pIpc->nStatus |= IPC_STATUS_CONNECTED;

    for (;;) {
        nLength = comboIpcPeek(pIpc->aTx, pIpc->nSizeTx);

        if (nLength == 0) {
            break;
        }

        pMessage = pIpc->aTx + 4;
        nOp = (nLength >= 5) ? pMessage[4] : 0;

        if (nOp == IPC_OP_HELLO) {
            /* HELLO_OUT: header{seq=0, op}, magic[8], seqGame, seqNet. Both sequences start at 0,
               so the guest's next message carries seq 0 and so does ours. */
            memset(aReply, 0, sizeof(aReply));
            aReply[4] = IPC_OP_HELLO;
            aReply[5] = 'O';
            aReply[6] = 'o';
            aReply[7] = 'T';
            aReply[8] = 'M';
            aReply[9] = 'M';
            aReply[10] = 0x7F;
            aReply[11] = 0x01;
            aReply[12] = 0x00;
            pIpc->nSeqOut = 0;
            comboIpcPush(pIpc->aRx, &pIpc->nSizeRx, aReply, 21);
        } else if (nOp == IPC_OP_WAL) {
            /* WAL_ACK carries back the token the guest put at the front of the WAL body. */
            if (nLength >= 9) {
                memset(aReply, 0, sizeof(aReply));
                aReply[0] = (u8)(pIpc->nSeqOut >> 24);
                aReply[1] = (u8)(pIpc->nSeqOut >> 16);
                aReply[2] = (u8)(pIpc->nSeqOut >> 8);
                aReply[3] = (u8)(pIpc->nSeqOut >> 0);
                aReply[4] = IPC_OP_WAL_ACK;
                aReply[5] = pMessage[5];
                aReply[6] = pMessage[6];
                aReply[7] = pMessage[7];
                aReply[8] = pMessage[8];
                if (comboIpcPush(pIpc->aRx, &pIpc->nSizeRx, aReply, 9)) {
                    pIpc->nSeqOut++;
                }
            }
        }

        comboIpcDrop(pIpc->aTx, &pIpc->nSizeTx, nLength + 4);
    }
}

#endif

/** @brief Services the transport: pushes whatever is queued, pulls whatever arrived. */
CT static void comboIpcPump(ComboIpc* pIpc, bool bNow) {
#if COMBO_IPC_BACKEND == COMBO_IPC_BACKEND_SOCKET
    s32 nResult;
    u32 nTick;
    bool bWasUp;

    nTick = OSGetTick();

    if (!bNow && (u32)OSDiffTick(nTick, pIpc->nTickPoll) < IPC_POLL_TICKS) {
        return;
    }

    pIpc->nTickPoll = nTick;
    bWasUp = (pIpc->nStatus & IPC_STATUS_CONNECTED) ? true : false;

    if (!comboNetPoll()) {
        if (bWasUp) {
            IPC_REPORT0(kMsgDropped);
            comboIpcFlush(pIpc);
        }
        return;
    }

    if (!bWasUp) {
        IPC_REPORT0(kMsgConnected);
        pIpc->nStatus |= IPC_STATUS_CONNECTED;
    }

    while (pIpc->nSizeTx != 0) {
        nResult = comboNetSend(pIpc->aTx, (s32)pIpc->nSizeTx);

        if (nResult < 0) {
            IPC_REPORT0(kMsgDropped);
            comboNetClose();
            comboIpcFlush(pIpc);
            return;
        }

        if (nResult == 0) {
            break;
        }

        IPC_TRACE(OSReport(kTraceSend, nResult, pIpc->nSizeTx));
        comboIpcDrop(pIpc->aTx, &pIpc->nSizeTx, (u32)nResult);
    }

    while (pIpc->nSizeRx < COMBO_IPC_BUFFER_SIZE) {
        nResult = comboNetRecv(pIpc->aRx + pIpc->nSizeRx, (s32)(COMBO_IPC_BUFFER_SIZE - pIpc->nSizeRx));

        if (nResult < 0) {
            IPC_REPORT0(kMsgDropped);
            comboNetClose();
            comboIpcFlush(pIpc);
            return;
        }

        if (nResult == 0) {
            break;
        }

        pIpc->nSizeRx += (u32)nResult;
        IPC_TRACE(OSReport(kTraceRecv, nResult, pIpc->nSizeRx));
    }
#elif COMBO_IPC_BACKEND == COMBO_IPC_BACKEND_LOOPBACK
    if (!bNow && (u32)OSDiffTick(OSGetTick(), pIpc->nTickPoll) < IPC_POLL_TICKS) {
        return;
    }

    pIpc->nTickPoll = OSGetTick();
    comboIpcLoopPump(pIpc);
#else
    (void)bNow;
#endif
}

/** @brief WRITE_LEN: hands one message from guest RAM to the transport. */
CT static void comboIpcWrite(ComboIpc* pIpc, u32 nSize) {
    pIpc->nStatus &= ~IPC_STATUS_ERROR;

    if (!(pIpc->nStatus & IPC_STATUS_CONNECTED)) {
        IPC_TRACE(OSReport(kTraceWrite, nSize, kWhyDown));
        pIpc->nStatus |= IPC_STATUS_ERROR;
        return;
    }

    if (nSize == 0 || nSize > COMBO_IPC_MSG_MAX) {
        IPC_TRACE(OSReport(kTraceWrite, nSize, kWhySize));
        pIpc->nStatus |= IPC_STATUS_ERROR;
        return;
    }

    if (pIpc->nSizeTx + nSize + 4 > COMBO_IPC_BUFFER_SIZE) {
        IPC_TRACE(OSReport(kTraceWrite, nSize, kWhyFull));
        pIpc->nStatus |= IPC_STATUS_ERROR;
        return;
    }

    if (!comboIpcRamCopy(pIpc, pIpc->nAddressRAM, pIpc->aTx + pIpc->nSizeTx + 4, nSize, false)) {
        IPC_TRACE(OSReport(kTraceWrite, nSize, kWhyRam));
        pIpc->nStatus |= IPC_STATUS_ERROR;
        return;
    }

    comboIpcPutLE32(pIpc->aTx + pIpc->nSizeTx, nSize);
    pIpc->nSizeTx += nSize + 4;
    IPC_TRACE(OSReport(kTraceQueued, nSize, pIpc->nSizeTx));

    // The guest polls for the answer immediately
    comboIpcPump(pIpc, true);
}

/** @brief READ_LEN: moves at most one waiting message into guest RAM and latches its length. */
CT static void comboIpcRead(ComboIpc* pIpc, u32 nMax) {
    u32 nLength;

    pIpc->nLengthRead = 0;
    comboIpcPump(pIpc, false);

    if (!(pIpc->nStatus & IPC_STATUS_CONNECTED)) {
        return;
    }

    /* A length the guest cannot take, or a nonsense one, means the stream is out of step. Truncating
       would only desync the guest's sequence check, so drop the link and let it resynchronise. */
    if (pIpc->nSizeRx >= 4) {
        nLength = comboIpcGetLE32(pIpc->aRx);

        if (nLength == 0 || nLength > COMBO_IPC_MSG_MAX || nLength > nMax) {
            IPC_REPORT1(kMsgOverrun, nLength);
            comboNetClose();
            comboIpcFlush(pIpc);
            return;
        }
    }

    nLength = comboIpcPeek(pIpc->aRx, pIpc->nSizeRx);

    if (nLength == 0) {
        return;
    }

    if (!comboIpcRamCopy(pIpc, pIpc->nAddressRAM, pIpc->aRx + 4, nLength, true)) {
        return;
    }

    comboIpcDrop(pIpc->aRx, &pIpc->nSizeRx, nLength + 4);
    pIpc->nLengthRead = nLength;
    IPC_TRACE(OSReport(kTraceDeliver, nLength));
}

CT bool comboIpcPut8(ComboIpc* pIpc, u32 nAddress, s8* pData) { return false; }

CT bool comboIpcPut16(ComboIpc* pIpc, u32 nAddress, s16* pData) { return false; }

CT bool comboIpcPut32(ComboIpc* pIpc, u32 nAddress, s32* pData) {
    u32 nRegister;

    nRegister = nAddress & 0x3F;
    IPC_TRACE(OSReport(kTraceReg, nRegister, kTracePut, *pData, pIpc->nStatus, pIpc->nSizeTx, pIpc->nSizeRx));

    if (nRegister == IPC_REG_KEY) {
        if ((u32)*pData == IPC_MAGIC_IN) {
            if (pIpc->nKey != IPC_MAGIC_OUT) {
                IPC_REPORT0(kMsgArmed);
                comboNetOpen();
            }
            pIpc->nKey = IPC_MAGIC_OUT;
        } else {
            pIpc->nKey = 0;
        }
    } else if (nRegister == IPC_REG_STATUS) {
        /* read-only */
    } else if (nRegister == IPC_REG_RAM_ADDR) {
        pIpc->nAddressRAM = *pData & 0x1FFFFFFF;
    } else if (nRegister == IPC_REG_WRITE_LEN) {
        comboIpcWrite(pIpc, (u32)*pData);
    } else if (nRegister == IPC_REG_READ_LEN) {
        comboIpcRead(pIpc, (u32)*pData);
    } else {
        return false;
    }

    return true;
}

CT bool comboIpcPut64(ComboIpc* pIpc, u32 nAddress, s64* pData) { return false; }

CT bool comboIpcGet8(ComboIpc* pIpc, u32 nAddress, s8* pData) { return false; }

CT bool comboIpcGet16(ComboIpc* pIpc, u32 nAddress, s16* pData) { return false; }

CT bool comboIpcGet32(ComboIpc* pIpc, u32 nAddress, s32* pData) {
    u32 nRegister;

    nRegister = nAddress & 0x3F;

    if (nRegister == IPC_REG_KEY) {
        *pData = (s32)pIpc->nKey;
    } else if (nRegister == IPC_REG_STATUS) {
        if (pIpc->nKey == IPC_MAGIC_OUT) {
            /* IPC_Refresh() reads this once per guest frame, which makes it the pump. */
            comboIpcPump(pIpc, false);

            if (comboIpcPeek(pIpc->aRx, pIpc->nSizeRx) != 0) {
                pIpc->nStatus |= IPC_STATUS_READ_READY;
            } else {
                pIpc->nStatus &= ~IPC_STATUS_READ_READY;
            }
        }
        *pData = (s32)(pIpc->nStatus & 7);
    } else if (nRegister == IPC_REG_RAM_ADDR) {
        *pData = (s32)pIpc->nAddressRAM;
    } else if (nRegister == IPC_REG_WRITE_LEN) {
        *pData = 0;
    } else if (nRegister == IPC_REG_READ_LEN) {
        *pData = (s32)pIpc->nLengthRead;
    } else {
        return false;
    }

    return true;
}

CT bool comboIpcGet64(ComboIpc* pIpc, u32 nAddress, s64* pData) { return false; }

CT bool comboIpcEvent(ComboIpc* pIpc, s32 nEvent, void* pArgument) {
    switch (nEvent) {
        case 2:
            pIpc->pSystem = pArgument;
            pIpc->nKey = 0;
            pIpc->nAddressRAM = 0;
            pIpc->nLengthRead = 0;
            pIpc->nStatus = 0;
            pIpc->nSizeRx = 0;
            pIpc->nSizeTx = 0;
            pIpc->nTickPoll = OSGetTick();
            pIpc->nSeqOut = 0;
            break;
        case 3:
            comboNetClose();
            break;
        case 0x1002:
            if (!cpuSetDevicePut(SYSTEM_CPU(pIpc->pSystem), (CpuDevice*)pArgument, (Put8Func)comboIpcPut8,
                                 (Put16Func)comboIpcPut16, (Put32Func)comboIpcPut32, (Put64Func)comboIpcPut64)) {
                return false;
            }
            if (!cpuSetDeviceGet(SYSTEM_CPU(pIpc->pSystem), (CpuDevice*)pArgument, (Get8Func)comboIpcGet8,
                                 (Get16Func)comboIpcGet16, (Get32Func)comboIpcGet32, (Get64Func)comboIpcGet64)) {
                return false;
            }
            break;
        case 0:
        case 1:
            break;
        case 0x1003:
        case 0x1004:
        case 0x1007:
            break;
        default:
            return false;
    }

    return true;
}

CD _XL_OBJECTTYPE gClassComboIpc = {
    kClassName,
    sizeof(ComboIpc),
    NULL,
    (EventFunc)comboIpcEvent,
};

/** @brief Creates the device and maps it into the guest's address space. */
CT bool comboIpcCreate(void* pSystem, void* pCPU) {
    if (sSlot.pObject != NULL) {
        return true;
    }

    if (!xlObjectMake((void**)&sSlot.pObject, pSystem, &gClassComboIpc)) {
        return false;
    }

    if (!cpuMapObject((Cpu*)pCPU, sSlot.pObject, COMBO_IPC_ADDRESS_0, COMBO_IPC_ADDRESS_1, 0)) {
        return false;
    }

    return true;
}

CT bool comboIpcDestroy(void) {
    if (sSlot.pObject == NULL) {
        return true;
    }

    return xlObjectFree((void**)&sSlot.pObject);
}

#endif
