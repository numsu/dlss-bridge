// Business-logic test for the DLSS-off idle-notice gate: the bridge logs its
// one-shot "DLSS appears disabled" hint only after enough rendering activity
// (Vulkan presents, D3D12 submissions) with zero successful game creates.
#include "dlss_off_gate.h"
#include <assert.h>
#include <stdio.h>

int main()
{
    // Silent before the threshold: process start and loader stalls never trip.
    assert(!DlssOffShouldLog(0, 0, 0));
    assert(!DlssOffShouldLog(1, 0, 0));
    assert(!DlssOffShouldLog(kDlssOffActivityThreshold - 1, 0, 0));
    // Fires exactly at the threshold with no creates and no prior log.
    assert(DlssOffShouldLog(kDlssOffActivityThreshold, 0, 0));
    assert(DlssOffShouldLog(kDlssOffActivityThreshold + 4096, 0, 0));
    // Any successful game create (SR, RR, or FG passthrough) suppresses it.
    assert(!DlssOffShouldLog(kDlssOffActivityThreshold, 1, 0));
    assert(!DlssOffShouldLog(1ULL << 40, 7, 0));
    // One-shot: a prior firing never repeats, even with more activity.
    assert(!DlssOffShouldLog(kDlssOffActivityThreshold, 0, 1));
    assert(!DlssOffShouldLog(1ULL << 40, 0, 1));
    assert(!DlssOffShouldLog(0, 0, 1));
    printf("dlss-off-gate OK (threshold=%llu)\n", kDlssOffActivityThreshold);
    return 0;
}
