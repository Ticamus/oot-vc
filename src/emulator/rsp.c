#include "emulator/rsp.h"
#include "emulator/frame.h"
#include "emulator/ram.h"
#include "emulator/system.h"
#include "emulator/vc64_RVL.h"
#include "revolution/os/OSAlarm.h"
#include "revolution/os/OSThread.h"
#include "emulator/xlHeap.h"

// MM caches the owning System in the Rsp object rather than reaching for the
// global; OoT reads the global directly, so this expands to the original code.
#if IS_MM
#define RSP_HOST(pRSP) ((pRSP)->pHost)
#else
#define RSP_HOST(pRSP) (gpSystem)
#endif


static bool rspLoadMatrix(Rsp* pRSP, s32 nAddress, Mtx44 matrix) {
    s32* pMtx;
    s32 nDataA;
    s32 nDataB;
    f32 rScale;
    f32 rUpper;
    f32 rLower;
    u16 nUpper;
    u16 nLower;

    rScale = 1.0f / 65536.0f;
    if (!ramGetBuffer(SYSTEM_RAM(RSP_HOST(pRSP)), (void**)&pMtx, nAddress, NULL)) {
        return false;
    }

    nDataA = pMtx[0];
    nDataB = pMtx[8];
    nUpper = nDataA >> 16;
    nLower = nDataB >> 16;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[0][0] = rUpper + rLower * rScale;
    nUpper = nDataA & 0xFFFF;
    nLower = nDataB & 0xFFFF;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[0][1] = rUpper + rLower * rScale;

    nDataA = pMtx[1];
    nDataB = pMtx[9];
    nUpper = nDataA >> 16;
    nLower = nDataB >> 16;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[0][2] = rUpper + rLower * rScale;
    nUpper = nDataA & 0xFFFF;
    nLower = nDataB & 0xFFFF;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[0][3] = rUpper + rLower * rScale;

    nDataA = pMtx[2];
    nDataB = pMtx[10];
    nUpper = nDataA >> 16;
    nLower = nDataB >> 16;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[1][0] = rUpper + rLower * rScale;
    nUpper = nDataA & 0xFFFF;
    nLower = nDataB & 0xFFFF;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[1][1] = rUpper + rLower * rScale;

    nDataA = pMtx[3];
    nDataB = pMtx[11];
    nUpper = nDataA >> 16;
    nLower = nDataB >> 16;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[1][2] = rUpper + rLower * rScale;
    nUpper = nDataA & 0xFFFF;
    nLower = nDataB & 0xFFFF;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[1][3] = rUpper + rLower * rScale;

    nDataA = pMtx[4];
    nDataB = pMtx[12];
    nUpper = nDataA >> 16;
    nLower = nDataB >> 16;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[2][0] = rUpper + rLower * rScale;
    nUpper = nDataA & 0xFFFF;
    nLower = nDataB & 0xFFFF;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[2][1] = rUpper + rLower * rScale;

    nDataA = pMtx[5];
    nDataB = pMtx[13];
    nUpper = nDataA >> 16;
    nLower = nDataB >> 16;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[2][2] = rUpper + rLower * rScale;
    nUpper = nDataA & 0xFFFF;
    nLower = nDataB & 0xFFFF;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[2][3] = rUpper + rLower * rScale;

    nDataA = pMtx[6];
    nDataB = pMtx[14];
    nUpper = nDataA >> 16;
    nLower = nDataB >> 16;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[3][0] = rUpper + rLower * rScale;
    nUpper = nDataA & 0xFFFF;
    nLower = nDataB & 0xFFFF;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[3][1] = rUpper + rLower * rScale;

    nDataA = pMtx[7];
    nDataB = pMtx[15];
    nUpper = nDataA >> 16;
    nLower = nDataB >> 16;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[3][2] = rUpper + rLower * rScale;
    nUpper = nDataA & 0xFFFF;
    nLower = nDataB & 0xFFFF;
    OSs16tof32((s16*)&nUpper, &rUpper);
    OSu16tof32(&nLower, &rLower);
    matrix[3][3] = rUpper + rLower * rScale;

    return true;
}

