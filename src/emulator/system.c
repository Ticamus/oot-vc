#include "emulator/system.h"
#include "emulator/ai.h"
#include "emulator/codeRVL.h"
#include "emulator/controller.h"
#include "emulator/cpu.h"
#include "emulator/disk.h"
#include "emulator/flash.h"
#include "emulator/frame.h"
#include "emulator/helpRVL.h"
#include "emulator/library.h"
#include "emulator/mi.h"
#include "emulator/pak.h"
#include "emulator/pi.h"
#include "emulator/pif.h"
#include "emulator/ram.h"
#include "emulator/rdb.h"
#include "emulator/rdp.h"
#include "emulator/rom.h"
#include "emulator/rsp.h"
#include "emulator/si.h"
#include "emulator/soundRVL.h"
#include "emulator/sram.h"
#include "emulator/vc64_RVL.h"
#include "emulator/vi.h"
#include "emulator/video.h"
#include "emulator/xlHeap.h"
#include "emulator/storeRVL.h"
#include "macros.h"
#include "revolution/os.h"
#include "revolution/vi.h"
#include "stdlib.h"
#include "string.h"

#undef SYSTEM_PTR
#if IS_OOT || IS_MT
#define SYSTEM_PTR (gpSystem)
#elif IS_MM
#define SYSTEM_PTR (pSystem)
#endif

#if IS_MM
_XL_OBJECTTYPE gClassSystem = {
    "SYSTEM (N64)",
    sizeof(System),
    NULL,
    (EventFunc)systemEvent,
}; // size = 0x10
#endif

// clang-format off
static u32 contMap[][GCN_BTN_COUNT] = {
    // Controller Configuration No. 1
    {
        N64_BTN_A,      // GCN_BTN_A
        N64_BTN_B,      // GCN_BTN_B
        N64_BTN_UNSET,  // GCN_BTN_X
        N64_BTN_UNSET,  // GCN_BTN_Y
        N64_BTN_L,      // GCN_BTN_L
        N64_BTN_R,      // GCN_BTN_R
        N64_BTN_Z,      // GCN_BTN_Z
        N64_BTN_START,  // GCN_BTN_START
        0x08000000,     // GCN_BTN_UNK8
        0x04000000,     // GCN_BTN_UNK9
        0x02000000,     // GCN_BTN_UNK10
        0x01000000,     // GCN_BTN_UNK11
        N64_BTN_DUP,    // GCN_BTN_DPAD_UP
        N64_BTN_DDOWN,  // GCN_BTN_DPAD_DOWN
        N64_BTN_DLEFT,  // GCN_BTN_DPAD_LEFT
        N64_BTN_DRIGHT, // GCN_BTN_DPAD_RIGHT
        N64_BTN_CUP,    // GCN_BTN_CSTICK_UP
        N64_BTN_CDOWN,  // GCN_BTN_CSTICK_DOWN
        N64_BTN_CLEFT,  // GCN_BTN_CSTICK_LEFT
        N64_BTN_CRIGHT, // GCN_BTN_CSTICK_RIGHT
    },
    // Controller Configuration No. 2
    {
        N64_BTN_A,      // GCN_BTN_A
        N64_BTN_B,      // GCN_BTN_B
        N64_BTN_UNSET,  // GCN_BTN_X
        N64_BTN_UNSET,  // GCN_BTN_Y
        N64_BTN_Z,      // GCN_BTN_L
        N64_BTN_R,      // GCN_BTN_R
        N64_BTN_Z,      // GCN_BTN_Z
        N64_BTN_START,  // GCN_BTN_START
        0x08000000,     // GCN_BTN_UNK8
        0x04000000,     // GCN_BTN_UNK9
        0x02000000,     // GCN_BTN_UNK10
        0x01000000,     // GCN_BTN_UNK11
        N64_BTN_DUP,    // GCN_BTN_DPAD_UP
        N64_BTN_DDOWN,  // GCN_BTN_DPAD_DOWN
        N64_BTN_DLEFT,  // GCN_BTN_DPAD_LEFT
        N64_BTN_DRIGHT, // GCN_BTN_DPAD_RIGHT
        N64_BTN_CUP,    // GCN_BTN_CSTICK_UP
        N64_BTN_CDOWN,  // GCN_BTN_CSTICK_DOWN
        N64_BTN_CLEFT,  // GCN_BTN_CSTICK_LEFT
        N64_BTN_CRIGHT, // GCN_BTN_CSTICK_RIGHT
    },
    // Controller Configuration No. 3
    {
        N64_BTN_A,      // GCN_BTN_A
        N64_BTN_B,      // GCN_BTN_B
        N64_BTN_CRIGHT, // GCN_BTN_X
        N64_BTN_CLEFT,  // GCN_BTN_Y
        N64_BTN_Z,      // GCN_BTN_L
        N64_BTN_R,      // GCN_BTN_R
        N64_BTN_CDOWN,  // GCN_BTN_Z
        N64_BTN_START,  // GCN_BTN_START
        0x08000000,     // GCN_BTN_UNK8
        0x04000000,     // GCN_BTN_UNK9
        0x02000000,     // GCN_BTN_UNK10
        0x01000000,     // GCN_BTN_UNK11
        N64_BTN_L,      // GCN_BTN_DPAD_UP
        N64_BTN_L,      // GCN_BTN_DPAD_DOWN
        N64_BTN_L,      // GCN_BTN_DPAD_LEFT
        N64_BTN_L,      // GCN_BTN_DPAD_RIGHT
        N64_BTN_CUP,    // GCN_BTN_CSTICK_UP
        N64_BTN_CDOWN,  // GCN_BTN_CSTICK_DOWN
        N64_BTN_CLEFT,  // GCN_BTN_CSTICK_LEFT
        N64_BTN_CRIGHT, // GCN_BTN_CSTICK_RIGHT
    },
    // Controller Configuration No. 4
    {
        N64_BTN_A,      // GCN_BTN_A
        N64_BTN_B,      // GCN_BTN_B
        N64_BTN_Z,      // GCN_BTN_X
        N64_BTN_Z,      // GCN_BTN_Y
        N64_BTN_Z,      // GCN_BTN_L
        N64_BTN_R,      // GCN_BTN_R
        N64_BTN_L,      // GCN_BTN_Z
        N64_BTN_START,  // GCN_BTN_START
        0x08000000,     // GCN_BTN_UNK8
        0x04000000,     // GCN_BTN_UNK9
        0x02000000,     // GCN_BTN_UNK10
        0x01000000,     // GCN_BTN_UNK11
        N64_BTN_DUP,    // GCN_BTN_DPAD_UP
        N64_BTN_DDOWN,  // GCN_BTN_DPAD_DOWN
        N64_BTN_DLEFT,  // GCN_BTN_DPAD_LEFT
        N64_BTN_DRIGHT, // GCN_BTN_DPAD_RIGHT
        N64_BTN_CUP,    // GCN_BTN_CSTICK_UP
        N64_BTN_CDOWN,  // GCN_BTN_CSTICK_DOWN
        N64_BTN_CLEFT,  // GCN_BTN_CSTICK_LEFT
        N64_BTN_CRIGHT, // GCN_BTN_CSTICK_RIGHT
    }, 
    // Controller Configuration No. 5
    {
        N64_BTN_A,      // GCN_BTN_A
        N64_BTN_B,      // GCN_BTN_B
        N64_BTN_CDOWN,  // GCN_BTN_X
        N64_BTN_CLEFT,  // GCN_BTN_Y
        N64_BTN_Z,      // GCN_BTN_L
        N64_BTN_R,      // GCN_BTN_R
        N64_BTN_CRIGHT, // GCN_BTN_Z
        N64_BTN_START,  // GCN_BTN_START
        0x08000000,     // GCN_BTN_UNK8
        0x04000000,     // GCN_BTN_UNK9
        0x02000000,     // GCN_BTN_UNK10
        0x01000000,     // GCN_BTN_UNK11
        N64_BTN_CUP,    // GCN_BTN_DPAD_UP
        N64_BTN_CUP,    // GCN_BTN_DPAD_DOWN
        N64_BTN_CUP,    // GCN_BTN_DPAD_LEFT
        N64_BTN_CUP,    // GCN_BTN_DPAD_RIGHT
        N64_BTN_CUP,    // GCN_BTN_CSTICK_UP
        N64_BTN_CDOWN,  // GCN_BTN_CSTICK_DOWN
        N64_BTN_CLEFT,  // GCN_BTN_CSTICK_LEFT
        N64_BTN_CRIGHT, // GCN_BTN_CSTICK_RIGHT
    },
};
// clang-format on

#if IS_OOT || IS_MT
static SystemDevice gaSystemDevice[] = {
    {
        SOT_HELP,
        &gClassHelpMenu,
        0,
        {0},
    },
    {
        SOT_FRAME,
        &gClassFrame,
        0,
        {0},
    },
    {
        SOT_LIBRARY,
        &gClassLibrary,
        0,
        {0},
    },
    {
        SOT_CODE,
        &gClassCode,
        0,
        {0},
    },
    {
        SOT_AUDIO,
        &gClassAudio,
        0,
        {0},
    },
    {
        SOT_VIDEO,
        &gClassVideo,
        0,
        {0},
    },
    {
        SOT_CONTROLLER,
        &gClassController,
        0,
        {0},
    },
    {
        SOT_CPU,
        &gClassCPU,
        1,
        {{0, 0x00000000, 0xFFFFFFFF}},
    },
    {
        SOT_RAM,
        &gClassRAM,
        3,
        {{256, 0x00000000, 0x03EFFFFF}, {2, 0x03F00000, 0x03FFFFFF}, {1, 0x04700000, 0x047FFFFF}},
    },
    {
        SOT_RDP,
        &gClassRDP,
        2,
        {{0, 0x04100000, 0x041FFFFF}, {1, 0x04200000, 0x042FFFFF}},
    },
    {
        SOT_RSP,
        &gClassRSP,
        1,
        {{0, 0x04000000, 0x040FFFFF}},
    },
    {
        SOT_MI,
        &gClassMI,
        1,
        {{0, 0x04300000, 0x043FFFFF}},
    },
    {
        SOT_VI,
        &gClassVI,
        1,
        {{0, 0x04400000, 0x044FFFFF}},
    },
    {
        SOT_AI,
        &gClassAI,
        1,
        {{0, 0x04500000, 0x045FFFFF}},
    },
    {
        SOT_PI,
        &gClassPI,
        1,
        {{0, 0x04600000, 0x046FFFFF}},
    },
    {
        SOT_SI,
        &gClassSI,
        1,
        {{0, 0x04800000, 0x048FFFFF}},
    },
    {
        SOT_RDB,
        &gClassRdb,
        1,
        {{0, 0x04900000, 0x0490FFFF}},
    },
    {
        SOT_DISK,
        &gClassDD,
        2,
        {{0, 0x05000000, 0x05FFFFFF}, {1, 0x06000000, 0x06FFFFFF}},
    },
    {
        SOT_ROM,
        &gClassROM,
        2,
        {{0, 0x10000000, 0x1FBFFFFF}, {1, 0x1FF00000, 0x1FF0FFFF}},
    },
    {
        SOT_PIF,
        &gClassPIF,
        1,
        {{0, 0x1FC00000, 0x1FC007FF}},
    },
    {
        SOT_NONE,
        NULL,
        0,
        {0},
    },
};

