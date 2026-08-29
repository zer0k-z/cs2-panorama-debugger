#include "window_d3d.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <cstring>

#include <atomic>

#include "core/console.h"
#include "core/safemem.h"
#include "platform/module.h"
#include "platform/vtable_hook.h"

namespace panodbg
{

namespace
{

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;

std::string g_deviceNote = "not searched";
std::string g_swapChainNote = "not created";
std::string g_presentNote = "not presented";
uint64_t g_presentCount = 0;

// Magenta: nothing in CS2's UI is this colour, so if the window shows it the
// pixels came from here and nowhere else.
constexpr float kProbeColour[4] = {1.0f, 0.0f, 1.0f, 1.0f};

// ID3D11DeviceContext::OMSetRenderTargets. Fixed by the COM layout:
// IUnknown(3) + ID3D11DeviceChild(4) starts the context methods at 7.
constexpr size_t kOMSetRenderTargets = 33;

using OMSetRenderTargetsFn = void(__stdcall*)(ID3D11DeviceContext* self, UINT numViews,
											  ID3D11RenderTargetView* const* views,
											  ID3D11DepthStencilView* depth);

platform::VTableHook g_omHook;

// Every render target the engine binds passes through here, and the debugger's
// is the one whose size matches its window.
//
// Keyed by size, not by view: the engine binds dozens of views but only a
// handful of shapes, and 32 view slots filled before the debugger's ever
// appeared. Each size keeps one referenced texture.
constexpr size_t kSeenSlots = 64;

struct SeenTarget
{
	ID3D11Texture2D* texture = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t format = 0;
	uint64_t binds = 0;
};

SeenTarget g_seen[kSeenSlots];
std::atomic<uint32_t> g_seenCount{0};

// The last positive identification. Size alone stops working when the window
// shrinks: panorama reuses the oversized target it already has, and the game's
// own layers then cover the window too -- which is how the HUD ended up in the
// debugger window.
ID3D11Texture2D* g_lockedTexture = nullptr;
uint32_t g_lockedWidth = 0;
uint32_t g_lockedHeight = 0;

std::atomic<ID3D11Texture2D*> g_capturedTexture{nullptr};
uint32_t g_wantWidth = 0;
uint32_t g_wantHeight = 0;
std::atomic<uint64_t> g_redirectedBinds{0};

using OMSetRenderTargetsFn = void(__stdcall*)(ID3D11DeviceContext* self, UINT numViews,
											  ID3D11RenderTargetView* const* views,
											  ID3D11DepthStencilView* depth);

// D3D11 allows a copy within a typeless family but not across families, and the
// debugger's target is R8G8B8A8_TYPELESS against our R8G8B8A8_UNORM back buffer,
// so an equality test would reject a copy the runtime accepts.
uint32_t FormatFamily(uint32_t format)
{
	if (format >= 27 && format <= 32)  // R8G8B8A8_TYPELESS .. _SINT
		return 27;
	if (format >= 87 && format <= 93)  // B8G8R8A8/B8G8R8X8 group
		return 87;
	if (format >= 23 && format <= 26)  // R10G10B10A2 group
		return 23;
	if (format >= 9 && format <= 14)   // R16G16B16A16 group
		return 9;
	return format;
}

void NoteRenderTarget(ID3D11RenderTargetView* view)
{
	ID3D11Resource* resource = nullptr;
	view->GetResource(&resource);
	if (!resource)
		return;

	ID3D11Texture2D* texture = nullptr;
	if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
										   reinterpret_cast<void**>(&texture))) &&
		texture)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		texture->GetDesc(&desc);

		const uint32_t count = g_seenCount.load(std::memory_order_relaxed);
		bool known = false;
		for (uint32_t i = 0; i < count && i < kSeenSlots; ++i)
		{
			if (g_seen[i].width == desc.Width && g_seen[i].height == desc.Height &&
				g_seen[i].format == desc.Format)
			{
				// Same shape is not the same texture: with the render-target
				// cache off the surface allocates a fresh one each frame, and
				// holding the first pointer freezes the window on one frame.
				if (g_seen[i].texture != texture)
				{
					if (g_lockedTexture == g_seen[i].texture)
					{
						g_lockedTexture->Release();
						texture->AddRef();
						g_lockedTexture = texture;
					}
					if (g_seen[i].texture)
						g_seen[i].texture->Release();
					texture->AddRef();
					g_seen[i].texture = texture;
				}
				++g_seen[i].binds;
				known = true;
				break;
			}
		}

		if (!known)
		{
			// Resizes keep introducing sizes, so dropping new entries when full
			// loses the target panorama allocates on growth. Evict the
			// least-used instead, never the locked one.
			uint32_t slot = count;
			if (count >= kSeenSlots)
			{
				uint64_t fewest = UINT64_MAX;
				slot = kSeenSlots;
				for (uint32_t i = 0; i < kSeenSlots; ++i)
				{
					if (g_seen[i].texture == g_lockedTexture)
						continue;
					if (g_seen[i].binds < fewest)
					{
						fewest = g_seen[i].binds;
						slot = i;
					}
				}
				if (slot < kSeenSlots && g_seen[slot].texture)
					g_seen[slot].texture->Release();
			}

			if (slot < kSeenSlots)
			{
				texture->AddRef();
				g_seen[slot].texture = texture;
				g_seen[slot].width = desc.Width;
				g_seen[slot].height = desc.Height;
				g_seen[slot].format = desc.Format;
				g_seen[slot].binds = 1;
				if (slot == count)
					g_seenCount.store(count + 1, std::memory_order_release);
				g_redirectedBinds.fetch_add(1, std::memory_order_relaxed);
			}
		}
		texture->Release();
	}
	resource->Release();
}

