/**
 * @file crashScreen.c
 *
 * Visual crash debugger.
 */
#include "emulator/system.h"
#include "emulator/vc64_RVL.h"
#include "macros.h"
#include "revolution/os.h"
#include "revolution/vi.h"
#include "stdio.h"

#pragma section ".crashtext"
#pragma section ".crashdata" ".crashbss"

// GXAbortFrame. mm-j takes its GX from the extracted objects (configure.py only compiles
// revolution/gx/ for the oot-* versions), and dtk left this one auto-named, so it has to be
// called through its address symbol. Identified by matching GXMisc.o's function sizes against
// config/mm-j/symbols.txt: GXSetMisc(0x8C) / GXFlush(0x5C) / __GXAbort(0x164) /
// GXAbortFrame(0x1B4) / GXSetDrawSync(0xB4) line up exactly, and the first and last of those
// are independently named in mm-j at 0x800AA144 and 0x800AA544, bracketing the run.
extern void fn_800AA390(void); // GXAbortFrame

#define CT DECL_SECTION(".crashtext")
#define CD DECL_SECTION(".crashdata")

#define CRASH_REPORT_PAGE_MAX 4

#define CRASH_N64_STACK_LINES 18
#define CRASH_N64_SCAN_WORDS 2048
#define CRASH_N64_RA_SCAN_WORDS 64
#define CRASH_N64_RAM_LO 0x80000000
#define CRASH_N64_RAM_HI 0x80800000

#define CRASH_SPLASH_SECONDS 4
#define CRASH_PAGE_SECONDS 8

#define CRASH_FB_W 640
#define CRASH_FB_H 480
#define CRASH_FG 0xFF80
#define CRASH_BG 0x1F80

#define CRASH_MARGIN_X 16
#define CRASH_MARGIN_Y 24
#define CRASH_CHAR_W 16
#define CRASH_LINE_H 18

CD static char sCrashPage[CRASH_REPORT_PAGE_MAX][768] = { { 1 } };
CD static s32 sCrashPageCount = 1;
CD static s32 sCrashEntryCount = -1;

// All display text lives in .crashdata
CD static const char kFmtTitle[] = "== CRASH ==";
CD static const char kFmtSrr[] = "\nSRR0=%08x SRR1=%08x";
CD static const char kFmtLr[] = "\nLR  =%08x";
CD static const char kFmtDar[] = "\nDAR =%08x DSISR=%08x";
CD static const char kFmtWiiGpr[] = "\n\nWii GPRs:";
CD static const char kFmtGprPair[] = "\nr%-2d=%08x r%-2d=%08x";
CD static const char kFmtWiiGprCont[] = "Wii GPRs (cont.):";
CD static const char kFmtBackChain[] = "\n\nWii back chain:";
CD static const char kFmtStackLine[] = "\n%08x: lr=%08x";
CD static const char kFmtN64[] = "N64 CPU:";
CD static const char kFmtN64Half[] = "\nHalf: %s";
CD static const char kHalfOoT[] = "OoT";
CD static const char kHalfMM[] = "MM";
CD static const char kFmtN64Pc[] = "\nPC=%08x RA=%08x";
CD static const char kFmtN64Func[] = "\nFunc %08x-%08x";
CD static const char kFmtNewline[] = "\n";
CD static const char kFmtN64GprPair[] = "\ns%-2d=%08x s%-2d=%08x";
CD static const char kFmtN64Stack[] = "N64 STACK TRACE";
CD static const char kFmtN64StackSp[] = "\nSP=%08x RA=%08x\n";
CD static const char kFmtN64StackLine[] = "\n%08x: %08x";
CD static const char kFmtFooter[] = "page %d/%d";
CD static const char kFmtLogPage[] = "\n--- crash page %d/%d ---\n%s\n";
CD static const char kFmtCount[] = "%d";
CD static const char kSplashBar[] = "************";
CD static const char kSplashMsg[] = "Oh! MY GOD!!";

// 8x8 font, printable ASCII from 0x20. Not static, declared in emulator/crashScreen.h.
CD const u8 gCrashFont[0x5F][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
};

