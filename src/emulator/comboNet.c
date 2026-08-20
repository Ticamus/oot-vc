/**
 * @file comboNet.c
 *
 * TCP transport for the OoTMM multiplayer bridge, driven directly through IOS.
 *
 * The DOL has no socket library, so the /dev/net interfaces are accessed directly,
 * following libogc's network_wii implementation.
 *
 * /dev/net/kd/request is used to bring up the network interface, while /dev/net/ip/top
 * provides the socket operations:
 *   - SO_STARTUP / SO_GETHOSTID
 *   - SO_SOCKET / SO_FCNTL
 *   - SO_CONNECT / SO_CLOSE
 *   - SENDTO / RECVFROM
 *
 * The socket is non-blocking. sendto/recvfrom return EAGAIN when there is no work,
 * and connect returns EINPROGRESS/EALREADY while the connection is in progress.
 * This ensures the emulator is never stalled by network I/O.
 *
 * All operations run on the emulator's main thread through the register handlers
 * in comboIpc.c.
 */
#include "emulator/comboIpc.h"

#if IS_MM && COMBO_MULTI

#include "emulator/xlHeap.h"
#include "revolution/ipc.h"
#include "revolution/os.h"
#include "string.h"

#pragma section ".crashtext"
#pragma section ".crashdata" \
                ".crashbss"

#define CT DECL_SECTION(".crashtext")
#define CD DECL_SECTION(".crashdata")

#if COMBO_IPC_BACKEND == COMBO_IPC_BACKEND_SOCKET

s32 fn_800C5728(s32 fd);
s32 fn_800C5EE0(s32 fd, s32 type, void* pIn, s32 nIn, void* pOut, s32 nOut);

#define COMBO_IOS_Close fn_800C5728
#define COMBO_IOS_Ioctl fn_800C5EE0

#define NET_IOCTL_KD_STARTUP 6

#define NET_IOCTL_SO_CLOSE 3
#define NET_IOCTL_SO_CONNECT 4
#define NET_IOCTL_SO_FCNTL 5
#define NET_IOCTL_SO_SOCKET 15
#define NET_IOCTL_SO_GETHOSTID 16
#define NET_IOCTL_SO_STARTUP 31
#define NET_IOCTLV_SO_RECVFROM 12
#define NET_IOCTLV_SO_SENDTO 13

#define NET_AF_INET 2
#define NET_SOCK_STREAM 1

#define NET_F_SETFL 4
#define NET_O_NONBLOCK 0x04

#define NET_EAGAIN (-6)
#define NET_EALREADY (-7)
#define NET_EINPROGRESS (-26)
#define NET_EISCONN (-30)

/* Steps of the bring-up. Each one is a single ioctl, and only KD_STARTUP can take real time. */
#define NET_STATE_CLOSED 0
#define NET_STATE_KD_OPEN 1
#define NET_STATE_KD_WAIT 2
#define NET_STATE_IP_OPEN 3
#define NET_STATE_HOST_WAIT 4
#define NET_STATE_CONNECT 5
#define NET_STATE_READY 6
#define NET_STATE_FAILED 7

/* Layout of the one aligned slab. IOS wants every buffer it touches 32-byte aligned, and that
   includes the io-vector array itself, so each region gets its own 64-byte lane. */
#define NET_OFF_IN 0x000
#define NET_OFF_OUT 0x040
#define NET_OFF_VEC 0x080
#define NET_OFF_ADDR 0x0C0
#define NET_OFF_DATA 0x100
#define NET_SLAB_SIZE (NET_OFF_DATA + COMBO_IPC_IO_SIZE + 0x20)

typedef struct ComboNet {
    /* 0x00 */ u8* pSlab;
    /* 0x04 */ u8* pAligned;
    /* 0x08 */ s32 nState;
    /* 0x0C */ s32 nFileKD;
    /* 0x10 */ s32 nFileIP;
    /* 0x14 */ s32 nSocket;
    /* 0x18 */ u32 nTickRetry;
    /* 0x1C */ u32 nTries;
    /* 0x20 */ volatile s32 nAsyncResult;
    /* 0x24 */ volatile s32 nAsyncPending;
} ComboNet;

CD static ComboNet sNet = {NULL, NULL, NET_STATE_CLOSED, -1, -1, -1, 0, 0, 0, 0};

CD static char kPathKD[] = "/dev/net/kd/request";
CD static char kPathIP[] = "/dev/net/ip/top";