void __stdcall OMSetRenderTargetsDetour(ID3D11DeviceContext* self, UINT numViews,
										ID3D11RenderTargetView* const* views,
										ID3D11DepthStencilView* depth)
{
	if (numViews >= 1 && views && views[0])
		NoteRenderTarget(views[0]);

	g_omHook.GetOriginal<OMSetRenderTargetsFn>()(self, numViews, views, depth);
}

// Whether the device Present hook should do our copy. Set once the swapchain and
// the hook are both up, cleared on shutdown.
std::atomic<bool> g_presentOurs{false};
std::string g_presentHookNote = "not installed";

void PresentOwnWindow()
{
	if (!g_context || !g_swapChain || !g_renderTarget)
		return;

	ID3D11Texture2D* source = nullptr;
	const uint32_t count = g_seenCount.load(std::memory_order_acquire);
	{
		// Panorama rounds each dimension up to a multiple of 32
		// ((n + 31) & 0xFFE0, minimum 32), so an exact match only happens when
		// the window already is one. Prefer exact, then rounded.
		auto RoundUp32 = [](uint32_t value) { return value < 32 ? 32u : ((value + 31) & ~31u); };
		const uint32_t roundedWidth = RoundUp32(g_wantWidth);
		const uint32_t roundedHeight = RoundUp32(g_wantHeight);

		for (uint32_t i = 0; i < count && i < kSeenSlots; ++i)
		{
			if (g_seen[i].width == g_wantWidth && g_seen[i].height == g_wantHeight)
			{
				source = g_seen[i].texture;
				break;
			}
		}
		if (!source)
		{
			for (uint32_t i = 0; i < count && i < kSeenSlots; ++i)
			{
				if (g_seen[i].width == roundedWidth && g_seen[i].height == roundedHeight)
				{
					source = g_seen[i].texture;
					break;
				}
			}
		}

		// Positive identification: keep it, with its own reference and size, so
		// the lock survives eviction from the observed list.
		if (source && source != g_lockedTexture)
		{
			if (g_lockedTexture)
				g_lockedTexture->Release();
			source->AddRef();
			g_lockedTexture = source;

			D3D11_TEXTURE2D_DESC lockedDesc = {};
			source->GetDesc(&lockedDesc);
			g_lockedWidth = lockedDesc.Width;
			g_lockedHeight = lockedDesc.Height;
		}

		// No match means the window shrank inside the target still being drawn
		// into. Re-searching by size finds one of the game's layers instead.
		if (!source && g_lockedTexture && g_lockedWidth >= g_wantWidth &&
			g_lockedHeight >= g_wantHeight)
		{
			source = g_lockedTexture;
		}

		// Deliberately no "smallest target that covers the window" fallback: it
		// matches the game's own layers just as readily and puts the HUD here.
	}

	if (source)
	{
		// Sizes rarely agree, so copy the overlapping region rather than the whole
		// resource: CopyResource would silently do nothing on any mismatch.
		ID3D11Texture2D* backBuffer = nullptr;
		if (SUCCEEDED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
											 reinterpret_cast<void**>(&backBuffer))) &&
			backBuffer)
		{
			D3D11_TEXTURE2D_DESC srcDesc = {};
			D3D11_TEXTURE2D_DESC dstDesc = {};
			source->GetDesc(&srcDesc);
			backBuffer->GetDesc(&dstDesc);

			if (FormatFamily(srcDesc.Format) == FormatFamily(dstDesc.Format))
			{
				D3D11_BOX box = {};
				box.right = srcDesc.Width < dstDesc.Width ? srcDesc.Width : dstDesc.Width;
				box.bottom = srcDesc.Height < dstDesc.Height ? srcDesc.Height : dstDesc.Height;
				box.back = 1;
				g_context->CopySubresourceRegion(backBuffer, 0, 0, 0, 0, source, 0, &box);
			}
			else
			{
				g_context->ClearRenderTargetView(g_renderTarget, kProbeColour);
			}
			backBuffer->Release();
		}
	}
	else
	{
		g_context->ClearRenderTargetView(g_renderTarget, kProbeColour);
	}

	const HRESULT hr = g_swapChain->Present(0, 0);
	++g_presentCount;

	char text[192];
	_snprintf_s(text, sizeof(text), _TRUNCATE, "hr=0x%08lX count=%llu captured=%s targetsSeen=%u",
				static_cast<unsigned long>(hr), static_cast<unsigned long long>(g_presentCount),
				source ? "yes" : "no", g_seenCount.load(std::memory_order_relaxed));
	g_presentNote = text;
}

