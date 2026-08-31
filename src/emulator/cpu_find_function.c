#include "emulator/comboPerf.h"
#include "emulator/cpu.h"
#include "emulator/mips.h"
#include "emulator/system.h"
#include "macros.h"

// OoTMM combo needs a handful of changes to the function-boundary scanner that
// retail's version cannot make; see COMBO_TAIL_CALL below.

bool treeInit(Cpu* pCPU, s32 root_address);
bool treeInsert(Cpu* pCPU, s32 start, s32 end);
bool treeSearchNode(CpuFunction* tree, s32 target, CpuFunction** node);
extern u8 Opcode[];
extern u8 SpecialOpcode[];
extern u8 RegimmOpcode[];

#if IS_MM
// cpuFindFunction bounds a function by scanning forward until a `jr` that no forward branch
// reaches past. That holds for IDO-compiled retail games, but not for OoTMM's GCC build: GCC
// reorders blocks past the epilogue and leaves for good through `j <other function>` tail calls, so
// the scan finds no terminator and walks off the end into overlay data until it dies on a bad
// opcode or unmapped memory, reported as a link stub jump to address 0.
//
// Fix, gated on the combo so retail MM is untouched:
//   - `addiu sp,sp,imm` in the delay slot of a `j` ends the function (case 0x09 below)
//   - an invalid opcode or failed fetch closes the function at the last decoded word instead of
//     returning false;
//   - end rules must not fire below the entry the caller asked for (startAddress).
// Interior entry (a jump into the middle of a function) still uses retail's own backwards hunt for
// a `sw ra` prologue.
#define COMBO_TAIL_CALL 1

// The tail-call rule above can't fire when a forward `j` blocks itself: retail records any forward
// `j` within 0x1000 as anAddr[2], and every end rule refuses to close before it, so
// `j <other function>` + `addiu sp,sp,+imm` pins anAddr[2] past itself and defeats its own guard
// (e.g. OoTMM's kaleido_scope hooks, which swallowed ~15 GCC functions into one node). Fix: a
// frame-tearing forward `j` no longer extends the function, so the tail-call rule above closes it
// at the `j`.
#define COMBO_TAIL_CALL_TARGET 1

// COMBO_TAIL_CALL only recognises a `j` whose delay slot tears a frame down. A frameless sibling
// call (`j Foo` / `nop`) is what GCC emits first, and nothing closed the function on it (the
// Farore's Wind crash). Rule: a `j` whose target is below anAddr[0] leaves the region for good, so
// the function ends after its delay slot. Backwards only: a far forward `j` can still be an
// intra-function long branch, and truncating there would be worse than the over-merge it prevents.
#define COMBO_TAIL_CALL_BACK 1

// bCombo is cpuFindFunction's local copy of gIsOotmmCombo. MWCC can't keep the global in a
// register across the CPU_DEVICE_GET32 calls between reads, so the local avoids reloading it.
#define COMBO_NOT_BEFORE_ENTRY(current, start) (!bCombo || (current) >= (start))
#define COMBO_CLOSE_ON_INVALID(valid) (bCombo && !(valid))

// ramGet32 returns *pData = 0 and true for anything past pRAM->nSize, and a zero word decodes as a
// valid nop, so a scan that leaves RDRAM would otherwise read an endless run of "legal" nops. 8 MB
// because the combo forces the Expansion Pak; KSEG0 because cpu_execute_jump.c folds aliases before
// the lookup.
#define COMBO_RAM_WINDOW 1
#define COMBO_IN_RAM(nAddress) ((u32)(nAddress) >= 0x80000000 && (u32)(nAddress) < 0x80800000)

// Where to close a scan that has run off a function's end into an ActorProfile's action tables,
// which decode as valid opcodes with forward branches that keep the scan going: close at the last
// `jr $ra` seen at or after the requested entry, since that point is inside real code and a genuine
// exit. COMBO_SCAN_CAP backstops a run of legal nops that never hits an invalid word.
//
// nLastJr uses 0 as "none", not a negative sentinel: a KSEG0 address is negative as s32.
#define COMBO_CLOSE_AT_LAST_JR 1
// Bytes past beginAddress with no accepted terminator before a runaway is presumed. Must exceed the
// largest real function. FileSelect_CustomFileInfoDraw is 0x348C bytes.
#define COMBO_SCAN_CAP 0x8000