inline bool rspSetDL(Rsp* pRSP, s32 nOffsetRDRAM, bool bPush) {
    s32 nAddress;
    s32* pDL;

    nAddress = SEGMENT_ADDRESS(pRSP, nOffsetRDRAM);
    if (!ramGetBuffer(SYSTEM_RAM(RSP_HOST(pRSP)), (void**)&pDL, nAddress, NULL)) {
        return false;
    }

    if (bPush && ++pRSP->iDL >= ARRAY_COUNT(pRSP->apDL)) {
        return false;
    }

    pRSP->apDL[pRSP->iDL] = (u64*)pDL;
    return true;
}

inline bool rspPopDL(Rsp* pRSP) {
    if (pRSP->iDL == 0) {
        return false;
    } else {
        pRSP->iDL--;
        return true;
    }
}

static inline bool rspSetupUCode(Rsp* pRSP) {
    Frame* pFrame;

    pFrame = SYSTEM_FRAME(RSP_HOST(pRSP));
    if (pRSP->eTypeUCode == RUT_L3DEX1 || pRSP->eTypeUCode == RUT_L3DEX2) {
        frameSetFill(pFrame, false);
    } else {
        frameSetFill(pFrame, true);
    }
    return true;
}

static bool rspFindUCode(Rsp* pRSP, RspTask* pTask) {
    s32 nCountVertex;
    RspUCode* pUCode;
    RspUCodeType eType;
    void* pListNode;
    s32 nOffsetCode;
    s32 nOffsetData;
    u64 nFUData;
    u64* pFUData;
    u64* pFUCode;
    u64 nCheckSum;
    u32 nLengthData;
    unsigned int i;
    u32 nLengthCode;
    char aBigBuffer[4096];
    char acUCodeName[64];
    char temp_r22;
    char temp_r15;

    nOffsetCode = pTask->nOffsetCode & 0x7FFFFF;
    nOffsetData = pTask->nOffsetData & 0x7FFFFF;
    pListNode = pRSP->pListUCode->pNodeHead;
    nCheckSum = 0;

    while (pListNode != NULL) {
        pUCode = (RspUCode*)NODE_DATA(pListNode);
        if (pUCode->nOffsetCode == nOffsetCode && pUCode->nOffsetData == nOffsetData) {
            pRSP->eTypeUCode = pUCode->eType;
            pRSP->nCountVertex = pUCode->nCountVertex;
            rspSetupUCode(pRSP);
            return true;
        }
        pListNode = NODE_NEXT(pListNode);
    }

    nLengthData = pTask->nLengthData;
    nLengthCode = pTask->nLengthCode;
    if (!ramGetBuffer(SYSTEM_RAM(RSP_HOST(pRSP)), (void**)&pFUData, nOffsetData, NULL)) {
        return false;
    }
    if (!ramGetBuffer(SYSTEM_RAM(RSP_HOST(pRSP)), (void**)&pFUCode, nOffsetCode, NULL)) {
        return false;
    }

    eType = RUT_NONE;
    for (i = 0; i < (nLengthCode >> 3); i++) {
        nCheckSum += pFUCode[i];
    }

    for (i = 0; i < (nLengthData >> 3); i++) {
        nFUData = pFUData[i];
        aBigBuffer[8 * i + 0] = (nFUData >> 56) & 0xFF;
        aBigBuffer[8 * i + 1] = (nFUData >> 48) & 0xFF;
        aBigBuffer[8 * i + 2] = (nFUData >> 40) & 0xFF;
        aBigBuffer[8 * i + 3] = (nFUData >> 32) & 0xFF;
        aBigBuffer[8 * i + 4] = (nFUData >> 24) & 0xFF;
        aBigBuffer[8 * i + 5] = (nFUData >> 16) & 0xFF;
        aBigBuffer[8 * i + 6] = (nFUData >> 8) & 0xFF;
        aBigBuffer[8 * i + 7] = (nFUData >> 0) & 0xFF;

        if (((nFUData >> 32) & 0xFFFFFFFF) != 'RSP ') {
            continue;
        }

        if (((nFUData >> 8) & 0xFFFFFF) == '\0Gfx') {
            nFUData = pFUData[i + 1];
            if ((nFUData & 0xFFFF) == 'F3') {
                nFUData = pFUData[i + 2];
                temp_r22 = (nFUData >> 48) & 0xFF;
                acUCodeName[0] = 'F';
                acUCodeName[1] = '3';
                acUCodeName[2] = (nFUData >> 56) & 0xFF;
                acUCodeName[3] = (nFUData >> 48) & 0xFF;
                acUCodeName[4] = (nFUData >> 40) & 0xFF;
                acUCodeName[5] = (nFUData >> 32) & 0xFF;

                nFUData = pFUData[i + 3];
                if (((nFUData >> 24) & 0xFF) == '0' || ((nFUData >> 24) & 0xFF) == '1') {
                    temp_r15 = (nFUData >> 24) & 0xFF;
                    acUCodeName[6] = (nFUData >> 24) & 0xFF;
                    acUCodeName[7] = (nFUData >> 16) & 0xFF;
                    acUCodeName[8] = (nFUData >> 8) & 0xFF;
                    acUCodeName[9] = (nFUData >> 0) & 0xFF;
                    acUCodeName[10] = '\0';

                    if (temp_r22 == 'Z') {
                        pRSP->nVersionUCode = 0;
                        eType = RUT_ZSORT;
                        nCountVertex = 64;
                    } else {
                        if (temp_r15 == '0') {
                            pRSP->nVersionUCode = 2;
                        } else {
                            pRSP->nVersionUCode = 0;
                        }
                        eType = RUT_F3DEX1;
                        nCountVertex = 32;
                    }
                    break;
                } else if ((nFUData & 0xFF) == '2') {
                    acUCodeName[6] = nFUData & 0xFF;

                    nFUData = pFUData[i + 4];
                    acUCodeName[7] = (nFUData >> 56) & 0xFF;
                    acUCodeName[8] = (nFUData >> 48) & 0xFF;
                    acUCodeName[9] = (nFUData >> 40) & 0xFF;
                    acUCodeName[10] = '\0';

                    if (temp_r22 == 'Z') {
                        pRSP->nVersionUCode = 4;
                        eType = RUT_F3DEX2;
                        nCountVertex = 64;
                    } else {
                        pRSP->nVersionUCode = 0;
                        eType = RUT_F3DEX2;
                        nCountVertex = 64;
                    }
                    break;
                } else {
                    continue;
                }
            } else if ((nFUData & 0xFFFF) == 'L3') {
                nFUData = pFUData[i + 2];
                acUCodeName[0] = 'L';
                acUCodeName[1] = '3';
                acUCodeName[2] = (nFUData >> 56) & 0xFF;
                acUCodeName[3] = (nFUData >> 48) & 0xFF;
                acUCodeName[4] = (nFUData >> 40) & 0xFF;
                acUCodeName[5] = (nFUData >> 32) & 0xFF;

                nFUData = pFUData[i + 3];
                if (((nFUData >> 24) & 0xFF) == '0' || ((nFUData >> 24) & 0xFF) == '1') {
                    acUCodeName[6] = (nFUData >> 24) & 0xFF;
                    acUCodeName[7] = (nFUData >> 16) & 0xFF;
                    acUCodeName[8] = (nFUData >> 8) & 0xFF;
                    acUCodeName[9] = (nFUData >> 0) & 0xFF;
                    acUCodeName[10] = '\0';

                    pRSP->nVersionUCode = 0;
                    eType = RUT_L3DEX1;
                    nCountVertex = 32;
                    break;
                } else if ((nFUData & 0xFF) == '2') {
                    acUCodeName[6] = nFUData & 0xFF;

                    nFUData = pFUData[i + 4];
                    acUCodeName[7] = (nFUData >> 56) & 0xFF;
                    acUCodeName[8] = (nFUData >> 48) & 0xFF;
                    acUCodeName[9] = (nFUData >> 40) & 0xFF;
                    acUCodeName[10] = '\0';

                    pRSP->nVersionUCode = 0;
                    eType = RUT_L3DEX2;
                    nCountVertex = 32;
                    break;
                } else if ((nFUData & 0xFF) == 'Z') {
                    nFUData = pFUData[i + 2];
                    if (((nFUData >> 32) & 0xFFFFFFFF) == 'Sort') {
                        acUCodeName[6] = (nFUData >> 8) & 0xFF;
                        acUCodeName[7] = (nFUData >> 0) & 0xFF;

                        nFUData = pFUData[i + 3];
                        acUCodeName[8] = (nFUData >> 56) & 0xFF;
                        acUCodeName[9] = (nFUData >> 48) & 0xFF;
                        acUCodeName[10] = '\0';

                        pRSP->nVersionUCode = 3;
                        eType = RUT_ZSORT;
                        nCountVertex = 64;
                        // bug? missing "break;"
                    }
                } else {
                    continue;
                }
            }
        }

        if (((nFUData >> 16) & 0xFFFF) == 'SW') {
            nFUData = pFUData[i + 2];

            acUCodeName[0] = 'F';
            acUCodeName[1] = 'a';
            acUCodeName[2] = 's';
            acUCodeName[3] = 't';
            acUCodeName[4] = '3';
            acUCodeName[5] = 'D';
            acUCodeName[6] = ' ';
            acUCodeName[7] = (nFUData >> 56) & 0xFF;
            acUCodeName[8] = (nFUData >> 48) & 0xFF;
            acUCodeName[9] = (nFUData >> 40) & 0xFF;
            acUCodeName[10] = (nFUData >> 32) & 0xFF;
            acUCodeName[11] = '\0';

            if (nCheckSum == 0x3DDCC2B9DE351A0A) {
                pRSP->nVersionUCode = 1;
            } else if (nCheckSum == 0x8B9D7CFA7270C5A4) {
                pRSP->nVersionUCode = 5;
            } else {
                pRSP->nVersionUCode = 0;
            }

            eType = RUT_FAST3D;
            nCountVertex = 32;
            break;
        } else if ((pFUData[i + 1] & 0xFFFF) == 'S2') {
            acUCodeName[0] = 'S';
            acUCodeName[1] = '2';
            acUCodeName[2] = 'D';
            acUCodeName[3] = 'E';
            acUCodeName[4] = 'X';
            acUCodeName[5] = '2';
            acUCodeName[6] = '\0';

            pRSP->nVersionUCode = 0;
            eType = RUT_S2DEX2;
            nCountVertex = 0;
            break;
        }
    }

    if (eType == RUT_NONE) {
        for (i = 0; i < nLengthData - 34; i++) {
            if (!(aBigBuffer[i + 0] == 'R' && aBigBuffer[i + 1] == 'S' && aBigBuffer[i + 2] == 'P')) {
                continue;
            }

            if (aBigBuffer[i + 4] == 'S' && aBigBuffer[i + 5] == 'W') {
                acUCodeName[0] = 'F';
                acUCodeName[1] = 'A';
                acUCodeName[2] = 'S';
                acUCodeName[3] = 'T';
                acUCodeName[4] = '3';
                acUCodeName[5] = 'D';
                acUCodeName[6] = ' ';
                acUCodeName[7] = aBigBuffer[i + 16];
                acUCodeName[8] = aBigBuffer[i + 17];
                acUCodeName[9] = aBigBuffer[i + 18];
                acUCodeName[10] = aBigBuffer[i + 19];
                acUCodeName[11] = '\0';

                if (nCheckSum == 0x3DDCC2B9DE351A0A) {
                    pRSP->nVersionUCode = 1;
                } else if (nCheckSum == 0x8B9D7CFA7270C5A4) {
                    pRSP->nVersionUCode = 5;
                } else {
                    pRSP->nVersionUCode = 1;
                }
                eType = RUT_FAST3D;
                nCountVertex = 32;
                break;
            } else if (aBigBuffer[i + 4] == 'G' && aBigBuffer[i + 5] == 'f' && aBigBuffer[i + 6] == 'x') {
                if (aBigBuffer[i + 14] == 'F' && aBigBuffer[i + 15] == '3') {
                    acUCodeName[0] = 'F';
                    acUCodeName[1] = '3';
                    acUCodeName[2] = aBigBuffer[i + 16];
                    acUCodeName[3] = aBigBuffer[i + 17];
                    acUCodeName[4] = aBigBuffer[i + 18];
                    acUCodeName[5] = aBigBuffer[i + 19];
                    acUCodeName[6] = ' ';

                    if (aBigBuffer[i + 28] == '0' || aBigBuffer[i + 28] == '1') {
                        acUCodeName[7] = aBigBuffer[i + 28];
                        acUCodeName[8] = aBigBuffer[i + 29];
                        acUCodeName[9] = aBigBuffer[i + 30];
                        acUCodeName[10] = aBigBuffer[i + 31];
                        acUCodeName[11] = '\0';

                        if (aBigBuffer[i + 17] == 'Z') {
                            pRSP->nVersionUCode = 0;
                            eType = RUT_ZSORT;
                            nCountVertex = 64;
                        } else {
                            if (aBigBuffer[i + 28] == '0') {
                                pRSP->nVersionUCode = 2;
                            } else {
                                pRSP->nVersionUCode = 0;
                            }
                            eType = RUT_F3DEX1;
                            nCountVertex = 32;
                        }
                        break;
                    } else if (aBigBuffer[i + 31] == '2') {
                        acUCodeName[7] = aBigBuffer[i + 31];
                        acUCodeName[8] = aBigBuffer[i + 32];
                        acUCodeName[9] = aBigBuffer[i + 33];
                        acUCodeName[10] = aBigBuffer[i + 34];
                        acUCodeName[11] = '\0';

                        if (aBigBuffer[i + 17] == 'Z') {
                            pRSP->nVersionUCode = 4;
                            eType = RUT_F3DEX2;
                            nCountVertex = 64;
                        } else {
                            pRSP->nVersionUCode = 0;
                            eType = RUT_F3DEX2;
                            nCountVertex = 64;
                        }
                        break;
                    }
                } else if (aBigBuffer[i + 14] == 'L' && aBigBuffer[i + 15] == '3') {
                    acUCodeName[0] = 'L';
                    acUCodeName[1] = '3';
                    acUCodeName[2] = aBigBuffer[i + 16];
                    acUCodeName[3] = aBigBuffer[i + 17];
                    acUCodeName[4] = aBigBuffer[i + 18];
                    acUCodeName[5] = ' ';

                    if (aBigBuffer[i + 28] == '0' || aBigBuffer[i + 28] == '1') {
                        acUCodeName[6] = aBigBuffer[i + 28];
                        acUCodeName[7] = aBigBuffer[i + 29];
                        acUCodeName[8] = aBigBuffer[i + 30];
                        acUCodeName[9] = aBigBuffer[i + 31];
                        acUCodeName[10] = '\0';

                        pRSP->nVersionUCode = 0;
                        eType = RUT_L3DEX1;
                        nCountVertex = 32;
                        break;
                    } else if (aBigBuffer[i + 31] == '2') {
                        acUCodeName[6] = aBigBuffer[i + 31];
                        acUCodeName[7] = aBigBuffer[i + 32];
                        acUCodeName[8] = aBigBuffer[i + 33];
                        acUCodeName[9] = aBigBuffer[i + 34];
                        acUCodeName[10] = '\0';

                        pRSP->nVersionUCode = 0;
                        eType = RUT_L3DEX2;
                        nCountVertex = 32;
                        break;
                    }
                } else if (aBigBuffer[i + 14] == 'S' && aBigBuffer[i + 15] == '2' && aBigBuffer[i + 16] == 'D' &&
                           aBigBuffer[i + 17] == 'E' && aBigBuffer[i + 18] == 'X') {
                    acUCodeName[0] = 'S';
                    acUCodeName[1] = '2';
                    acUCodeName[2] = 'D';
                    acUCodeName[3] = 'E';
                    acUCodeName[4] = 'X';
                    acUCodeName[5] = ' ';

                    if (aBigBuffer[i + 21] == '0' || aBigBuffer[i + 21] == '1') {
                        acUCodeName[6] = aBigBuffer[i + 21];
                        acUCodeName[7] = aBigBuffer[i + 22];
                        acUCodeName[8] = aBigBuffer[i + 23];
                        acUCodeName[9] = aBigBuffer[i + 24];
                        acUCodeName[10] = '\0';

                        pRSP->nVersionUCode = 0;
                        eType = RUT_S2DEX1;
                        nCountVertex = 0;
                        break;
                    } else if (aBigBuffer[i + 31] == '2') {
                        acUCodeName[6] = aBigBuffer[i + 31];
                        acUCodeName[7] = aBigBuffer[i + 32];
                        acUCodeName[8] = aBigBuffer[i + 33];
                        acUCodeName[9] = aBigBuffer[i + 34];
                        acUCodeName[10] = '\0';

                        pRSP->nVersionUCode = 0;
                        eType = RUT_S2DEX2;
                        nCountVertex = 0;
                        break;
                    }
                }

                if (aBigBuffer[i + 14] == 'Z' && aBigBuffer[i + 15] == 'S' && aBigBuffer[i + 16] == 'o' &&
                    aBigBuffer[i + 17] == 'r' && aBigBuffer[i + 18] == 't') {
                    acUCodeName[0] = 'Z';
                    acUCodeName[1] = 'S';
                    acUCodeName[2] = 'o';
                    acUCodeName[3] = 'r';
                    acUCodeName[4] = 't';
                    acUCodeName[5] = ' ';
                    acUCodeName[6] = aBigBuffer[i + 22];
                    acUCodeName[7] = aBigBuffer[i + 23];
                    acUCodeName[8] = aBigBuffer[i + 24];
                    acUCodeName[9] = aBigBuffer[i + 25];
                    acUCodeName[10] = '\0';

                    pRSP->nVersionUCode = 3;
                    eType = RUT_ZSORT;
                    nCountVertex = 64;
                    // bug? missing "break;"
                }
            }
        }
    }

    if (eType == RUT_NONE) {
        if (nCheckSum == 0x3DDCC2B9DE351A0A) {
            pRSP->nVersionUCode = 1;
        } else if (nCheckSum == 0x8B9D7CFA7270C5A4) {
            pRSP->nVersionUCode = 5;
        } else {
            pRSP->nVersionUCode = 0;
        }
        acUCodeName[0] = 'F';
        acUCodeName[1] = 'A';
        acUCodeName[2] = 'S';
        acUCodeName[3] = 'T';
        acUCodeName[4] = '3';
        acUCodeName[5] = 'D';
        acUCodeName[6] = '?';
        acUCodeName[7] = '\0';

        eType = RUT_FAST3D;
        nCountVertex = 32;
    }

    if (!xlListMakeItem(pRSP->pListUCode, (void**)&pUCode)) {
        return false;
    }

    pUCode->eType = eType;
    pUCode->nCountVertex = nCountVertex;
    pUCode->nOffsetCode = nOffsetCode;
    pUCode->nLengthCode = nLengthCode;
    pUCode->nOffsetData = nOffsetData;
    pUCode->nLengthData = nLengthData;

    pRSP->eTypeUCode = pUCode->eType;
    pRSP->nCountVertex = pUCode->nCountVertex;
    if (pRSP->nVersionUCode == 5) {
        pRSP->n2TriMult = 2;
    } else {
        pRSP->n2TriMult = 1;
    }

    strcpy(pUCode->acUCodeName, acUCodeName);
    pUCode->nUCodeCheckSum = nCheckSum;
    rspSetupUCode(pRSP);

    return true;
}

// clang-format off
#include "emulator/rspASM.c"
#include "emulator/_aspMain.c"
#include "emulator/_gspJPEG.c"
#include "emulator/_gspS2DEX.c"
#include "emulator/_gspF3DEX.c"
#include "emulator/_rspTail.c"
// clang-format on

_XL_OBJECTTYPE gClassRSP = {
    "RSP",
    sizeof(Rsp),
    NULL,
    (EventFunc)rspEvent,
};