// How far into an engine object to look for a COM pointer. Bounded so a layout
// change cannot run away.
constexpr size_t kDeviceScanBytes = 4096;

// CRenderDeviceDx11's ID3D11Device, from the D3D11CreateDevice call sites. The
// context is the next field at +0xF50, but GetImmediateContext is used instead.
constexpr size_t kDeviceD3DDeviceOffset = 0xF48;

// A COM object's vtable lives in its own module -- a safer first test than
// calling QueryInterface on an arbitrary pointer.
bool LooksLikeD3D11Object(void* candidate, uint8_t* moduleBase, size_t moduleSize)
{
	if (!candidate || !safemem::HasPlausibleVTable(candidate))
		return false;

	void* vtable = nullptr;
	if (!safemem::Read(candidate, &vtable, sizeof(vtable)))
		return false;

	auto address = reinterpret_cast<uintptr_t>(vtable);
	auto base = reinterpret_cast<uintptr_t>(moduleBase);
	return address >= base && address < base + moduleSize;
}

} // namespace

bool D3DOwnWindowInitialize(void* source2RenderDevice)
{
	if (g_device)
		return true;

	if (!source2RenderDevice)
	{
		g_deviceNote = "no Source2 render device";
		return false;
	}

	const platform::Module d3d11 = platform::Module::FromName("d3d11.dll");
	if (!d3d11.IsValid())
	{
		g_deviceNote = "d3d11.dll not loaded";
		return false;
	}

	auto* object = static_cast<uint8_t*>(source2RenderDevice);

	// Both creation paths pass these to D3D11CreateDevice:
	//     lea rax, [rsi+0F50h]   ppImmediateContext
	//     lea rcx, [rsi+0F48h]   ppDevice
	// (the adapter is at +0F40h). QueryInterface still has the final say.
	{
		void* candidate = nullptr;
		if (safemem::Read(object + kDeviceD3DDeviceOffset, &candidate, sizeof(candidate)) &&
			candidate && safemem::HasPlausibleVTable(candidate))
		{
			ID3D11Device* device = nullptr;
			auto* unknown = static_cast<IUnknown*>(candidate);
			if (SUCCEEDED(unknown->QueryInterface(__uuidof(ID3D11Device),
												  reinterpret_cast<void**>(&device))) &&
				device)
			{
				g_device = device; // QueryInterface already took our reference
				g_device->GetImmediateContext(&g_context);

				char text[128];
				_snprintf_s(text, sizeof(text), _TRUNCATE, "at device+%zu -> %p",
							kDeviceD3DDeviceOffset, static_cast<void*>(g_device));
				g_deviceNote = text;
				Console::Print(std::string("ID3D11Device ") + g_deviceNote);
				return true;
			}
		}
	}

	// Otherwise fall back to looking for it: any field whose vtable lives in
	// d3d11.dll and which answers to ID3D11Device.
	for (size_t offset = 0; offset + sizeof(void*) <= kDeviceScanBytes; offset += sizeof(void*))
	{
		void* candidate = nullptr;
		if (!safemem::Read(object + offset, &candidate, sizeof(candidate)))
			continue;
		if (!LooksLikeD3D11Object(candidate, d3d11.GetBase(), d3d11.GetSize()))
			continue;

		ID3D11Device* device = nullptr;
		auto* unknown = static_cast<IUnknown*>(candidate);
		if (FAILED(unknown->QueryInterface(__uuidof(ID3D11Device),
										   reinterpret_cast<void**>(&device))) ||
			!device)
		{
			continue;
		}

		g_device = device; // QueryInterface already took our reference
		g_device->GetImmediateContext(&g_context);

		char text[128];
		_snprintf_s(text, sizeof(text), _TRUNCATE, "found at device+%zu -> %p", offset,
					static_cast<void*>(g_device));
		g_deviceNote = text;
		Console::Print(std::string("panorama d3d: ID3D11Device ") + g_deviceNote);
		return true;
	}

	// Report every COM-looking field and the module its vtable belongs to. A
	// wrapped device -- Steam overlay, debug layer, vendor shim -- has its
	// vtable outside d3d11.dll and fails the test above; this shows that.
	void* firstVTable = nullptr;
	safemem::Read(source2RenderDevice, &firstVTable, sizeof(firstVTable));
	Console::Printf("no ID3D11Device within %zu bytes of %p (its vtable is %p)", kDeviceScanBytes,
					source2RenderDevice, firstVTable);

	// Raw qwords around ppDevice, unfiltered: a populated field that fails the
	// plausibility test shows up only here.
	for (size_t offset = 0xF00; offset <= 0xFC0; offset += 8)
	{
		void* value = nullptr;
		if (!safemem::Read(object + offset, &value, sizeof(value)))
		{
			Console::Printf("  +%-5zu <unreadable>", offset);
			continue;
		}
		Console::Printf("  +%-5zu (0x%zX) %p%s", offset, offset, value,
						offset == kDeviceD3DDeviceOffset ? "   <-- expected ID3D11Device" : "");
	}

	int reported = 0;
	for (size_t offset = 0; offset + sizeof(void*) <= kDeviceScanBytes && reported < 24;
		 offset += sizeof(void*))
	{
		void* candidate = nullptr;
		if (!safemem::Read(object + offset, &candidate, sizeof(candidate)) || !candidate)
			continue;
		if (!safemem::HasPlausibleVTable(candidate))
			continue;

		void* vtable = nullptr;
		safemem::Read(candidate, &vtable, sizeof(vtable));

		char owner[MAX_PATH] = "?";
		HMODULE module = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
								   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							   static_cast<LPCSTR>(vtable), &module) &&
			module)
		{
			char path[MAX_PATH] = {};
			if (GetModuleFileNameA(module, path, sizeof(path)))
			{
				const char* slash = strrchr(path, '\\');
				strncpy_s(owner, slash ? slash + 1 : path, _TRUNCATE);
			}
		}

		Console::Printf("  +%-4zu %p  vtable %p  %s", offset, candidate, vtable, owner);
		++reported;
	}

	g_deviceNote = "no ID3D11Device inside the Source2 device object";
	return false;
}

