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
 * Payloads are big-endian and pass through untouched. The two directions do not share a framing on
 * the wire; see IPC_SYNC_* below.
 */
#include "emulator/comboIpc.h"

#if !defined(IS_MM)
#error "comboIpc.h did not bring in macros.h: IS_MM is undefined, so this unit would compile to nothing."
#endif

#if IS_MM && COMBO_MULTI

#include "emulator/cpu.h"
#include "emulator/crashScreen.h"
#include "emulator/ram.h"
#include "emulator/system.h"
#include "emulator/vc64_RVL.h"
#include "emulator/xlCoreRVL.h"
#include "emulator/xlObject.h"
#include "revolution/os.h"
#include "revolution/vi.h"
#include "stdio.h"
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

/*
 * Relay -> game framing:
 *
 *   4F 4D A5 5A   sync word
 *   LL LL         payload length (u16 LE)
 *   CK CK         checksum (u16 LE): sum of payload bytes + length
 *   <payload>     LL bytes
 *   <padding>     0..3 bytes, to align to 4 bytes
 *
 * Game -> relay still uses only a length prefix. SENDTO gives us the exact byte count,
 * and this direction has never had sync issues.
 *
 * The padding is needed because of how IOS handles SO_RECVFROM: depending on how much
 * has already been read, it may leave up to 3 bytes unwritten while still consuming them.
 * Since every OoTMM message is 1 byte off a multiple of 4 after adding the length prefix,
 * skipping the padding would shift the stream by 3 bytes from the first message.
 *
 * The sync word lets us recover if a TCP segment splits a frame. The checksum also catches
 * corruption inside a payload..
 */
#define IPC_SYNC_0 0x4F
#define IPC_SYNC_1 0x4D
#define IPC_SYNC_2 0xA5
#define IPC_SYNC_3 0x5A
#define IPC_RX_HEADER 8
#define IPC_ROUND4(n) (((n) + 3) & ~3)

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
    /* 0x020 */ u32 nSeqOut; // loopback backend only: the sequence it stamps on its fake replies
    /* 0x024 */ u32 nTickUp;
    /* 0x028 */ s32 nPumps;
    /* 0x02C */ s32 nMsgTx;
    /* 0x030 */ s32 nMsgRx;
    /* 0x034 */ s32 nDrops;
    /* 0x038 */ s32 nDropWhy;
    /* 0x03C */ s32 nDropInfo;
    /* 0x040 */ s32 nDropErr;
    /* 0x044 */ u32 anDropHead[3];
    /* 0x050 */ s32 nEmpty; // frames skipped because the guest's buffer was smaller (overlay `z`)
    /* 0x054 */ s32 nRxFirst;
    /* 0x058 */ u32 anRxFirst[2];
    /* 0x060 */ s32 nHellos;
    /* 0x064 */ s32 nResalutes;
    /* 0x068 */ s32 nWriteErr;
    /* 0x06C */ s32 nLastRxLen;
    /* 0x070 */ s32 nLastRxOp;
    /* 0x074 */ s32 nDropped;
    /* 0x078 */ s32 nWriteWhy;
    /* 0x07C */ s32 nRxThisLink;
    /* 0x080 */ s32 nStale;
    /* 0x084 */ u32 nTxHold;
    /* 0x088 */ s32 nResync;
    /* 0x08C */ u8 aRx[COMBO_IPC_BUFFER_SIZE];
    /* 0x108C */ u8 aTx[COMBO_IPC_BUFFER_SIZE];
} ComboIpc;

CD static char kClassName[] = "ComboIPC";

#if COMBO_IPC_TRACE
//! Writes only: STATUS is read every guest frame, so tracing reads would spend the whole budget
//! before anything interesting happened.
CD static char kTraceReg[] = "comboIpc: put %02x = %08x (status %x tx %d rx %d)\n";
CD static char kTraceWrite[] = "comboIpc: write %d rejected: %s\n";
CD static char kTraceQueued[] = "comboIpc: queued %d, tx now %d\n";
CD static char kTraceSend[] = "comboIpc: sent %d of %d\n";
CD static char kTraceRecv[] = "comboIpc: got %d bytes, rx now %d\n";
CD static char kTraceDeliver[] = "comboIpc: delivered %d to the guest\n";
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

