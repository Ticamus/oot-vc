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
 * The socket is non-blocking throughout, so nothing here ever stalls the emulator: sendto/recvfrom
 * answer EAGAIN when there is no work and connect answers EINPROGRESS/EALREADY while it is dialling.
 * Everything runs on the main thread, from the register handlers in comboIpc.c.
 */
#include "emulator/comboIpc.h"

#if !defined(IS_MM)
#error "comboIpc.h did not bring in macros.h: IS_MM is undefined, so this unit would compile to nothing."
#endif

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
#define NET_IOCTL_NCD_LINKSTATUS 7

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

/* The codes that actually mean the connection is gone; everything else is "not now" and gets retried.
   Whitelisting EAGAIN instead was fine under Dolphin and tore the link down constantly on a console,
   where a non-blocking socket answers with more than one transient code. */
#define NET_ECONNABORTED (-13)
#define NET_ECONNREFUSED (-14)
#define NET_ECONNRESET (-15)
#define NET_ENOTCONN (-56)
#define NET_EPIPE (-66)
#define NET_ETIMEDOUT (-76)

/* Ours, not IOS's: reads unambiguously on the overlay as "we stopped waiting". */
#define NET_ETIMEOUT (-9001)

/* How long an interface may take to associate and pick up a lease. A cold wifi association on a
   console is nothing like Dolphin answering instantly, hence the patience. */
#define NET_SECONDS_LINK 6
#define NET_SECONDS_DHCP 45
#define NET_SECONDS_CONNECT 10

/* Steps of the bring-up, in libogc's order: wait for the interface, then NWC24, then sockets. Each is
   a single ioctl, and only KD_STARTUP can take real time. */
#define NET_STATE_CLOSED 0
#define NET_STATE_LINK 1
#define NET_STATE_KD_OPEN 2
#define NET_STATE_KD_WAIT 3
#define NET_STATE_IP_OPEN 4
#define NET_STATE_HOST_WAIT 5
#define NET_STATE_CONNECT 6
#define NET_STATE_READY 7
#define NET_STATE_FAILED 8

/* Layout of the one aligned slab. IOS wants every buffer it touches 32-byte aligned, and that
   includes the io-vector array itself, so each region gets its own 64-byte lane. */
#define NET_OFF_IN 0x000
#define NET_OFF_OUT 0x040
#define NET_OFF_VEC 0x080
#define NET_OFF_ADDR 0x0C0

/* Send and receive get their own lanes. Sharing one fed the outgoing queue straight back in: RECVFROM
   can report a byte count without writing anything, and what was left in the lane was the game's own
   last SENDTO. */
#define NET_OFF_SEND 0x100
#define NET_OFF_RECV (NET_OFF_SEND + COMBO_IPC_IO_SIZE)
#define NET_SLAB_SIZE (NET_OFF_RECV + COMBO_IPC_IO_SIZE + 0x20)

/* Written into the receive lane before every RECVFROM so the hole IOS leaves at the head of a
   misaligned read is recognisable. Position-dependent rather than a constant, so real payload does not
   look like it; a run of three matching positions is a 1-in-16-million coincidence.

   With every frame padded to a multiple of four the hole should always be zero, and comboNetGetStalls()
   is on the overlay as `s<n>` to say whether that held. */
#define NET_MARK(i) ((u8)((i) ^ 0x5A))
#define NET_MARK_RUN 3

typedef struct ComboNet {
    /* 0x00 */ u8* pSlab;
    /* 0x04 */ u8* pAligned;
    /* 0x08 */ s32 nState;
    /* 0x0C */ s32 nFileKD;
    /* 0x10 */ s32 nFileNCD;
    /* 0x14 */ u32 nTickState;
    /* 0x18 */ s32 nFileIP;
    /* 0x1C */ s32 nSocket;
    /* 0x20 */ u32 nTickRetry;
    /* 0x24 */ volatile s32 nAsyncResult;
    /* 0x28 */ volatile s32 nAsyncPending;
    /* 0x2C */ s32 nLastError;
    /* 0x30 */ u32 nAddress;
    /* 0x34 */ s32 nStalls;
} ComboNet;

