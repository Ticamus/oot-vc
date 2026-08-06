#include "emulator/cpu.h"
#include "emulator/mips.h"
#include "emulator/system.h"
#include "macros.h"

// Split out of cpu.c so this single function can be built from source and linked while the rest of
// cpu.c still comes from the extracted object. See config/mm-j/splits.txt.
//
// It is carved because the OoTMM combo needs a handful of changes to the function-boundary scanner
// that retail's version cannot make; see COMBO_TAIL_CALL below.

// Defined in cpu.c. dtk exposes every function it recovers with global scope, so these resolve
// across the split without any symbols.txt change; the three opcode tables did need naming, since
// they were sitting anonymously inside lbl_8014F0C0.
bool treeInit(Cpu* pCPU, s32 root_address);
bool treeInsert(Cpu* pCPU, s32 start, s32 end);
bool treeSearchNode(CpuFunction* tree, s32 target, CpuFunction** node);
extern u8 Opcode[];
extern u8 SpecialOpcode[];
extern u8 RegimmOpcode[];

#if IS_MM
//! Not in the original game. Ported faithfully from the GameCube version of this port
//! (`~/OoTMMNew/oot-gc`, cpu.c cpuFindFunction), which hit exactly this wall and solved it; the
//! OoTMM combo is what makes it necessary.
//!
//! cpuFindFunction bounds a function by scanning forward from its entry until it meets a `jr` that no
//! forward branch reaches past. That holds for the IDO-compiled retail games. It does not hold for
//! OoTMM, which is built with GCC: GCC reorders blocks past the epilogue and leaves for good through
//! `j <other function>` tail calls, so a function's only frame teardown can sit in the middle. The scan
//! then finds no terminator, walks off the end of the code into the overlay's data -- an ActorProfile
//! and its action-function tables decode as perfectly "valid" opcodes -- and eventually dies on a
//! reserved encoding or outside mapped memory.
//!
//! Dying there is not a contained failure. cpuFindFunction returns false, so cpuMakeFunction and
//! cpuFindAddress do too, so cpuExecuteUpdate does, and the link stub cpuMakeLink() built ends in
//! `mtlr r3` + `blr` without ever testing what the helper returned: the console fetches from address
//! 0 and reports "Unhandled Exception 3" with SRR0 and LR both zero. That is the crash this fixes.
//!
//! The changes match the GameCube port exactly, all gated on the combo so retail MM keeps its behaviour:
//!   - `addiu sp,sp,imm` in the delay slot of a `j` ends the function (case 0x09 below): a tail call
//!     tears down the frame and control leaves for good, so the function ends there;
//!   - an invalid opcode closes the function at the last decoded word instead of returning false;
//!   - a failed fetch does the same rather than abandoning the lookup;
//!   - the end rules must not fire below the entry the caller asked for (startAddress), or the range
//!     inserted would not contain it and treeSearch would fail anyway.
//! The interior-entry case -- a jump into the middle of a function -- is handled by retail's own
//! backwards hunt for a `sw ra` prologue, which the GameCube port keeps unchanged for the combo. An
//! earlier mm-j version replaced it with a hunt for the nearest terminator; that landed on the
//! intermediate `jr $ra` early-return epilogues GCC scatters through a function and truncated it, which
//! is what broke the file-select screen. Reverted to match the reference.
#define COMBO_TAIL_CALL 1
#define COMBO_NOT_BEFORE_ENTRY(current, start) (!gIsOotmmCombo || (current) >= (start))
#define COMBO_CLOSE_ON_INVALID(valid) (gIsOotmmCombo && !(valid))

//! Not in the original game. The GameCube port carries this and it is not optional: `ramGet32`
//! answers `*pData = 0` and **`true`** for anything at or past `pRAM->nSize` (ram.c ~309), a zero word
//! is `sll zero,zero,0`, and `SpecialOpcode[0]` says valid. So a scan that leaves RDRAM reads an endless
//! run of legal nops -- neither the invalid-opcode close nor the failed-fetch close can fire -- and
//! climbs until another device's read computes a host pointer past the end of MEM1, taking a DSI inside
//! this function. 8 MB because systemSetupGameRAM forces the Expansion Pak for the combo, and because
//! every address reaching here is KSEG0 (cpu_execute_jump.c folds the aliases before the lookup).
#define COMBO_RAM_WINDOW 1
#define COMBO_IN_RAM(nAddress) ((u32)(nAddress) >= 0x80000000 && (u32)(nAddress) < 0x80800000)