CT static u32 CrashMfdar(void);
CT static u32 CrashMfdsisr(void);
CT static char* CrashAppend(char* p, char* end, const char* format, ...);
CT static bool CrashN64Read(Cpu* pCPU, u32 addr, u32* pValue);
CT static bool CrashN64Plausible(u32 addr);
CT static bool CrashInCompiledCode(Cpu* pCPU, u32 addr, u32* pHeapBase);
CT static bool CrashRecoverN64Ra(Cpu* pCPU, u32 gcnPtr, u32* pN64Ra);
CT static u32 CrashN64TraceStart(Cpu* pCPU);
CT static u32 CrashN64Sp(Cpu* pCPU, OSContext* ctx);
CT static char* CrashN64Stack(Cpu* pCPU, OSContext* ctx, char* p, char* end);
CT static void CrashBuildPages(OSContext* ctx, u32 dsisr, u32 dar);
CT static void CrashClear(u16* fb);
CT static void CrashFillRect(u16* fb, s32 x, s32 y, s32 w, s32 h, u16 color);
CT static void CrashDrawSplash(u16* fb);
CT static void CrashDrawCountdown(u16* fb, s32 remaining);
CT static void CrashDrawChar(u16* fb, s32 x, s32 y, char ch);
CT static s32 CrashDrawText(u16* fb, s32 x, s32 y, const char* s);
CT static void CrashDrawPage(u16* fb, const char* page, s32 pageIndex, s32 pageCount);
CT static void CrashDelaySeconds(s32 seconds);
CT s32 CrashScreenEnter(void);

CT static ASM u32 CrashMfdar(void) {
#ifdef __MWERKS__ // clang-format off
    nofralloc
    mfspr r3, 19
    blr
#endif // clang-format on
}

CT static ASM u32 CrashMfdsisr(void) {
#ifdef __MWERKS__ // clang-format off
    nofralloc
    mfspr r3, 18
    blr
#endif // clang-format on
}

CT static char* CrashAppend(char* p, char* end, const char* format, ...) {
    va_list list;
    s32 written;

    if (p >= end) {
        return end;
    }

    va_start(list, format);
    written = vsnprintf(p, end - p, format, list);
    va_end(list);

    if (written <= 0) {
        return p;
    }
    if (p + written >= end) {
        return end;
    }
    return p + written;
}

CT static bool CrashN64Read(Cpu* pCPU, u32 addr, u32* pValue) {
    return CPU_DEVICE_GET32(pCPU->apDevice, pCPU->aiDevice, addr, pValue);
}

// Coarse plausibility filter for a value that's *supposed* to be an N64 code or stack address
CT static bool CrashN64Plausible(u32 addr) {
    return addr >= CRASH_N64_RAM_LO && addr < CRASH_N64_RAM_HI && (addr & 3) == 0;
}

/**
 * @brief Is `addr` a host address inside one of the recompiler's compiled-code heaps, and if so
 * where does that heap start? The heaps are what cpuTreeInit allocates
 * anything the recompiler produces, and every retargeted return address, lives in one of them.
 *
 * Two uses: bounding CrashRecoverN64Ra's backward scan to memory that is always safe to read,
 * and deciding whether the faulting PC was inside recompiled code (see CrashN64Sp).
 */
CT static bool CrashInCompiledCode(Cpu* pCPU, u32 addr, u32* pHeapBase) {
    if (addr >= (u32)pCPU->gHeap1 && addr < (u32)pCPU->gHeap1 + 0x400000) {
        if (pHeapBase != NULL) {
            *pHeapBase = (u32)pCPU->gHeap1;
        }
        return true;
    }
    if (addr >= (u32)pCPU->gHeap2 && addr < (u32)pCPU->gHeap2 + 0x104000) {
        if (pHeapBase != NULL) {
            *pHeapBase = (u32)pCPU->gHeap2;
        }
        return true;
    }
    return false;
}

/**
 * @brief Recovers the real N64 return address embedded at a JAL call site, given the PPC
 * pointer that ends up on the N64 stack in its place.
 */
CT static bool CrashRecoverN64Ra(Cpu* pCPU, u32 gcnPtr, u32* pN64Ra) {
    u32* p;
    u32* limit;
    u32 heapBase;
    s32 i;
    u16 lo;
    bool foundLo;

    if ((gcnPtr & 3) != 0) {
        return false;
    }
    if (!CrashInCompiledCode(pCPU, gcnPtr, &heapBase)) {
        return false;
    }
    limit = (u32*)heapBase;

    p = (u32*)gcnPtr;
    foundLo = false;
    for (i = 0; i < CRASH_N64_RA_SCAN_WORDS && p > limit; i++) {
        p--;
        if ((*p >> 16) == 0x60E7) { // ori r7, r7, imm
            lo = (u16)*p;
            foundLo = true;
            break;
        }
    }
    if (!foundLo) {
        return false;
    }

    for (; i < CRASH_N64_RA_SCAN_WORDS && p > limit; i++) {
        p--;
        if ((*p >> 16) == 0x3CE0) { // lis r7, imm
            *pN64Ra = (*p << 16) | lo;
            return (*pN64Ra & 0x80000000) != 0;
        }
    }
    return false;
}