#if COMBO_IPC_REPORT
CD static char kMsgOpenFail[] = "comboNet: cannot open %s (%d)\n";
CD static char kMsgHostId[] = "comboNet: wii address %d.%d.%d.%d\n";
CD static char kMsgConnect[] = "comboNet: connect -> %d\n";
CD static char kMsgGiveUp[] = "comboNet: bring-up failed at step %d (%d)\n";
#define NET_REPORT1(s, a) OSReport(s, a)
#define NET_REPORT2(s, a, b) OSReport(s, a, b)
#define NET_REPORT4(s, a, b, c, d) OSReport(s, a, b, c, d)
#else
#define NET_REPORT1(s, a) ((void)0)
#define NET_REPORT2(s, a, b) ((void)0)
#define NET_REPORT4(s, a, b, c, d) ((void)0)
#endif

CT static u8* comboNetBuffer(s32 nOffset) { return sNet.pAligned + nOffset; }

CT static void comboNetPutWord(s32 nOffset, s32 nIndex, u32 nValue) {
    *(u32*)(sNet.pAligned + nOffset + nIndex * 4) = nValue;
}

/** @brief Callback for the one asynchronous ioctl (the wifi bring-up). Runs on the IPC interrupt. */
CT static s32 comboNetAsyncDone(s32 nResult, void* pArgument) {
    sNet.nAsyncResult = nResult;
    sNet.nAsyncPending = 0;
    return nResult;
}

/** @brief Releases the socket, keeping the interface up so a retry is cheap. */
CT static void comboNetDropSocket(void) {
    if (sNet.nSocket >= 0) {
        comboNetPutWord(NET_OFF_IN, 0, (u32)sNet.nSocket);
        COMBO_IOS_Ioctl(sNet.nFileIP, NET_IOCTL_SO_CLOSE, comboNetBuffer(NET_OFF_IN), 4, NULL, 0);
        sNet.nSocket = -1;
    }
}

/** @brief Arms a retry after COMBO_IPC_RETRY_US, from wherever the bring-up stopped. */
CT static void comboNetFail(s32 nStep, s32 nResult) {
    NET_REPORT2(kMsgGiveUp, nStep, nResult);
    comboNetDropSocket();
    sNet.nState = NET_STATE_FAILED;
    sNet.nTickRetry = OSGetTick();
    sNet.nAsyncPending = 0;
}

/**
 * @brief Creates a non-blocking socket and starts a connect.
 *
 * F_SETFL has to land before SO_CONNECT, or the connect ioctl blocks inside IOS until
 * the TCP handshake finishes or times out, and that stall would be the emulator's.
 */
CT static bool comboNetConnect(void) {
    s32 nResult;
    u8* pAddress;

    comboNetPutWord(NET_OFF_IN, 0, NET_AF_INET);
    comboNetPutWord(NET_OFF_IN, 1, NET_SOCK_STREAM);
    comboNetPutWord(NET_OFF_IN, 2, 0);
    nResult = COMBO_IOS_Ioctl(sNet.nFileIP, NET_IOCTL_SO_SOCKET, comboNetBuffer(NET_OFF_IN), 12, NULL, 0);

    if (nResult < 0) {
        comboNetFail(NET_STATE_CONNECT, nResult);
        return false;
    }

    sNet.nSocket = nResult;

    comboNetPutWord(NET_OFF_IN, 0, (u32)sNet.nSocket);
    comboNetPutWord(NET_OFF_IN, 1, NET_F_SETFL);
    comboNetPutWord(NET_OFF_IN, 2, NET_O_NONBLOCK);
    nResult = COMBO_IOS_Ioctl(sNet.nFileIP, NET_IOCTL_SO_FCNTL, comboNetBuffer(NET_OFF_IN), 12, NULL, 0);

    if (nResult < 0) {
        comboNetFail(NET_STATE_CONNECT, nResult);
        return false;
    }

    /* in = {socket, has_addr, sockaddr_in padded to 28}; sockaddr_in is
       {u8 len, u8 family, u16 port, u32 addr, u8 zero[8]}. */
    memset(comboNetBuffer(NET_OFF_ADDR), 0, 36);
    comboNetPutWord(NET_OFF_ADDR, 0, (u32)sNet.nSocket);
    comboNetPutWord(NET_OFF_ADDR, 1, 1);
    pAddress = comboNetBuffer(NET_OFF_ADDR) + 8;
    pAddress[0] = 8;
    pAddress[1] = NET_AF_INET;
    pAddress[2] = (u8)(COMBO_IPC_PORT >> 8);
    pAddress[3] = (u8)(COMBO_IPC_PORT & 0xFF);
    pAddress[4] = (u8)(COMBO_IPC_HOST >> 24);
    pAddress[5] = (u8)(COMBO_IPC_HOST >> 16);
    pAddress[6] = (u8)(COMBO_IPC_HOST >> 8);
    pAddress[7] = (u8)(COMBO_IPC_HOST >> 0);

    nResult = COMBO_IOS_Ioctl(sNet.nFileIP, NET_IOCTL_SO_CONNECT, comboNetBuffer(NET_OFF_ADDR), 36, NULL, 0);
    NET_REPORT1(kMsgConnect, nResult);

    if (nResult == 0 || nResult == NET_EISCONN) {
        sNet.nState = NET_STATE_READY;
        return true;
    }

    if (nResult == NET_EINPROGRESS || nResult == NET_EALREADY || nResult == NET_EAGAIN) {
        sNet.nState = NET_STATE_CONNECT;
        return true;
    }

    comboNetFail(NET_STATE_CONNECT, nResult);
    return false;
}

