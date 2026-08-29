#pragma once

namespace panodbg
{

// The Vulkan half of the same job as window_d3d.h. CS2's tools launcher forces
// Vulkan (-gpuraytracing wins over -dx11), where there is no ID3D11Device and no
// OMSetRenderTargets to observe. Both backends implement RenderDevice003, so the
// frame boundary -- Present at vtable slot 16 -- is shared.

// True when the loaded render system is the Vulkan one.
bool VkOwnWindowIsActive();

// Reads the game's Vulkan handles from fixed offsets (see the .cpp) and starts
// watching render-target creation. Not by scanning and probing:
// vkGetDeviceProcAddr on a candidate makes the loader dereference it as a
// dispatch table, so a wrong guess faults instead of being refused.
bool VkOwnWindowInitialize(void* source2RenderDevice);

// Creates or recreates our surface and swapchain on the game's device.
bool VkOwnWindowCreateSwapChain(void* hwnd, int width, int height);

// Hooks CRenderDeviceBase::Present (vtable slot 16), the same slot the D3D11
// backend uses, and draws our window from there.
bool VkOwnWindowInstallDevicePresentHook(void* source2RenderDevice);

// Drops the surface and swapchain but keeps watching render-target creation, so
// a reopened debugger still recognises a target allocated during the first.
void VkOwnWindowCloseWindow();

// Releases everything and removes every hook. Must run before the DLL unloads.
void VkOwnWindowShutdown();

} // namespace panodbg