// used by Diddy Kong Racing and Paper Mario in systemSetupGameALL
u32 lbl_8016FEA0[] = {
    0x3C1A8007, 0x275ACEC0, 0x03400008, 0x00000000, 0x3C010010, 0x012A4824, 0x01214823, 0x3C01A460, 0xAC290000,
    0x3C08A460, 0x8D080010, 0x31080002, 0x5500FFFD, 0x3C08A460, 0x24081000, 0x010B4020, 0x010A4024, 0x3C01A460,
    0xAC280004, 0x3C0A0010, 0x254A0003, 0x3C01A460, 0xAC2A000C, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3C1A8007, 0x275ACEC0, 0x03400008, 0x00000000,
    0x8D6B0010, 0x316B0001, 0x1560FFF0, 0x00000000, 0x3C0BB000, 0x8D640008, 0x3C010010, 0x02C02825, 0x00812023,
    0x3C016C07, 0x34218965, 0x00A10019, 0x27BDFFE0, 0xAFBF001C, 0xAFB00014, 0x3C1F0010, 0x00001825, 0x00004025,
    0x00804825, 0x240D0020, 0x00001012, 0x24420001, 0x00403825, 0x00405025, 0x00405825, 0x00408025, 0x00403025,
    0x00406025, 0x3C1A8007, 0x275ACEC0, 0x03400008, 0x00000000, 0x00602825, 0x254A0001, 0x3043001F, 0x01A37823,
    0x01E2C006, 0x00627004, 0x01D82025, 0x00C2082B, 0x00A03825, 0x01625826, 0x10200004, 0x02048021, 0x00E2C826,
    0x10000002, 0x03263026, 0x00C43026, 0x25080004, 0x00507826, 0x25290004, 0x151FFFE8, 0x01EC6021, 0x00EA7026,
    0x01CB3821, 0x0206C026, 0x030C8021, 0x3C0BB000, 0x8D680010, 0x14E80006, 0x08018B0A, 0x00000000, 0xAFBAFFF0,
    0x3C1A8006, 0x04110003, 0x00000000, 0x0411FFFF, 0x00000000, 0x3C09A408, 0x8D290000, 0x8FB00014, 0x8FBF001C,
    0x11200006, 0x27BD0020, 0x240A0041, 0x3C01A404, 0xAC2A0010, 0x3C01A408, 0xAC200000, 0x3C0B00AA, 0x356BAAAE,
    0x3C01A404, 0xAC2B0010, 0x3C01A430, 0x24080555, 0xAC28000C, 0x3C01A480, 0xAC200018, 0x3C01A450, 0xAC20000C,
    0x3C01A430, 0x24090800, 0xAC290000, 0x24090002, 0x3C01A460, 0xAC290010, 0x3C08A000, 0x35080300, 0x240917D7,
    0xAD090010, 0xAD140000, 0xAD130004, 0xAD15000C, 0x12600004, 0xAD170014, 0x3C09A600, 0x10000003, 0x25290000,
    0x3C09B000, 0x25290000, 0xAD090008, 0x3C08A400, 0x25080000, 0x21091000, 0x240AFFFF, 0x25080004, 0x1509FFFE,
    0xAD0AFFFC, 0x3C08A400, 0x25081000, 0x21091000, 0x25080004, 0x1509FFFE, 0xAD0AFFFC, 0x3C0AA400, 0x240B17D7,
    0xAD4B1000, 0x3C0BB000, 0x254A1000, 0x8D690008, 0x3C010010, 0x01214823, 0x01200008, 0x00000000, 0xFFFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
};
#endif

#if IS_OOT || IS_MT
static SystemRomConfig gSystemRomConfigurationList;
#elif IS_MM
static SystemRomConfig gSystemRomConfigurationList[1];
#endif

static inline void systemSetControllerConfiguration(SystemRomConfig* pRomConfig, s32 controllerConfig1,
                                                    s32 controllerConfig2, bool bSetControllerConfig,
                                                    bool bSetRumbleConfig) {
    s32 iConfigList;

    if (bSetRumbleConfig) {
        pRomConfig->rumbleConfiguration = 0;
    }

    for (iConfigList = 0; iConfigList < 4; iConfigList++) {
#if IS_MM
        ((bool (*)(u32*, u32*))simulatorCopyControllerMap)(
            (u32*)pRomConfig->controllerConfiguration[iConfigList],
            contMap[((controllerConfig1 >> (iConfigList * 8)) & 0x7F)]);
#else
        simulatorCopyControllerMap(SYSTEM_CONTROLLER(SYSTEM_PTR),
                                   (u32*)pRomConfig->controllerConfiguration[iConfigList],
                                   contMap[((controllerConfig1 >> (iConfigList * 8)) & 0x7F)]);
#endif
        pRomConfig->rumbleConfiguration |= (1 << (iConfigList * 8)) & (controllerConfig1 >> 7);
    }

    if (bSetControllerConfig) {
        pRomConfig->normalControllerConfig = controllerConfig2;
        pRomConfig->currentControllerConfig = controllerConfig2;
    }
}

extern _XL_OBJECTTYPE gClassEEPROM;
#if IS_MM

// Zelda Collection (Ura Zelda / Master Quest) ROM codes
static char szCodeCZLJ[8] = "CZLJ";
static char szCodeCZLE[8] = "CZLE";
// Conker's Bad Fur Day (US)
static char szCodeNFUE[8] = "NFUE";

extern s32 lbl_802006B0;

static bool systemSetupGameRAM(System* pSystem) {
    char* szExtra;
    bool bExpansion;
    s32 nSizeRAM;
    s32 nSizeCacheROM;
    s32 nSizeExtra;
    Rom* pROM;
    u32 nCode;
    u32 iCode;
    u32 anCode[0x100]; // size = 0x400

    bExpansion = false;
    pROM = SYSTEM_ROM(pSystem);

    if (!romCopy(pROM, anCode, 0x1000, sizeof(anCode), NULL)) {
        return false;
    }

    nCode = 0;
    for (iCode = 0; iCode < ARRAY_COUNT(anCode); iCode++) {
        nCode += anCode[iCode];
    }

    // Ocarina of Time or Majora's Mask
    if (gpSystem->eTypeROM == NZSJ || gpSystem->eTypeROM == NZSE || gpSystem->eTypeROM == NZSP) {
        bExpansion = true;
    }

    if (romTestCode(pROM, szCodeCZLJ) || romTestCode(pROM, szCodeCZLE) || gpSystem->eTypeROM == NZSJ ||
        gpSystem->eTypeROM == NZSE || gpSystem->eTypeROM == NZSP) {
        switch (nCode) {
            case 0x5CAC1CF7:
                lbl_802006B0 = 2;
                break;
            case 0x184CED80:
                lbl_802006B0 = 3;
                break;
            case 0x5CAC1C27:
                lbl_802006B0 = 0;
                break;
            case 0x5CAC1C8F:
                lbl_802006B0 = romTestCode(pROM, szCodeCZLE) ? 2 : 0;
                break;
            case 0x184CED18:
                lbl_802006B0 = 1;
                break;
            case 0x54A8645A:
            case 0x421E812A:
            case 0x54A59B56:
            case 0x421EB8E9:
                lbl_802006B0 = 4;
                break;
            case 0x7E8BEE60:
                lbl_802006B0 = 5;
                break;
        }

        if (lbl_802006B0 & 1) {
            bExpansion = true;
        }
    }

    // Conker's Bad Fur Day
    if (romTestCode(pROM, szCodeNFUE)) {
        bExpansion = true;
    }

    if (bExpansion) {
        nSizeRAM = 0x800000;
        nSizeCacheROM = 0x400000;
    } else {
        nSizeRAM = 0x400000;
        nSizeCacheROM = 0x800000;
    }

    if (simulatorGetArgument(SAT_CONTROLLER, &szExtra)) {
        nSizeExtra = atoi(szExtra) << 20;

        if (nSizeExtra > nSizeCacheROM - 0x100000) {
            nSizeExtra = nSizeCacheROM - 0x100000;
        }

        nSizeRAM += nSizeExtra;
    }

    if (!ramSetSize(SYSTEM_RAM(pSystem), nSizeRAM)) {
        return false;
    }

    return true;
}

bool systemGetInitialConfiguration(System* pSystem, Rom* pROM, s32 iConfig) {
    char* szArgument;

    if (!romGetCode(pROM, gSystemRomConfigurationList[iConfig].szCodeROM)) {
        return false;
    }

    systemSetControllerConfiguration(&gSystemRomConfigurationList[iConfig], 0, 0, false, true);
    gSystemRomConfigurationList[iConfig].storageDevice = 0;

    // Ocarina of Time or Majora's Mask
    if (gpSystem->eTypeROM == NZSJ || gpSystem->eTypeROM == NZSE || gpSystem->eTypeROM == NZSP) {
        gSystemRomConfigurationList[iConfig].storageDevice = 2;

        if (!simulatorGetArgument(SAT_VIBRATION, &szArgument) || *szArgument == '1') {
            if (!simulatorGetArgument(SAT_RESET, &szArgument) || *szArgument == '0') {
                systemSetControllerConfiguration(&gSystemRomConfigurationList[iConfig], 0x82828282, 0x82828282, true,
                                                 true);
            } else {
                systemSetControllerConfiguration(&gSystemRomConfigurationList[iConfig], 0x80808080, 0x80808080, true,
                                                 true);
            }
        } else {
            if (!simulatorGetArgument(SAT_RESET, &szArgument) || *szArgument == '0') {
                systemSetControllerConfiguration(&gSystemRomConfigurationList[iConfig], 0x02020202, 0x02020202, true,
                                                 true);
            } else {
                systemSetControllerConfiguration(&gSystemRomConfigurationList[iConfig], 0, 0, true, true);
            }
        }
    }

    return true;
}

#endif