//! The live object. Kept out of System's apObject[] on purpose: a new SOT_ would grow that struct.
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
 * The address comes from the guest, so ramGetBuffer's clamp against the installed RAM size is the
 * only bounds check there is.
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
    pIpc->nTxHold = 0;
    pIpc->nSeqOut = 0;
    pIpc->nHellos = 0;
    pIpc->nRxThisLink = 0;
    pIpc->nStatus &= ~(IPC_STATUS_READ_READY | IPC_STATUS_CONNECTED);
}

/**
 * @brief Takes the link down and records why, since the overlay is all there is to read on hardware.
 *
 *   1 the transport gave up   2 send failed   3 recv failed   5 system teardown
 *
 * 4 was "unreadable length" and is gone: that case is a resynchronise now. Tearing the session down
 * over a three-byte framing slip took the client's attach with it every time.
 */
CT static void comboIpcDown(ComboIpc* pIpc, s32 nWhy, s32 nInfo, s32 nError) {
    s32 iWord;

    /* The head of the queue in wire order, so the overlay's hex reads like the bytes on the wire. */
    for (iWord = 0; iWord < 3; iWord++) {
        u32 nOffset = (u32)iWord * 4;

        pIpc->anDropHead[iWord] = (pIpc->nSizeRx >= nOffset + 4)
                                      ? ((u32)pIpc->aRx[nOffset] << 24) | ((u32)pIpc->aRx[nOffset + 1] << 16) |
                                            ((u32)pIpc->aRx[nOffset + 2] << 8) | (u32)pIpc->aRx[nOffset + 3]
                                      : 0;
    }

    IPC_REPORT0(kMsgDropped);
    pIpc->nDrops++;
    pIpc->nDropWhy = nWhy;
    pIpc->nDropInfo = nInfo;
    pIpc->nDropErr = nError;
    comboIpcFlush(pIpc);
}

/** @brief True when the four bytes at pQueue are the sync word. */
CT static bool comboIpcIsSync(const u8* pQueue) {
    return pQueue[0] == IPC_SYNC_0 && pQueue[1] == IPC_SYNC_1 && pQueue[2] == IPC_SYNC_2 &&
           pQueue[3] == IPC_SYNC_3;
}

/**
 * @brief Payload length of the complete inbound frame at the head, 0 if there is not one yet.
 *
 * 0 covers both "not yet" and "not a frame"; comboIpcRxBroken is what tells those apart.
 */
CT static u32 comboIpcPeekRx(const u8* pQueue, u32 nSize) {
    u32 nLength;
    u32 nCheck;
    u32 iByte;

    if (nSize < IPC_RX_HEADER || !comboIpcIsSync(pQueue)) {
        return 0;
    }

    nLength = (u32)pQueue[4] | ((u32)pQueue[5] << 8);

    if (nLength == 0 || nLength > COMBO_IPC_MSG_MAX || nSize < IPC_RX_HEADER + IPC_ROUND4(nLength)) {
        return 0;
    }

    nCheck = nLength;

    for (iByte = 0; iByte < nLength; iByte++) {
        nCheck += pQueue[IPC_RX_HEADER + iByte];
    }

    if ((nCheck & 0xFFFF) != ((u32)pQueue[6] | ((u32)pQueue[7] << 8))) {
        return 0;
    }

    return nLength;
}

/**
 * @brief Is the head of the receive queue provably broken, rather than merely incomplete?
 *
 * Broken means no sync word, an impossible length, or a whole frame whose check does not add up.
 * Anything else is a frame still on its way and waiting is the right answer. Getting this wrong
 * either stalls the queue for good or resynchronises through healthy traffic.
 */