//! nTickState starts non-zero so the object stays in PROGBITS .crashdata: an all-zero initialiser
//! would land it in NOBITS .crashbss, which adds an entry to _bss_init_info and shifts the image.
CD static ComboNet sNet = {NULL, NULL, NET_STATE_CLOSED, -1, -1, 1, -1, -1, 0, 0, 0, 0, 0, 0};

/* Short names for the on-screen status, indexed by NET_STATE_*. An array rather than a switch: MWCC
   turns a dense switch into a jump table and jump tables live in .data. */
CD static char kStateClosed[] = "off";
CD static char kStateLink[] = "link";
CD static char kStateKdOpen[] = "wifi";
CD static char kStateKdWait[] = "wifi..";
CD static char kStateIpOpen[] = "sock";
CD static char kStateHostWait[] = "dhcp";
CD static char kStateConnect[] = "conn..";
CD static char kStateReady[] = "READY";
CD static char kStateFailed[] = "FAILED";

CD static char* kStateNames[9] = {kStateClosed,   kStateLink,    kStateKdOpen, kStateKdWait, kStateIpOpen,
                                  kStateHostWait, kStateConnect, kStateReady,  kStateFailed};

CD static char kPathNCD[] = "/dev/net/ncd/manage";
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

/** @brief Does this result mean the connection is dead, as opposed to "nothing right now"? */
CT static bool comboNetIsFatal(s32 nResult) {
    return nResult == NET_ECONNABORTED || nResult == NET_ECONNREFUSED || nResult == NET_ECONNRESET ||
           nResult == NET_ENOTCONN || nResult == NET_EPIPE || nResult == NET_ETIMEDOUT;
}

/** @brief The last raw IOS result, so a caller can report the real code instead of a sentinel. */
CT s32 comboNetGetError(void) { return sNet.nLastError; }

/** @brief Times IOS reported bytes it had not written. Should stay at 0; reported, never acted on. */
CT s32 comboNetGetStalls(void) { return sNet.nStalls; }

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
    sNet.nLastError = nResult;
    comboNetDropSocket();
    sNet.nState = NET_STATE_FAILED;
    sNet.nTickState = OSGetTick();
    sNet.nTickRetry = sNet.nTickState;
    sNet.nAsyncPending = 0;
}

/** @brief Moves to a step and stamps it, so each one can have its own patience. */
CT static void comboNetEnter(s32 nState) {
    sNet.nState = nState;
    sNet.nTickState = OSGetTick();
}

/** @brief Seconds spent in the current step. */
CT static u32 comboNetElapsed(void) { return (u32)OSDiffTick(OSGetTick(), sNet.nTickState); }

/**
 * @brief Creates a non-blocking socket and starts a connect.
 *
 * Order matters: F_SETFL has to land before SO_CONNECT, or the connect ioctl blocks inside IOS until
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
        comboNetEnter(NET_STATE_READY);
        return true;
    }

    if (nResult == NET_EINPROGRESS || nResult == NET_EALREADY || nResult == NET_EAGAIN) {
        comboNetEnter(NET_STATE_CONNECT);
        return true;
    }

    comboNetFail(NET_STATE_CONNECT, nResult);
    return false;
}

/**
 * @brief Re-issues connect on the pending socket; -30 EISCONN is how IOS says "you are through".
 *
 * Bounded: with no route IOS answers "in progress" forever, and sitting in this step was how a failed
 * bring-up became permanent.
 */
CT static void comboNetConnectPoll(void) {
    s32 nResult;

    nResult = COMBO_IOS_Ioctl(sNet.nFileIP, NET_IOCTL_SO_CONNECT, comboNetBuffer(NET_OFF_ADDR), 36, NULL, 0);

    if (nResult == 0 || nResult == NET_EISCONN) {
        comboNetEnter(NET_STATE_READY);
        return;
    }

    if (nResult == NET_EINPROGRESS || nResult == NET_EALREADY || nResult == NET_EAGAIN) {
        if (comboNetElapsed() > OSSecondsToTicks(NET_SECONDS_CONNECT)) {
            comboNetFail(NET_STATE_CONNECT, NET_ETIMEOUT);
        }
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
        comboNetEnter(NET_STATE_LINK);
    }
}

CT void comboNetClose(void) {
    comboNetDropSocket();

    if (sNet.nState == NET_STATE_READY || sNet.nState == NET_STATE_CONNECT) {
        sNet.nState = NET_STATE_FAILED;
        sNet.nTickState = OSGetTick();
        sNet.nTickRetry = sNet.nTickState;
    }
}