//! Not in the original game, and not in the GameCube port -- the GameCube port avoids the need for it in
//! cpuGetPPC, which mm-j does not have in source, so it has to be handled here. Where to close a scan that
//! has stopped bounding a function and started reading whatever follows it: an ActorProfile and its
//! action-function tables sit right after a function, decode as perfectly valid opcodes with forward
//! branches that keep the scan going, and are eventually cut off by an invalid word. Closing at that word
//! (the `!valid` default, `current_address - 4`) leaves the data in the range, and cpuGetPPC refuses to
//! compile it -- cpuMakeFunction then returns false and the recompiler's link stub jumps to address 0.
//! Measured: entry 801FC01C, scan bounded 801FC010..801FC35C closing on `73730000` (ASCII), while the real
//! function ends at 801FC0AC with an ActorProfile after it. Close at the last `jr $ra` seen at or after the
//! requested entry instead: it is inside real code, it is a genuine exit, and everything past it is left
//! for a later lookup to bound on its own. A COMBO_SCAN_CAP backstops the case where no invalid word ever
//! comes (a run of legal nops); COMBO_RAM_WINDOW already handles leaving RDRAM.
//!
//! NOT a negative sentinel for nLastJr: a KSEG0 address is negative as s32, so a `>= 0` test would reject
//! every real value it holds. 0 means "none", tested `!= 0`.
#define COMBO_CLOSE_AT_LAST_JR 1
//! Bytes of code past beginAddress with no accepted terminator before a runaway is presumed. It must
//! exceed the largest real function, or it truncates one mid-scan: FileSelect_CustomFileInfoDraw is
//! **0x348C bytes** (a giant draw function), so the earlier 0x2000 fired inside it and CLOSE_AT_LAST_JR
//! then closed at an intermediate `jr $ra` epilogue -- the file-select crash, reintroduced by too small a
//! cap. This is only a backstop anyway: COMBO_RAM_WINDOW catches a scan that leaves RDRAM, and an invalid
//! opcode closes the ordinary over-run into an ActorProfile (the Kokiri case, cut off at 0x350, far below
//! any cap). The cap only has to stop a pure run of legal nops, which is rare, so err large.
#define COMBO_SCAN_CAP 0x8000

//! Not in the original game, and it replaces an earlier attempt that did real damage -- keep the reason
//! recorded so it is not reintroduced. The function tree cannot represent two functions whose ranges
//! overlap: treeInsertNode keys purely on nAddress0, and treeSearchNode navigates by containment, so a
//! small function sitting inside a larger one makes the larger one unreachable from any address past the
//! small one's end. cpuFindFunction then returns false on the range it just inserted successfully -- a jump
//! to 0. It happens when a jump into the middle of a function (a switch case block reached through a
//! `jr v0` jump table) creates a fragment there first, and the enclosing bounds are discovered later.
//! Measured: tree held [80736A00,80736A38] (a fragment ending on a switch dispatch's `jr v0`), scan for a
//! case block at 80736A5C walked back to 8073692C and wanted [8073692C,80736AC8], which overlaps it.
//!
//! NEVER repair this by deleting compiled functions here: treeDeleteNode runs treeCallerCheck, which
//! reverts the lis/ori of every patched call site aimed at a deleted function -- cpu.c's own "VC Crash".
//! Both tree failures (a duplicate nAddress0, or an overlap) come from the backwards restart machinery
//! having moved anAddr[0] off the requested address, so retry the whole scan with every backwards restart
//! disabled instead. That bounds the function from theAddress itself, which is always sound -- the guest
//! jumped there -- and cannot collide, since nothing contains theAddress and the retry never goes below it.
//! The only node removed is the one the failed attempt just made, via fn_8003F330 and only when its
//! pfCode == NULL, i.e. never any compiled code or call site.
#define COMBO_SCAN_FROM_ENTRY 1
bool fn_8003F330(Cpu* pCPU, CpuFunction* pFunction);

//! -1 normal, 1 while the retry pass is running. Initialised, so it lands in .data.
static s32 gComboScanFromEntry = -1;

#define COMBO_MAY_RESTART (!gIsOotmmCombo || gComboScanFromEntry < 0)
#define COMBO_RETRY_FROM_ENTRY(pCPU, nAddress, ppNode) \
    (gIsOotmmCombo && gComboScanFromEntry < 0 && comboRetryFromEntry((pCPU), (nAddress), (ppNode)))
#else
#define COMBO_NOT_BEFORE_ENTRY(current, start) true
#define COMBO_CLOSE_ON_INVALID(valid) false
#define COMBO_MAY_RESTART true
#define COMBO_RETRY_FROM_ENTRY(pCPU, nAddress, ppNode) false
#endif

#if IS_MM
//! Debug only. Every exit records why, so the update-fail logger in cpu_execute_update.c can say what
//! the scanner decided -- the one thing the SRR0=0/LR=0 crash dump cannot show. All initialised, so they
//! land in .data: this TU's split claims no .bss.
//! Reasons: 0 tree hit, 1 bounded and found, 4 treeInsert failed, 5 inserted range did not contain the
//! entry, 6 fell out of the scan loop.
s32 gComboFindAddr = -1;
s32 gComboFindReason = -1;
s32 gComboFindStart = -1;
s32 gComboFindEnd = -1;
s32 gComboFindStop = -1;
#define COMBO_FIND_NOTE(r, s, e)     \
    do {                             \
        gComboFindReason = (r);      \
        gComboFindStart = (s);       \
        gComboFindEnd = (e);         \
        gComboFindStop = current_address; \
    } while (0)
#else
#define COMBO_FIND_NOTE(r, s, e) (void)0
#endif

// Inlined into cpuFindFunction by MWCC, so it travels with it rather than staying behind in cpu.c.
static inline bool treeSearch(Cpu* pCPU, s32 target, CpuFunction** node) {
    CpuTreeRoot* root = pCPU->gTree;
    bool flag;

    if (target < root->root_address) {
        flag = treeSearchNode(root->left, target, node);
    } else {
        flag = treeSearchNode(root->right, target, node);
    }
    return flag;
}