CT static u32 CrashN64TraceStart(Cpu* pCPU) {
    CpuFunction* node = pCPU->pFunctionLast;
    u32 ra = (u32)pCPU->nReturnAddrLast;

    if (node != NULL && ra >= (u32)node->nAddress0 && ra < (u32)node->nAddress1) {
        return ra;
    }
    return pCPU->nPC;
}

CT static u32 CrashN64Sp(Cpu* pCPU, OSContext* ctx) {
    u32 live = ctx->gprs[31];

    if (CrashInCompiledCode(pCPU, ctx->srr0, NULL) && CrashN64Plausible(live)) {
        return live;
    }
    return pCPU->aGPR[29].u32;
}

CT static char* CrashN64Stack(Cpu* pCPU, OSContext* ctx, char* p, char* end) {
    u32 sp = CrashN64Sp(pCPU, ctx);
    u32 pc = CrashN64TraceStart(pCPU);
    u32 ra = pCPU->aGPR[31].u32; // $ra, possibly a retargeted PPC pointer
    u32 resolved;
    s32 line;

    if (CrashRecoverN64Ra(pCPU, ra, &resolved)) {
        ra = resolved;
    } else if (!CrashN64Plausible(ra)) {
        ra = 0;
    }

    p = CrashAppend(p, end, kFmtN64StackSp, sp, ra);

    for (line = 0; line < CRASH_N64_STACK_LINES; line++) {
        u32 frameStart = pc;
        u32 target = 0;
        bool armed = false; // a jr/j was decoded, its delay slot is next
        bool arming;
        s32 i;

        p = CrashAppend(p, end, kFmtN64StackLine, sp, pc);

        for (i = 0; i < CRASH_N64_SCAN_WORDS; i++, pc += 4) {
            u32 op;
            u32 raw;

            if (!CrashN64Plausible(pc) || !CrashN64Read(pCPU, pc, &op)) {
                return p;
            }

            arming = false;

            if ((op & 0xFFFF0000) == 0x27BD0000) { // addiu sp, sp, imm
                sp += (u32)(s16)op;
            } else if ((op & 0xFFFF0000) == 0x8FBF0000) { // lw ra, imm(sp)
                if (CrashN64Read(pCPU, sp + (u32)(s16)op, &raw)) {
                    if (CrashRecoverN64Ra(pCPU, raw, &resolved)) {
                        ra = resolved;
                    } else if (CrashN64Plausible(raw)) {
                        ra = raw;
                    }
                }
            } else if (op == 0x03E00008) { // jr ra
                target = ra;
                arming = true;
            } else if ((op >> 26) == 0x02) { // j target
                target = ((op & 0x03FFFFFF) << 2) | CRASH_N64_RAM_LO;
                arming = true;
            }

            if (armed) {
                break;
            }
            armed = arming;
        }

        // `target == frameStart` catches the degenerate case where no `lw ra` was ever seen and
        // every frame returns to the one it started in, which would otherwise print the same
        // line CRASH_N64_STACK_LINES times.
        if (!armed || target == frameStart || !CrashN64Plausible(target)) {
            break;
        }
        pc = target;
    }

    return p;
}

/**
 * @brief Fills sCrashPage[0..sCrashPageCount-1]: exception summary + Wii r0-r15, then Wii
 * r16-r31 + back chain, then N64 CPU state.
 */