bool D3DOwnWindowCreateSwapChain(void* hwnd, int width, int height)
{
	if (!g_device || !hwnd)
	{
		g_swapChainNote = "no device / hwnd";
		return false;
	}

	if (g_renderTarget)
	{
		g_renderTarget->Release();
		g_renderTarget = nullptr;
	}
	if (g_swapChain)
	{
		g_swapChain->Release();
		g_swapChain = nullptr;
	}

	// The factory has to be the one that made the device's adapter, or the
	// swapchain cannot share the device.
	IDXGIDevice* dxgiDevice = nullptr;
	IDXGIAdapter* adapter = nullptr;
	IDXGIFactory* factory = nullptr;

	bool ok = false;
	if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice),
										   reinterpret_cast<void**>(&dxgiDevice))) &&
		SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
		SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))))
	{
		DXGI_SWAP_CHAIN_DESC desc = {};
		desc.BufferCount = 2;
		desc.BufferDesc.Width = static_cast<UINT>(width);
		desc.BufferDesc.Height = static_cast<UINT>(height);
		desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.OutputWindow = static_cast<HWND>(hwnd);
		desc.SampleDesc.Count = 1;
		desc.Windowed = TRUE;
		desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		if (SUCCEEDED(factory->CreateSwapChain(g_device, &desc, &g_swapChain)) && g_swapChain)
		{
			ID3D11Texture2D* backBuffer = nullptr;
			if (SUCCEEDED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
												 reinterpret_cast<void**>(&backBuffer))) &&
				backBuffer)
			{
				ok = SUCCEEDED(
					g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget));
				backBuffer->Release();
			}
		}
	}

	if (factory)
		factory->Release();
	if (adapter)
		adapter->Release();
	if (dxgiDevice)
		dxgiDevice->Release();

	// Remembered so the target can be recognised by size at the
	// OMSetRenderTargets hook.
	if (g_wantWidth != static_cast<uint32_t>(width) ||
		g_wantHeight != static_cast<uint32_t>(height))
	{
		// Deliberately not cleared on growth: panorama allocates a new target
		// only once the current one stops covering the window, so growing inside
		// an oversized target produces nothing to re-lock against and the window
		// went magenta. Outgrowing it fails the covers-check below, which makes
		// panorama allocate and the next size match re-lock.
	}

	g_wantWidth = static_cast<uint32_t>(width);
	g_wantHeight = static_cast<uint32_t>(height);

	char text[160];
	_snprintf_s(text, sizeof(text), _TRUNCATE, "%s swapchain=%p rtv=%p %dx%d",
				ok ? "created" : "FAILED", static_cast<void*>(g_swapChain),
				static_cast<void*>(g_renderTarget), width, height);
	g_swapChainNote = text;
	Console::Print(std::string("panorama d3d: ") + g_swapChainNote);
	return ok;
}