#if IS_MM
//! Not in the original game. GCC compiles a `switch` to a jump table: `sll idx,idx,2; lui base;
//! (addiu base); addu ptr,base,idx; lw target,off(ptr); jr target`, with the table of case-block
//! addresses in .rodata. The scanner ends a function on any `jr` past its forward branches, so it
//! truncates such a function at the dispatch and every case block becomes a separate fragment --
//! compiled mid-function with the wrong register/stack context, and unrepresentable in the tree when
//! they overlap. That is the soulInfosMm switch crash.
//!
//! Retail (IDO) switches are NOT affected and must stay untouched: they carry a `sltiu at,idx,N;
//! beqz at,default` bound check whose `beqz` is a forward branch that already pushes anAddr[1] past the
//! `jr`, so the function never truncates there. Only a **data-driven** switch with no bound check (like
//! soulInfosMm, whose index comes from a data table) truncates. So this is applied only when the `jr`
//! would actually truncate (current > anAddr[1]) -- see the jr case -- which leaves every retail switch
//! exactly as before and rebounds only the data-driven ones over their case blocks. Validated offline
//! against OoT ntsc-1.0: of 213 `jr $reg`, the strict pattern matches 96, all real switches with a
//! valid table, and all 96 carry the `beqz` so none are touched by the gated call.
#define COMBO_JT_MAX_ENTRIES 64

//! The value materialised into nReg by a nearby `lui`(+`addiu`/`ori`) above nAtAddr, or 0 if none.
static s32 comboRegValue(CpuDevice** apDevice, u8* aiDevice, s32 nAtAddr, s32 nReg) {
    s32 nProbe;
    u32 nOp;
    s32 nLo;
    u8 bHaveLo;

    nLo = 0;
    bHaveLo = false;
    for (nProbe = nAtAddr - 4; nProbe >= nAtAddr - 0x60; nProbe -= 4) {
        if (!COMBO_IN_RAM(nProbe) || !CPU_DEVICE_GET32(apDevice, aiDevice, nProbe, &nOp)) {
            break;
        }
        if (MIPS_OP(nOp) == 0x0F && MIPS_RT(nOp) == nReg) { // lui nReg, hi
            return (MIPS_IMM_U16(nOp) << 16) + nLo;
        }
        if (!bHaveLo && MIPS_RT(nOp) == nReg && MIPS_RS(nOp) == nReg) {
            if (MIPS_OP(nOp) == 0x09) { // addiu
                nLo = MIPS_IMM_S16(nOp);
                bHaveLo = true;
            } else if (MIPS_OP(nOp) == 0x0D) { // ori
                nLo = MIPS_IMM_U16(nOp);
                bHaveLo = true;
            }
        }
    }
    return 0;
}

//! If nReg was set by `sll nReg, src, 2` (a scaled switch index) just above nAtAddr, the source
//! register src; otherwise -1.
static s32 comboScaledIndex(CpuDevice** apDevice, u8* aiDevice, s32 nAtAddr, s32 nReg) {
    s32 nProbe;
    u32 nOp;

    for (nProbe = nAtAddr - 4; nProbe >= nAtAddr - 0x20; nProbe -= 4) {
        if (!COMBO_IN_RAM(nProbe) || !CPU_DEVICE_GET32(apDevice, aiDevice, nProbe, &nOp)) {
            break;
        }
        if (MIPS_OP(nOp) == 0 && MIPS_FUNCT(nOp) == 0 && MIPS_RD(nOp) == nReg && MIPS_SA(nOp) == 2) {
            return MIPS_RT(nOp); // sll nReg, src, 2
        }
        if (MIPS_OP(nOp) == 0 && MIPS_FUNCT(nOp) == 8) { // a jr closes the window
            break;
        }
    }
    return -1;
}

//! The bound N of a `sltiu x, nSrcReg, N` above nAtAddr (the switch's range check), or 0 if none. Gives
//! the exact table size; without it the table is read until a non-conforming word.
static s32 comboSltiuBound(CpuDevice** apDevice, u8* aiDevice, s32 nAtAddr, s32 nSrcReg) {
    s32 nProbe;
    u32 nOp;

    for (nProbe = nAtAddr - 4; nProbe >= nAtAddr - 0x2C; nProbe -= 4) {
        if (!COMBO_IN_RAM(nProbe) || !CPU_DEVICE_GET32(apDevice, aiDevice, nProbe, &nOp)) {
            break;
        }
        if (MIPS_OP(nOp) == 0x0B && MIPS_RS(nOp) == nSrcReg) { // sltiu x, nSrcReg, N
            return MIPS_IMM_U16(nOp);
        }
    }
    return 0;
}

