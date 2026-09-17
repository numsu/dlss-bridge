// D3D11 injected host: process and module scope for Direct3D 11 capture.
//
// Same suspended-process injection as the Vulkan host (the session launcher
// starts the target suspended, loads the hook DLL, calls its explicit
// initializer, then resumes). Only the interception scope differs:
//
//   * Vulkan host patches vulkan-1.dll imports and enables external-memory /
//     timeline-semaphore features.
//   * D3D11 host patches d3d11.dll / dxgi.dll imports in the application
//     executable, modules under its directory, and non-system modules reached
//     through their LoadLibrary calls -- the same bounded 60-second scan.
//
// NGX D3D11 modules are discovered through the same TryHookModule scan as the
// Vulkan ones (see dlss5-vk-bridge.cpp): every module exporting
// NVSDK_NGX_D3D11_CreateFeature / EvaluateFeature gets a distinct detour, and
// a thread-local nesting depth arms the bridge on the outermost game call
// only. The D3D11 device itself needs no hook: the evaluate entry point hands
// the bridge the game's ID3D11DeviceContext, which is sufficient for inline
// copy / evaluate / copy-back through shared NT handles.

#pragma once

// D3D11 host identity for the compiled component registry. The controller
// selects capture "d3d11" when the host provides D3D11Frames.
static const char *D3D11HostId() { return "injected-d3d11"; }

// D3D11 games resolve NGX through the same loader paths as Vulkan ones; no
// additional feature struct needs injecting. This hook exists so the component
// inventory has a stable implementation file distinct from hook_host.inc.
static void D3D11HostNoteInit()
{
    Log("[host] D3D11 injected host ready (module scope mirrors the Vulkan host)");
}