#if IS_MM
bool systemSetStorageDevice(System* pSystem, SystemObjectType eStorageDevice, void* pArgument, s32 param4)
#else
bool systemSetStorageDevice(System* pSystem, SystemObjectType eStorageDevice, void* pArgument)
#endif
{
    switch (eStorageDevice) {
#if IS_OOT || IS_MT
        case SOT_PAK:
            if (!xlObjectMake(&pSystem->apObject[SOT_PAK], pArgument, &gClassPak)) {
                return false;
            }

            if (!cpuMapObject(SYSTEM_CPU(gpSystem), pSystem->apObject[SOT_PAK], 0x08000000, 0x0801FFFF, 0)) {
                return false;
            }
            break;
#endif
        case SOT_SRAM:
            if (!xlObjectMake(&pSystem->apObject[SOT_SRAM], pArgument, &gClassSram)) {
                return false;
            }

#if IS_MM
            if (SYSTEM_SRAM(pSystem)->pStore != NULL) {
                SYSTEM_SRAM(pSystem)->pStore->unk_04 = param4;
            }
#endif

            if (!cpuMapObject(SYSTEM_CPU(SYSTEM_PTR), pSystem->apObject[SOT_SRAM], 0x08000000, 0x08007FFF, 0)) {
                return false;
            }
            break;
        case SOT_FLASH:
            if (!xlObjectMake(&pSystem->apObject[SOT_FLASH], pArgument, &gClassFlash)) {
                return false;
            }

#if IS_MM
            if (SYSTEM_FLASH(pSystem)->pStore != NULL) {
                SYSTEM_FLASH(pSystem)->pStore->unk_04 = param4;
            }
#endif

            if (!cpuMapObject(SYSTEM_CPU(SYSTEM_PTR), pSystem->apObject[SOT_FLASH], 0x08000000, 0x0801FFFF, 0)) {
                return false;
            }
            break;
#if IS_MM
        //! TODO: SOT_EEPROM and related headers?
        case SOT_PAK:
            if (!xlObjectMake(&pSystem->apObject[SOT_PAK], pArgument, &gClassEEPROM)) {
                return false;
            }

            if (SYSTEM_PAK(pSystem)->pStore != NULL) {
                SYSTEM_PAK(pSystem)->pStore->unk_04 = param4;
            }
            break;
#endif
        default:
            return false;
    }

    return true;
}

#if IS_OOT
bool systemCreateStorageDevice(System* pSystem, void* pArgument) {
    SystemDevice* pDevice;
    SystemDeviceInfo* pInfo;
    s32 i;
    s32 nSlotUsed;
    SystemObjectType storageDevice;
    void** ppObject;
    s32 iDevice;

    for (i = 0; i < ARRAY_COUNT(pSystem->apObject); i++) {
        pSystem->apObject[i] = NULL;
    }

    iDevice = 0;

    while ((pDevice = &gaSystemDevice[iDevice], storageDevice = pDevice->storageDevice, storageDevice) != SOT_NONE) {
        ppObject = &pSystem->apObject[storageDevice];

        if (pSystem->apObject[storageDevice] == NULL) {
            if (!xlObjectMake(ppObject, pArgument, pDevice->pClass)) {
                return false;
            }
        } else {
            return false;
        }

        nSlotUsed = pDevice->nSlotUsed;

        if (nSlotUsed > 0) {
            for (i = 0; i < nSlotUsed; i++) {
                pInfo = &pDevice->aDeviceSlot[i];

                if (storageDevice == SOT_CPU) {
                    if (!cpuMapObject(SYSTEM_CPU(pSystem), pSystem, pInfo->nAddress0, pInfo->nAddress1, pInfo->nType)) {
                        return false;
                    }
                } else {
                    if (!cpuMapObject(SYSTEM_CPU(pSystem), *ppObject, pInfo->nAddress0, pInfo->nAddress1,
                                      pInfo->nType)) {
                        return false;
                    }
                }
            }
        }

        iDevice++;
    }

    return true;
}
#endif

#if IS_OOT || IS_MT
static bool systemSetupGameRAM(System* pSystem) {
    char* szExtra;
    bool bExpansion;
    s32 nSizeRAM;
    s32 nSizeCacheROM;
    s32 nSizeExtra;
    Rom* pROM;
    u32 nCode;
    u32 iCode;
    u32 anCode[0x100]; // size = 0x400

    bExpansion = false;
    pROM = SYSTEM_ROM(pSystem);

    if (!romCopy(SYSTEM_ROM(gpSystem), anCode, 0x1000, sizeof(anCode), NULL)) {
        return false;
    }

    nCode = 0;
    for (iCode = 0; iCode < ARRAY_COUNT(anCode); iCode++) {
        nCode += anCode[iCode];
    }

    // Ocarina of Time or Majora's Mask
    if (pSystem->eTypeROM == NZSJ || pSystem->eTypeROM == NZSE || pSystem->eTypeROM == NZSP) {
        bExpansion = true;
    } else if (nCode == 0x184CED80 || nCode == 0x184CED18 || nCode == 0x7E8BEE60) {
        bExpansion = true;
    }

    // Conker's Bad Fur Day
    if (pSystem->eTypeROM == NFUJ || pSystem->eTypeROM == NFUE || pSystem->eTypeROM == NFUP) {
        bExpansion = true;
    }

    if (bExpansion) {
        nSizeRAM = 0x800000;
        nSizeCacheROM = 0x400000;
    } else {
        nSizeRAM = 0x400000;
        nSizeCacheROM = 0x800000;
    }

    if (!ramSetSize(SYSTEM_RAM(gpSystem), nSizeRAM)) {
        return false;
    }

    if (!ramWipe(SYSTEM_RAM(gpSystem))) {
        return false;
    }

    return true;
}
#endif

#if IS_OOT || IS_MT
static inline void systemSetupGameALL_Inline(void) {
    s32 iController;

    for (iController = 0; iController < 4; iController++) {
        simulatorCopyControllerMap(SYSTEM_CONTROLLER(gpSystem),
                                   (u32*)&gSystemRomConfigurationList.controllerConfiguration[iController],
                                   (u32*)&contMap[0]);
    }
}
#endif