/**
 * @brief Advances the bring-up by at most one step and reports whether the link is usable.
 *
 * One step per call: the caller runs inside a guest register access, so this returns promptly and
 * gets called again next frame. if/else rather than switch, because MWCC turns a dense switch into a
 * jump table and jump tables live in .data, which would shift the retail image.
 */
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

        /* Only shortcut to a fresh socket when the interface is genuinely up: without a local address
           there is nothing to connect from, and retrying forever is how this got stuck. */
        if (sNet.nFileIP >= 0 && sNet.nAddress != 0) {
            comboNetEnter(NET_STATE_CONNECT);
            comboNetConnect();
        } else {
            sNet.nAddress = 0;
            comboNetEnter(NET_STATE_LINK);
        }

        return sNet.nState == NET_STATE_READY;
    }

    if (sNet.nState == NET_STATE_LINK) {
        /* Best-effort: libogc waits on this before touching NWC24, but SO_GETHOSTID is the real
           authority on a usable interface, so after NET_SECONDS_LINK we carry on regardless. */
        if (sNet.nFileNCD < 0) {
            sNet.nFileNCD = IOS_Open(kPathNCD, IPC_OPEN_NONE);
        }

        if (sNet.nFileNCD >= 0 && comboNetElapsed() <= OSSecondsToTicks(NET_SECONDS_LINK)) {
            nResult =
                COMBO_IOS_Ioctl(sNet.nFileNCD, NET_IOCTL_NCD_LINKSTATUS, NULL, 0, comboNetBuffer(NET_OFF_OUT), 32);

            if (nResult < 0) {
                return false;
            }
        }

        comboNetEnter(NET_STATE_KD_OPEN);
        return false;
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

        comboNetEnter(NET_STATE_KD_WAIT);
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

        comboNetEnter(NET_STATE_IP_OPEN);
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

        comboNetEnter(NET_STATE_HOST_WAIT);
        return false;
    }

    if (sNet.nState == NET_STATE_HOST_WAIT) {
        /* SO_GETHOSTID returns the address as its result, and 0 until the interface has a lease. The
           gate: without one, CONNECT polls a route that does not exist and never resolves. */
        nHost = (u32)COMBO_IOS_Ioctl(sNet.nFileIP, NET_IOCTL_SO_GETHOSTID, NULL, 0, NULL, 0);

        if (nHost == 0) {
            if (comboNetElapsed() > OSSecondsToTicks(NET_SECONDS_DHCP)) {
                /* Back to the beginning, not on to the connect: the interface never came up. */
                comboNetFail(NET_STATE_HOST_WAIT, NET_ETIMEOUT);
            }
            return false;
        }

        sNet.nAddress = nHost;
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
 *
 * A short count is normal and not a loss: IOS took the first nResult bytes of the lane and the caller
 * keeps the rest queued. comboIpcSent() tracks which frame that leaves half-sent.
 */
CT s32 comboNetSend(const void* pData, s32 nSize) {
    IPCIOVector* pVector;
    u8* pLane;
    s32 nResult;

    if (sNet.nState != NET_STATE_READY) {
        return -1;
    }

    pLane = comboNetBuffer(NET_OFF_SEND);

    if (nSize > COMBO_IPC_IO_SIZE) {
        nSize = COMBO_IPC_IO_SIZE;
    }

    memcpy(pLane, pData, nSize);

    /* params = {socket, flags, has_destination, sockaddr[28]} */
    memset(comboNetBuffer(NET_OFF_IN), 0, 40);
    comboNetPutWord(NET_OFF_IN, 0, (u32)sNet.nSocket);

    pVector = (IPCIOVector*)comboNetBuffer(NET_OFF_VEC);
    pVector[0].base = pLane;
    pVector[0].length = nSize;
    pVector[1].base = comboNetBuffer(NET_OFF_IN);
    pVector[1].length = 40;

    nResult = IOS_Ioctlv(sNet.nFileIP, NET_IOCTLV_SO_SENDTO, 2, 0, pVector);

    if (nResult >= 0) {
        return nResult;
    }

    if (comboNetIsFatal(nResult)) {
        sNet.nLastError = nResult;
        return -1;
    }

    /* -6 EAGAIN and every other transient: keep the bytes queued and try again next frame. Not
       recorded, or an idle poll would overwrite the error the status line exists to show. */
    return 0;
}

/**
 * @brief Pulls bytes off the socket. Returns how many arrived, 0 for "nothing", -1 for a dead link.
 *
 * A recv of 0 on a stream socket is the peer closing, which is a dead link and not an idle one.
 */
CT s32 comboNetRecv(void* pData, s32 nSize) {
    IPCIOVector* pVector;
    u8* pLane;
    s32 nResult;
    s32 iByte;
    s32 nRun;

    if (sNet.nState != NET_STATE_READY) {
        return -1;
    }

    if (nSize > COMBO_IPC_IO_SIZE) {
        nSize = COMBO_IPC_IO_SIZE;
    }

    /* Never ask for a length that is not a multiple of four: IOS eats the unaligned head of a read, so
       a request stopping mid-word puts the stream out of phase by itself and the *next* read loses the
       bytes. The caller's limit is its free queue space, which goes odd as the queue fills. */
    nSize &= ~3;

    if (nSize < 4) {
        return 0;
    }

    /* params = {socket, flags} */
    comboNetPutWord(NET_OFF_IN, 0, (u32)sNet.nSocket);
    comboNetPutWord(NET_OFF_IN, 1, 0);

    pLane = comboNetBuffer(NET_OFF_RECV);

    for (iByte = 0; iByte < nSize; iByte++) {
        pLane[iByte] = NET_MARK(iByte);
    }

    pVector = (IPCIOVector*)comboNetBuffer(NET_OFF_VEC);
    pVector[0].base = comboNetBuffer(NET_OFF_IN);
    pVector[0].length = 8;
    pVector[1].base = pLane;
    pVector[1].length = nSize;
    pVector[2].base = NULL;
    pVector[2].length = 0;

    nResult = IOS_Ioctlv(sNet.nFileIP, NET_IOCTLV_SO_RECVFROM, 1, 2, pVector);

    if (nResult > 0) {
        if (nResult > nSize) {
            sNet.nLastError = nResult;
            return -1;
        }

        /* Skip the hole IOS leaves at the head of a misaligned read (see NET_MARK). Those bytes are
           gone either way, so this is simply the reaction that keeps the stream readable; the sync word
           a stage up is what recovers from the loss. A run has to reach NET_MARK_RUN to be believed. */
        nRun = 0;

        while (nRun < nResult && pLane[nRun] == NET_MARK(nRun)) {
            nRun++;
        }

        if (nRun < NET_MARK_RUN) {
            nRun = 0;
        } else {
            sNet.nStalls++;
        }

        if (nRun >= nResult) {
            return 0;
        }

        memcpy(pData, pLane + nRun, nResult - nRun);
        return nResult - nRun;
    }

    /* 0 on a stream socket is the peer closing, which is not the same as an idle socket. */
    if (nResult == 0) {
        sNet.nLastError = 0;
        return -1;
    }

    if (comboNetIsFatal(nResult)) {
        sNet.nLastError = nResult;
        return -1;
    }

    return 0;
}

CT void comboNetGetInfo(ComboNetInfo* pInfo) {
    s32 iState;

    iState = sNet.nState;

    if (iState < 0 || iState >= (s32)ARRAY_COUNT(kStateNames)) {
        iState = NET_STATE_CLOSED;
    }

    pInfo->szState = kStateNames[iState];
    pInfo->nError = sNet.nLastError;
    pInfo->nAddressLocal = sNet.nAddress;
    pInfo->nAddressRemote = COMBO_IPC_HOST;
    pInfo->nPort = COMBO_IPC_PORT;
}

#else

/* Backends other than SOCKET need the transport to exist but do nothing. */
CD static char kStateOff[] = "n/a";

CT void comboNetOpen(void) {}
CT void comboNetClose(void) {}

CT void comboNetGetInfo(ComboNetInfo* pInfo) {
    pInfo->szState = kStateOff;
    pInfo->nError = 0;
    pInfo->nAddressLocal = 0;
    pInfo->nAddressRemote = 0;
    pInfo->nPort = 0;
}
CT bool comboNetPoll(void) { return false; }
CT s32 comboNetSend(const void* pData, s32 nSize) { return -1; }
CT s32 comboNetRecv(void* pData, s32 nSize) { return -1; }

#endif

#endif