CT static bool comboIpcRxBroken(const u8* pQueue, u32 nSize) {
    u32 nLength;

    if (nSize < 4) {
        return false;
    }

    if (!comboIpcIsSync(pQueue)) {
        return true;
    }

    if (nSize < IPC_RX_HEADER) {
        return false;
    }

    nLength = (u32)pQueue[4] | ((u32)pQueue[5] << 8);

    if (nLength == 0 || nLength > COMBO_IPC_MSG_MAX) {
        return true;
    }

    if (nSize < IPC_RX_HEADER + IPC_ROUND4(nLength)) {
        return false;
    }

    /* All there, so a refusal from the peek can only be the check. */
    return comboIpcPeekRx(pQueue, nSize) == 0;
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

/**
 * @brief Skips to the next sync word after a hole in the inbound stream.
 */
CT static void comboIpcResync(ComboIpc* pIpc) {
    u32 nOffset;

    pIpc->nResync++;

    for (nOffset = 1; nOffset + 4 <= pIpc->nSizeRx; nOffset++) {
        if (comboIpcIsSync(pIpc->aRx + nOffset)) {
            comboIpcDrop(pIpc->aRx, &pIpc->nSizeRx, nOffset);
            return;
        }
    }

    /* None yet. Keep three bytes in case a sync word is straddling the read; either way the head is
       now a sync word or too short to look at, so the caller's loop ends. */
    if (pIpc->nSizeRx > 3) {
        comboIpcDrop(pIpc->aRx, &pIpc->nSizeRx, pIpc->nSizeRx - 3);
    }
}

/**
 * @brief Removes the bytes the socket accepted, remembering a frame that only went out in part.
 */
CT static void comboIpcSent(ComboIpc* pIpc, u32 nCount) {
    u32 nHold;

    while (nCount != 0 && pIpc->nSizeTx != 0) {
        nHold = (pIpc->nTxHold != 0) ? pIpc->nTxHold : comboIpcGetLE32(pIpc->aTx) + 4;

        if (nCount < nHold) {
            pIpc->nTxHold = nHold - nCount;
            comboIpcDrop(pIpc->aTx, &pIpc->nSizeTx, nCount);
            return;
        }

        nCount -= nHold;
        pIpc->nTxHold = 0;
        comboIpcDrop(pIpc->aTx, &pIpc->nSizeTx, nHold);
    }
}

/**
 * @brief Makes room in the send queue by discarding position updates.
 */
CT static bool comboIpcMakeRoom(ComboIpc* pIpc, u32 nNeed) {
    u32 nOffset;
    u32 nLength;

    while (pIpc->nSizeTx + nNeed > COMBO_IPC_BUFFER_SIZE) {
        /* Start past the frame already half-way out; the prefixes only line up again beyond it. */
        nOffset = pIpc->nTxHold;

        for (;;) {
            if (nOffset + 4 > pIpc->nSizeTx) {
                return false;
            }

            nLength = comboIpcGetLE32(pIpc->aTx + nOffset);

            if (nLength == 0 || nOffset + nLength + 4 > pIpc->nSizeTx) {
                return false;
            }

            if (nLength >= 5 && pIpc->aTx[nOffset + 8] == IPC_OP_POSITION) {
                break;
            }

            nOffset += nLength + 4;
        }

        memmove(pIpc->aTx + nOffset, pIpc->aTx + nOffset + nLength + 4, pIpc->nSizeTx - (nOffset + nLength + 4));
        pIpc->nSizeTx -= nLength + 4;
        pIpc->nDropped++;
    }

    return true;
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

/**
 * @brief Appends one message to the receive queue in the inbound framing.
 */
CT static bool comboIpcPushRx(ComboIpc* pIpc, const void* pPayload, u32 nSize) {
    u32 nTotal;
    u32 nCheck;
    u32 iByte;

    nTotal = IPC_RX_HEADER + IPC_ROUND4(nSize);

    if (nSize == 0 || nSize > COMBO_IPC_MSG_MAX || pIpc->nSizeRx + nTotal > COMBO_IPC_BUFFER_SIZE) {
        return false;
    }

    nCheck = nSize;

    for (iByte = 0; iByte < nSize; iByte++) {
        nCheck += ((const u8*)pPayload)[iByte];
    }

    pIpc->aRx[pIpc->nSizeRx + 0] = IPC_SYNC_0;
    pIpc->aRx[pIpc->nSizeRx + 1] = IPC_SYNC_1;
    pIpc->aRx[pIpc->nSizeRx + 2] = IPC_SYNC_2;
    pIpc->aRx[pIpc->nSizeRx + 3] = IPC_SYNC_3;
    pIpc->aRx[pIpc->nSizeRx + 4] = (u8)nSize;
    pIpc->aRx[pIpc->nSizeRx + 5] = (u8)(nSize >> 8);
    pIpc->aRx[pIpc->nSizeRx + 6] = (u8)nCheck;
    pIpc->aRx[pIpc->nSizeRx + 7] = (u8)(nCheck >> 8);
    memset(pIpc->aRx + pIpc->nSizeRx + IPC_RX_HEADER, 0, IPC_ROUND4(nSize));
    memcpy(pIpc->aRx + pIpc->nSizeRx + IPC_RX_HEADER, pPayload, nSize);
    pIpc->nSizeRx += nTotal;
    return true;
}

#if COMBO_IPC_BACKEND == COMBO_IPC_BACKEND_LOOPBACK

/**
 * @brief Length of the complete message at the head of the send queue, 0 if there is not one yet.
 */
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

/**
 * @brief In-emulator stand-in for the OoTMM client, for testing without a network.
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
            comboIpcPushRx(pIpc, aReply, 21);
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
                if (comboIpcPushRx(pIpc, aReply, 9)) {
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
    pIpc->nPumps++;
    bWasUp = (pIpc->nStatus & IPC_STATUS_CONNECTED) ? true : false;

    if (!comboNetPoll()) {
        if (bWasUp) {
            ComboNetInfo info;

            comboNetGetInfo(&info);
            comboIpcDown(pIpc, 1, 0, info.nError);
        }
        return;
    }

    if (!bWasUp) {
        IPC_REPORT0(kMsgConnected);
        pIpc->nStatus |= IPC_STATUS_CONNECTED;
        pIpc->nTickUp = nTick;
    }

    while (pIpc->nSizeTx != 0) {
        nResult = comboNetSend(pIpc->aTx, (s32)pIpc->nSizeTx);

        if (nResult < 0) {
            comboIpcDown(pIpc, 2, (s32)pIpc->nSizeTx, comboNetGetError());
            comboNetClose();
            return;
        }

        if (nResult == 0) {
            break;
        }

        IPC_TRACE(OSReport(kTraceSend, nResult, pIpc->nSizeTx));
        comboIpcSent(pIpc, (u32)nResult);
    }

    while (pIpc->nSizeRx < COMBO_IPC_BUFFER_SIZE) {
        nResult = comboNetRecv(pIpc->aRx + pIpc->nSizeRx, (s32)(COMBO_IPC_BUFFER_SIZE - pIpc->nSizeRx));

        if (nResult < 0) {
            comboIpcDown(pIpc, 3, (s32)pIpc->nSizeRx, comboNetGetError());
            comboNetClose();
            return;
        }

        if (nResult == 0) {
            break;
        }

        /* The first bytes off the socket, before any framing logic. If they disagree with what the
           relay printed, the corruption is in the transport rather than here. */
        if (pIpc->nRxFirst == 0) {
            s32 iWord;

            pIpc->nRxFirst = nResult;

            for (iWord = 0; iWord < 2; iWord++) {
                u8* pHead = pIpc->aRx + pIpc->nSizeRx + iWord * 4;

                pIpc->anRxFirst[iWord] =
                    (nResult >= (iWord + 1) * 4)
                        ? (((u32)pHead[0] << 24) | ((u32)pHead[1] << 16) | ((u32)pHead[2] << 8) | (u32)pHead[3])
                        : 0;
            }
        }

        pIpc->nSizeRx += (u32)nResult;
        IPC_TRACE(OSReport(kTraceRecv, nResult, pIpc->nSizeRx));
    }
#elif COMBO_IPC_BACKEND == COMBO_IPC_BACKEND_LOOPBACK
    if (!bNow && (u32)OSDiffTick(OSGetTick(), pIpc->nTickPoll) < IPC_POLL_TICKS) {
        return;
    }

    pIpc->nTickPoll = OSGetTick();
    pIpc->nPumps++;

    if (!(pIpc->nStatus & IPC_STATUS_CONNECTED)) {
        pIpc->nTickUp = pIpc->nTickPoll;
    }

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
        pIpc->nWriteErr++;
        pIpc->nWriteWhy = 1;
        pIpc->nStatus |= IPC_STATUS_ERROR;
        return;
    }

    if (nSize == 0 || nSize > COMBO_IPC_MSG_MAX) {
        IPC_TRACE(OSReport(kTraceWrite, nSize, kWhySize));
        pIpc->nWriteErr++;
        pIpc->nWriteWhy = 2;
        pIpc->nStatus |= IPC_STATUS_ERROR;
        return;
    }

    if (!comboIpcMakeRoom(pIpc, nSize + 4)) {
        IPC_TRACE(OSReport(kTraceWrite, nSize, kWhyFull));
        pIpc->nWriteErr++;
        pIpc->nWriteWhy = 3;
        pIpc->nStatus |= IPC_STATUS_ERROR;
        return;
    }

    if (!comboIpcRamCopy(pIpc, pIpc->nAddressRAM, pIpc->aTx + pIpc->nSizeTx + 4, nSize, false)) {
        IPC_TRACE(OSReport(kTraceWrite, nSize, kWhyRam));
        pIpc->nWriteErr++;
        pIpc->nWriteWhy = 4;
        pIpc->nStatus |= IPC_STATUS_ERROR;
        return;
    }

    /* A greeting after the first one means the guest dropped its own logical session:
       Multi_Disconnect() runs on every non-normal game mode (a cutscene, the pause menu, a scene
       change, an item-get) and it greets again on the connection it already has. The relay answers
       those itself, so the socket is left alone -- dropping it instead was the reconnect churn.

       nHellos is how many greetings are still owed an answer, so comboIpcRead delivers exactly one
       reply each; nResalutes is the running total for the overlay. */
    if (nSize >= 5 && pIpc->aTx[pIpc->nSizeTx + 4 + 4] == IPC_OP_HELLO) {
        if (pIpc->nRxThisLink != 0) {
            pIpc->nResalutes++;
        }

        pIpc->nHellos++;
    }

    comboIpcPutLE32(pIpc->aTx + pIpc->nSizeTx, nSize);
    pIpc->nSizeTx += nSize + 4;
    pIpc->nMsgTx++;
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

    for (;;) {
        nLength = comboIpcPeekRx(pIpc->aRx, pIpc->nSizeRx);

        if (nLength == 0) {
            if (comboIpcRxBroken(pIpc->aRx, pIpc->nSizeRx)) {
                comboIpcResync(pIpc);
                continue;
            }

            return;
        }

        if (nLength >= 5 && pIpc->aRx[IPC_RX_HEADER + 4] == IPC_OP_HELLO && pIpc->nHellos == 0) {
            comboIpcDrop(pIpc->aRx, &pIpc->nSizeRx, IPC_RX_HEADER + IPC_ROUND4(nLength));
            pIpc->nStale++;
            continue;
        }

        break;
    }

    if (nLength > nMax) {
        IPC_REPORT1(kMsgOverrun, nLength);
        comboIpcDrop(pIpc->aRx, &pIpc->nSizeRx, IPC_RX_HEADER + IPC_ROUND4(nLength));
        pIpc->nEmpty++;
        return;
    }

    if (!comboIpcRamCopy(pIpc, pIpc->nAddressRAM, pIpc->aRx + IPC_RX_HEADER, nLength, true)) {
        return;
    }

    if (nLength >= 5 && pIpc->aRx[IPC_RX_HEADER + 4] == IPC_OP_HELLO) {
        pIpc->nHellos--;
    }

    pIpc->nLastRxLen = (s32)nLength;
    pIpc->nLastRxOp = (nLength >= 5) ? (s32)pIpc->aRx[IPC_RX_HEADER + 4] : -1;

    comboIpcDrop(pIpc->aRx, &pIpc->nSizeRx, IPC_RX_HEADER + IPC_ROUND4(nLength));
    pIpc->nLengthRead = nLength;
    pIpc->nRxThisLink++;
    pIpc->nMsgRx++;
    IPC_TRACE(OSReport(kTraceDeliver, nLength));
}