//! If the `jr nJrReg` at nJrAddr is a switch jump-table dispatch, the highest case-block address in the
//! table; otherwise 0. Requires the full idiom -- `lw target,off(ptr)`, `addu ptr,base,idx`, `base` from
//! a `lui`(+addiu), `idx` from a `sll _,_,2` -- so a plain indirect `jr` (a function pointer, no addu/sll)
//! never matches and cannot be mistaken for a table. The table size is the `sltiu` bound when present.
static s32 comboJumpTableMax(CpuDevice** apDevice, u8* aiDevice, s32 nJrAddr, s32 nJrReg, s32 nFuncStart) {
    u32 nLoad;
    u32 nAddu;
    u32 nEntry;
    s32 nPtrReg;
    s32 nBaseVal;
    s32 nIdxSrc;
    s32 nTable;
    s32 nBound;
    s32 nLimit;
    s32 nMax;
    s32 iCand;
    s32 i;

    // jr-4 must load the jr register: `lw nJrReg, off(nPtrReg)`.
    if (!CPU_DEVICE_GET32(apDevice, aiDevice, nJrAddr - 4, &nLoad) || MIPS_OP(nLoad) != 0x23 ||
        MIPS_RT(nLoad) != nJrReg) {
        return 0;
    }
    nPtrReg = MIPS_RS(nLoad);

    // jr-8 must be `addu nPtrReg, base, index`.
    if (!CPU_DEVICE_GET32(apDevice, aiDevice, nJrAddr - 8, &nAddu) || MIPS_OP(nAddu) != 0 ||
        MIPS_FUNCT(nAddu) != 0x21 || MIPS_RD(nAddu) != nPtrReg) {
        return 0;
    }

    // One operand of the addu is a lui-materialised table base, the other a `sll _,_,2` scaled index.
    nBaseVal = 0;
    nIdxSrc = -1;
    for (iCand = 0; iCand < 2; iCand++) {
        s32 nBaseReg = iCand == 0 ? MIPS_RS(nAddu) : MIPS_RT(nAddu);
        s32 nIdxReg = iCand == 0 ? MIPS_RT(nAddu) : MIPS_RS(nAddu);
        s32 nVal = comboRegValue(apDevice, aiDevice, nJrAddr, nBaseReg);
        s32 nSrc = comboScaledIndex(apDevice, aiDevice, nJrAddr, nIdxReg);

        if (nVal != 0 && nSrc >= 0) {
            nBaseVal = nVal;
            nIdxSrc = nSrc;
            break;
        }
    }
    if (nIdxSrc < 0) {
        return 0;
    }

    nTable = nBaseVal + MIPS_IMM_S16(nLoad);
    if (!COMBO_IN_RAM(nTable)) {
        return 0;
    }

    nBound = comboSltiuBound(apDevice, aiDevice, nJrAddr, nIdxSrc);
    nLimit = nBound != 0 && nBound <= COMBO_JT_MAX_ENTRIES ? nBound : COMBO_JT_MAX_ENTRIES;
    nMax = 0;
    for (i = 0; i < nLimit; i++) {
        if (!CPU_DEVICE_GET32(apDevice, aiDevice, nTable + i * 4, &nEntry)) {
            break;
        }
        // A real entry is a word-aligned case block of this function. The first non-conforming word ends
        // the table (only used when there is no sltiu bound).
        if ((nEntry & 3) != 0 || !COMBO_IN_RAM(nEntry) || (s32)nEntry < nFuncStart - 0x40 ||
            (s32)nEntry > nFuncStart + 0x2000) {
            break;
        }
        if ((s32)nEntry > nMax) {
            nMax = nEntry;
        }
    }
    return nMax;
}

//! Not in the original game, see COMBO_SCAN_FROM_ENTRY. Recursion depth 1: gComboScanFromEntry keeps the
//! retry from retrying.
static bool comboRetryFromEntry(Cpu* pCPU, s32 theAddress, CpuFunction** tree_node) {
    bool cpuFindFunction(Cpu* pCPU, s32 theAddress, CpuFunction** tree_node);
    bool bResult;

    gComboScanFromEntry = 1;
    bResult = cpuFindFunction(pCPU, theAddress, tree_node);
    gComboScanFromEntry = -1;
    return bResult;
}
#endif