CT static void CrashBuildPages(OSContext* ctx, u32 dsisr, u32 dar) {
    char* p;
    char* end;
    u32* sp;
    s32 depth;
    s32 i;
    Cpu* pCPU;

    sCrashPageCount = 0;

    p = sCrashPage[sCrashPageCount];
    end = p + sizeof(sCrashPage[0]);
    p = CrashAppend(p, end, kFmtTitle);
    p = CrashAppend(p, end, kFmtSrr, ctx->srr0, ctx->srr1);
    p = CrashAppend(p, end, kFmtLr, ctx->lr);
    p = CrashAppend(p, end, kFmtDar, dar, dsisr);
    p = CrashAppend(p, end, kFmtWiiGpr);
    for (i = 0; i < 16; i += 2) {
        p = CrashAppend(p, end, kFmtGprPair, i, ctx->gprs[i], i + 1, ctx->gprs[i + 1]);
    }
    sCrashPageCount++;

    p = sCrashPage[sCrashPageCount];
    end = p + sizeof(sCrashPage[0]);
    p = CrashAppend(p, end, kFmtWiiGprCont);
    for (i = 16; i < 32; i += 2) {
        p = CrashAppend(p, end, kFmtGprPair, i, ctx->gprs[i], i + 1, ctx->gprs[i + 1]);
    }
    p = CrashAppend(p, end, kFmtBackChain);
    for (depth = 0, sp = (u32*)ctx->gprs[1]; sp != NULL && sp != (u32*)0xFFFFFFFF && depth < 8;
         depth++, sp = (u32*)sp[0]) {
        p = CrashAppend(p, end, kFmtStackLine, sp, sp[1]);
    }
    sCrashPageCount++;

    if (gpSystem != NULL && (pCPU = SYSTEM_CPU(gpSystem)) != NULL) {
        p = sCrashPage[sCrashPageCount];
        end = p + sizeof(sCrashPage[0]);
        p = CrashAppend(p, end, kFmtN64);
        p = CrashAppend(p, end, kFmtN64Half, pCPU->isMM ? kHalfMM : kHalfOoT);
        p = CrashAppend(p, end, kFmtN64Pc, pCPU->nPC, pCPU->nReturnAddrLast);
        if (pCPU->pFunctionLast != NULL) {
            p = CrashAppend(p, end, kFmtN64Func, pCPU->pFunctionLast->nAddress0, pCPU->pFunctionLast->nAddress1);
        }
        p = CrashAppend(p, end, kFmtNewline);
        for (i = 0; i < 32; i += 2) {
            p = CrashAppend(p, end, kFmtN64GprPair, i, pCPU->aGPR[i].u32, i + 1, pCPU->aGPR[i + 1].u32);
        }
        sCrashPageCount++;

        // N64 stack trace: a real multi-frame unwind, see CrashN64Stack.
        p = sCrashPage[sCrashPageCount];
        end = p + sizeof(sCrashPage[0]);
        p = CrashAppend(p, end, kFmtN64Stack);
        p = CrashN64Stack(pCPU, ctx, p, end);
        sCrashPageCount++;
    }
}

// Fill the whole framebuffer with the dark backdrop color.
// TODO: Don't fill everything
CT static void CrashClear(u16* fb) {
    s32 i;
    for (i = 0; i < CRASH_FB_W * CRASH_FB_H; i++) {
        fb[i] = CRASH_BG;
    }
}

// Fill an axis-aligned rectangle (framebuffer pixels).
CT static void CrashFillRect(u16* fb, s32 x, s32 y, s32 w, s32 h, u16 color) {
    s32 yy;
    s32 xx;
    for (yy = 0; yy < h; yy++) {
        for (xx = 0; xx < w; xx++) {
            fb[(y + yy) * CRASH_FB_W + (x + xx)] = color;
        }
    }
}

// Blit one glyph at 2x scale (each font pixel -> a 2x2 block of foreground pixels). Only set
// pixels are drawn; the backdrop comes from CrashClear. x is kept even so each written pair
// lands on a proper YUYV [Y0 U][Y1 V] boundary.
CT static void CrashDrawChar(u16* fb, s32 x, s32 y, char ch) {
    const u8* g;
    s32 row;
    s32 col;

    if ((u8)ch < 0x20 || (u8)ch > 0x7E) {
        return;
    }
    g = gCrashFont[(u8)ch - 0x20];

    for (row = 0; row < 8; row++) {
        u8 bits = g[row];
        for (col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                u16* q = fb + (y + row * 2) * CRASH_FB_W + (x + col * 2);
                q[0] = CRASH_FG;
                q[1] = CRASH_FG;
                q[CRASH_FB_W] = CRASH_FG;
                q[CRASH_FB_W + 1] = CRASH_FG;
            }
        }
    }
}

// Draw a run of glyphs starting at (x, y); returns the resulting x. Stops before running off
// the right edge. Callers handle '\n' themselves.
CT static s32 CrashDrawText(u16* fb, s32 x, s32 y, const char* s) {
    for (; *s != '\0'; s++) {
        if (x > CRASH_FB_W - CRASH_CHAR_W) {
            break;
        }
        CrashDrawChar(fb, x, y, *s);
        x += CRASH_CHAR_W;
    }
    return x;
}