#if COMBO_IPC_OVERLAY

/** @brief Link status at the bottom of the picture */
#define IPC_OVL_SCALE 2
#define IPC_OVL_CHAR_W (8 * IPC_OVL_SCALE)
#define IPC_OVL_LINE_H (8 * IPC_OVL_SCALE + 2)
#define IPC_OVL_COLS 36
#define IPC_OVL_ROWS 8
#define IPC_OVL_LINE_MAX 64
#define IPC_OVL_X 24
#define IPC_OVL_PAD 4

/* YUYV, what the external framebuffer holds: Y high byte, neutral 0x80 chroma. */
#define IPC_OVL_FG 0xFF80
#define IPC_OVL_BG 0x1080

/* The XFBs live in MEM2 (around 0x90000000) */
#define IPC_OVL_FB_COUNT 2
#define IPC_OVL_MEM1_LO 0x80000000
#define IPC_OVL_MEM1_HI 0x81800000
#define IPC_OVL_MEM2_LO 0x90000000
#define IPC_OVL_MEM2_HI 0x94000000

#define IPC_OVL_BUILD_TICKS OSMicrosecondsToTicks(250000)
#define IPC_OVL_HOLD_TICKS OSSecondsToTicks(COMBO_IPC_OVERLAY_HOLD)

