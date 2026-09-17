# Diagnosing frame pacing

The bridge log is written to the per-launch session directory as `dlss5-vk-bridge.log`. The launch command keeps logs under `~/.local/state/dlss-bridge/logs/<session-id>/` when the game closes. For one useful run, include a stationary scene, look toward a distant area, then move through it for at least 30 seconds. Mark the approximate time when a visible stutter occurs. All log timestamps use the game's local clock, and bridge frame numbers correlate records across threads.

The diagnostic build records slow events, rather than every frame:

- `[pace] game NGX` reports the interval between calls to the game's DLSS feature. It provides a pre-DLSS clock even when present interception is unavailable. A gap here without a nearby bridge stall suggests the game produced the frame late.
- `[pace]` reports `vkQueuePresentKHR` intervals and time spent inside the present call. A long interval with no nearby bridge stall points toward game rendering, asset streaming, or another pre-present process. A long present call points toward swapchain/display pacing. A first-present entry confirms interception is active; if absent, the Vulkan present function may have been resolved outside the injected hook's observable paths.
- `[present-gate]` reports time deliberately spent outside the display driver while a submitted bridge worker finishes. The game's Vulkan command buffer already waits for that result. Keeping the equivalent host wait outside `vkQueuePresentKHR` prevents the presentation path and the private D3D12 recorder from forming a driver-lock cycle. A sustained multi-second gate wait means the producer itself is blocked and should be correlated with the worker-stage records.
- `[stall] game-thread NGX evaluate` divides time between the game's NGX call and bridge frame capture. In neural-only mode, the game call is skipped after latching. A slow capture points at Vulkan input recording or frame-slot management.
- `[vk-submit]` reports a call that blocks the game thread in Vulkan submission. `matched-slots` and `frame` show whether it carries the bridge's work.
- `[stall] frame ... handoff` divides worker time into waiting for Vulkan input copies, private evaluate/transfer, and output-fence completion. A slow input span with a blocked matched submit suggests game GPU scheduling; a slow evaluate span should be compared with OptiScaler/NGX and D3D12 fence logs.
- `[stall] OptiScaler/NGX EvaluateFeature` measures the CPU time spent inside the private D3D12 NGX/OptiScaler entry point. If it coincides with a long handoff, the problem is in that component's CPU path; GPU timestamps alone cannot reveal it.
- `[stall] D3D12 ...` shows command submission, allocator retirement, and fence waits. Fence targets and completed values separate slow GPU retirement from slow CPU command recording.
- `[profile]` summarizes fresh output rate, repeats, output age, and neural/GPU timing percentiles. `[pace]` summarizes every 128 presents, including counts of intervals over 25 and 50 ms. A repeat is a missed fresh result, not necessarily a delayed presentation.

Slow-event thresholds are 8 ms for key CPU stages, 20 ms for the complete handoff, and 50 ms for an individual present interval. Ordinary 30 FPS intervals are summarized instead of logged per frame. Logging uses the existing bounded bridge log, so a diagnostic run does not create a per-frame trace file.

These records identify the stage that introduces the gap. They cannot name a source line inside a closed OptiScaler or driver binary; a second, targeted experiment may be needed after the stage is identified.