#if IS_OOT || IS_MT
static bool systemSetupGameALL(System* pSystem) {
    char* szArgument;
    s32* pBuffer2;
    s32* pBuffer;
    int i;
    s32 iController;
    s32 nSizeSound;
    u32 pArgument;
    s32 var_r28;
    s64 var_r27;
    s32 var_r26;
    SystemObjectType storageDevice;
    Rom* pROM;
    Cpu* pCPU;
    Pif* pPIF;

    pCPU = SYSTEM_CPU(gpSystem);
    pROM = SYSTEM_ROM(gpSystem);
    pPIF = SYSTEM_PIF(gpSystem);

    pArgument = 0;
    storageDevice = SOT_NONE;
    var_r26 = 0xFF;
    var_r27 = 0xFF;
    var_r28 = 0xFF;
    nSizeSound = 0x2000;

    gSystemRomConfigurationList.rumbleConfiguration = pArgument;

    systemSetupGameALL_Inline();

    if (!romGetCode(pROM, (s32*)&pSystem->eTypeROM)) {
        return false;
    }

    switch (pSystem->eTypeROM) {
        case NSMJ:
        case NSME:
        case NSMP:
            gSystemRomConfigurationList.storageDevice = SOT_RSP;
            nSizeSound = 0x2000;
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            var_r28 = 0xBE;
            var_r27 = 0xBE;
            var_r26 = 0xBE;
            if (!cpuSetCodeHack(pCPU, 0x80317938, 0x5420FFFE, 0)) {
                return false;
            }
            if (pSystem->eTypeROM == NSMJ) {
                systemSetControllerConfiguration(&gSystemRomConfigurationList, 0x81818181, 0x81818181, true, true);
                if (!cpuSetCodeHack(pCPU, 0x802F2458, 0x83250002, -1)) {
                    return false;
                }
            } else {
                systemSetControllerConfiguration(&gSystemRomConfigurationList, 0x01010101, 0x01010101, true, true);
            }
            break;
        case NKTJ:
        case NKTE:
        case NKTP:
            gSystemRomConfigurationList.storageDevice = SOT_RSP;
            gSystemRomConfigurationList.rumbleConfiguration = 0;
            var_r28 = 0xBE;
            var_r27 = 0xBE;
            var_r26 = 0xBE;
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            systemSetControllerConfiguration(&gSystemRomConfigurationList, 0x03030303, 0x83838383, true, false);
            if (pSystem->eTypeROM == NKTJ) {
                if (!cpuSetCodeHack(pCPU, 0x802A4118, 0x3C068015, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x800729D4, 0x27BDFFD8, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8003FBC4, 0x20A50001, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8003FBD4, 0x00084040, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8003FC68, 0x20A50001, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8003FC74, 0x00084040, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x800987E8, 0x25AD8008, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80098888, 0x3C00E700, -1)) {
                    return false;
                }
            } else if (pSystem->eTypeROM == NKTP) {
                if (!cpuSetCodeHack(pCPU, 0x802A4160, 0x3C068015, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80072E34, 0x27BDFFD8, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80040054, 0x20A50001, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80040094, 0x00084040, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x800400F8, 0x20A50001, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80040134, 0x00084040, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80098F04, 0x25AD8008, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80098FA4, 0x3C0DE700, -1)) {
                    return false;
                }
            } else if (pSystem->eTypeROM == NKTE) {
                if (!cpuSetCodeHack(pCPU, 0x802A4160, 0x3C068015, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80072E54, 0x27BDFFD8, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80040074, 0x20A50001, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x800400B4, 0x00084040, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80040118, 0x20A50001, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80040154, 0x00084040, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80098F04, 0x25AD8008, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80098FA4, 0x3C0DE700, -1)) {
                    return false;
                }
            }
            pCPU->nCompileFlag |= 0x110;
            break;
        case NZLP:
        case CZLJ:
        case CZLE:
            pArgument = 0x8000;
            nSizeSound = 0x1000;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D9;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer, 0, NULL)) {
                return false;
            }
            pBuffer[0xBA] = 0xC86E2000;
            pBuffer[0xBEC7D] = 0xAD090010;
            pBuffer[0xBF870] = 0xAD170014;
            storageDevice = SOT_SRAM;
            gSystemRomConfigurationList.storageDevice = SOT_PIF;
            systemSetControllerConfiguration(&gSystemRomConfigurationList, 0x02020202, 0x02020202, true, true);
            if (pSystem->eTypeROM == CZLE) {
                if (!cpuSetCodeHack(pCPU, 0x80062D64, 0x94639680, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8006E468, 0x97040000, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8005BB14, 0x9463D040, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80066638, 0x97040000, -1)) {
                    return false;
                }
            } else if (pSystem->eTypeROM == CZLJ) {
                if (!cpuSetCodeHack(pCPU, 0x80062D64, 0x94639680, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8006E468, 0x97040000, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8005BB34, 0x9463D040, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x80066658, 0x97040000, -1)) {
                    return false;
                }
            } else if (pSystem->eTypeROM == NZLP) {
                if (!cpuSetCodeHack(pCPU, 0x80062D64, 0x94639680, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8006E468, 0x97040000, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8005BB3C, 0x9502000C, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x800665E8, 0x97040000, -1)) {
                    return false;
                }
            }
            pCPU->nCompileFlag |= 0x110;
            break;
        case NZSE:
        case NZSP:
        case NZSJ:
            nSizeSound = 0x1000;
            storageDevice = SOT_FLASH;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D9;
            gSystemRomConfigurationList.storageDevice = SOT_RAM;
            if (!simulatorGetArgument(SAT_RESET, &szArgument) || *szArgument == '1') {
                if (!simulatorGetArgument(SAT_CONTROLLER, &szArgument) || *szArgument == '0') {
                    systemSetControllerConfiguration(&gSystemRomConfigurationList, 0x82828282, 0x82828282, true, true);
                } else {
                    systemSetControllerConfiguration(&gSystemRomConfigurationList, 0x80808080, 0x80808080, true, true);
                }
            } else {
                if (!simulatorGetArgument(SAT_CONTROLLER, &szArgument) || *szArgument == '0') {
                    systemSetControllerConfiguration(&gSystemRomConfigurationList, 0x02020202, 0x02020202, true, true);
                } else {
                    systemSetControllerConfiguration(&gSystemRomConfigurationList, 0, 0, true, true);
                }
            }
            if (!cpuSetCodeHack(pCPU, 0x801C6FC0, 0x95630000, -1)) {
                return false;
            }
            if (pSystem->eTypeROM == NZSJ) {
                if (!cpuSetCodeHack(pCPU, 0x80177CF4, 0x95630000, -1)) {
                    return false;
                }
            } else if (pSystem->eTypeROM == NZSE) {
                if (!cpuSetCodeHack(pCPU, 0x80177D34, 0x95630000, -1)) {
                    return false;
                }
            } else {
                if (!cpuSetCodeHack(pCPU, 0x801786B4, 0x95630000, -1)) {
                    return false;
                }
            }
            pCPU->nCompileFlag |= 0x1010;
            break;
        case NFXJ:
        case NFXP:
        case NFXE:
            gSystemRomConfigurationList.storageDevice = SOT_RSP;
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            systemSetControllerConfiguration(&gSystemRomConfigurationList, 0x84848484, 0x84848484, true, true);
            if (pSystem->eTypeROM == NFXJ) {
                if (!cpuSetCodeHack(pCPU, 0x8019F548, 0xA2000000, 0)) {
                    return false;
                }
            } else if (pSystem->eTypeROM == NFXE) {
                if (!cpuSetCodeHack(pCPU, 0x801989D0, 0xA2000000, 0)) {
                    return false;
                }
            }
            GXSetDispCopyGamma(GX_GM_1_7);
            pCPU->nCompileFlag |= 0x110;
            break;
        case NPWE:
        case NPWP:
        case NPWJ:
            gSystemRomConfigurationList.storageDevice = SOT_RSP;
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            break;
        case NAFE:
        case NAFP:
        case NAFJ:
            storageDevice = SOT_FLASH;
            gSystemRomConfigurationList.storageDevice = SOT_RAM;
            break;
        case NBCJ:
        case NBCP:
        case NBCE:
            gSystemRomConfigurationList.storageDevice = SOT_RSP;
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D6;
            if (!cpuSetCodeHack(pCPU, 0x80244CFC, 0x1420FFFA, 0)) {
                return false;
            }
            break;
        case NBYP:
        case NBYJ:
        case NBYE:
            if (!cpuSetCodeHack(pCPU, 0x8007ADD0, 0x1440FFF9, 0)) {
                return false;
            }
            break;
        case NCUJ:
        case NCUP:
        case NCUE:
            gSystemRomConfigurationList.storageDevice = SOT_RSP;
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D6;
            if (!cpuSetCodeHack(pCPU, 0x80103E0C, 0x1616FFF2, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80111B00, 0x51E0FFFF, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80111B04, 0x8C4F0000, 0)) {
                return false;
            }
            break;
        case NDYE:
        case NDYP:
        case NDYJ:
            gSystemRomConfigurationList.storageDevice = SOT_RAM;
            storageDevice = SOT_FLASH;
            pArgument = 0x4000;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D7;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer, 0, NULL)) {
                return false;
            }
            if (!xlHeapCopy(pBuffer, lbl_8016FEA0, 0x300)) {
                return false;
            }
            if (!rspGetIMEM(SYSTEM_RSP(gpSystem), (void**)&pBuffer, 0, 4)) {
                return false;
            }
            pBuffer[0] = 0x17D7;
            if (!rspGetDMEM(SYSTEM_RSP(gpSystem), (void**)&pBuffer, 0, 4)) {
                return false;
            }
            pBuffer[0] = -1;
            break;
        case NDOE:
        case NDOP:
        case NDOJ:
            if (!cpuSetCodeHack(pCPU, 0x80000A04, 0x1462FFFF, 0)) {
                return false;
            }
            break;
        case NN6P:
        case NN6J:
        case NN6E:
            if (!cpuSetCodeHack(pCPU, 0x800005EC, 0x3C028001, -1)) {
                return false;
            }
            if (pSystem->eTypeROM == NN6J) {
                if (!cpuSetCodeHack(pCPU, 0x8006D458, 0x0C0189E9, 0x0C0189A3)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8006D664, 0x0C0189E9, 0x0C0189A3)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8006D6D0, 0x0C0189E9, 0x0C0189A3)) {
                    return false;
                }
            } else {
                if (!cpuSetCodeHack(pCPU, 0x8006D338, 0x0C0189A9, 0x0C018963)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8006D544, 0x0C0189A9, 0x0C018963)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8006D5B0, 0x0C0189A9, 0x0C018963)) {
                    return false;
                }
            }
            gSystemRomConfigurationList.storageDevice = SOT_RSP;
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            pCPU->nCompileFlag |= 0x10;
            break;
        case NSIJ:
            pArgument = 0x8000;
            gSystemRomConfigurationList.storageDevice = SOT_PIF;
            storageDevice = SOT_SRAM;
            break;
        case NFZP:
        case NFZJ:
        case CFZE:
            nSizeSound = 0x8000;
            gSystemRomConfigurationList.storageDevice = SOT_PIF;
            pArgument = 0x8000;
            storageDevice = SOT_SRAM;
            break;
        case NLRJ:
        case NLRP:
        case NLRE:
            if (!cpuSetCodeHack(pCPU, 0x80097B6C, 0x1443FFF9, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80097BF4, 0x1443FFF9, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80096D08, 0x08025B40, 0x1000FFFF)) {
                return false;
            }
            break;
        case NMFJ:
        case NMFP:
        case NMFE:
            gSystemRomConfigurationList.storageDevice = SOT_RSP;
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            if (!cpuSetCodeHack(pCPU, 0x800B2DCC, 0x8C430004, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800B2E70, 0x8C430004, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80029EB8, 0x8C4252CC, -1)) {
                return false;
            }
            break;
        case NK4E:
        case NK4P:
        case NK4J:
            if (!aiEnable(gpSystem->apObject[8], 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80020BCC, 0x8DF80034, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80020EBC, 0x8DEFF330, -1)) {
                return false;
            }
            gSystemRomConfigurationList.storageDevice = SOT_AI;
            pArgument = 0x4000;
            storageDevice = SOT_FLASH;
            pCPU->nCompileFlag |= 0x110;
            break;
        case CLBP:
        case CLBE:
        case CLBJ:
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            break;
        case NMWP:
        case NMWE:
        case NMWJ:
            gSystemRomConfigurationList.storageDevice = SOT_RSP;
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D6;
            break;
        case NMVJ:
        case NMVP:
        case NMVE:
            gSystemRomConfigurationList.storageDevice = SOT_AI;
            pArgument = 0x4000;
            storageDevice = SOT_FLASH;
            break;
        case NRIP:
        case NRIE:
        case NRIJ:
            gSystemRomConfigurationList.storageDevice = SOT_AI;
            pArgument = 0x4000;
            storageDevice = SOT_FLASH;
            break;
        case NMQJ:
        case NMQP:
        case NMQE:
            gSystemRomConfigurationList.storageDevice = SOT_RAM;
            storageDevice = SOT_FLASH;
            pArgument = 0x20000;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer, 0, NULL)) {
                return false;
            }
            if (!xlHeapCopy(pBuffer, lbl_8016FEA0, 0x300)) {
                return false;
            }
            if (!rspGetIMEM(SYSTEM_RSP(gpSystem), (void**)&pBuffer, 0, 4)) {
                return false;
            }
            pBuffer[0] = 0x17D7;
            if (!rspGetDMEM(SYSTEM_RSP(gpSystem), (void**)&pBuffer, 0, 4)) {
                return false;
            }
            pBuffer[0] = -1;
            if (pSystem->eTypeROM == NMQE) {
                if (!cpuSetCodeHack(pCPU, 0x8005E98C, 0x1040FFFF, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8005F2D8, 0x1440FFFD, -1)) {
                    return false;
                }
            } else if (pSystem->eTypeROM == NMQJ) {
                if (!cpuSetCodeHack(pCPU, 0x8005E63C, 0x1040FFFF, -1)) {
                    return false;
                }
                if (!cpuSetCodeHack(pCPU, 0x8005EF88, 0x1440FFFD, -1)) {
                    return false;
                }
            }
            break;
        case NPOE:
        case NPOP:
        case NPOJ:
            storageDevice = SOT_FLASH;
            gSystemRomConfigurationList.storageDevice = SOT_RAM;
            break;
        case NQKJ:
        case NQKP:
        case NQKE:
            if (!cpuSetCodeHack(pCPU, 0x8004989C, 0x1459FFFB, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80049FF0, 0x1608FFFB, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x8004A384, 0x15E0FFFB, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x8004A97C, 0x15E0FFFB, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80048FF8, 0x1000FFFD, 0x1000FFFF)) {
                return false;
            }
            break;
        case NGUJ:
        case NGUP:
        case NGUE:
            gSystemRomConfigurationList.storageDevice = SOT_RSP;
            pArgument = 0x1000;
            storageDevice = SOT_FLASH;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D6;
            if (!cpuSetCodeHack(pCPU, 0x80025D30, 0x3C018006, -1)) {
                return false;
            }
            break;
        case NSQP:
        case NSQJ:
        case NSQE:
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D6;
            break;
        case NOBJ:
        case NOBP:
        case NOBE:
            pArgument = 0x8000;
            gSystemRomConfigurationList.storageDevice = SOT_PIF;
            storageDevice = SOT_SRAM;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D6;
            break;
        case NRXP:
        case NRXJ:
        case NRXE:
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D6;
            break;
        case NALJ:
        case NALP:
        case NALE:
            gSystemRomConfigurationList.storageDevice = SOT_AI;
            pArgument = 0x4000;
            storageDevice = SOT_FLASH;
            if (!cpuSetCodeHack(pCPU, 0x8000092C, 0x3C028004, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x8002103C, 0x3C028004, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80021048, 0x3C028004, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800A1BB8, 0x1440FFFD, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800A1BE0, 0x1440FFFD, 0)) {
                return false;
            }
            pCPU->nCompileFlag |= 0x110;
            break;
        case NTEJ:
        case NTEP:
        case NTEA:
            pArgument = 0x8000;
            gSystemRomConfigurationList.storageDevice = SOT_PIF;
            storageDevice = SOT_SRAM;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D7;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer, 0, NULL)) {
                return false;
            }
            pBuffer[0x80] = 0xAC290000;
            pBuffer[0xA1] = 0x240B17D7;
            if (!rspGetIMEM(SYSTEM_RSP(gpSystem), (void**)&pBuffer, 0, 4)) {
                return false;
            }
            pBuffer[0] = 0x17D7;
            if (!rspGetDMEM(SYSTEM_RSP(gpSystem), (void**)&pBuffer, 0, 4)) {
                return false;
            }
            pBuffer[0] = -1;
            if (!cpuSetCodeHack(pCPU, 0x8000017C, 0x14E80006, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80000188, 0x16080003, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800F04E8, 0x1218FFFB, 0)) {
                return false;
            }
            break;
        case NYLJ:
        case NYLP:
        case NYLE:
            gSystemRomConfigurationList.storageDevice = SOT_PIF;
            storageDevice = SOT_SRAM;
            if (!cpuSetCodeHack(pCPU, 0x800A58F8, 0x8C62FF8C, -1)) {
                return false;
            }
            pCPU->nCompileFlag |= 0x10;
            break;
        case NTUE:
        case NTUP:
        case NTUJ:
            if (!cpuSetCodeHack(pCPU, 0x8002BDD0, 0xA0000000, 0)) {
                return false;
            }
            break;
        case NWRE:
        case NWRP:
        case NWRJ:
            if (!cpuSetCodeHack(pCPU, 0x8004795C, 0x1448FFFC, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80047994, 0x144AFFFC, 0)) {
                return false;
            }
            pCPU->nCompileFlag |= 0x10;
            break;
        case NYSJ:
        case NYSP:
        case NYSE:
            gSystemRomConfigurationList.storageDevice = SOT_AI;
            pArgument = 0x4000;
            storageDevice = SOT_FLASH;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
                return false;
            }
            pBuffer2[4] = 0x17D8;
            if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer, 0, NULL)) {
                return false;
            }
            pBuffer[0x59] = 0x01EC6021;
            pBuffer[0xAE] = 0x8941680C;
            if (!rspGetIMEM(SYSTEM_RSP(gpSystem), (void**)&pBuffer, 0, 4)) {
                return false;
            }
            pBuffer[0] = 0x17D8;
            if (!rspGetDMEM(SYSTEM_RSP(gpSystem), (void**)&pBuffer, 0, 4)) {
                return false;
            }
            pBuffer[0] = -1;
            break;
        case NBNP:
        case NBNE:
        case NBNJ:
            gSystemRomConfigurationList.storageDevice = SOT_AI;
            pArgument = 0x4000;
            storageDevice = SOT_FLASH;
            if (!cpuSetCodeHack(pCPU, 0x80000548, 0x08000156, 0x1000FFFF)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x80000730, 0x3C02800C, -1)) {
                return false;
            }
            break;
        case NRBJ:
        case NRBP:
        case NRBE:
            gSystemRomConfigurationList.storageDevice = SOT_AI;
            pArgument = 0x4000;
            storageDevice = SOT_FLASH;
            if (!cpuSetCodeHack(pCPU, 0x80066884, 0x8C62FF8C, 0xFFFFFFFF)) {
                return false;
            }
            pCPU->nCompileFlag |= 0x110;
            break;
    }

    if (storageDevice != SOT_NONE && !systemSetStorageDevice(pSystem, storageDevice, (void*)pArgument)) {
        return false;
    }

    if (!fn_8005329C(SYSTEM_FRAME(gpSystem), var_r28, var_r27, var_r26)) {
        return false;
    }

    if (!soundSetBufferSize(SYSTEM_SOUND(gpSystem), nSizeSound)) {
        return false;
    }

    systemSetControllerConfiguration(&gSystemRomConfigurationList, gSystemRomConfigurationList.currentControllerConfig,
                                     gSystemRomConfigurationList.currentControllerConfig, false, true);

    for (iController = 0; iController < 4; iController++) {
        simulatorSetControllerMap(SYSTEM_CONTROLLER(gpSystem), iController,
                                  (u32*)&gSystemRomConfigurationList.controllerConfiguration[iController]);

        if (gSystemRomConfigurationList.storageDevice & 0x10) {
            if (!pifSetControllerType(pPIF, iController, CT_CONTROLLER_W_PAK)) {
                return false;
            }
        } else if (gSystemRomConfigurationList.rumbleConfiguration & (1 << (iController << 3))) {
            if (!pifSetControllerType(pPIF, iController, CT_CONTROLLER_W_RPAK)) {
                return false;
            }
        } else {
            if (!pifSetControllerType(pPIF, iController, CT_CONTROLLER)) {
                return false;
            }
        }
    }

    return true;
}
#elif IS_MM
extern s32 lbl_801FF810;
extern f32 lbl_801FF814;
extern const f32 lbl_80201504;
extern const f32 lbl_80201508;
extern s32 lbl_802006B0;
extern u8 lbl_8014E550[0x300];