#define IPC_OVL_OFF 1
#define IPC_OVL_ON 2

CD static char sOvlLine[IPC_OVL_ROWS][IPC_OVL_LINE_MAX] = {" ", " ", " ", " ", " ", " ", " ", " "};
CD static s32 sOvlShow = IPC_OVL_OFF;
CD static u32 sOvlTick = 1;

CD static VIRetraceCallback sOvlPrev = (VIRetraceCallback)-1;

CD static char kOvlState[] = "IPC %s e%d p%d";
CD static char kOvlLocal[] = "wii %d.%d.%d.%d";
CD static char kOvlRemote[] = "pc  %d.%d.%d.%d:%d";
CD static char kOvlCounts[] = "tx %d rx %d st %d ko %d";
CD static char kOvlDrop[] = "drop %d i%d e%d z%d h%d x%d";
CD static char kOvlHead[] = "rx %08x %08x %08x";
CD static char kOvlFirst[] = "f%d %08x %08x s%d y%d";
CD static char kOvlWhy[] = "w%d/%d last %d op%d p%d";

CT static bool comboIpcOvlIsRam(u32 nAddress) {
    return (nAddress >= IPC_OVL_MEM1_LO && nAddress < IPC_OVL_MEM1_HI) ||
           (nAddress >= IPC_OVL_MEM2_LO && nAddress < IPC_OVL_MEM2_HI);
}

