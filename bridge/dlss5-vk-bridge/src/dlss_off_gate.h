// One-shot gate for the "DLSS not enabled in game" idle notice.
//
// The bridge stays inert when the game never creates an NGX feature (DLSS off
// in the game's graphics settings): there is nothing to mirror, so no neural
// work runs. After enough observed rendering activity (Vulkan presents,
// D3D12 game-queue submissions) with zero successful game creates, the bridge
// logs a single hint telling the user to enable DLSS. The check is pure and
// portable so the same header compiles into the Windows bridge and the Linux
// unit test.
#pragma once

// ~8 s of rendering at 60 fps (D3D12 usually submits 1-3 lists per frame, so
// its covering fence reaches this in the same order of time). Firing on
// activity rather than wall-clock time keeps the notice silent during process
// start and loader stalls: presents/submissions prove frames are flowing.
static const unsigned long long kDlssOffActivityThreshold = 512ULL;

inline bool DlssOffShouldLog(unsigned long long activity, long creates_observed,
                             long already_logged)
{
    return !already_logged && creates_observed == 0 &&
           activity >= kDlssOffActivityThreshold;
}