static bool systemSetupGameALL(System* pSystem) {
    s32 nSize;
    s32* pBuffer2;
    s32* pBuffer;

    s32 nSizeSound;
    s32 iController;
    s32 i;
    u64 nTimeRetrace;
    char acCode[5];
    Cpu* pCPU;
    Rom* pROM;
    Sound* pSound;
    s32 defaultConfiguration;

    SystemObjectType var_r25;
    s32 var_r24;
    u32 var_r23;
    u32 nClockSpeed;
    s32 nControllerConfig;

    pSystem->storageDevice = SOT_SRAM;
    pCPU = SYSTEM_CPU(pSystem);
    var_r25 = SOT_NONE;
    pROM = SYSTEM_ROM(pSystem);
    var_r24 = 0;
    pSound = SYSTEM_SOUND(pSystem);
    var_r23 = 0x17D7;

    if (!ramGetBuffer(SYSTEM_RAM(pSystem), (void**)&pBuffer2, 0x300, NULL)) {
        return false;
    }

    if (gpSystem->eTypeROM == NZSP) {
        ((u32*)pBuffer2)[0] = 0;
    } else {
        ((u32*)pBuffer2)[0] = 1;
    }
    ((u32*)pBuffer2)[1] = 0;
    ((u32*)pBuffer2)[2] = 0xB0000000;
    ((u32*)pBuffer2)[3] = 0;
    ((u32*)pBuffer2)[4] = 0x17D7;
    ((u32*)pBuffer2)[5] = 1;

    nTimeRetrace = OSSecondsToTicks(1.0f / 60.0f);

    if (!ramGetSize(SYSTEM_RAM(pSystem), &nSize)) {
        return false;
    }

    ((u32*)pBuffer2)[6] = nSize;
    systemGetInitialConfiguration(pSystem, pROM, 0);

    pSystem->unk_94 = 1;

    if (gSystemRomConfigurationList[0].storageDevice & 1) {
        var_r25 = SOT_SRAM;
        var_r24 = 0x8000;
    } else if (gSystemRomConfigurationList[0].storageDevice & 2) {
        var_r25 = SOT_FLASH;
        var_r24 = 0x40000;
    } else if (gSystemRomConfigurationList[0].storageDevice & 4) {
        var_r25 = SOT_PAK;
        var_r24 = 0x200;
    } else if (gSystemRomConfigurationList[0].storageDevice & 8) {
        var_r25 = SOT_PAK;
        var_r24 = 0x800;
    }

    if (var_r25 != SOT_NONE && !systemSetStorageDevice(pSystem, var_r25, (void*)var_r24, pSystem->unk_94)) {
        return false;
    }

    if (gpSystem->eTypeROM == NZSJ || gpSystem->eTypeROM == NZSE || gpSystem->eTypeROM == NZSP) {
        Frame* pFrame = SYSTEM_FRAME(gpSystem); // temp_r5
        Rsp* pRSP = SYSTEM_RSP(gpSystem); // temp_r23

        pSystem->storageDevice = SOT_ROM;

        pRSP->nMode = 0;
        pRSP->unk_59D4 = 1;

        nClockSpeed = OS_TIME_SPEED;
        defaultConfiguration = nClockSpeed / 250 & ~3;
        nSizeSound = (nClockSpeed / 125 & ~7) - nClockSpeed / 1000;

        pRSP->unk_59DC = defaultConfiguration;
        pRSP->unk_59D8 = 0;
        pRSP->unk_59E4 = nSizeSound;
        pRSP->unk_59E0 = 0;
        pRSP->unk_59FC = nSizeSound;
        pRSP->unk_59F8 = 0;
        pRSP->unk_59EC = defaultConfiguration;
        pRSP->unk_59E8 = 0;
        pRSP->unk_59F4 = nClockSpeed / 1000;
        pRSP->unk_59F0 = 0;

        pFrame->unk_55928 = lbl_80201504;
        pSound->unk_53C = 1;
        pSound->unk_534 = 6;

        lbl_801FF810 = 2;
        lbl_801FF814 = lbl_80201508;

        if (!ramGetBuffer(SYSTEM_RAM(pSystem), (void**)&pBuffer2, 0x300, NULL)) {
            return false;
        }
        pBuffer2[4] = 0x17D9;
        var_r23 = 0x17D9;

        if (lbl_802006B0 & 1) {
            if (!cpuSetCodeHack(pCPU, 0x801C6FC0, 0x95630000, -1)) {
                return false;
            }
        } else if (gpSystem->eTypeROM == NZSJ) {
            if (!cpuSetCodeHack(pCPU, 0x80173FF0, 0x95630000, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BE86C, 0x860C0000, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BE870, 0x860D0004, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BE8F4, 0x86180000, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BE908, 0x86190004, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BE91C, 0x86080002, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BE934, 0x8609FFFA, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BE948, 0x860AFFFE, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BE97C, 0x844EFFFA, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BE990, 0x844FFFFE, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BEA08, 0x860A0006, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BEA1C, 0x860B000A, -1)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BFB14, 0x0C025414, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BFB2C, 0x0C02335C, 0)) {
                return false;
            }
            if (!cpuSetCodeHack(pCPU, 0x800BFB34, 0x8FB9004C, 0x24190000)) {
                return false;
            }
        }

        pCPU->nCompileFlag |= 0x1010;
        fn_800818F0(SYSTEM_CONTROLLER(gpSystem), 1);
    } else if (!romGetCode(pROM, acCode)) {
        return false;
    }

    if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer2, 0x300, NULL)) {
        return false;
    }

    pBuffer2[4] = var_r23;

    if (var_r23 == 0x17D7) {
        if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer, 0, 0)) {
            return false;
        }
        if (!xlHeapCopy(pBuffer, &lbl_8014E550, 0x300)) {
            return false;
        }
        if (!fn_80054D6C(SYSTEM_RSP(gpSystem), &pBuffer, 0, 4)) {
            return false;
        }
        pBuffer[0] = 0x17D7;
        if (!fn_80054D54(SYSTEM_RSP(gpSystem), &pBuffer, 0, 4)) {
            return false;
        }
        pBuffer[0] = -1;
    } else if (var_r23 == 0x17D8) {
        if (ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer, 0, 0) == 0) {
            return false;
        }
        pBuffer[0x59] = 0x01EC6021;
        pBuffer[0xAE] = 0x8941680C;
        if (fn_80054D6C(SYSTEM_RSP(gpSystem), &pBuffer, 0, 4) == 0) {
            return false;
        }
        pBuffer[0] = 0x17D8;
        if (fn_80054D54(SYSTEM_RSP(gpSystem), &pBuffer, 0, 4) == 0) {
            return false;
        }
        pBuffer[0] = -1;
    } else if (var_r23 == 0x17D9) {
        if (ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&pBuffer, 0, 0) == 0) {
            return false;
        }
        pBuffer[0xBA] = 0xC86E2000;
        pBuffer[0xBEC7D] = 0xAD090010;
        pBuffer[0xBF870] = 0xAD170014;
    }

    pCPU->nTimeRetrace = nTimeRetrace;
    nControllerConfig = gSystemRomConfigurationList[0].currentControllerConfig;
    gSystemRomConfigurationList[0].rumbleConfiguration = 0;
    for (iController = 0; iController < 4; iController++) {
        ((bool (*)(u32*, u32*))simulatorCopyControllerMap)(
            (u32*)gSystemRomConfigurationList[0].controllerConfiguration[iController],
            contMap[(nControllerConfig >> (iController * 8)) & 0x7F]);
        gSystemRomConfigurationList[0].rumbleConfiguration |= (1 << (iController * 8)) & (nControllerConfig >> 7);
    }

    for (iController = 0; iController < 4; iController++) {
        fn_80007118((u32*)gSystemRomConfigurationList[0].controllerConfiguration[iController], iController);
        //     simulatorSetControllerMap(SYSTEM_CONTROLLER(pSystem), iController,
        //                               (u32*)&gSystemRomConfigurationList[0].controllerConfiguration[iController]);
    }
    return true;
}
#endif

