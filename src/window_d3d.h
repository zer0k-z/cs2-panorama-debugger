#pragma once

namespace panodbg
{

// The D3D11 half of getting the debugger's pixels into its own OS window. Dota
// renders its debugger as a scene view with "outputcolor" bound to its own
// swapchain; engine2 has no such path, and its panorama views are drawn only
// from inside CGameUIService, which is not safe to run twice in a frame. So:
// observe the render target panorama binds, copy it at the frame boundary.

// Finds the game's ID3D11Device inside the Source2 render device (a COM pointer
// whose vtable lives in d3d11.dll, confirmed by QueryInterface). Cheap to repeat.
bool D3DOwnWindowInitialize(void* source2RenderDevice);

// Creates or recreates the swapchain for hwnd. Called again on every resize.
bool D3DOwnWindowCreateSwapChain(void* hwnd, int width, int height);

// Observes OMSetRenderTargets (COM vtable index 33), recording bound targets
// deduped by size and format. Substitution does not work: panorama's draws do
// not run on the thread that runs the layer render, and a global redirect
// hijacks every thread's target and kills the display driver.
bool D3DOwnWindowInstallObserveHook();

// Hooks CRenderDeviceBase::Present (vtable slot 16) and copies after the game's
// own present. Not IDXGISwapChain::Present: the Steam overlay shares that vtable
// and re-patches it on every swapchain recreation, and the two then recurse
// until the stack overflows. The frame boundary itself is required.
bool D3DOwnWindowInstallDevicePresentHook(void* source2RenderDevice);

// Removes both hooks and releases everything. Must run before the DLL unloads.
void D3DOwnWindowShutdown();

} // namespace panodbg
