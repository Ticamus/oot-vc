#include "emulator/cpu.h"
#include "macros.h"

// Splitted out to fix a stale root->restore pointer (VC Crash 2)

bool treeAdjustRoot(Cpu* pCPU, s32 new_start, s32 new_end);
bool treeKillRange(Cpu* pCPU, CpuFunction* tree, s32 start, s32 end);

#define COMBO_FIX_STALE_RESTORE 1

bool cpuDMAUpdateFunction(Cpu* pCPU, s32 start, s32 end) {
    CpuTreeRoot* root = pCPU->gTree;
    s32 count;
    bool cancel;

    if (root == NULL) {
        return true;
    }

    if ((start < root->root_address) && (end > root->root_address)) {
        treeAdjustRoot(pCPU, start, end);
    }

    if (root->kill_limit != 0) {
        if (root->restore != NULL) {
#if COMBO_FIX_STALE_RESTORE
            cancel = (start <= root->restore->nAddress1) && (end >= root->restore->nAddress0);
#else
            cancel = false;
            if (start <= root->restore->nAddress0) {
                if ((end >= root->restore->nAddress1) || (end >= root->restore->nAddress0)) {
                    cancel = true;
                }
            } else {
                if ((end >= root->restore->nAddress1) &&
                    ((start <= root->restore->nAddress0) || (start <= root->restore->nAddress1))) {
                    cancel = true;
                }
            }
#endif
            if (cancel) {
                root->restore = NULL;
                root->restore_side = 0;
            }
        }
    }

    if (start < root->root_address) {
        do {
            count = treeKillRange(pCPU, root->left, start, end);
            root->total = root->total - count;
        } while (count != 0);
    } else {
        do {
            count = treeKillRange(pCPU, root->right, start, end);
            root->total = root->total - count;
        } while (count != 0);
    }

    return true;
}