bool D3DOwnWindowInstallObserveHook()
{
	if (g_omHook.IsInstalled())
		return true;
	if (!g_context)
		return false;

	auto** vtable = *reinterpret_cast<void***>(g_context);
	return g_omHook.Install(vtable, kOMSetRenderTargets,
							reinterpret_cast<void*>(&OMSetRenderTargetsDetour));
}

// CRenderDeviceBase::Present, render device vtable slot 16. Not
// IDXGISwapChain::Present: the Steam overlay shares that vtable and re-patches
// it whenever DXGI recreates the swapchain, after which the two recurse until
// the stack dies. Slot 16 runs at the same point in the frame.
constexpr size_t kDevicePresentSlot = 16;

platform::VTableHook g_devicePresentHook;
using DevicePresentFn = int64_t(__fastcall*)(void* device, int64_t swapChain, void* a3, void* a4,
											 void* a5);

int64_t __fastcall DevicePresentDetour(void* device, int64_t swapChain, void* a3, void* a4,
									   void* a5)
{
	const int64_t result =
		g_devicePresentHook.GetOriginal<DevicePresentFn>()(device, swapChain, a3, a4, a5);

	// After the game's own present, so the frame it copies from is finished.
	if (g_presentOurs.load(std::memory_order_relaxed))
		PresentOwnWindow();

	return result;
}

bool D3DOwnWindowInstallDevicePresentHook(void* source2RenderDevice)
{
	if (g_devicePresentHook.IsInstalled())
	{
		g_presentOurs.store(true, std::memory_order_relaxed);
		return true;
	}
	if (!source2RenderDevice || !safemem::HasPlausibleVTable(source2RenderDevice))
	{
		g_presentHookNote = "no Source2 render device";
		return false;
	}

	void** vtable = *static_cast<void***>(source2RenderDevice);
	if (!g_devicePresentHook.Install(vtable, kDevicePresentSlot,
									 reinterpret_cast<void*>(&DevicePresentDetour)))
	{
		g_presentHookNote = "device Present hook failed";
		return false;
	}

	g_presentOurs.store(true, std::memory_order_relaxed);
	g_presentHookNote = "hooked CRenderDeviceBase::Present";
	return true;
}

namespace
{

} // namespace

void D3DOwnWindowShutdown()
{
	// Hooks first. A vtable entry still pointing into this image after the DLL
	// unloads is a crash the next time the game renders a frame.
	g_presentOurs.store(false, std::memory_order_relaxed);
	g_devicePresentHook.Remove();
	g_omHook.Remove();

	for (uint32_t i = 0; i < g_seenCount.load(std::memory_order_relaxed) && i < kSeenSlots; ++i)
	{
		if (g_seen[i].texture)
			g_seen[i].texture->Release();
		g_seen[i] = SeenTarget{};
	}
	g_seenCount.store(0, std::memory_order_release);

	if (g_lockedTexture)
	{
		g_lockedTexture->Release();
		g_lockedTexture = nullptr;
	}
	g_lockedWidth = 0;
	g_lockedHeight = 0;

	if (g_renderTarget)
	{
		g_renderTarget->Release();
		g_renderTarget = nullptr;
	}
	if (g_swapChain)
	{
		g_swapChain->Release();
		g_swapChain = nullptr;
	}
	if (g_context)
	{
		g_context->Release();
		g_context = nullptr;
	}
	if (g_device)
	{
		g_device->Release();
		g_device = nullptr;
	}
	g_swapChainNote = "not created";
	g_presentNote = "not presented";
}

} // namespace panodbg