//! x stays even in every caller so each pair lands on a proper YUYV [Y0 U][Y1 V] boundary.
CT static void comboIpcOvlChar(u16* pFB, s32 nStride, s32 x, s32 y, char ch) {
    const u8* pGlyph;
    s32 iRow;
    s32 iCol;
    s32 iX;
    s32 iY;

    if ((u8)ch < 0x20 || (u8)ch > 0x7E) {
        return;
    }

    pGlyph = gCrashFont[(u8)ch - 0x20];

    for (iRow = 0; iRow < 8; iRow++) {
        u8 nBits = pGlyph[iRow];

        for (iCol = 0; iCol < 8; iCol++) {
            if (nBits & (1 << iCol)) {
                for (iY = 0; iY < IPC_OVL_SCALE; iY++) {
                    u16* p = pFB + (y + iRow * IPC_OVL_SCALE + iY) * nStride + (x + iCol * IPC_OVL_SCALE);

                    for (iX = 0; iX < IPC_OVL_SCALE; iX++) {
                        p[iX] = IPC_OVL_FG;
                    }
                }
            }
        }
    }
}

CT static void comboIpcOvlRect(u16* pFB, s32 nStride, s32 x, s32 y, s32 w, s32 h, u16 nColor) {
    s32 iX;
    s32 iY;

    for (iY = 0; iY < h; iY++) {
        u16* p = pFB + (y + iY) * nStride + x;

        for (iX = 0; iX < w; iX++) {
            p[iX] = nColor;
        }
    }
}