/** @brief Re-issues connect on the pending socket; -30 EISCONN is how IOS says "you are through". */
CT static void comboNetConnectPoll(void) {
    s32 nResult;

    nResult = COMBO_IOS_Ioctl(sNet.nFileIP, NET_IOCTL_SO_CONNECT, comboNetBuffer(NET_OFF_ADDR), 36, NULL, 0);

    if (nResult == 0 || nResult == NET_EISCONN) {
        sNet.nState = NET_STATE_READY;
        return;
    }

    if (nResult == NET_EINPROGRESS || nResult == NET_EALREADY || nResult == NET_EAGAIN) {
        return;
    }

    comboNetFail(NET_STATE_CONNECT, nResult);
}

CT void comboNetOpen(void) {
    if (sNet.pSlab == NULL) {
        if (!xlHeapTake((void**)&sNet.pSlab, NET_SLAB_SIZE)) {
            return;
        }
        sNet.pAligned = (u8*)(((u32)sNet.pSlab + 0x1F) & ~0x1F);
        memset(sNet.pAligned, 0, NET_SLAB_SIZE - 0x20);
    }

    if (sNet.nState == NET_STATE_CLOSED) {
        sNet.nState = NET_STATE_KD_OPEN;
        sNet.nTries = 0;
    }
}

CT void comboNetClose(void) {
    comboNetDropSocket();

    if (sNet.nState == NET_STATE_READY || sNet.nState == NET_STATE_CONNECT) {
        sNet.nState = NET_STATE_FAILED;
        sNet.nTickRetry = OSGetTick();
    }
}

/** @brief Advances the bring-up by at most one step and reports whether the link is usable. */
CT bool comboNetPoll(void) {
    s32 nResult;
    u32 nHost;

    if (sNet.pAligned == NULL) {
        return false;
    }

    if (sNet.nState == NET_STATE_FAILED) {
        if ((u32)OSDiffTick(OSGetTick(), sNet.nTickRetry) < OSMicrosecondsToTicks(COMBO_IPC_RETRY_US)) {
            return false;
        }

        sNet.nTries = 0;

        if (sNet.nFileIP >= 0) {
            sNet.nState = NET_STATE_CONNECT;
            comboNetConnect();
        } else {
            sNet.nState = NET_STATE_KD_OPEN;
        }

        return sNet.nState == NET_STATE_READY;
    }

    if (sNet.nState == NET_STATE_KD_OPEN) {
        if (sNet.nFileKD < 0) {
            sNet.nFileKD = IOS_Open(kPathKD, IPC_OPEN_NONE);

            if (sNet.nFileKD < 0) {
                NET_REPORT2(kMsgOpenFail, kPathKD, sNet.nFileKD);
                comboNetFail(NET_STATE_KD_OPEN, sNet.nFileKD);
                return false;
            }
        }

        /* The wifi bring-up. Asynchronous because this is the one call that can take seconds. */
        sNet.nAsyncPending = 1;
        sNet.nAsyncResult = 0;
        nResult = IOS_IoctlAsync(sNet.nFileKD, NET_IOCTL_KD_STARTUP, NULL, 0, comboNetBuffer(NET_OFF_OUT), 32,
                                 comboNetAsyncDone, NULL);

        if (nResult < 0) {
            comboNetFail(NET_STATE_KD_OPEN, nResult);
            return false;
        }

        sNet.nState = NET_STATE_KD_WAIT;
        return false;
    }

    if (sNet.nState == NET_STATE_KD_WAIT) {
        if (sNet.nAsyncPending != 0) {
            return false;
        }

        if (sNet.nAsyncResult < 0) {
            comboNetFail(NET_STATE_KD_WAIT, sNet.nAsyncResult);
            return false;
        }

        sNet.nState = NET_STATE_IP_OPEN;
        return false;
    }

    if (sNet.nState == NET_STATE_IP_OPEN) {
        if (sNet.nFileIP < 0) {
            sNet.nFileIP = IOS_Open(kPathIP, IPC_OPEN_NONE);

            if (sNet.nFileIP < 0) {
                NET_REPORT2(kMsgOpenFail, kPathIP, sNet.nFileIP);
                comboNetFail(NET_STATE_IP_OPEN, sNet.nFileIP);
                return false;
            }
        }

        nResult = COMBO_IOS_Ioctl(sNet.nFileIP, NET_IOCTL_SO_STARTUP, NULL, 0, NULL, 0);

        if (nResult < 0) {
            comboNetFail(NET_STATE_IP_OPEN, nResult);
            return false;
        }

        sNet.nState = NET_STATE_HOST_WAIT;
        sNet.nTries = 0;
        return false;
    }

    if (sNet.nState == NET_STATE_HOST_WAIT) {
        /* SO_GETHOSTID returns the address as its result, and 0 until DHCP is done. */
        nHost = (u32)COMBO_IOS_Ioctl(sNet.nFileIP, NET_IOCTL_SO_GETHOSTID, NULL, 0, NULL, 0);

        if (nHost == 0) {
            if (++sNet.nTries > 1200) {
                comboNetFail(NET_STATE_HOST_WAIT, 0);
            }
            return false;
        }

        NET_REPORT4(kMsgHostId, (nHost >> 24) & 0xFF, (nHost >> 16) & 0xFF, (nHost >> 8) & 0xFF, nHost & 0xFF);
        comboNetConnect();
        return sNet.nState == NET_STATE_READY;
    }

    if (sNet.nState == NET_STATE_CONNECT) {
        comboNetConnectPoll();
        return sNet.nState == NET_STATE_READY;
    }

    return sNet.nState == NET_STATE_READY;
}