bool cpuFindFunction(Cpu* pCPU, s32 theAddress, CpuFunction** tree_node) {
    CpuDevice** apDevice;
    u8* aiDevice;
    u32 opcode;
    u8 follow;
    u8 valid;
    u8 check;
    u8 end_flag;
    u8 save_restore;
    u8 alert;
    s32 beginAddress;
    s32 cheat_address;
    s32 current_address;
    s32 temp_address;
    s32 branch;
    int anAddr[3];
#if IS_MM
    //! Not in the original game, see COMBO_TAIL_CALL.
    u8 was_jump;
    u8 fetchFailed;
    s32 startAddress;
    //! Address of the most recent `jr $ra` at or after the requested entry, or 0 for none. See
    //! COMBO_CLOSE_AT_LAST_JR.
    s32 nLastJr;
    //! Not in the original game, see comboJumpTableMax. Per-function: a switch dispatch has been followed,
    //! so the function ends at the next prologue rather than merging into it.
    u8 sawJumpTable;
    s32 nJtMax;
#endif

    save_restore = false;
    alert = false;
    cheat_address = 0;
#if IS_MM
    was_jump = 0;
    fetchFailed = false;
    startAddress = theAddress;
    nLastJr = 0;
    sawJumpTable = false;
    //! Debug only, see COMBO_FIND_NOTE.
    gComboFindAddr = theAddress;
    gComboFindReason = -1;
#endif
    if (pCPU->gTree == NULL) {
        check = 0;
        pCPU->survivalTimer = 1;
        if (!xlHeapTake((void**)&pCPU->gTree, sizeof(CpuTreeRoot))) {
            return false;
        }
        treeInit(pCPU, 0x80150002);
    } else {
        check = 1;
        if (treeSearch(pCPU, theAddress, tree_node)) {
            pCPU->pFunctionLast = *tree_node;
#if IS_MM
            gComboFindReason = 0;
            gComboFindStart = (*tree_node)->nAddress0;
            gComboFindEnd = (*tree_node)->nAddress1;
#endif
            return true;
        }
    }

    anAddr[0] = 0;
    anAddr[1] = 0;
    anAddr[2] = 0;
    aiDevice = pCPU->aiDevice;
    apDevice = pCPU->apDevice;
    beginAddress = branch = theAddress;
    current_address = theAddress;

    do {
#if IS_MM
        //! Not in the original game, see COMBO_RAM_WINDOW. This test has to come before the fetch, both
        //! because the fetch cannot report the condition and because the fetch is what faults. Retail MM
        //! gives up on the whole lookup on a failed fetch, and that `return false` is what the
        //! recompiler's link stub turns into a jump to address 0; under the combo the scan instead closes
        //! the function at the last word it did read, below.
        if (gIsOotmmCombo && !COMBO_IN_RAM(current_address)) {
            fetchFailed = true;
        } else if (gIsOotmmCombo && nLastJr != 0 && (u32)(current_address - beginAddress) > COMBO_SCAN_CAP) {
            //! Not in the original game, see COMBO_CLOSE_AT_LAST_JR. Past this much code with no accepted
            //! terminator the scan is not bounding a function any more, it is reading whatever follows one.
            fetchFailed = true;
        } else if (!CPU_DEVICE_GET32(apDevice, aiDevice, current_address, &opcode)) {
            if (!gIsOotmmCombo) {
                return false;
            }
            fetchFailed = true;
        } else {
            fetchFailed = false;
        }

        //! One-instruction window: `j` sets was_jump, the top of the next iteration -- the delay
        //! slot -- promotes it to 2, and the iteration after that clears it.
        if (was_jump == 1) {
            was_jump = 2;
        } else {
            was_jump = 0;
        }
#else
        CPU_DEVICE_GET32(apDevice, aiDevice, current_address, &opcode);
#endif
        follow = true;

#if IS_MM
        //! Not in the original game, see comboJumpTableMax. anAddr[0] == 0 marks a fresh function segment;
        //! cold boot scans many functions in one call, so these per-function flags must reset with them.
        if (anAddr[0] == 0) {
            sawJumpTable = false;
        }
#endif

        if (check == 0) {
            if (opcode != 0 && anAddr[0] == 0) {
                anAddr[0] = current_address;
            }
        } else {
            if (anAddr[0] == 0) {
                anAddr[0] = current_address;
            }
        }

        valid = true;
        end_flag = 0;

#if IS_MM
        //! `else switch` rather than wrapping the switch in a block: the body below stays
        //! line-for-line comparable with cpu.c's copy.
        if (fetchFailed) {
            valid = false;
        } else
#endif
        switch ((u8)MIPS_OP(opcode)) {
            case 0x00: { // special
                switch ((u8)MIPS_FUNCT(opcode)) {
                    case 0x08: // jr
#if IS_MM
                        //! Not in the original game, see COMBO_CLOSE_AT_LAST_JR. Remembered whether or not
                        //! it is accepted as the end: a `jr $ra` past the requested entry is a place the
                        //! function can legally stop, and if the scan later turns out to be reading data,
                        //! the last one seen is the only sound place to close.
                        if (gIsOotmmCombo && MIPS_RS(opcode) == 31 && !save_restore &&
                            current_address >= startAddress) {
                            nLastJr = current_address;
                        }

                        //! Not in the original game, see comboJumpTableMax. Only when this `jr $reg` would
                        //! otherwise truncate the function (current past every forward branch) is it worth
                        //! testing for a switch dispatch -- that is exactly the data-driven case that has no
                        //! `beqz` bound check, i.e. the one that fragments. Retail switches keep anAddr[1]
                        //! ahead via their `beqz default`, so current <= anAddr[1] and this is skipped.
                        if (gIsOotmmCombo && MIPS_RS(opcode) != 31 && !save_restore &&
                            (anAddr[1] == 0 || current_address > anAddr[1]) &&
                            COMBO_NOT_BEFORE_ENTRY(current_address, startAddress)) {
                            nJtMax = comboJumpTableMax(apDevice, aiDevice, current_address, MIPS_RS(opcode),
                                                       anAddr[0]);
                            if (nJtMax > current_address) {
                                anAddr[1] = nJtMax;
                                sawJumpTable = true;
                                break;
                            }
                        }
#endif
                        if (!save_restore && (anAddr[1] == 0 || current_address > anAddr[1]) &&
                            (anAddr[2] == 0 || current_address >= anAddr[2]) &&
                            COMBO_NOT_BEFORE_ENTRY(current_address, startAddress)) {
                            end_flag = 111;
                        }
                        break;
                    case 0x0D: // break
                        if ((anAddr[1] == 0 || current_address > anAddr[1]) &&
                            (anAddr[2] == 0 || current_address >= anAddr[2]) &&
                            COMBO_NOT_BEFORE_ENTRY(current_address, startAddress)) {
                            end_flag = 111;
                            save_restore = false;
                        }
                        break;
                    default:
                        valid = SpecialOpcode[MIPS_FUNCT(opcode)];
                        break;
                }
                break;
            }
            case 0x02: // j
#if IS_MM
                //! Not in the original game, see COMBO_TAIL_CALL.
                if (gIsOotmmCombo) {
                    was_jump = 1;
                }
#endif
                if ((branch = (MIPS_TARGET(opcode) << 2) | (current_address & 0xF0000000)) >= current_address &&
                    branch - current_address <= 0x1000) {
                    if (anAddr[2] == 0) {
                        anAddr[2] = branch;
                    } else if (branch > anAddr[2]) {
                        anAddr[2] = branch;
                    }
                }
                break;
#if IS_MM
            case 0x03: // jal
                // A call as the very first instruction means we most likely started
                // scanning inside a function rather than at its entry.
                if (anAddr[0] == current_address) {
                    alert = true;
                }
                break;
#endif
            case 0x01: // regimm
                switch ((u8)MIPS_RT(opcode)) {
                    case 0x00: // bltz
                    case 0x01: // bgez
                    case 0x02: // bltzl
                    case 0x03: // bgezl
                    case 0x10: // bltzal
                    case 0x11: // bgezal
                    case 0x12: // bltzall
                    case 0x13: // bgezall
                        branch = MIPS_IMM_S16(opcode) * 4;
                        if (branch < 0) {
                            if (check == 1 && COMBO_MAY_RESTART && current_address + branch + 4 < beginAddress) {
                                anAddr[0] = 0;
                                anAddr[1] = 0;
                                anAddr[2] = 0;
                                current_address = beginAddress = current_address + branch + 4;
                                alert = true;
                                continue;
                            }
                        } else {
                            if (anAddr[1] == 0) {
                                anAddr[1] = current_address + branch;
                            } else if (current_address + branch > anAddr[1]) {
                                anAddr[1] = current_address + branch;
                            }
                        }
                        break;
                    default:
                        valid = RegimmOpcode[MIPS_RT(opcode)];
                        break;
                }
                break;
            case 0x04: // beq
            case 0x14: // beql
                branch = MIPS_IMM_S16(opcode) * 4;
                if (branch < 0) {
                    if (check == 1 && COMBO_MAY_RESTART && current_address + branch + 4 < beginAddress) {
                        anAddr[0] = 0;
                        anAddr[1] = 0;
                        anAddr[2] = 0;
                        current_address = beginAddress = current_address + branch + 4;
                        alert = true;
                        continue;
                    }

                    temp_address = current_address + 8;
                    CPU_DEVICE_GET32(apDevice, aiDevice, temp_address, &opcode);
                    if (opcode == 0) {
                        do {
                            temp_address += 4;
                            CPU_DEVICE_GET32(apDevice, aiDevice, temp_address, &opcode);
                        } while (opcode == 0);

                        if (MIPS_OP(opcode) != 0x23) { // lw
                            current_address = temp_address - 8;
                            if ((anAddr[1] == 0 || current_address > anAddr[1]) &&
                                (anAddr[2] == 0 || current_address >= anAddr[2])) {
                                end_flag = 111;
                                save_restore = false;
                            }
                        } else {
                            current_address = temp_address - 4;
                        }
                    }
                } else {
                    if (anAddr[1] == 0) {
                        anAddr[1] = current_address + branch;
                    } else if (current_address + branch > anAddr[1]) {
                        anAddr[1] = current_address + branch;
                    }
                }
                break;
            case 0x05: // bne
            case 0x06: // blez
            case 0x07: // bgtz
            case 0x15: // bnel
            case 0x16: // blezl
            case 0x17: // bgtzl
                branch = MIPS_IMM_S16(opcode) * 4;
                if (branch < 0) {
                    if (check == 1 && COMBO_MAY_RESTART && current_address + branch + 4 < beginAddress) {
                        anAddr[0] = 0;
                        anAddr[1] = 0;
                        anAddr[2] = 0;
                        current_address = beginAddress = current_address + branch + 4;
                        alert = true;
                        continue;
                    }
                } else {
                    if (anAddr[1] == 0) {
                        anAddr[1] = current_address + branch;
                    } else if (current_address + branch > anAddr[1]) {
                        anAddr[1] = current_address + branch;
                    }
                }
                break;
            case 0x10: // cop0
                switch ((u8)MIPS_FUNCT(opcode)) {
                    case 0x01: // tlbr
                    case 0x02: // tlbwi
                    case 0x05: // tlbwr
                    case 0x08: // tlbp
                        break;
                    case 0x18: // eret
                        if ((anAddr[1] == 0 || current_address > anAddr[1]) &&
                            (anAddr[2] == 0 || current_address >= anAddr[2])) {
                            end_flag = 222;
                            save_restore = false;
                        }
                        break;
                    default:
                        switch ((u8)MIPS_FMT(opcode)) {
                            case 0x08:
                                switch (MIPS_FT(opcode)) {
                                    case 0x00:
                                    case 0x01:
                                    case 0x02:
                                    case 0x03:
                                        branch = MIPS_IMM_S16(opcode) * 4;
                                        if (branch < 0) {
                                            if (check == 1 && COMBO_MAY_RESTART && current_address + branch + 4 < beginAddress) {
                                                anAddr[0] = 0;
                                                anAddr[1] = 0;
                                                anAddr[2] = 0;
                                                current_address = beginAddress = current_address + branch + 4;
                                                alert = true;
                                                continue;
                                            }
                                        } else {
                                            if (anAddr[1] == 0) {
                                                anAddr[1] = current_address + branch;
                                            } else if (current_address + branch > anAddr[1]) {
                                                anAddr[1] = current_address + branch;
                                            }
                                        }
                                        break;
                                }
                                break;
                        }
                        break;
                }
                break;
            case 0x11: // cop1
                if (MIPS_FMT(opcode) == 0x08) {
                    switch ((u8)MIPS_FT(opcode)) {
                        case 0x00:
                        case 0x01:
                        case 0x02:
                        case 0x03:
                            branch = MIPS_IMM_S16(opcode) * 4;
                            if (branch < 0) {
                                if (check == 1 && COMBO_MAY_RESTART && current_address + branch + 4 < beginAddress) {
                                    anAddr[0] = 0;
                                    anAddr[1] = 0;
                                    anAddr[2] = 0;
                                    current_address = beginAddress = current_address + branch + 4;
                                    alert = true;
                                    continue;
                                }
                            } else {
                                if (anAddr[1] == 0) {
                                    anAddr[1] = current_address + branch;
                                } else if (current_address + branch > anAddr[1]) {
                                    anAddr[1] = current_address + branch;
                                }
                            }
                            break;
                    }
                }
                break;
            case 0x2B: // sw
                if (MIPS_RT(opcode) == 31) {
                    save_restore = true;
                }
                break;
            case 0x23: // lw
                if (MIPS_RT(opcode) == 31) {
                    save_restore = false;
                    if (check == 1 && alert && COMBO_MAY_RESTART) {
                        //! Interior-entry recovery, kept identical to retail MM and to the GameCube
                        //! port: hunt backwards for the prologue's `sw ra`, then walk further back to a
                        //! word that is already inside a known function. The instruction after that is
                        //! the true entry.
                        while (true) {
                            CPU_DEVICE_GET32(apDevice, aiDevice, beginAddress, &opcode);
                            if (MIPS_OP(opcode) == 0x2B && MIPS_RT(opcode) == 31) { // sw ra, ...
                                break;
                            }
                            beginAddress -= 4;
                        }

                        do {
                            beginAddress -= 4;
                            CPU_DEVICE_GET32(apDevice, aiDevice, beginAddress, &opcode);
                            if (opcode != 0 && treeSearch(pCPU, beginAddress - 4, tree_node)) {
                                break;
                            }
#if IS_MM
                            // MM also inspects the instruction ahead of the candidate and
                            // stops on a padding word or on a jr.
                            CPU_DEVICE_GET32(apDevice, aiDevice, beginAddress - 4, &opcode);
                            if (opcode == 0) {
                                beginAddress -= 4;
                                break;
                            }
                            if (MIPS_OP(opcode) == 0 && MIPS_FUNCT(opcode) == 8) { // jr
                                break;
                            }
#endif
                        } while (opcode != 0);

                        anAddr[0] = 0;
                        anAddr[1] = 0;
                        anAddr[2] = 0;
                        current_address = beginAddress = beginAddress + 4;
                        alert = false;
                        continue;
                    }
                }
                break;
#if IS_MM
            //! Not in the original game, see COMBO_TAIL_CALL. `addiu sp,sp,imm` in the delay slot of
            //! a `j` is a tail call: the stack frame is torn down and control leaves for good, so the
            //! function ends here. Guarded exactly like the `jr` rule above, so a forward branch or a
            //! near `j` reaching past this point still keeps the scan going.
            case 0x09: // addiu
                if (gIsOotmmCombo && MIPS_RT(opcode) == 29 && MIPS_RS(opcode) == 29 && was_jump && !save_restore &&
                    (anAddr[1] == 0 || current_address > anAddr[1]) &&
                    (anAddr[2] == 0 || current_address >= anAddr[2]) && current_address >= startAddress) {
                    end_flag = 1;
                }
                //! Not in the original game, see comboJumpTableMax. After a followed jump table the case
                //! blocks run up to the next function; a frame-allocating `addiu sp,sp,-imm` past the last
                //! case block (anAddr[1]) is that next prologue, so end this function just before it.
                else if (gIsOotmmCombo && sawJumpTable && MIPS_RT(opcode) == 29 && MIPS_RS(opcode) == 29 &&
                         MIPS_IMM_S16(opcode) < 0 && current_address > anAddr[1] &&
                         current_address > startAddress) {
                    end_flag = 2;
                }
                break;
#endif
            default:
                valid = Opcode[MIPS_OP(opcode)];
                break;
        }

        if (end_flag != 0 || COMBO_CLOSE_ON_INVALID(valid)) {
#if IS_MM
            //! Not in the original game, see COMBO_TAIL_CALL. Retail lets an invalid opcode fall out of
            //! the loop and returns false; here the function is closed so the caller gets bounds it can
            //! compile instead of a jump to address 0.
            if (!valid) {
                //! Not in the original game, see COMBO_CLOSE_AT_LAST_JR. Closing where the decode broke
                //! (`current_address - 4`) hands cpuGetPPC a range with an ActorProfile and its tables in
                //! it, which it refuses; the last `jr $ra` at or after the entry is both compilable and a
                //! real exit. Falls back to the break point only when no `jr $ra` was ever seen.
                if (nLastJr != 0) {
                    anAddr[2] = nLastJr + 4;
                } else {
                    anAddr[2] = current_address - 4;
                }
            } else if (end_flag == 2) {
                //! Not in the original game, see comboJumpTableMax. End before the next function's prologue.
                anAddr[2] = current_address - 4;
            } else
#endif
            if (end_flag == 111) {
                anAddr[2] = current_address + 4;
                current_address += 8;
            } else {
                anAddr[2] = current_address;
                current_address += 4;
            }

            // Per-ROM function-boundary fixups. MM's cpuFindFunction has none of these.
#if !IS_MM
            if (check == 1) {
                if (CPU_SYSTEM(pCPU)->eTypeROM == NM8E) {
                    if (anAddr[2] == 0x802F1FF0) {
                        anAddr[0] = 0x802F1F50;
                    } else if (anAddr[2] == 0x80038308) {
                        anAddr[0] = 0x800382F0;
                    }
                } else if (CPU_SYSTEM(pCPU)->eTypeROM == NMFE) {
                    if (anAddr[2] == 0x8009E420) {
                        anAddr[0] = 0x8009E380;
                    }
                } else if (CPU_SYSTEM(pCPU)->eTypeROM == NMQE || CPU_SYSTEM(pCPU)->eTypeROM == NMQJ || CPU_SYSTEM(pCPU)->eTypeROM == NMQP) {
                    if (anAddr[0] == 0x802C88FC) {
                        anAddr[2] = 0x802C8974;
                    } else if (anAddr[0] == 0x802C8978) {
                        anAddr[2] = 0x802C8A5C;
                    } else if (anAddr[0] == 0x802C8A60) {
                        anAddr[2] = 0x802C8C60;
                    }
                }
            }
#endif

            if (!treeInsert(pCPU, anAddr[0], anAddr[2])) {
                //! Not in the original game, see COMBO_SCAN_FROM_ENTRY. treeInsertNode rejects a
                //! nAddress0 it already holds, whatever the end, so a function first entered before one of
                //! its intermediate epilogues gets the short bounds, and a later entry past that epilogue
                //! cannot be inserted at all. Nothing was inserted, so retrying costs nothing.
                if (COMBO_RETRY_FROM_ENTRY(pCPU, theAddress, tree_node)) {
                    return true;
                }
                COMBO_FIND_NOTE(4, anAddr[0], anAddr[2]);
                return false;
            }

            if (cheat_address != 0) {
                treeSearch(pCPU, cheat_address, tree_node);
                (*tree_node)->timeToLive = 0;
                cheat_address = 0;
            }

            if (check == 1) {
                if (treeSearch(pCPU, theAddress, tree_node)) {
                    pCPU->pFunctionLast = *tree_node;
                    COMBO_FIND_NOTE(1, (*tree_node)->nAddress0, (*tree_node)->nAddress1);
                    return true;
                }
#if IS_MM
                //! Not in the original game, see COMBO_SCAN_FROM_ENTRY. The insert landed but the entry is
                //! unreachable, so the range overlaps an older, smaller function and breaks the tree's
                //! keyed-on-nAddress0 navigation. Take the node we just made back out first -- it has no
                //! compiled code yet, so this frees nothing the guest could be inside -- then retry.
                if (gIsOotmmCombo) {
                    if (treeSearch(pCPU, anAddr[0], tree_node) && (*tree_node)->pfCode == NULL) {
                        fn_8003F330(pCPU, *tree_node);
                    }

                    if (COMBO_RETRY_FROM_ENTRY(pCPU, theAddress, tree_node)) {
                        return true;
                    }
                }
#endif
                COMBO_FIND_NOTE(5, anAddr[0], anAddr[2]);
                return false;
            }

            anAddr[0] = 0;
            anAddr[1] = 0;
            anAddr[2] = 0;
            follow = false;
            if (check == 0 && pCPU->gTree->total > 3970) {
                valid = false;
            }
        }

        if (follow) {
            current_address += 4;
        }
    } while (valid);

    if (check == 0) {
        treeInsert(pCPU, 0x80000180, 0x8000018C);
        treeSearch(pCPU, 0x80000180, tree_node);
        (*tree_node)->timeToLive = 0;

        if (treeSearch(pCPU, theAddress, tree_node)) {
            pCPU->pFunctionLast = *tree_node;
            COMBO_FIND_NOTE(1, (*tree_node)->nAddress0, (*tree_node)->nAddress1);
            return true;
        } else {
            COMBO_FIND_NOTE(5, anAddr[0], anAddr[2]);
            return false;
        }
    }

    COMBO_FIND_NOTE(6, anAddr[0], anAddr[2]);
    return false;
}