static bool systemGetException(System* pSystem, SystemInterruptType eType, SystemException* pException) {
    pException->nMask = 0x00;
    pException->szType = "";
    pException->eType = eType;
    pException->eCode = CEC_NONE;
    pException->eTypeMips = MIT_NONE;

    switch (eType) {
        case SIT_SW0:
            pException->nMask = 0x05;
            pException->szType = "SW0";
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_SW1:
            pException->nMask = 0x06;
            pException->szType = "SW1";
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_CART:
            pException->nMask = 0x0C;
            pException->szType = "CART";
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_COUNTER:
            pException->nMask = 0x84;
            pException->szType = "COUNTER";
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_RDB:
            pException->nMask = 0x24;
            pException->szType = "RDB";
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_SP:
            pException->nMask = 0x04;
            pException->szType = "SP";
            pException->eTypeMips = MIT_SP;
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_SI:
            pException->nMask = 0x04;
            pException->szType = "SI";
            pException->eTypeMips = MIT_SI;
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_AI:
            pException->nMask = 0x04;
            pException->szType = "AI";
            pException->eTypeMips = MIT_AI;
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_VI:
            pException->nMask = 0x04;
            pException->szType = "VI";
            pException->eTypeMips = MIT_VI;
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_PI:
            pException->nMask = 0x04;
            pException->szType = "PI";
            pException->eTypeMips = MIT_PI;
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_DP:
            pException->nMask = 0x04;
            pException->szType = "DP";
            pException->eTypeMips = MIT_DP;
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_CPU_BREAK:
            pException->szType = "BREAK (CPU)";
            pException->eCode = CEC_BREAK;
            break;
        case SIT_SP_BREAK:
            pException->nMask = 0x04;
            pException->szType = "BREAK (SP)";
            pException->eCode = CEC_INTERRUPT;
            break;
        case SIT_FAULT:
            pException->szType = "FAULT";
            break;
        case SIT_THREADSTATUS:
            pException->szType = "THREADSTATUS";
            break;
        case SIT_PRENMI:
            pException->szType = "PRENMI";
            pException->eCode = CEC_INTERRUPT;
            break;
        default:
            return false;
    }

    return true;
}

static bool systemGet8(System* pSystem, u32 nAddress, s8* pData) {
#if IS_OOT || IS_MT
    s64 pnPC;

    *pData = 0;

    if (!cpuGetXPC(SYSTEM_CPU(gpSystem), &pnPC, NULL, NULL)) {
#if IS_MT
        SAFE_FAILED("system.c", 3260);
        return false;
#endif
    }

    return false;
#elif IS_MM
    *pData = 0;
    return true;
#endif
}

static bool systemGet16(System* pSystem, u32 nAddress, s16* pData) {
#if IS_OOT || IS_MT
    s64 pnPC;

    *pData = 0;

    if (!cpuGetXPC(SYSTEM_CPU(gpSystem), &pnPC, NULL, NULL)) {
#if IS_MT
        SAFE_FAILED("system.c", 3260);
        return false;
#endif
    }

    return false;
#elif IS_MM
    *pData = 0;
    return true;
#endif
}

static bool systemGet32(System* pSystem, u32 nAddress, s32* pData) {
#if IS_OOT || IS_MT
    s64 pnPC;

    *pData = 0;

    if (!cpuGetXPC(SYSTEM_CPU(gpSystem), &pnPC, NULL, NULL)) {
#if IS_MT
        SAFE_FAILED("system.c", 3260);
        return false;
#endif
    }

    return false;
#elif IS_MM
    *pData = 0;
    return true;
#endif
}

static bool systemGet64(System* pSystem, u32 nAddress, s64* pData) {
#if IS_OOT || IS_MT
    s64 pnPC;

    *pData = 0;

    if (!cpuGetXPC(SYSTEM_CPU(gpSystem), &pnPC, NULL, NULL)) {
#if IS_MT
        SAFE_FAILED("system.c", 3260);
        return false;
#endif
    }

    return false;
#elif IS_MM
    *pData = 0;
    return true;
#endif
}

static bool systemPut8(System* pSystem, u32 nAddress, s8* pData) {
#if IS_OOT || IS_MT
    s64 pnPC;

    if (!cpuGetXPC(SYSTEM_CPU(gpSystem), &pnPC, NULL, NULL)) {
#if IS_MT
        SAFE_FAILED("system.c", 3260);
        return false;
#endif
    }

    return false;
#elif IS_MM
    return true;
#endif
}

static bool systemPut16(System* pSystem, u32 nAddress, s16* pData) {
#if IS_OOT || IS_MT
    s64 pnPC;

    if (!cpuGetXPC(SYSTEM_CPU(gpSystem), &pnPC, NULL, NULL)) {
#if IS_MT
        SAFE_FAILED("system.c", 3260);
        return false;
#endif
    }

    return false;
#elif IS_MM
    return true;
#endif
}

static bool systemPut32(System* pSystem, u32 nAddress, s32* pData) {
#if IS_OOT || IS_MT
    s64 pnPC;

    if (!cpuGetXPC(SYSTEM_CPU(gpSystem), &pnPC, NULL, NULL)) {
#if IS_MT
        SAFE_FAILED("system.c", 3260);
        return false;
#endif
    }

    return false;
#elif IS_MM
    return true;
#endif
}

static bool systemPut64(System* pSystem, u32 nAddress, s64* pData) {
#if IS_OOT || IS_MT
    s64 pnPC;

    if (!cpuGetXPC(SYSTEM_CPU(gpSystem), &pnPC, NULL, NULL)) {
#if IS_MT
        SAFE_FAILED("system.c", 3260);
        return false;
#endif
    }

    return false;
#elif IS_MM
    return true;
#endif
}

#if IS_OOT || IS_MT
static bool systemGetBlock(System* pSystem, CpuBlock* pBlock) {
    void* pBuffer;

    if (pBlock->nAddress1 < 0x04000000) {
        if (!ramGetBuffer(SYSTEM_RAM(gpSystem), &pBuffer, pBlock->nAddress1, &pBlock->nSize)) {
            SAFE_FAILED("system.c", 3260);
            return false;
        }

        xlHeapFill8(pBuffer, pBlock->nSize, 0xFF);
    }

    if (pBlock->pfUnknown != NULL && !pBlock->pfUnknown(pBlock, 1)) {
        SAFE_FAILED("system.c", 3266);
        return false;
    }

    return true;
}

static inline bool fn_8000A504_UnknownInline(System* pSystem, CpuBlock** pBlock) {
    s32 i;

    for (i = 0; i < ARRAY_COUNT(pSystem->aBlock); i++) {
        if (*pBlock == &pSystem->aBlock[i]) {
            pSystem->storageDevice &= ~(1 << i);
            return true;
        }
    }

    return false;
}