/**
 * @brief Clears the framebuffer and draws one '\n'-separated page, plus the "page x/y"
 * footer in the bottom-right corner.
 */
CT static void CrashDrawPage(u16* fb, const char* page, s32 pageIndex, s32 pageCount) {
    char line[64];
    const char* src = page;
    s32 x = CRASH_MARGIN_X;
    s32 y = CRASH_MARGIN_Y;
    s32 len;

    CrashClear(fb);

    for (; *src != '\0'; src++) {
        if (*src == '\n') {
            x = CRASH_MARGIN_X;
            y += CRASH_LINE_H;
            continue;
        }
        if (y <= CRASH_FB_H - CRASH_LINE_H) {
            CrashDrawChar(fb, x, y, *src);
        }
        x += CRASH_CHAR_W;
    }

    len = sprintf(line, kFmtFooter, pageIndex + 1, pageCount);
    CrashDrawText(fb, CRASH_FB_W - len * CRASH_CHAR_W - CRASH_MARGIN_X, CRASH_FB_H - CRASH_LINE_H - 8, line);
}

// Opening splash, centered: the "Oh! MY GOD!!"
CT static void CrashDrawSplash(u16* fb) {
    s32 x = (CRASH_FB_W - 12 * CRASH_CHAR_W) / 2; // both lines are 12 chars wide
    s32 y = (CRASH_FB_H - 3 * CRASH_LINE_H) / 2;

    CrashClear(fb);
    CrashDrawText(fb, x, y, kSplashBar);
    CrashDrawText(fb, x, y + CRASH_LINE_H, kSplashMsg);
    CrashDrawText(fb, x, y + 2 * CRASH_LINE_H, kSplashBar);
}

// Redraw the countdown number in the top-right corner 
CT static void CrashDrawCountdown(u16* fb, s32 remaining) {
    char num[8];
    s32 boxW = 3 * CRASH_CHAR_W;
    s32 len = sprintf(num, kFmtCount, remaining);

    CrashFillRect(fb, CRASH_FB_W - boxW - 8, 8, boxW, CRASH_LINE_H, CRASH_BG);
    CrashDrawText(fb, CRASH_FB_W - len * CRASH_CHAR_W - 8, 8, num);
}

CT static void CrashDelaySeconds(s32 seconds) {
    s64 start = OSGetTime();
    s64 target = (s64)OSSecondsToTicks(seconds);

    while (OSGetTime() - start < target) {}
}

// See crashScreen.h
CT s32 CrashScreenEnter(void) {
    return ++sCrashEntryCount;
}

CT void CrashScreenShow(void) {
    OSContext* ctx = OS_CURRENT_CONTEXT;
    u16* fb = (u16*)lbl_8017B1E0[0].unk_04;
    s32 pageIndex = 0;
    s32 i;

    OSDisableInterrupts();
    fn_800AA390(); // GXAbortFrame

    for (i = 0; i < OS_ERR_MAX; i++) {
        __OSErrorTable[i] = NULL;
    }

    CrashBuildPages(ctx, CrashMfdsisr(), CrashMfdar());

    // Mirror the whole report into the log before touching the screen
    for (pageIndex = 0; pageIndex < sCrashPageCount; pageIndex++) {
        OSReport(kFmtLogPage, pageIndex + 1, sCrashPageCount, sCrashPage[pageIndex]);
    }
    pageIndex = 0;

    if (fb == NULL) {
        PPCHalt();
    }

    // Point VI at our target buffer once; we redraw into it in place each page.
    VISetNextFrameBuffer(fb);
    VISetBlack(FALSE);
    VIFlush();

    // Opening splash.
    CrashDrawSplash(fb);
    DCStoreRange(fb, CRASH_FB_W * CRASH_FB_H * 2);
    CrashDelaySeconds(CRASH_SPLASH_SECONDS);

    for (;;) {
        s32 remaining;

        for (remaining = CRASH_PAGE_SECONDS; remaining > 0; remaining--) {
            CrashDrawPage(fb, sCrashPage[pageIndex], pageIndex, sCrashPageCount);
            CrashDrawCountdown(fb, remaining);
            DCStoreRange(fb, CRASH_FB_W * CRASH_FB_H * 2);
            CrashDelaySeconds(1);
        }

        pageIndex = (pageIndex + 1) % sCrashPageCount;
    }
}
