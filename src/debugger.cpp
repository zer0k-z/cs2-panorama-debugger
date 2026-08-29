#include "debugger.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/console.h"
#include "core/safemem.h"
#include "platform/module.h"
#include "platform/vtable_hook.h"
#include "window_d3d.h"
#include "window_input.h"
#include "window_vulkan.h"

namespace panodbg
{

namespace
{

// --- Offsets, slots and signatures ---------------------------------------
//
// Everything a game update can move. Each entry says what it is and how to find
// it again.

// panorama::CUIEngineSource2, vtable ??_7CUIEngineSource2@panorama@@6B@.
constexpr size_t kEngineRunFrame = 5; // VProf string "CUIEngine::RunFrame"

// CreateNewOffscreenUIWindow(w, h, name, InputContextHandle_t, bDrawToBackBuffer).
// Offscreen, not the UI-layer creator at slot 9: the last argument reaches
// surface+88, and the render path uses
//     surface+82 = panorama_use_backbuffer_directly && surface+88
// Slot 9 hardcodes that flag to 1, so with it the only off-backbuffer target
// comes from forcing the convar globally, which breaks the game's own UI.
constexpr size_t kEngineCreateOffscreenWindow = 10;

// CUIEngine::m_vecWindows. Used only to find a live window's input object.
constexpr size_t kEngineWindowCount = 384;
constexpr size_t kEngineWindowData = 392;

// panorama::CTopLevelWindowSource2, vtable
// ??_7CTopLevelWindowSource2@panorama@@6B@.
constexpr size_t kWindowSetScaleFactor = 7;   // SetWindowScaleFactor(float)
constexpr size_t kWindowGetSurfaceWidth = 12; // *(uint32*)(this + 48)
constexpr size_t kWindowGetSurfaceHeight = 13;
constexpr size_t kWindowOnWindowResize = 15; // (uint32 width, uint32 height)
constexpr size_t kWindowActivate = 18;       // (bool sendInput)
constexpr size_t kWindowIsVisible = 44;      // surface != 0 && *(this + 361) == 0
constexpr size_t kWindowSetVisible = 45;     // *(this + 361) = !arg
constexpr size_t kWindowSetPlatWindow = 56;  // *(this + 408) = arg

// Slot 7, not slot 55: slot 7 writes window+88, dirties the surface and marks
// every panel for relayout; slot 55 writes an unrelated field at +92 and
// relayouts nothing. Applying the scale is what re-lays-out a window --
// OnWindowResize alone changes the sizes and leaves the panels where they were.

// CUIWindowInput at window+64. Slot 2 writes the float mouse position at
// +116/+120, which the hit test reads; slot 1 writes a different pair.
constexpr size_t kWindowInput = 64;
constexpr size_t kInputSetMousePosition = 2;

// CSource2Surface, stored by BInitializeSurface at both +112 and +352.
constexpr size_t kWindowSurface = 112;

// The flags Dota's engine2 passes to Plat_CreateWindow for its own debugger.
constexpr uint32_t kDebuggerWindowFlags = 1079;
constexpr int kDebuggerWindowWidth = 1920;
constexpr int kDebuggerWindowHeight = 800;

constexpr const char* kDebuggerPanelId = "panorama_debugger";
constexpr const char* kDebuggerViewName = "PanoramaDebugger";

// engine2 CGameUIService (GameUIService_001), slot 33:
// AddPanoramaView(viewName, window, priority, fullscreen). A window that is not
// in m_Views is never drawn. flagA is the sort priority -- m_Views is a sorted
// insert -- and flagB picks fullscreen (1) over a fixed 800px height (0).
constexpr size_t kGameUIServiceAddPanoramaView = 33;
constexpr int kViewPriority = 0;
constexpr int kViewFullscreen = 1;

// CPanoramaEngineHandler, a static in engine2 with no accessor. Unique lea in
// CGameUIService::AddPanoramaView:
//   movzx eax,[rsp+60h] / lea rcx,<handler> / movzx r9d,bl
constexpr uint8_t kHandlerPattern[] = {0x0F, 0xB6, 0x44, 0x24, 0x60, 0x48, 0x8D, 0x0D,
									   0x00, 0x00, 0x00, 0x00, 0x44, 0x0F, 0xB6, 0xCB};
constexpr const char* kHandlerMask = "xxxxxxxx????xxxx";
constexpr size_t kHandlerDispOffset = 8;
constexpr size_t kHandlerNextInsn = 12;

// m_vecWindowInputOrder. AddPanoramaView also joins the window to engine2's
// input dispatch, whatever context it was created with, and engine2 offers each
// window the event until one handles it -- so a click on empty space in the game
// falls through to the debugger. Removed every frame, because the list is
// rebuilt whenever the views change.
constexpr size_t kHandlerInputOrderCount = 64;
constexpr size_t kHandlerInputOrderData = 72;

// engine2's RenderDevice003 global, backend-agnostic (dx11, vulkan or empty).
// Unique in engine2:
//   mov rdi,cs:<global> / mov rcx,rsi / or byte ptr [rbp+26h],20h
//   / mov rax,[rdi] / mov rbx,[rax+8]
constexpr uint8_t kEngineRenderDevicePattern[] = {0x48, 0x8B, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x48,
												  0x8B, 0xCE, 0x80, 0x4D, 0x26, 0x20, 0x48, 0x8B,
												  0x07, 0x48, 0x8B, 0x58, 0x08};
constexpr const char* kEngineRenderDeviceMask = "xxx????xxxxxxxxxxxxxx";
constexpr size_t kEngineRenderDeviceDispOffset = 3;
constexpr size_t kEngineRenderDeviceNextInsn = 7;

// Fallback: the device global CRenderDeviceMgrDx11::CreateDevice publishes.
//   mov rax,cs:<deviceGlobal> / mov edx,r12d / mov rcx,rbp / mov cs:<copy>,rax
constexpr uint8_t kRenderDeviceGlobalPattern[] = {0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x41,
												  0x8B, 0xD4, 0x48, 0x8B, 0xCD, 0x48, 0x89, 0x05,
												  0x00, 0x00, 0x00, 0x00};
constexpr const char* kRenderDeviceGlobalMask = "xxx????xxxxxxxxx????";
constexpr size_t kRenderDeviceDispOffset = 3;
constexpr size_t kRenderDeviceNextInsn = 7;

// The CUIEngineSource2 singleton, a file-static in panorama.dll with no export
// and no RTTI. Prologue of an accessor thunk, ending on the mov that loads it.
constexpr uint8_t kEngineAccessorPattern[] = {0x83, 0x39, 0x00, 0x75, 0x06, 0x83, 0x79,
											  0x04, 0xFF, 0x74, 0x1C, 0x4C, 0x8B, 0x05};
constexpr const char* kEngineAccessorMask = "xxxxxxxxxxxxxx";
constexpr size_t kEngineAccessorDispOffset = 14;
constexpr size_t kEngineAccessorNextInsn = 18;

// CPanoramaUIClient::CreateDebugger, slot 16 of ??_7CPanoramaUIClient@@6B@ in
// panoramauiclient.dll -- reachable from the "layout/debugger.xml" string. The
// wildcards are its rip-relative g_pMemAlloc reference. `this` is dead on
// arrival (rcx is overwritten before use); the real arguments are (window, id).
constexpr uint8_t kCreateDebuggerPattern[] = {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83,
	0xEC, 0x20, 0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF2, 0xBA,
	0x10, 0x00, 0x00, 0x00, 0x49, 0x8B, 0xF8, 0x48, 0x8B, 0x08, 0x48, 0x8B, 0x01,
	0xFF, 0x50, 0x08, 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x74, 0x4D};
constexpr const char* kCreateDebuggerMask = "xxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxx";

// CPanoramaClientDebugger scalar-deleting destructor. We own this object; no
// engine2 handler tracks it, so nothing else will ever free it.
constexpr size_t kDebuggerDestructor = 0;

// client.dll, how the game's own UI takes input from gameplay:
//   AddGameInputHandler(panel, EGameInputFlags, dbgContextName) -> uint64 handle
//   ReleaseGameInputHandler(handle)
// A NULL panel makes the filter unconditional until released.
//
// EGameInputFlags: 0x01 UIEnableMouseCursor, 0x04 UIEnableControllerInput,
// 0x08 UIEnableKeyInput, 0x10 DenyGameMouseMovement, 0x20 DenyGameMouseClicks,
// 0x40 DenyGameControllerInput, 0x80 DenyGameKeys. Live: Scoreboard 0x11,
// HudChat 0x19, CustomHudLayout 0x31, MainMenu 0xFD.
constexpr uint32_t kGameInputCaptureAll = 0xFD;

// One wrapper yields all three: its prologue is followed by a call to the
// manager accessor (a bare `lea rax,<global>; ret`) and then by the call to
// AddGameInputHandler, so both rel32s and the manager fall out of one match.
constexpr uint8_t kGameInputWrapperPattern[] = {
	0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0x79, 0x18, 0x00, 0x48, 0x8B, 0xD9, 0x75, 0x69,
	0x48, 0x89, 0x6C, 0x24, 0x30, 0x48, 0x89, 0x74, 0x24, 0x38, 0x48, 0x89, 0x7C, 0x24, 0x40};
constexpr const char* kGameInputWrapperMask = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
constexpr size_t kWrapperAccessorCall = 31; // E8 rel32
constexpr size_t kWrapperAddCall = 97;      // E8 rel32

constexpr uint8_t kReleaseGameInputPattern[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
												0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
												0x8B, 0xF9, 0x48, 0x8B, 0xF2, 0x48, 0x8D, 0x0D};
constexpr const char* kReleaseGameInputMask = "xxxxxxxxxxxxxxxxxxxxxxxx";

// Polled, not hooked: a hook would have to decide whether to swallow the key.
constexpr int kToggleKey = VK_F6;

// --- Resolved state -------------------------------------------------------

using CreateDebuggerFn = void*(__fastcall*)(void* unused, void* window, const char* id);
using RunFrameFn = void(__fastcall*)(void* engine);
using CreateOffscreenWindowFn = void*(__fastcall*)(void* engine, uint32_t width, int32_t height,
												   const char* name, void* inputContext,
												   char drawToBackBuffer);
using AddGameInputHandlerFn = uint64_t(__fastcall*)(void* self, void* panel, uint32_t flags,
													const char* debugName);
using ReleaseGameInputHandlerFn = void(__fastcall*)(void* self, uint64_t handle);

// tier0. PlatWindow_t is an opaque handle, not an HWND.
using PlatCreateWindowFn = void*(__cdecl*)(void* parent, const char* title, int x, int y, int w,
										   int h, uint32_t flags, void* unknown);
using PlatDestroyWindowFn = void(__cdecl*)(void* platWindow);
using PlatWindowToOsHandleFn = void*(__cdecl*)(void* platWindow);

void** g_engineGlobal = nullptr; // &CUIEngineSource2*, not the engine itself
CreateDebuggerFn g_createDebugger = nullptr;
void* g_renderDevice = nullptr;
void* g_gameUIService = nullptr;
void* g_panoramaHandler = nullptr;

PlatCreateWindowFn g_platCreateWindow = nullptr;
PlatDestroyWindowFn g_platDestroyWindow = nullptr;
PlatWindowToOsHandleFn g_platWindowToOsHandle = nullptr;

void* g_gameInputManager = nullptr;
AddGameInputHandlerFn g_addGameInputHandler = nullptr;
ReleaseGameInputHandlerFn g_releaseGameInputHandler = nullptr;
uint64_t g_gameInputHandle = 0;

platform::VTableHook g_runFrameHook;

// Owned by us and only ever touched on the game thread inside the frame hook.
void* g_debugger = nullptr;
void* g_ownPlatWindow = nullptr;
void* g_ownUiWindow = nullptr;
int32_t g_ownWindowWidth = kDebuggerWindowWidth;
int32_t g_ownWindowHeight = kDebuggerWindowHeight;

// Cross-thread requests, drained by the frame hook. The open sequence advances
// one step per frame: the window has to exist before it can be a view, and the
// view before the render path is switched on.
std::atomic<int> g_openStep{0};
bool g_debuggerVisible = false;

// Run on the first frame, not at load: the engine may have no command buffer
// yet. r_wait_on_present keeps our copy reading a finished frame;
// r_always_render_all_windows stops panorama skipping non-main windows, which
// otherwise freezes the debugger on whatever it last drew.
constexpr const char* kStartupCommands[] = {"r_wait_on_present 1",
											"r_always_render_all_windows 1"};

void (*g_consoleCommand)(const char*) = nullptr;
bool g_startupCommandsRun = false;
std::atomic<bool> g_closeRequested{false};
std::atomic<uint64_t> g_pendingResize{0};
std::atomic<int> g_gameInputRequest{-1};

bool g_toggleKeyWasDown = false;

// Which HWND the render path was built for. Cleared on close: Windows may hand
// the same HWND value back, which would otherwise look like "already built".
void* g_d3dBuiltFor = nullptr;

// Decided when the path is built; needed again to tear the right one down.
bool g_backendIsVulkan = false;

// --- Helpers --------------------------------------------------------------

void* GetEngine()
{
	if (!g_engineGlobal)
		return nullptr;

	// Populated when engine2 creates the UI engine, long after panorama.dll
	// loads, so null means "not yet" rather than an error.
	void* engine = nullptr;
	if (!safemem::Read(g_engineGlobal, &engine, sizeof(engine)))
		return nullptr;

	return safemem::HasPlausibleVTable(engine) ? engine : nullptr;
}

// Any live window's CUIWindowInput; the logger hooks the shared vtable.
void* GetAnyWindowInput(void* engine)
{
	if (!engine)
		return nullptr;

	int32_t count = 0;
	void** data = nullptr;
	if (!safemem::Read(static_cast<uint8_t*>(engine) + kEngineWindowCount, &count, sizeof(count)) ||
		!safemem::Read(static_cast<uint8_t*>(engine) + kEngineWindowData, &data, sizeof(data)) ||
		count <= 0 || count > 64 || !data)
	{
		return nullptr;
	}

	for (int32_t i = 0; i < count; ++i)
	{
		void* window = nullptr;
		if (!safemem::Read(data + i, &window, sizeof(window)) ||
			!safemem::HasPlausibleVTable(window))
		{
			continue;
		}

		void* input = nullptr;
		if (safemem::Read(static_cast<uint8_t*>(window) + kWindowInput, &input, sizeof(input)) &&
			input && safemem::HasPlausibleVTable(input))
		{
			return input;
		}
	}
	return nullptr;
}

// Which module provides the render device depends on launch options, and it
// appears later than the UI engine, so this is retried from the open path too.
void ResolveRenderDevice()
{
	if (g_renderDevice)
		return;

	// engine2 first: it holds whichever render system was actually loaded.
	{
		const platform::Module engine2 = platform::Module::FromName("engine2.dll");
		if (engine2.IsValid())
		{
			if (auto* site = static_cast<uint8_t*>(
					engine2.FindPattern(kEngineRenderDevicePattern, kEngineRenderDeviceMask)))
			{
				int32_t displacement = 0;
				memcpy(&displacement, site + kEngineRenderDeviceDispOffset, sizeof(displacement));
				auto** global =
					reinterpret_cast<void**>(site + kEngineRenderDeviceNextInsn + displacement);

				void* device = nullptr;
				if (safemem::Read(global, &device, sizeof(device)) &&
					safemem::HasPlausibleVTable(device))
				{
					g_renderDevice = device;
					Console::Printf("render device %p from engine2", device);
					return;
				}
			}
		}
	}

	static const char* kModules[] = {"rendersystemdx11.dll", "rendersystemvulkan.dll"};
	for (const char* moduleName : kModules)
	{
		const platform::Module module = platform::Module::FromName(moduleName);
		if (!module.IsValid())
			continue;

		if (auto* site = static_cast<uint8_t*>(
				module.FindPattern(kRenderDeviceGlobalPattern, kRenderDeviceGlobalMask)))
		{
			int32_t displacement = 0;
			memcpy(&displacement, site + kRenderDeviceDispOffset, sizeof(displacement));
			auto** deviceGlobal =
				reinterpret_cast<void**>(site + kRenderDeviceNextInsn + displacement);

			void* device = nullptr;
			if (safemem::Read(deviceGlobal, &device, sizeof(device)) &&
				safemem::HasPlausibleVTable(device))
			{
				g_renderDevice = device;
				Console::Printf("render device %p from %s (global %p)", device, moduleName,
								static_cast<void*>(deviceGlobal));
				break;
			}
		}

		// rendersystemvulkan exports RenderDevice003; rendersystemdx11 serves
		// only RenderDeviceMgr001, which is not the device.
		if (!g_renderDevice)
		{
			if (void* exported = module.FindInterface("RenderDevice003"))
			{
				g_renderDevice = exported;
				Console::Printf("render device %p from %s CreateInterface", exported,
								moduleName);
				break;
			}
		}
	}

	if (!g_renderDevice)
		Console::Warn("render device not found (game update?) -- nothing will render");
}

void SetGameInputCapture(bool enabled)
{
	if (!g_gameInputManager || !g_addGameInputHandler || !g_releaseGameInputHandler)
		return;
	if (enabled == (g_gameInputHandle != 0))
		return;

	if (enabled)
	{
		g_gameInputHandle = g_addGameInputHandler(g_gameInputManager, nullptr,
												  kGameInputCaptureAll, kDebuggerViewName);
	}
	else
	{
		g_releaseGameInputHandler(g_gameInputManager, g_gameInputHandle);
		g_gameInputHandle = 0;
	}
}

// Every frame: engine2 rebuilds the list whenever the views change, and
// compacting a few pointers is cheaper than hooking the rebuild.
void RemoveOwnWindowFromInputOrder()
{
	if (!g_panoramaHandler || !g_ownUiWindow)
		return;

	auto* handler = static_cast<uint8_t*>(g_panoramaHandler);
	int32_t count = 0;
	void** order = nullptr;
	if (!safemem::Read(handler + kHandlerInputOrderCount, &count, sizeof(count)) ||
		!safemem::Read(handler + kHandlerInputOrderData, &order, sizeof(order)) || count <= 0 ||
		count > 64 || !order)
	{
		return;
	}

	for (int32_t i = 0; i < count; ++i)
	{
		void* window = nullptr;
		if (!safemem::Read(order + i, &window, sizeof(window)) || window != g_ownUiWindow)
			continue;

		for (int32_t j = i; j + 1 < count; ++j)
			order[j] = order[j + 1];
		const int32_t shortened = count - 1;
		memcpy(handler + kHandlerInputOrderCount, &shortened, sizeof(shortened));
		return;
	}
}

// Our own swapchain on the game's device, plus the copy issued from Source2's
// device Present.
void EnsureD3DPath()
{
	ResolveRenderDevice();
	if (!g_renderDevice)
	{
		Console::Warn("no render device -- the window will stay blank");
		return;
	}
	// The tools launcher forces Vulkan (-gpuraytracing wins over -dx11), so
	// neither backend can be assumed.
	if (VkOwnWindowIsActive())
	{
		if (!VkOwnWindowInitialize(g_renderDevice))
		{
			Console::Warn("vulkan backend unavailable -- the window will stay blank");
			return;
		}
		g_backendIsVulkan = true;
	}
	else if (!D3DOwnWindowInitialize(g_renderDevice))
	{
		Console::Warn("could not find the game's ID3D11Device -- the window will stay blank");
		return;
	}
	if (!g_platWindowToOsHandle || !g_ownPlatWindow)
		return;

	void* hwnd = g_platWindowToOsHandle(g_ownPlatWindow);
	if (!hwnd || hwnd == g_d3dBuiltFor)
		return;
	const bool created = g_backendIsVulkan
							 ? VkOwnWindowCreateSwapChain(hwnd, g_ownWindowWidth, g_ownWindowHeight)
							 : D3DOwnWindowCreateSwapChain(hwnd, g_ownWindowWidth,
														   g_ownWindowHeight);
	if (!created)
	{
		Console::Warn("swapchain creation failed -- the window will stay blank");
		return;
	}

	g_d3dBuiltFor = hwnd;
	if (g_backendIsVulkan)
	{
		VkOwnWindowInstallDevicePresentHook(g_renderDevice);
	}
	else
	{
		D3DOwnWindowInstallObserveHook();
		D3DOwnWindowInstallDevicePresentHook(g_renderDevice);
	}

	// engine2 pumps input only for windows it created.
	OwnWindowInputAttach(hwnd);
	Console::Printf("render path installed (device %p, hwnd %p)", g_renderDevice, hwnd);
}

// All three move together, or the image stretches into a stale back buffer
// while hit-testing still uses the old size.
void ApplyOwnWindowResize(int32_t width, int32_t height)
{
	if (width <= 0 || height <= 0 || !g_ownUiWindow || !safemem::HasPlausibleVTable(g_ownUiWindow))
		return;
	if (width == g_ownWindowWidth && height == g_ownWindowHeight)
		return;

	g_ownWindowWidth = width;
	g_ownWindowHeight = height;

	void** windowVTable = *static_cast<void***>(g_ownUiWindow);
	using ResizeFn = void(__fastcall*)(void* self, uint32_t width, uint32_t height);
	reinterpret_cast<ResizeFn>(windowVTable[kWindowOnWindowResize])(
		g_ownUiWindow, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

	// Held at 1.0, but re-applied because this is the call that relayouts.
	using SetScaleFn = void(__fastcall*)(void* self, float scale);
	reinterpret_cast<SetScaleFn>(windowVTable[kWindowSetScaleFactor])(g_ownUiWindow, 1.0f);

	if (g_platWindowToOsHandle && g_ownPlatWindow)
	{
		if (void* hwnd = g_platWindowToOsHandle(g_ownPlatWindow))
		{
			if (g_backendIsVulkan)
				VkOwnWindowCreateSwapChain(hwnd, width, height);
			else
				D3DOwnWindowCreateSwapChain(hwnd, width, height);
		}
	}
}

// --- Open and close -------------------------------------------------------

bool CreateWindowAndDebugger(void* engine)
{
	if (!g_platCreateWindow)
	{
		Console::Warn("Plat_CreateWindow unavailable");
		return false;
	}

	g_ownPlatWindow = g_platCreateWindow(nullptr, "Panorama Debugger", 80, 80,
										 kDebuggerWindowWidth, kDebuggerWindowHeight,
										 kDebuggerWindowFlags, nullptr);
	if (!g_ownPlatWindow)
	{
		Console::Warn("Plat_CreateWindow failed");
		return false;
	}

	g_ownWindowWidth = kDebuggerWindowWidth;
	g_ownWindowHeight = kDebuggerWindowHeight;

	// A NULL input context on purpose: one of our own sits above the game's
	// "Panorama UI" in the input stack, and engine2 then routes the GAME
	// window's mouse into ours.
	void** engineVTable = *static_cast<void***>(engine);
	auto createOffscreen =
		reinterpret_cast<CreateOffscreenWindowFn>(engineVTable[kEngineCreateOffscreenWindow]);
	g_ownUiWindow = createOffscreen(engine, kDebuggerWindowWidth, kDebuggerWindowHeight,
									kDebuggerViewName, nullptr, /*bDrawToBackBuffer*/ 0);
	if (!g_ownUiWindow || !safemem::HasPlausibleVTable(g_ownUiWindow))
	{
		Console::Warn("CreateNewOffscreenUIWindow failed");
		if (g_platDestroyWindow)
			g_platDestroyWindow(g_ownPlatWindow);
		g_ownPlatWindow = nullptr;
		return false;
	}

	void** windowVTable = *static_cast<void***>(g_ownUiWindow);

	// Before anything else: AddPanoramaView calls slot 54, which early-outs
	// unless the surface has a size and window+92 is non-zero, and the offscreen
	// creator leaves the scale at zero.
	using SetScaleFn = void(__fastcall*)(void* self, float scale);
	reinterpret_cast<SetScaleFn>(windowVTable[kWindowSetScaleFactor])(g_ownUiWindow, 1.0f);

	using ResizeFn = void(__fastcall*)(void* self, uint32_t width, uint32_t height);
	reinterpret_cast<ResizeFn>(windowVTable[kWindowOnWindowResize])(
		g_ownUiWindow, kDebuggerWindowWidth, kDebuggerWindowHeight);

	using SetPlatWindowFn = void(__fastcall*)(void* self, void* platWindow);
	reinterpret_cast<SetPlatWindowFn>(windowVTable[kWindowSetPlatWindow])(g_ownUiWindow,
																		 g_ownPlatWindow);

	g_debugger = g_createDebugger(nullptr, g_ownUiWindow, kDebuggerPanelId);
	if (!g_debugger)
		Console::Warn("CreateDebugger returned null");

	using ActivateFn = void(__fastcall*)(void* self, char sendInput);
	reinterpret_cast<ActivateFn>(windowVTable[kWindowActivate])(g_ownUiWindow, 0);

	Console::Printf("window %p, debugger %p", g_ownUiWindow, g_debugger);
	return true;
}

void RegisterAsViewAndShow()
{
	if (!g_gameUIService || !g_ownUiWindow || !safemem::HasPlausibleVTable(g_gameUIService))
	{
		Console::Warn("no GameUIService to register the view with");
		return;
	}

	void** vtable = *static_cast<void***>(g_gameUIService);
	using AddViewFn = void*(__fastcall*)(void* self, const char* name, void* window, char a, char b);
	reinterpret_cast<AddViewFn>(vtable[kGameUIServiceAddPanoramaView])(
		g_gameUIService, kDebuggerViewName, g_ownUiWindow, static_cast<char>(kViewPriority),
		static_cast<char>(kViewFullscreen));

	void** windowVTable = *static_cast<void***>(g_ownUiWindow);

	// AddPanoramaView resizes the window itself -- with flag B it resizes to the
	// main window and applies its own scale factor -- so put both back.
	using ResizeFn = void(__fastcall*)(void* self, uint32_t width, uint32_t height);
	reinterpret_cast<ResizeFn>(windowVTable[kWindowOnWindowResize])(
		g_ownUiWindow, static_cast<uint32_t>(g_ownWindowWidth),
		static_cast<uint32_t>(g_ownWindowHeight));

	using SetScaleFn = void(__fastcall*)(void* self, float scale);
	reinterpret_cast<SetScaleFn>(windowVTable[kWindowSetScaleFactor])(g_ownUiWindow, 1.0f);

	// An invisible window is neither painted nor hit-tested.
	using SetVisibleFn = void(__fastcall*)(void* self, char visible);
	reinterpret_cast<SetVisibleFn>(windowVTable[kWindowSetVisible])(g_ownUiWindow, 1);
}

void CloseDebuggerWindow()
{
	if (!g_ownUiWindow || !safemem::HasPlausibleVTable(g_ownUiWindow))
		return;

	// First, or the game stays uncontrollable after the debugger is gone.
	SetGameInputCapture(false);

	OwnWindowInputDetach();
	D3DOwnWindowShutdown();
	VkOwnWindowCloseWindow();
	g_d3dBuiltFor = nullptr;
	g_backendIsVulkan = false;

	// Hidden, not destroyed, and reused on the next F6. engine2 has no way to
	// withdraw a panorama view -- it only appends to m_Views and clears the whole
	// vector at shutdown -- and each entry holds the window at +32, so destroying
	// it (CUIEngineSource2 slot 12) would leave the render walking freed memory.
	// Creating a fresh one per open is what leaked a "PanoramaDebugger" entry
	// each time.
	void** windowVTable = *static_cast<void***>(g_ownUiWindow);
	using SetVisibleFn = void(__fastcall*)(void* self, char visible);
	reinterpret_cast<SetVisibleFn>(windowVTable[kWindowSetVisible])(g_ownUiWindow, 0);

	if (g_platWindowToOsHandle && g_ownPlatWindow)
	{
		if (auto* hwnd = static_cast<HWND>(g_platWindowToOsHandle(g_ownPlatWindow)))
			ShowWindow(hwnd, SW_HIDE);
	}

	g_debuggerVisible = false;
	Console::Print("closed");
}

// Slot 18 does the panorama-side work but runs with sendInput=0 while the game
// holds focus, so the OS half is done here too.
void FocusOwnWindow()
{
	if (!g_platWindowToOsHandle || !g_ownPlatWindow)
		return;

	auto* hwnd = static_cast<HWND>(g_platWindowToOsHandle(g_ownPlatWindow));
	if (!hwnd)
		return;

	SetForegroundWindow(hwnd);
	SetFocus(hwnd);
}

// The reopen path: the engine's side is already in place.
void ShowExistingDebuggerWindow()
{
	if (!g_ownUiWindow || !safemem::HasPlausibleVTable(g_ownUiWindow))
		return;

	void** windowVTable = *static_cast<void***>(g_ownUiWindow);
	using SetVisibleFn = void(__fastcall*)(void* self, char visible);
	reinterpret_cast<SetVisibleFn>(windowVTable[kWindowSetVisible])(g_ownUiWindow, 1);

	if (g_platWindowToOsHandle && g_ownPlatWindow)
	{
		if (auto* hwnd = static_cast<HWND>(g_platWindowToOsHandle(g_ownPlatWindow)))
			ShowWindow(hwnd, SW_SHOW);
	}

	using ActivateFn = void(__fastcall*)(void* self, char sendInput);
	reinterpret_cast<ActivateFn>(windowVTable[kWindowActivate])(g_ownUiWindow, 0);

	g_debuggerVisible = true;
}

// Only at unload, where the view's dangling pointer no longer matters.
void DestroyDebuggerWindow()
{
	if (g_debugger && safemem::HasPlausibleVTable(g_debugger))
	{
		// Scalar-deleting destructor: flags bit 0 also frees the object.
		using DtorFn = void(__fastcall*)(void* self, char flags);
		void** vtable = *static_cast<void***>(g_debugger);
		reinterpret_cast<DtorFn>(vtable[kDebuggerDestructor])(g_debugger, 1);
	}
	g_debugger = nullptr;

	if (g_ownPlatWindow && g_platDestroyWindow)
		g_platDestroyWindow(g_ownPlatWindow);
	g_ownPlatWindow = nullptr;
	g_ownUiWindow = nullptr;
}

// --- The game-thread pump -------------------------------------------------

void __fastcall RunFrameDetour(void* engine)
{
	// Any live window arms it, so the event template and pick detection work
	// before we have a window of our own.
	static bool loggerArmed = false;
	if (!loggerArmed)
		loggerArmed = OwnWindowInputInstallLogger(GetAnyWindowInput(engine));

	if (!g_startupCommandsRun && g_consoleCommand)
	{
		g_startupCommandsRun = true;
		for (const char* command : kStartupCommands)
		{
			g_consoleCommand(command);
			Console::Printf("%s", command);
		}
	}

	const bool down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
	if (down && !g_toggleKeyWasDown)
	{
		if (g_debuggerVisible || g_openStep.load(std::memory_order_relaxed))
			g_closeRequested.store(true, std::memory_order_release);
		else
			g_openStep.store(1, std::memory_order_release);
	}
	g_toggleKeyWasDown = down;

	if (g_closeRequested.exchange(false, std::memory_order_acquire))
	{
		g_openStep.store(0, std::memory_order_relaxed);
		CloseDebuggerWindow();
	}

	switch (g_openStep.load(std::memory_order_relaxed))
	{
	case 1:
		// Once per session: a second window is a second m_Views entry that
		// nothing can remove.
		if (g_ownUiWindow)
		{
			ShowExistingDebuggerWindow();
			g_openStep.store(3, std::memory_order_relaxed);
			break;
		}
		g_openStep.store(CreateWindowAndDebugger(engine) ? 2 : 0, std::memory_order_relaxed);
		break;
	case 2:
		RegisterAsViewAndShow();
		g_debuggerVisible = true;
		g_openStep.store(3, std::memory_order_relaxed);
		break;
	case 3:
		EnsureD3DPath();
		g_openStep.store(4, std::memory_order_relaxed);
		break;
	case 4:
		// Held while open: the engine only shows the cursor while the GAME
		// window has focus, which is when Inspect needs it.
		SetGameInputCapture(true);
		// Last, so the earlier steps do not undo it.
		FocusOwnWindow();
		g_openStep.store(0, std::memory_order_relaxed);
		break;
	default:
		break;
	}

	if (const int wanted = g_gameInputRequest.exchange(-1, std::memory_order_acquire); wanted >= 0)
		SetGameInputCapture(wanted != 0);

	if (const uint64_t packed = g_pendingResize.exchange(0, std::memory_order_acquire); packed)
	{
		ApplyOwnWindowResize(static_cast<int32_t>(packed >> 32),
							 static_cast<int32_t>(packed & 0xFFFFFFFF));
	}

	RemoveOwnWindowFromInputOrder();

	g_runFrameHook.GetOriginal<RunFrameFn>()(engine);
}

} // namespace

// --- Public API -----------------------------------------------------------

void SetConsoleCommandSink(void (*sink)(const char* command))
{
	g_consoleCommand = sink;
}

bool Initialize()
{
	const platform::Module panorama = platform::Module::FromName("panorama.dll");
	const platform::Module client = platform::Module::FromName("panoramauiclient.dll");
	if (!panorama.IsValid() || !client.IsValid())
		return false;

	if (!g_engineGlobal)
	{
		auto* accessor = static_cast<uint8_t*>(
			panorama.FindPattern(kEngineAccessorPattern, kEngineAccessorMask));
		if (accessor)
		{
			int32_t displacement = 0;
			memcpy(&displacement, accessor + kEngineAccessorDispOffset, sizeof(displacement));
			g_engineGlobal =
				reinterpret_cast<void**>(accessor + kEngineAccessorNextInsn + displacement);
		}
		else
		{
			Console::Warn("CUIEngine singleton pattern not found (game update?)");
		}
	}

	if (!g_createDebugger)
	{
		g_createDebugger = reinterpret_cast<CreateDebuggerFn>(
			client.FindPattern(kCreateDebuggerPattern, kCreateDebuggerMask));
		if (!g_createDebugger)
			Console::Warn("CreateDebugger pattern not found (game update?)");
	}

	ResolveRenderDevice();

	if (!g_gameUIService || !g_panoramaHandler)
	{
		const platform::Module engine2 = platform::Module::FromName("engine2.dll");
		if (engine2.IsValid())
		{
			if (!g_gameUIService)
				g_gameUIService = engine2.FindInterface("GameUIService_001");

			if (!g_panoramaHandler)
			{
				if (auto* site =
						static_cast<uint8_t*>(engine2.FindPattern(kHandlerPattern, kHandlerMask)))
				{
					int32_t displacement = 0;
					memcpy(&displacement, site + kHandlerDispOffset, sizeof(displacement));
					g_panoramaHandler = site + kHandlerNextInsn + displacement;
				}
				else
				{
					Console::Warn("CPanoramaEngineHandler pattern not found (game update?)");
				}
			}
		}
	}

	if (!g_addGameInputHandler)
	{
		const platform::Module clientDll = platform::Module::FromName("client.dll");
		if (clientDll.IsValid())
		{
			if (auto* wrapper = static_cast<uint8_t*>(
					clientDll.FindPattern(kGameInputWrapperPattern, kGameInputWrapperMask)))
			{
				auto FollowCall = [](uint8_t* site) -> uint8_t* {
					int32_t rel = 0;
					memcpy(&rel, site + 1, sizeof(rel));
					return site + 5 + rel;
				};

				// `lea rax, <global>; ret`
				uint8_t* accessor = FollowCall(wrapper + kWrapperAccessorCall);
				int32_t displacement = 0;
				memcpy(&displacement, accessor + 3, sizeof(displacement));
				g_gameInputManager = accessor + 7 + displacement;

				g_addGameInputHandler = reinterpret_cast<AddGameInputHandlerFn>(
					FollowCall(wrapper + kWrapperAddCall));
			}
			else
			{
				Console::Warn("AddGameInputHandler pattern not found (game update?)");
			}

			g_releaseGameInputHandler = reinterpret_cast<ReleaseGameInputHandlerFn>(
				clientDll.FindPattern(kReleaseGameInputPattern, kReleaseGameInputMask));
		}
	}

	// Exported by name from tier0, so no pattern needed.
	if (!g_platCreateWindow)
	{
		if (const HMODULE tier0 = GetModuleHandleA("tier0.dll"))
		{
			g_platCreateWindow =
				reinterpret_cast<PlatCreateWindowFn>(GetProcAddress(tier0, "Plat_CreateWindow"));
			g_platDestroyWindow =
				reinterpret_cast<PlatDestroyWindowFn>(GetProcAddress(tier0, "Plat_DestroyWindow"));
			g_platWindowToOsHandle = reinterpret_cast<PlatWindowToOsHandleFn>(
				GetProcAddress(tier0, "Plat_WindowToOsSpecificHandle"));
		}
	}

	// Needs a live engine, which engine2 creates late -- retried, not failed.
	if (!g_runFrameHook.IsInstalled())
	{
		if (void* engine = GetEngine())
		{
			void** vtable = *static_cast<void***>(engine);
			g_runFrameHook.Install(vtable, kEngineRunFrame,
								   reinterpret_cast<void*>(&RunFrameDetour));
			if (g_runFrameHook.IsInstalled())
			{
				// A half-resolved plugin still loads and still answers F6, then
				// does something inexplicable. Print what was found.
				Console::Printf("engine %p  debugger factory %p  render device %p", engine,
								reinterpret_cast<void*>(g_createDebugger), g_renderDevice);
				Console::Printf("gameui service %p  panorama handler %p  game input %p",
								g_gameUIService, g_panoramaHandler,
								reinterpret_cast<void*>(g_addGameInputHandler));
				// Now, not at F6: panorama pools render targets, so the Vulkan
				// backend has to be watching before the first one is allocated.
				if (VkOwnWindowIsActive() && VkOwnWindowInitialize(g_renderDevice))
					g_backendIsVulkan = true;

				Console::Print("ready -- press F6 to toggle the debugger");
			}
		}
	}

	return g_runFrameHook.IsInstalled();
}

void Shutdown()
{
	// Order matters: stop being called before tearing down what the callee uses.
	g_runFrameHook.Remove();

	// Unload runs on Metamod's thread and removing the hook does not wait for a
	// frame already inside the detour. No handshake exists, so: long enough.
	Sleep(100);

	CloseDebuggerWindow();
	DestroyDebuggerWindow();
	OwnWindowInputRemoveLogger();
	D3DOwnWindowShutdown();
	VkOwnWindowShutdown();
}

void RequestOwnWindowResize(int width, int height)
{
	if (width <= 0 || height <= 0)
		return;

	// Packed into one 64-bit value so a size is never half-updated.
	const uint64_t packed = (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) |
							static_cast<uint32_t>(height);
	g_pendingResize.store(packed, std::memory_order_release);
}

void NotifyForeignMouseDown()
{
	if (!g_gameInputHandle)
		return;

	// Only while the game window has focus, which is where a pick happens.
	// Releasing on our own window's events would make it unusable.
	if (g_ownPlatWindow && g_platWindowToOsHandle)
	{
		if (GetForegroundWindow() == static_cast<HWND>(g_platWindowToOsHandle(g_ownPlatWindow)))
			return;
	}

	g_gameInputRequest.store(0, std::memory_order_release);
}

void RequestDebuggerClose()
{
	g_closeRequested.store(true, std::memory_order_release);
}

void RequestGameInputCapture(bool enabled)
{
	g_gameInputRequest.store(enabled ? 1 : 0, std::memory_order_release);
}

void* GetOwnWindowInput()
{
	if (!g_ownUiWindow || !safemem::HasPlausibleVTable(g_ownUiWindow))
		return nullptr;

	void* input = nullptr;
	if (!safemem::Read(static_cast<uint8_t*>(g_ownUiWindow) + kWindowInput, &input,
					   sizeof(input)) ||
		!input || !safemem::HasPlausibleVTable(input))
	{
		return nullptr;
	}
	return input;
}

void SetOwnWindowMousePosition(float x, float y)
{
	void* input = GetOwnWindowInput();
	if (!input)
		return;

	void** vtable = *static_cast<void***>(input);
	using SetMouseFn = void(__fastcall*)(void* self, float x, float y);
	reinterpret_cast<SetMouseFn>(vtable[kInputSetMousePosition])(input, x, y);
}

} // namespace panodbg