static bool fn_8000A504(CpuBlock* pBlock, bool bUnknown) {
    u32 nAddressOffset[32];
    u32 nAddress;
    u32* pnAddress;
    u32 nAddressEnd;
    s32 nCount;
    s32 i;

    if (bUnknown == true) {
        nAddress = pBlock->nAddress1;

        if (nAddress < 0x04000000) {
            nAddressEnd = (nAddress + pBlock->nSize) - 1;

#if IS_OOT
            if (!rspInvalidateCache(SYSTEM_RSP(gpSystem), nAddress, nAddressEnd)) {
                return false;
            }
#endif
            if (!frameInvalidateCache(SYSTEM_FRAME(gpSystem), nAddress, nAddressEnd)) {
                return false;
            }

            if (!cpuGetOffsetAddress(SYSTEM_CPU(gpSystem), nAddressOffset, &nCount, pBlock->nAddress1, pBlock->nSize)) {
                return false;
            }

            for (i = 0, pnAddress = nAddressOffset; i < nCount; pnAddress++, i++) {
                if (!cpuInvalidateCache(SYSTEM_CPU(gpSystem), *pnAddress, (*pnAddress + pBlock->nSize) - 1)) {
                    return false;
                }
            }
        }

#if IS_OOT
        if (pBlock->pNext->pfUnknown != NULL) {
            pBlock->pNext->pfUnknown(pBlock->pNext, bUnknown);
        }
        if (!fn_8000A504_UnknownInline(gpSystem, &pBlock)) {
            return false;
        }
#endif
    }

    return true;
}
#elif IS_MM
static bool fn_8000A504(void) {
    u32 nAddressOffset[32];
    u32 nAddressEnd;
    u32* pnAddress;
    u32 nAddress;
    s32 nCount;
    s32 i;

    nAddress = gpSystem->cpuBlock.nAddress0;
    nAddressEnd = (nAddress + gpSystem->cpuBlock.nSize) ;

    if (!frameInvalidateCache(SYSTEM_FRAME(gpSystem), nAddress, nAddressEnd-- - 1)) {
        return false;
    }

    if (!rspInvalidateCache(SYSTEM_RSP(gpSystem), nAddress, nAddressEnd)) {
        return false;
    }

    if (!cpuGetOffsetAddress(SYSTEM_CPU(gpSystem), nAddressOffset, &nCount, gpSystem->cpuBlock.nAddress0,
                             gpSystem->cpuBlock.nSize)) {
        return false;
    }

    for (i = 0, pnAddress = nAddressOffset; i < nCount; pnAddress++, i++) {
        if (!cpuInvalidateCache(SYSTEM_CPU(gpSystem), *pnAddress, (*pnAddress + gpSystem->cpuBlock.nSize) - 1)) {
            return false;
        }
    }

    gpSystem->cpuBlock.nSize = 0;
    if (gpSystem->cpuBlock.pfUnknown != NULL && !gpSystem->cpuBlock.pfUnknown()) {
        return false;
    }

    return true;
}
#endif

#if IS_OOT
static inline bool systemGetNewBlock(System* pSystem, CpuBlock** ppBlock) {
    s32 i;

    for (i = 0; i < ARRAY_COUNT(pSystem->aBlock); i++) {
        if (!(pSystem->storageDevice & (1 << i))) {
            pSystem->storageDevice |= (1 << i);
            *ppBlock = &pSystem->aBlock[i];
            return true;
        }
    }

    *ppBlock = NULL;
    return false;
}

bool fn_8000A6A4(System* pSystem, CpuBlock* pBlock) {
    CpuBlock* pNewBlock;

    if (!systemGetNewBlock(pSystem, &pNewBlock)) {
        return false;
    }

    pNewBlock->pNext = pBlock;
    pNewBlock->nSize = pBlock->nSize;
    pNewBlock->pfUnknown = fn_8000A504;
    pNewBlock->nAddress0 = pBlock->nAddress0;
    pNewBlock->nAddress1 = pBlock->nAddress1;

    if (!cpuGetBlock(SYSTEM_CPU(gpSystem), pNewBlock)) {
        return false;
    }

    return true;
}
#elif IS_MM
bool fn_800166D0(System* pSystem, s32 nAddress0, s32 nAddress1, u32 nSize, UnknownBlockCallback pfUnknown) {
    void* spC;
    s32 sp8;

    sp8 = nSize;
    pSystem->cpuBlock.nSize = nSize;
    pSystem->cpuBlock.nAddress1 = nAddress1;
    pSystem->cpuBlock.pfUnknown = pfUnknown;
    pSystem->cpuBlock.nAddress0 = nAddress0 & 0x007FFFFF;

    if (!ramGetBuffer(SYSTEM_RAM(pSystem), &spC, nAddress0, (u32*)&sp8)) {
        return false;
    }

    if (pfUnknown == NULL) {
        if (!romCopy(SYSTEM_ROM(pSystem), spC, nAddress1, sp8, NULL)) {
            return false;
        }
        if (!fn_8000A504()) {
            return false;
        }
    } else {
        if (!romCopy(SYSTEM_ROM(pSystem), spC, nAddress1, sp8, fn_8000A504)) {
            return false;
        }
    }

    return true;
}
#endif

bool systemSetMode(System* pSystem, SystemMode eMode) {
    if (xlObjectTest(pSystem, &gClassSystem)) {
        pSystem->eMode = eMode;

        if (eMode == SM_STOPPED) {
            pSystem->nAddressBreak = -1;
        }

        return true;
    }

    return false;
}

bool systemGetMode(System* pSystem, SystemMode* peMode) {
    if (xlObjectTest(pSystem, &gClassSystem) && (peMode != NULL)) {
        *peMode = pSystem->eMode;
        return true;
    }

    return false;
}

bool fn_8000A830(System* pSystem, s32 nEvent, void* pArgument) {
    s32 i;

    for (i = 0; i < SOT_COUNT; i++) {
        if (pSystem->apObject[i] != NULL) {
            xlObjectEvent(pSystem->apObject[i], nEvent, pArgument);
        }
    }

    return true;
}

#if IS_OOT
bool fn_8000A8A8(System* pSystem) {
    fn_8000A830(pSystem, 0x1004, NULL);
    VISetBlack(true);
    VIFlush();
    VIWaitForRetrace();
    LCDisable();
    OSRestart(0x1234);
    return true;
}
#endif

static inline bool systemSetRamMode(System* pSystem) {
    s32 nSize;
    u32* anMode;

    if (!ramGetBuffer(SYSTEM_RAM(gpSystem), (void**)&anMode, 0x300, NULL)) {
        return false;
    }

    anMode[0] = 1;
    anMode[1] = 0;
    anMode[2] = 0xB0000000;
    anMode[3] = 0;
    anMode[4] = 0x17D5;
    anMode[5] = 1;

    if (!ramGetSize(SYSTEM_RAM(gpSystem), &nSize)) {
        return false;
    }

    anMode[6] = nSize;

    SYSTEM_CPU(gpSystem)->nTimeRetrace = OSSecondsToTicks(1.0f / 60.0f);

    return true;
}

bool systemReset(System* pSystem) {
    s64 nPC;
    s32 nOffsetRAM;
#if IS_OOT || IS_MT
    int eObject;
#elif IS_MM
    SystemObjectType eObject;
#endif
    CpuBlock block;

    pSystem->nAddressBreak = -1;

    if (romGetImage(SYSTEM_ROM(SYSTEM_PTR), NULL)) {
#if IS_OOT || IS_MT
        if (!systemSetupGameRAM(pSystem)) {
            return false;
        }
#elif IS_MM
        s32 nTypeROM;

        romGetCode(SYSTEM_ROM(pSystem), (char*)&nTypeROM);
        pSystem->eTypeROM = nTypeROM;

        if (!systemSetupGameRAM(pSystem)) {
            return false;
        }

        if (!ramWipe(SYSTEM_RAM(pSystem))) {
            return false;
        }
#endif

        if (!romGetPC(SYSTEM_ROM(SYSTEM_PTR), (u64*)&nPC)) {
            return false;
        }

#if IS_OOT || IS_MT
        block.nSize = 0x100000;
        block.pfUnknown = NULL;
        block.nAddress0 = 0x10001000;
        block.nAddress1 = nPC & 0x00FFFFFF;

        if (!fn_8000A6A4(pSystem, &block)) {
            return false;
        }
#elif IS_MM
        if (!fn_800166D0(pSystem, nPC & 0x00FFFFFF, 0x1000, 0x100000, NULL)) {
            return false;
        }
#endif

        if (!cpuReset(SYSTEM_CPU(SYSTEM_PTR))) {
            return false;
        }

#if IS_OOT || IS_MT
        if (!systemSetRamMode(pSystem)) {
            return false;
        }
#endif

        cpuSetXPC(SYSTEM_CPU(SYSTEM_PTR), nPC, 0, 0);

        if (!systemSetupGameALL(pSystem)) {
            return false;
        }

        for (eObject = 0; eObject < SOT_COUNT; eObject++) {
            if (pSystem->apObject[eObject] != NULL) {
                if (!xlObjectEvent(pSystem->apObject[eObject], 0x1003, NULL)) {
#if IS_MM
                    return false;
#endif
                }
            }
        }
    }

    return true;
}

static inline bool systemTestClassObject(System* pSystem) {
    if (xlObjectTest(pSystem, &gClassSystem)) {
        pSystem->eMode = SM_STOPPED;
        pSystem->nAddressBreak = -1;

        return true;
    }

    return false;
}

bool systemExecute(System* pSystem, s32 nCount) {
    if (!cpuExecute(SYSTEM_CPU(SYSTEM_PTR), nCount, pSystem->nAddressBreak)) {
        if (!systemTestClassObject(pSystem)) {
            return false;
        }

        return false;
    }

    if (pSystem->nAddressBreak == SYSTEM_CPU(SYSTEM_PTR)->nPC) {
        if (!systemTestClassObject(pSystem)) {
            return false;
        }
    }

    return true;
}

bool systemCheckInterrupts(System* pSystem) {
    s32 iException;
    s32 nMaskFinal;
    bool bUsed;
    bool bDone;
    SystemException exception;
    CpuExceptionCode eCodeFinal;

    nMaskFinal = 0;
    eCodeFinal = CEC_NONE;
    bDone = false;
    pSystem->bException = false;

    for (iException = 0; iException < ARRAY_COUNT(pSystem->anException); iException++) {
        if (pSystem->anException[iException] != 0) {
            pSystem->bException = true;

            if (!bDone) {
                if (!systemGetException(pSystem, iException, &exception)) {
                    SAFE_FAILED("system.c", 3716);
                    return false;
                }

                bUsed = false;

                if (exception.eCode == CEC_INTERRUPT) {
                    if (cpuTestInterrupt(SYSTEM_CPU(SYSTEM_PTR), exception.nMask) &&
                        (exception.eTypeMips == MIT_NONE ||
                         miSetInterrupt(SYSTEM_MI(SYSTEM_PTR), exception.eTypeMips))) {
                        bUsed = true;
                    }
                } else {
                    bDone = true;

                    if (nMaskFinal == 0) {
                        bUsed = true;
                        eCodeFinal = exception.eCode;
                    }
                }

                if (bUsed) {
                    nMaskFinal |= exception.nMask;
                    pSystem->anException[iException] = 0;
                }
            }
        }
    }

    if (nMaskFinal != 0) {
        if (!cpuException(SYSTEM_CPU(SYSTEM_PTR), CEC_INTERRUPT, nMaskFinal)) {
            SAFE_FAILED("system.c", 3752);
            return false;
        }
    } else {
        if ((eCodeFinal != CEC_NONE)) {
            if (!cpuException(SYSTEM_CPU(SYSTEM_PTR), eCodeFinal, 0)) {
                SAFE_FAILED("system.c", 3754);
                return false;
            }
        }
    }

    return true;
}

bool systemExceptionPending(System* pSystem, SystemInterruptType nException) {
    if ((nException > -1) && (nException < ARRAY_COUNT(pSystem->anException))) {
        if (pSystem->anException[nException] != 0) {
            return true;
        }

        return false;
    }

    return false;
}