CT static void comboIpcOvlStamp(u16* pFB, s32 nStride, s32 y0) {
    s32 iLine;

    for (iLine = 0; iLine < IPC_OVL_ROWS; iLine++) {
        s32 y = y0 + iLine * IPC_OVL_LINE_H;
        s32 x = IPC_OVL_X;
        const char* pText = sOvlLine[iLine];
        s32 nLen = 0;

        while (pText[nLen] != '\0' && nLen < IPC_OVL_COLS) {
            nLen++;
        }

        comboIpcOvlRect(pFB, nStride, x - IPC_OVL_PAD, y - 1, nLen * IPC_OVL_CHAR_W + 2 * IPC_OVL_PAD, IPC_OVL_LINE_H,
                        IPC_OVL_BG);

        while (nLen-- > 0) {
            comboIpcOvlChar(pFB, nStride, x, y, *pText++);
            x += IPC_OVL_CHAR_W;
        }
    }

    DCStoreRange(pFB + (y0 - 1) * nStride, IPC_OVL_ROWS * IPC_OVL_LINE_H * nStride * (s32)sizeof(u16));
}

CT static void comboIpcOvlRetrace(u32 nCount) {
    s32 nStride;
    s32 nHeight;
    s32 y0;
    s32 iFB;

    if (sOvlPrev != NULL && sOvlPrev != (VIRetraceCallback)-1) {
        sOvlPrev(nCount);
    }

    if (sOvlShow != IPC_OVL_ON || rmode == NULL) {
        return;
    }

    nStride = rmode->fbWidth;
    nHeight = rmode->xfbHeight;
    y0 = nHeight - IPC_OVL_ROWS * IPC_OVL_LINE_H - IPC_OVL_PAD;

    if (nStride < IPC_OVL_X + IPC_OVL_COLS * IPC_OVL_CHAR_W + 2 * IPC_OVL_PAD || y0 < 1) {
        return;
    }

    for (iFB = 0; iFB < IPC_OVL_FB_COUNT; iFB++) {
        u16* pFB = (u16*)lbl_8017B1E0[iFB].unk_04;

        if (comboIpcOvlIsRam((u32)pFB)) {
            comboIpcOvlStamp(pFB, nStride, y0);
        }
    }
}

/**
 * @brief Rebuilds the status text, and decides whether it is on screen.
 *
 * Visible while the link is down and for COMBO_IPC_OVERLAY_HOLD seconds after it comes up, so a
 * healthy session is not cluttered and any failure stays on screen. Nothing until a guest arms
 * the register block
 */