// The function tree can't represent overlapping ranges (treeInsertNode keys on nAddress0,
// treeSearchNode navigates by containment), so a small fragment sitting inside a later-discovered
// larger function makes the larger one unreachable. Happens when a jump into the middle of a
// function (a switch case reached via jump table) creates a fragment before the enclosing bounds
// are known.
//
// NEVER repair this by deleting compiled functions here: treeDeleteNode runs treeCallerCheck, which
// reverts patched call sites aimed at a deleted function. Instead retry the whole scan with
// backwards restart disabled, bounding the function from theAddress itself (always sound, since the
// guest jumped there).
#define COMBO_SCAN_FROM_ENTRY 1
bool fn_8003F330(Cpu* pCPU, CpuFunction* pFunction);

static s32 gComboScanFromEntry = -1; // -1 normal, 1 while the retry pass is running

#define COMBO_MAY_RESTART (!bCombo || gComboScanFromEntry < 0)
#define COMBO_RETRY_FROM_ENTRY(pCPU, nAddress, ppNode) \
    (bCombo && gComboScanFromEntry < 0 && comboRetryFromEntry((pCPU), (nAddress), (ppNode)))
#else
#define COMBO_NOT_BEFORE_ENTRY(current, start) true
#define COMBO_CLOSE_ON_INVALID(valid) false
#define COMBO_MAY_RESTART true
#define COMBO_RETRY_FROM_ENTRY(pCPU, nAddress, ppNode) false
#endif

#if IS_MM
// Debug only. Every exit records why, so the update-fail logger in cpu_execute_update.c can say
// what the scanner decided.
// Reasons: 0 tree hit, 1 bounded and found, 4 treeInsert failed, 5 inserted range did not contain the
// entry, 6 fell out of the scan loop.
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
// GCC compiles a `switch` to a jump table, and the scanner's rule of ending a function on any `jr`
// past its forward branches truncates it at the dispatch, splitting every case block into its own
// fragment (the soulInfosMm switch crash). Retail IDO switches are unaffected: their bound check is
// a forward branch that already pushes anAddr[1] past the `jr`. Only a data-driven switch with no
// bound check truncates.
#define COMBO_JT_MAX_ENTRIES 64

// The "next prologue ends the function" rule above assumes `addiu sp,sp,-imm` can only start a
// function. GCC breaks that by defering the frame setup past a leaf switch, so the prologue can sit
// just after the case blocks merge, still inside the same function. Closing there ends the block on
// a non-branch and the host code runs off the end of its buffer into uninitialised code heap. Only close at a prologue control cannot fall into
#define COMBO_JT_PROLOGUE 1

// The value materialised into nReg by a nearby `lui`(+`addiu`/`ori`) above nAtAddr, or 0 if none.
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

// If nReg was set by `sll nReg, src, 2` (a scaled switch index) just above nAtAddr, the source
// register src; otherwise -1.
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

// The bound N of a `sltiu x, nSrcReg, N` above nAtAddr (the switch's range check), or 0 if none.
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

// Is the `j` at nJumpAddr a tail call, i.e. does its delay slot tear the stack frame down with
// `addiu sp,sp,+imm`? See COMBO_TAIL_CALL_TARGET.
static bool comboIsTailCall(CpuDevice** apDevice, u8* aiDevice, s32 nJumpAddr) {
    u32 nSlot;

    if (!COMBO_IN_RAM(nJumpAddr + 4) || !CPU_DEVICE_GET32(apDevice, aiDevice, nJumpAddr + 4, &nSlot)) {
        return false;
    }

    return (nSlot & 0xFFFF0000) == 0x27BD0000 && MIPS_IMM_S16(nSlot) > 0;
}