static inline bool systemClearExceptions(System* pSystem) {
    int iException;

    pSystem->bException = false;

    for (iException = 0; iException < 16; iException++) {
        pSystem->anException[iException] = 0;
    }

    return true;
}

static inline bool systemFreeDevices(System* pSystem) {
    int storageDevice; // SystemObjectType

    for (storageDevice = 0; storageDevice < SOT_COUNT; storageDevice++) {
        if (pSystem->apObject[storageDevice] != NULL && !xlObjectFree(&pSystem->apObject[storageDevice])) {
            SAFE_FAILED("system.c", 873);
            return false;
        }
    }

    return true;
}

#if IS_OOT || IS_MT
bool systemEvent(System* pSystem, s32 nEvent, void* pArgument) {
    Cpu* pCPU;
    SystemException exception;
    SystemObjectType eObject;
    SystemObjectType storageDevice;

    switch (nEvent) {
        case 2:
            pSystem->storageDevice = SOT_CPU;
            pSystem->eMode = SM_STOPPED;
            pSystem->eTypeROM = NONE;
            pSystem->nAddressBreak = -1;
            systemClearExceptions(pSystem);
            if (!systemCreateStorageDevice(pSystem, pArgument)) {
                SAFE_FAILED("system.c", 3809);
                return false;
            }
            break;
        case 3:
            if (!systemFreeDevices(pSystem)) {
                SAFE_FAILED("system.c", 3813);
                return false;
            }
            break;
        case 0x1001:
            if (!systemGetException(pSystem, (SystemInterruptType)(s32)pArgument, &exception)) {
                SAFE_FAILED("system.c", 3831);
                return false;
            }
            if (exception.eTypeMips != MIT_NONE) {
                miResetInterrupt(SYSTEM_MI(gpSystem), exception.eTypeMips);
            }
            break;
        case 0x1000:
            if (((SystemInterruptType)(s32)pArgument > SIT_NONE) && ((SystemInterruptType)(s32)pArgument < SIT_COUNT)) {
                pSystem->bException = true;
                pSystem->anException[(SystemInterruptType)(s32)pArgument]++;
                break;
            }
            return false;
        case 0x1002:
            if (!cpuSetGetBlock(SYSTEM_CPU(gpSystem), pArgument, (GetBlockFunc)systemGetBlock)) {
                SAFE_FAILED("system.c", 3855);
                return false;
            }
            if (!cpuSetDevicePut(SYSTEM_CPU(gpSystem), pArgument, (Put8Func)systemPut8, (Put16Func)systemPut16,
                                 (Put32Func)systemPut32, (Put64Func)systemPut64)) {
                SAFE_FAILED("system.c", 3856);
                return false;
            }
            if (!cpuSetDeviceGet(SYSTEM_CPU(gpSystem), pArgument, (Get8Func)systemGet8, (Get16Func)systemGet16,
                                 (Get32Func)systemGet32, (Get64Func)systemGet64)) {
                SAFE_FAILED("system.c", 3857);
                return false;
            }
            break;
        case 0:
        case 1:
        case 5:
        case 6:
        case 7:
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
#elif IS_MM
extern _XL_OBJECTTYPE gClassSerial;

bool systemEvent(System* pSystem, s32 nEvent, void* pArgument) {
    Cpu* pCPU;
    SystemException exception;
    SystemObjectType eObject;
    SystemObjectType storageDevice;

    switch (nEvent) {
        case 2:
            pSystem->eMode = SM_STOPPED;
            pSystem->storageDevice = SOT_NONE;
            pSystem->nAddressBreak = -1;
            pSystem->cpuBlock.nSize = 0;

            for (eObject = 0; eObject < SOT_COUNT; eObject++) {
                pSystem->apObject[eObject] = NULL;
            }

            systemClearExceptions(pSystem);

            for (eObject = 0; eObject < SOT_COUNT; eObject++) {
                switch (eObject) {
                    case SOT_FRAME:
                        if (!xlObjectMake(&pSystem->apObject[SOT_FRAME], NULL, &gClassFrame)) {
                            return false;
                        }
                        break;
                    case SOT_CPU:
                        if (!xlObjectMake(&pSystem->apObject[SOT_CPU], pSystem, &gClassCPU)) {
                            return false;
                        }
                        pCPU = SYSTEM_CPU(pSystem);
                        if (!cpuMapObject(pCPU, pSystem, 0, 0xFFFFFFFF, 0)) {
                            return false;
                        }
                        break;
                    case SOT_PIF:
                        if (!xlObjectMake(&pSystem->apObject[SOT_PIF], pSystem, &gClassPIF)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_PIF], 0x1FC00000, 0x1FC007FF, 0)) {
                            return false;
                        }
                        break;
                    case SOT_RAM:
                        if (!xlObjectMake(&pSystem->apObject[SOT_RAM], pSystem, &gClassRAM)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_RAM], 0x00000000, 0x03EFFFFF, 0x100)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_RAM], 0x03F00000, 0x03FFFFFF, 2)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_RAM], 0x04700000, 0x047FFFFF, 1)) {
                            return false;
                        }
                        break;
                    case SOT_ROM:
                        if (!xlObjectMake(&pSystem->apObject[SOT_ROM], pSystem, &gClassROM)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_ROM], 0x10000000, 0x1FBFFFFF, 0)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_ROM], 0x1FF00000, 0x1FF0FFFF, 1)) {
                            return false;
                        }
                        break;
                    case SOT_RSP:
                        if (!xlObjectMake(&pSystem->apObject[SOT_RSP], pSystem, &gClassRSP)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_RSP], 0x04000000, 0x040FFFFF, 0)) {
                            return false;
                        }
                        break;
                    case SOT_RDP:
                        if (!xlObjectMake(&pSystem->apObject[SOT_RDP], pSystem, &gClassRDP)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_RDP], 0x04100000, 0x041FFFFF, 0)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_RDP], 0x04200000, 0x042FFFFF, 1)) {
                            return false;
                        }
                        break;
                    case SOT_MI:
                        if (!xlObjectMake(&pSystem->apObject[SOT_MI], pSystem, &gClassMI)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_MI], 0x04300000, 0x043FFFFF, 0)) {
                            return false;
                        }
                        break;
                    case SOT_DISK:
                        if (!xlObjectMake(&pSystem->apObject[SOT_DISK], pSystem, &gClassDD)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_DISK], 0x05000000, 0x05FFFFFF, 0)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_DISK], 0x06000000, 0x06FFFFFF, 1)) {
                            return false;
                        }
                        break;
                    case SOT_AI:
                        if (!xlObjectMake(&pSystem->apObject[SOT_AI], pSystem, &gClassAI)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_AI], 0x04500000, 0x045FFFFF, 0)) {
                            return false;
                        }
                        break;
                    case SOT_FLASH:
                        pSystem->apObject[SOT_FLASH] = NULL;
                        break;
                    case SOT_SRAM:
                        pSystem->apObject[SOT_SRAM] = NULL;
                        break;
                    case SOT_PAK:
                        pSystem->apObject[SOT_PAK] = NULL;
                        break;
                    case SOT_AUDIO:
                        if (!xlObjectMake(&pSystem->apObject[SOT_AUDIO], pSystem, &gClassAudio)) {
                            return false;
                        }
                        break;
                    case SOT_VI:
                        if (!xlObjectMake(&pSystem->apObject[SOT_VI], pSystem, &gClassVI)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_VI], 0x04400000, 0x044FFFFF, 0)) {
                            return false;
                        }
                        break;
                    case SOT_SI:
                        if (!xlObjectMake(&pSystem->apObject[SOT_SI], pSystem, &gClassSI)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_SI], 0x04800000, 0x048FFFFF, 0)) {
                            return false;
                        }
                        break;
                    case SOT_LIBRARY:
                        if (!xlObjectMake(&pSystem->apObject[SOT_LIBRARY], pSystem, &gClassLibrary)) {
                            return false;
                        }
                        break;
                    case SOT_PI:
                        if (!xlObjectMake(&pSystem->apObject[SOT_PI], pSystem, &gClassPI)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_PI], 0x04600000, 0x046FFFFF, 0)) {
                            return false;
                        }
                        break;
                    case SOT_RDB:
                        if (!xlObjectMake(&pSystem->apObject[SOT_RDB], pSystem, &gClassRdb)) {
                            return false;
                        }
                        if (!cpuMapObject(pCPU, pSystem->apObject[SOT_RDB], 0x04900000, 0x0490FFFF, 0)) {
                            return false;
                        }
                        break;
                    case SOT_CONTROLLER:
                        if (!xlObjectMake(&pSystem->apObject[SOT_CONTROLLER], pSystem, &gClassController)) {
                            return false;
                        }
                        break;
                    case SOT_HELP:
                        if (!xlObjectMake(&pSystem->apObject[SOT_HELP], pSystem, &gClassHelpMenu)) {
                            return false;
                        }
                        break;
                    default:
                        return false;
                }
            }
            break;
        case 3:
            for (storageDevice = 2; storageDevice < SOT_COUNT; storageDevice++) {
                if (!xlObjectFree(&pSystem->apObject[storageDevice])) {
                    return false;
                }
            }
            break;
        case 0x1001:
            if (!systemGetException(pSystem, (SystemInterruptType)(s32)pArgument, &exception)) {
                return false;
            }
            if (exception.eTypeMips != MIT_NONE) {
                miResetInterrupt(SYSTEM_MI(pSystem), exception.eTypeMips);
            }
            break;
        case 0x1000:
            if (((SystemInterruptType)(s32)pArgument > SIT_NONE) && ((SystemInterruptType)(s32)pArgument < SIT_COUNT)) {
                pSystem->bException = true;
                pSystem->anException[(SystemInterruptType)(s32)pArgument]++;
                break;
            }
            return false;
        case 0x1002:
            if (!cpuSetDevicePut(SYSTEM_CPU(pSystem), pArgument, (Put8Func)systemPut8, (Put16Func)systemPut16,
                                 (Put32Func)systemPut32, (Put64Func)systemPut64)) {
                return false;
            }
            if (!cpuSetDeviceGet(SYSTEM_CPU(pSystem), pArgument, (Get8Func)systemGet8, (Get16Func)systemGet16,
                                 (Get32Func)systemGet32, (Get64Func)systemGet64)) {
                return false;
            }
            break;
        case 0:
        case 1:
        case 5:
        case 6:
        case 7:
            break;
        case 0x1003:
            break;
        default:
            return false;
    }

    return true;
}
#endif

#if IS_OOT || IS_MT
_XL_OBJECTTYPE gClassSystem = {
    "SYSTEM",
    sizeof(System),
    NULL,
    (EventFunc)systemEvent,
}; // size = 0x10
#endif