CT static void comboIpcOverlay(ComboIpc* pIpc) {
    ComboNetInfo info;
    u32 nTick;
    u32 nLocal;
    u32 nRemote;

    if (pIpc->nKey != IPC_MAGIC_OUT) {
        return;
    }

    nTick = OSGetTick();

    if ((u32)OSDiffTick(nTick, sOvlTick) < IPC_OVL_BUILD_TICKS) {
        return;
    }

    sOvlTick = nTick;

    if (sOvlPrev == (VIRetraceCallback)-1) {
        sOvlPrev = VISetPostRetraceCallback(comboIpcOvlRetrace);
    }

    comboNetGetInfo(&info);
    nLocal = info.nAddressLocal;
    nRemote = info.nAddressRemote;

    sprintf(sOvlLine[0], kOvlState, info.szState, info.nError, pIpc->nPumps);
    sprintf(sOvlLine[1], kOvlLocal, (nLocal >> 24) & 0xFF, (nLocal >> 16) & 0xFF, (nLocal >> 8) & 0xFF, nLocal & 0xFF);
    sprintf(sOvlLine[2], kOvlRemote, (nRemote >> 24) & 0xFF, (nRemote >> 16) & 0xFF, (nRemote >> 8) & 0xFF,
            nRemote & 0xFF, info.nPort);
    sprintf(sOvlLine[3], kOvlCounts, pIpc->nMsgTx, pIpc->nMsgRx, pIpc->nStatus & 7, pIpc->nDrops);
    sprintf(sOvlLine[4], kOvlDrop, pIpc->nDropWhy, pIpc->nDropInfo, pIpc->nDropErr, pIpc->nEmpty,
            pIpc->nResalutes, pIpc->nStale);
    sprintf(sOvlLine[5], kOvlHead, pIpc->anDropHead[0], pIpc->anDropHead[1], pIpc->anDropHead[2]);
    sprintf(sOvlLine[6], kOvlFirst, pIpc->nRxFirst, pIpc->anRxFirst[0], pIpc->anRxFirst[1], comboNetGetStalls(),
            pIpc->nResync);
    sprintf(sOvlLine[7], kOvlWhy, pIpc->nWriteErr, pIpc->nWriteWhy, pIpc->nLastRxLen, pIpc->nLastRxOp, pIpc->nDropped);

    if (!(pIpc->nStatus & IPC_STATUS_CONNECTED) || (u32)OSDiffTick(nTick, pIpc->nTickUp) < IPC_OVL_HOLD_TICKS) {
        sOvlShow = IPC_OVL_ON;
    } else {
        sOvlShow = IPC_OVL_OFF;
    }
}

#else

#define comboIpcOverlay(pIpc) ((void)0)

#endif

CT bool comboIpcPut8(ComboIpc* pIpc, u32 nAddress, s8* pData) { return false; }

CT bool comboIpcPut16(ComboIpc* pIpc, u32 nAddress, s16* pData) { return false; }

CT bool comboIpcPut32(ComboIpc* pIpc, u32 nAddress, s32* pData) {
    u32 nRegister;

    nRegister = nAddress & 0x3F;
    IPC_TRACE(OSReport(kTraceReg, nRegister, *pData, pIpc->nStatus, pIpc->nSizeTx, pIpc->nSizeRx));

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

    comboIpcOverlay(pIpc);
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

            if (comboIpcPeekRx(pIpc->aRx, pIpc->nSizeRx) != 0) {
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

    comboIpcOverlay(pIpc);
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
            pIpc->nTickUp = pIpc->nTickPoll;
            pIpc->nPumps = 0;
            pIpc->nMsgTx = 0;
            pIpc->nMsgRx = 0;
            pIpc->nDrops = 0;
            pIpc->nDropWhy = 0;
            pIpc->nDropInfo = 0;
            pIpc->nDropErr = 0;
            pIpc->anDropHead[0] = 0;
            pIpc->anDropHead[1] = 0;
            pIpc->anDropHead[2] = 0;
            pIpc->nEmpty = 0;
            pIpc->nRxFirst = 0;
            pIpc->anRxFirst[0] = 0;
            pIpc->anRxFirst[1] = 0;
            pIpc->nHellos = 0;
            pIpc->nResalutes = 0;
            pIpc->nWriteErr = 0;
            pIpc->nLastRxLen = 0;
            pIpc->nLastRxOp = -1;
            pIpc->nDropped = 0;
            pIpc->nWriteWhy = 0;
            pIpc->nRxThisLink = 0;
            pIpc->nStale = 0;
            pIpc->nTxHold = 0;
            pIpc->nResync = 0;
            break;
        case 3:
            pIpc->nDropWhy = 5;
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