// Does nOp leave the block for good, so the word two below it starts fresh? `jal`/`bgezal` come
// back, so they do not count.
static bool comboIsTransfer(u32 nOp) {
    switch ((u8)MIPS_OP(nOp)) {
        case 0x00: // special
            return MIPS_FUNCT(nOp) == 0x08; // jr
        case 0x02: // j
            return true;
        case 0x04: // beq; `b` is beq with equal registers
        case 0x14: // beql
            return MIPS_RS(nOp) == MIPS_RT(nOp);
        case 0x10: // cop0
            return nOp == 0x42000018; // eret
        default:
            return false;
    }
}

// True when control can fall into nAddress from the word above, which a real function entry never
// can: the previous function leaves through a transfer at nAddress - 8 with its delay slot at
// nAddress - 4, or the gap above is nop padding
static bool comboFallsInto(CpuDevice** apDevice, u8* aiDevice, s32 nAddress) {
    u32 nOp;
    s32 nProbe;

    // Skip a run of padding nops
    nOp = 0;
    for (nProbe = nAddress - 4; nProbe >= nAddress - 0x20; nProbe -= 4) {
        if (!COMBO_IN_RAM(nProbe) || !CPU_DEVICE_GET32(apDevice, aiDevice, nProbe, &nOp)) {
            return false;
        }
        if (nOp != 0) {
            break;
        }
    }

    if (nOp == 0 || comboIsTransfer(nOp)) {
        return false;
    }

    // nProbe holds a delay slot or ordinary code; the transfer, if any, is the word below it.
    if (!COMBO_IN_RAM(nProbe - 4) || !CPU_DEVICE_GET32(apDevice, aiDevice, nProbe - 4, &nOp)) {
        return false;
    }
    return !comboIsTransfer(nOp);
}

// If the `jr nJrReg` at nJrAddr is a switch jump-table dispatch, the highest case-block address in
// the table; otherwise 0. Requires the full idiom, so a plain indirect `jr` never matches.
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

    COMBO_PERF_BUMP(nJtProbes);

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

