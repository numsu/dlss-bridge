// D3D12 injected host: process and module scope for Direct3D 12 capture.
//
// Same suspended-process injection as the Vulkan host (the session launcher
// starts the target suspended, loads the hook DLL, calls its explicit
// initializer, then resumes). Only the interception scope differs:
//
//   * Vulkan host patches vulkan-1.dll imports and enables external-memory /
//     timeline-semaphore features.
//   * D3D12 host patches d3d12.dll / dxgi.dll imports in the application
//     executable, modules under its directory, and non-system modules reached
//     through their LoadLibrary calls -- the same bounded 60-second scan.
//
// NGX D3D12 modules are discovered through the same TryHookModule scan as the
// Vulkan ones (see dlss5-vk-bridge.cpp): every module EXCEPT the session-local
// OptiScaler module exporting NVSDK_NGX_D3D12_CreateFeature / EvaluateFeature
// gets a distinct detour, and a thread-local nesting depth arms the bridge on
// the outermost game call only. Skipping the session-local module is what keeps
// the private neural session's own NGX calls from re-entering the detours;
// a thread-local in-bridge guard covers the residual case where a game
// interposer forwards through the private module.
//
// The D3D12 device itself needs no hook: the create/evaluate entry points hand
// the bridge the game's ID3D12GraphicsCommandList, from which the game device
// is adopted via GetDevice.

#pragma once

// D3D12 host identity for the compiled component registry. The controller
// selects capture "ngx-d3d12" when the host provides D3D12Frames.
static const char *D3D12HostId() { return "injected-d3d12"; }

// D3D12 games resolve NGX through the same loader paths as Vulkan ones; no
// additional feature struct needs injecting. This hook exists so the component
// inventory has a stable implementation file distinct from hook_host.inc.
static void D3D12HostNoteInit()
{
    Log("[host] D3D12 injected host ready (module scope mirrors the Vulkan host; "
        "session-local OptiScaler excluded from hooking)");
}