/**
 * @brief Hands bytes to the socket. Returns how many it took, 0 for "not now", -1 for a dead link.
 */
CT s32 comboNetSend(const void* pData, s32 nSize) {
    IPCIOVector* pVector;
    s32 nResult;

    if (sNet.nState != NET_STATE_READY) {
        return -1;
    }

    if (nSize > COMBO_IPC_IO_SIZE) {
        nSize = COMBO_IPC_IO_SIZE;
    }

    memcpy(comboNetBuffer(NET_OFF_DATA), pData, nSize);

    /* params = {socket, flags, has_destination, sockaddr[28]} */
    memset(comboNetBuffer(NET_OFF_IN), 0, 40);
    comboNetPutWord(NET_OFF_IN, 0, (u32)sNet.nSocket);

    pVector = (IPCIOVector*)comboNetBuffer(NET_OFF_VEC);
    pVector[0].base = comboNetBuffer(NET_OFF_DATA);
    pVector[0].length = nSize;
    pVector[1].base = comboNetBuffer(NET_OFF_IN);
    pVector[1].length = 40;

    nResult = IOS_Ioctlv(sNet.nFileIP, NET_IOCTLV_SO_SENDTO, 2, 0, pVector);

    if (nResult >= 0) {
        return nResult;
    }

    if (nResult == NET_EAGAIN) {
        return 0;
    }

    return -1;
}

/**
 * @brief Pulls bytes off the socket. Returns how many arrived, 0 for "nothing", -1 for a dead link.
 *
 * A recv of 0 on a stream socket is the peer closing, which is a dead link and not an idle one.
 */
CT s32 comboNetRecv(void* pData, s32 nSize) {
    IPCIOVector* pVector;
    s32 nResult;

    if (sNet.nState != NET_STATE_READY) {
        return -1;
    }

    if (nSize > COMBO_IPC_IO_SIZE) {
        nSize = COMBO_IPC_IO_SIZE;
    }

    /* params = {socket, flags} */
    comboNetPutWord(NET_OFF_IN, 0, (u32)sNet.nSocket);
    comboNetPutWord(NET_OFF_IN, 1, 0);

    pVector = (IPCIOVector*)comboNetBuffer(NET_OFF_VEC);
    pVector[0].base = comboNetBuffer(NET_OFF_IN);
    pVector[0].length = 8;
    pVector[1].base = comboNetBuffer(NET_OFF_DATA);
    pVector[1].length = nSize;
    pVector[2].base = NULL;
    pVector[2].length = 0;

    nResult = IOS_Ioctlv(sNet.nFileIP, NET_IOCTLV_SO_RECVFROM, 1, 2, pVector);

    if (nResult > 0) {
        if (nResult > nSize) {
            return -1;
        }
        memcpy(pData, comboNetBuffer(NET_OFF_DATA), nResult);
        return nResult;
    }

    if (nResult == NET_EAGAIN) {
        return 0;
    }

    return -1;
}

#else

/* Backends other than SOCKET need the transport to exist but do nothing. */
CT void comboNetOpen(void) {}
CT void comboNetClose(void) {}
CT bool comboNetPoll(void) { return false; }
CT s32 comboNetSend(const void* pData, s32 nSize) { return -1; }
CT s32 comboNetRecv(void* pData, s32 nSize) { return -1; }

#endif

#endif