// Recursion depth 1: gComboScanFromEntry keeps the retry from retrying.
static bool comboRetryFromEntry(Cpu* pCPU, s32 theAddress, CpuFunction** tree_node) {
    bool cpuFindFunction(Cpu* pCPU, s32 theAddress, CpuFunction** tree_node);
    bool bResult;

    COMBO_PERF_BUMP(nFindRetries);
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
    u8 was_jump; // see COMBO_TAIL_CALL
    u8 fetchFailed;
    s32 startAddress;
    s32 nLastJr; // most recent `jr $ra` at or after the entry, or 0; see COMBO_CLOSE_AT_LAST_JR
    // Per-function: a switch dispatch has been followed, so the function ends at the next prologue
    // rather than merging into it. See comboJumpTableMax.
    u8 sawJumpTable;
    s32 nJtMax;
    const bool bCombo = gIsOotmmCombo;
#endif

    save_restore = false;
    alert = false;
    cheat_address = 0;
#if IS_MM
    COMBO_PERF_BUMP(nFinds);
    was_jump = 0;
    fetchFailed = false;
    startAddress = theAddress;
    nLastJr = 0;
    sawJumpTable = false;
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
        COMBO_PERF_BUMP(nFindInsns);

        // See COMBO_RAM_WINDOW. Must come before the fetch, which is what faults; the combo closes
        // the function at the last word read instead of returning false.
        if (bCombo && !COMBO_IN_RAM(current_address)) {
            fetchFailed = true;
        } else if (bCombo && nLastJr != 0 && (u32)(current_address - beginAddress) > COMBO_SCAN_CAP) {
            fetchFailed = true;
        } else if (!CPU_DEVICE_GET32(apDevice, aiDevice, current_address, &opcode)) {
            if (!bCombo) {
                return false;
            }
            fetchFailed = true;
        } else {
            fetchFailed = false;
        }

        // One-instruction window: `j` sets was_jump, the delay slot promotes it to 2, and the
        // iteration after that clears it.
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
        // anAddr[0] == 0 marks a fresh function segment; cold boot scans many functions in one
        // call, so sawJumpTable must reset with them.
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
        // `else switch` rather than wrapping the switch in a block: the body below stays
        // line-for-line comparable with cpu.c's copy.
        if (fetchFailed) {
            valid = false;
        } else
#endif
        switch ((u8)MIPS_OP(opcode)) {
            case 0x00: { // special
                switch ((u8)MIPS_FUNCT(opcode)) {
                    case 0x08: // jr
#if IS_MM
                        // A `jr $ra` past the requested entry is a legal stopping point; see
                        // COMBO_CLOSE_AT_LAST_JR.
                        if (bCombo && MIPS_RS(opcode) == 31 && !save_restore &&
                            current_address >= startAddress) {
                            nLastJr = current_address;
                        }

                        // Only worth testing for a switch dispatch when this `jr $reg` would otherwise
                        // truncate the function. See comboJumpTableMax.
                        if (bCombo && MIPS_RS(opcode) != 31 && !save_restore &&
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
                if (bCombo) { // see COMBO_TAIL_CALL
                    was_jump = 1;
                }
#endif
                if ((branch = (MIPS_TARGET(opcode) << 2) | (current_address & 0xF0000000)) >= current_address &&
                    branch - current_address <= 0x1000) {
#if IS_MM
                    // A forward `j` whose delay slot tears the frame down is a tail call, so its target
                    // belongs to another function and must not extend this one. See COMBO_TAIL_CALL_TARGET.
                    if (!bCombo || !comboIsTailCall(apDevice, aiDevice, current_address))
#endif
                    {
                        if (anAddr[2] == 0) {
                            anAddr[2] = branch;
                        } else if (branch > anAddr[2]) {
                            anAddr[2] = branch;
                        }
                    }
                }
#if IS_MM
                // A `j` out of the region backwards is a frameless tail call and ends the function.
                // See COMBO_TAIL_CALL_BACK.
                else if (COMBO_TAIL_CALL_BACK && bCombo && branch < anAddr[0] && !save_restore &&
                         (anAddr[1] == 0 || current_address > anAddr[1]) &&
                         (anAddr[2] == 0 || current_address >= anAddr[2]) && current_address >= startAddress) {
                    end_flag = 111;
                }
#endif
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
                        // Interior-entry recovery: hunt backwards for the prologue's `sw ra`, then to
                        // a word already inside a known function. The instruction after that is the
                        // true entry.
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
            // `addiu sp,sp,imm` in the delay slot of a `j` is a tail call: the frame is torn down and
            // control leaves for good, so the function ends here. See COMBO_TAIL_CALL.
            case 0x09: // addiu
                if (bCombo && MIPS_RT(opcode) == 29 && MIPS_RS(opcode) == 29 && was_jump && !save_restore &&
                    (anAddr[1] == 0 || current_address > anAddr[1]) &&
                    (anAddr[2] == 0 || current_address >= anAddr[2]) && current_address >= startAddress) {
                    end_flag = 1;
                }
                // After a followed jump table the case blocks run up to the next function; a
                // frame-allocating `addiu sp,sp,-imm` past the last case block is that next prologue.
                // See comboJumpTableMax.
                else if (bCombo && sawJumpTable && MIPS_RT(opcode) == 29 && MIPS_RS(opcode) == 29 &&
                         MIPS_IMM_S16(opcode) < 0 && current_address > anAddr[1] &&
                         current_address > startAddress &&
                         !comboFallsInto(apDevice, aiDevice, current_address)) {
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
            // Retail lets an invalid opcode fall out of the loop and returns false; here the function
            // is closed so the caller gets bounds it can compile instead of a jump to address 0.
            if (!valid) {
                // See COMBO_CLOSE_AT_LAST_JR.
                if (nLastJr != 0) {
                    anAddr[2] = nLastJr + 4;
                } else {
                    anAddr[2] = current_address - 4;
                }
            } else if (end_flag == 2) {
                anAddr[2] = current_address - 4; // end before the next function's prologue; see comboJumpTableMax
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
                // See COMBO_SCAN_FROM_ENTRY. Nothing was inserted, so retrying costs nothing.
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
                // The insert landed but the entry is unreachable, so the range overlaps an older,
                // smaller function. Take the node we just made back out first (it has no compiled
                // code yet), then retry. See COMBO_SCAN_FROM_ENTRY.
                if (bCombo) {
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
