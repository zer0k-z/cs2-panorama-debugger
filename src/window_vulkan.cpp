#include "window_vulkan.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>

#include "core/console.h"
#include "core/safemem.h"
#include "platform/module.h"
#include "platform/vtable_hook.h"
#include "vulkan_min.h"

namespace panodbg
{

namespace
{

// rendersystemvulkan.dll. Read off the calls that produce each handle:
//
//   CRenderDeviceVulkan::CreateDevice (primary vtable slot 251)
//     vkCreateDevice(physicalDevice = [this+0x2900], ..., pDevice = this+0x28F0)
//     vkGetDeviceQueue([this+0x28F0], [this+0x2918], 0, this+0x2908)
//   CRenderDeviceMgrVulkan (vtable slot 0)
//     vkEnumeratePhysicalDevices([this+0xA0], ...)
//
// Both classes have their primary vtable at sub-object offset 0.
constexpr size_t kDeviceVkDevice = 0x28F0;
constexpr size_t kDeviceVkPhysicalDevice = 0x2900;
constexpr size_t kDeviceVkQueue = 0x2908;
constexpr size_t kDeviceQueueFamily = 0x2918;
constexpr size_t kMgrVkInstance = 0xA0;

constexpr size_t kDevicePresentSlot = 16;

VkInstance g_instance = nullptr;
VkPhysicalDevice g_physicalDevice = nullptr;
VkDevice g_device = nullptr;
VkQueue g_queue = nullptr;
uint32_t g_queueFamily = VK_QUEUE_FAMILY_IGNORED;

// Ours, on the game's device.
VkSurfaceKHR g_surface = VK_NULL_HANDLE_64;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE_64;
VkImage g_swapchainImages[8] = {};
uint32_t g_swapchainImageCount = 0;
VkCommandPool g_commandPool = VK_NULL_HANDLE_64;
VkCommandBuffer g_commandBuffer = nullptr;
VkSemaphore g_acquired = VK_NULL_HANDLE_64;
VkSemaphore g_blitted = VK_NULL_HANDLE_64;
uint32_t g_extentWidth = 0;
uint32_t g_extentHeight = 0;

platform::VTableHook g_devicePresentHook;
std::atomic<bool> g_presentOurs{false};

// rendersystemvulkan calls every Vulkan entry point through a writable global,
// so one pointer swap is the whole hook. These are filled by its two loader
// functions (sub_1800091E0 instance level, sub_1800098E0 device level), where
// the store follows the *next* `lea rdx, "vkNextName"` -- which is how to
// rebuild the global-to-name mapping after an update.
constexpr size_t kGlobalCreateImage = 0x62B3F0;
constexpr size_t kGlobalDestroyImage = 0x62B3F8;

void** g_createImageGlobal = nullptr;
void** g_destroyImageGlobal = nullptr;
PFN_vkCreateImage g_originalCreateImage = nullptr;
PFN_vkDestroyImage g_originalDestroyImage = nullptr;

// Candidate render targets, newest last. Panorama pools its targets, so this
// fills once and then stays still rather than churning per frame.
struct TrackedImage
{
	VkImage image;
	uint32_t width;
	uint32_t height;
};

constexpr size_t kMaxTracked = 64;
TrackedImage g_tracked[kMaxTracked] = {};
size_t g_trackedCount = 0;
std::mutex g_trackedMutex;

struct
{
	PFN_vkCreateWin32SurfaceKHR CreateWin32Surface;
	PFN_vkDestroySurfaceKHR DestroySurface;
	PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetSurfaceSupport;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetSurfaceCapabilities;
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetSurfaceFormats;
	PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetSurfacePresentModes;
	PFN_vkCreateSwapchainKHR CreateSwapchain;
	PFN_vkDestroySwapchainKHR DestroySwapchain;
	PFN_vkGetSwapchainImagesKHR GetSwapchainImages;
	PFN_vkAcquireNextImageKHR AcquireNextImage;
	PFN_vkQueuePresentKHR QueuePresent;
	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkResetCommandBuffer ResetCommandBuffer;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkCmdClearColorImage CmdClearColorImage;
	PFN_vkCmdBlitImage CmdBlitImage;
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkQueueWaitIdle QueueWaitIdle;
	PFN_vkDeviceWaitIdle DeviceWaitIdle;
	PFN_vkCreateSemaphore CreateSemaphore;
	PFN_vkDestroySemaphore DestroySemaphore;
} vk;

bool g_loaded = false;

// The loader exports everything we use, so no vkGetInstanceProcAddr -- which
// would need a valid instance before we have proven we have one.
bool LoadEntryPoints()
{
	if (g_loaded)
		return true;

	const HMODULE loader = GetModuleHandleA("vulkan-1.dll");
	if (!loader)
		return false;

	bool ok = true;
	auto get = [&](const char* name) {
		void* fn = reinterpret_cast<void*>(GetProcAddress(loader, name));
		if (!fn)
		{
			Console::Warnf("vulkan: loader has no %s", name);
			ok = false;
		}
		return fn;
	};

#define PANODBG_VK_LOAD(field, name) \
	vk.field = reinterpret_cast<decltype(vk.field)>(get(name))

	PANODBG_VK_LOAD(CreateWin32Surface, "vkCreateWin32SurfaceKHR");
	PANODBG_VK_LOAD(DestroySurface, "vkDestroySurfaceKHR");
	PANODBG_VK_LOAD(GetSurfaceSupport, "vkGetPhysicalDeviceSurfaceSupportKHR");
	PANODBG_VK_LOAD(GetSurfaceCapabilities, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
	PANODBG_VK_LOAD(GetSurfaceFormats, "vkGetPhysicalDeviceSurfaceFormatsKHR");
	PANODBG_VK_LOAD(GetSurfacePresentModes, "vkGetPhysicalDeviceSurfacePresentModesKHR");
	PANODBG_VK_LOAD(CreateSwapchain, "vkCreateSwapchainKHR");
	PANODBG_VK_LOAD(DestroySwapchain, "vkDestroySwapchainKHR");
	PANODBG_VK_LOAD(GetSwapchainImages, "vkGetSwapchainImagesKHR");
	PANODBG_VK_LOAD(AcquireNextImage, "vkAcquireNextImageKHR");
	PANODBG_VK_LOAD(QueuePresent, "vkQueuePresentKHR");
	PANODBG_VK_LOAD(CreateCommandPool, "vkCreateCommandPool");
	PANODBG_VK_LOAD(DestroyCommandPool, "vkDestroyCommandPool");
	PANODBG_VK_LOAD(AllocateCommandBuffers, "vkAllocateCommandBuffers");
	PANODBG_VK_LOAD(BeginCommandBuffer, "vkBeginCommandBuffer");
	PANODBG_VK_LOAD(EndCommandBuffer, "vkEndCommandBuffer");
	PANODBG_VK_LOAD(ResetCommandBuffer, "vkResetCommandBuffer");
	PANODBG_VK_LOAD(CmdPipelineBarrier, "vkCmdPipelineBarrier");
	PANODBG_VK_LOAD(CmdClearColorImage, "vkCmdClearColorImage");
	PANODBG_VK_LOAD(CmdBlitImage, "vkCmdBlitImage");
	PANODBG_VK_LOAD(GetDeviceProcAddr, "vkGetDeviceProcAddr");
	PANODBG_VK_LOAD(QueueSubmit, "vkQueueSubmit");
	PANODBG_VK_LOAD(QueueWaitIdle, "vkQueueWaitIdle");
	PANODBG_VK_LOAD(DeviceWaitIdle, "vkDeviceWaitIdle");
	PANODBG_VK_LOAD(CreateSemaphore, "vkCreateSemaphore");
	PANODBG_VK_LOAD(DestroySemaphore, "vkDestroySemaphore");

#undef PANODBG_VK_LOAD

	g_loaded = ok;
	return ok;
}

// What a dispatchable handle's first word holds varies (loader dispatch table,
// ICD magic, a layer's header), so this only rejects a bad read. The offsets are
// what establish that it is a handle.
bool LooksDispatchable(const void* candidate)
{
	return candidate && (reinterpret_cast<uintptr_t>(candidate) & 7) == 0 &&
		   safemem::IsReadable(candidate, sizeof(void*));
}

// On a failed read: distinguishes a moved field from the wrong object.
void DumpHandleFields(void* object)
{
	const platform::Module module = platform::Module::FromAddress(object);
	if (module.IsValid())
		Console::Warnf("vulkan: object %p is %s+0x%zX", object, module.GetName().c_str(),
					   static_cast<size_t>(static_cast<uint8_t*>(object) - module.GetBase()));

	for (size_t offset = kDeviceVkDevice - 0x10; offset <= kDeviceQueueFamily + 0x8; offset += 8)
	{
		uint64_t value = 0;
		if (!safemem::Read(static_cast<uint8_t*>(object) + offset, &value, sizeof(value)))
			continue;
		Console::Warnf("vulkan:   +0x%zX = 0x%llX%s", offset,
					   static_cast<unsigned long long>(value),
					   LooksDispatchable(reinterpret_cast<void*>(value)) ? "  dispatchable" : "");
	}
}

// An interface pointer is not the start of the object -- CRenderDeviceVulkan
// has a base sub-object at +3408 as well as the primary one at 0. MSVC
// records the distance in the complete object locator at vtable[-1].
void* CompleteObjectFrom(void* object)
{
	if (!safemem::HasPlausibleVTable(object))
		return nullptr;

	void** vtable = *static_cast<void***>(object);
	void* locator = nullptr;
	if (!safemem::Read(&vtable[-1], &locator, sizeof(locator)) ||
		!safemem::IsReadable(locator, 8))
		return object;

	uint32_t signature = 0;
	uint32_t offset = 0;
	safemem::Read(locator, &signature, sizeof(signature));
	safemem::Read(static_cast<uint8_t*>(locator) + 4, &offset, sizeof(offset));
	// Signature 1 is the 64-bit image-relative form; anything else means this is
	// not RTTI and the pointer is best left alone.
	if (signature != 1 || offset > 0x10000)
		return object;

	return static_cast<uint8_t*>(object) - offset;
}

// The object's identity in the terms the IDA database uses.
void ReportObject(const char* what, void* object)
{
	if (!safemem::HasPlausibleVTable(object))
	{
		Console::Warnf("vulkan: %s %p has no vtable", what, object);
		return;
	}

	void* vtable = *static_cast<void**>(object);
	const platform::Module module = platform::Module::FromAddress(vtable);
	Console::Warnf("vulkan: %s %p, vtable %s+0x%zX", what, object,
				   module.IsValid() ? module.GetName().c_str() : "?",
				   module.IsValid() ? static_cast<size_t>(static_cast<uint8_t*>(vtable) -
														  module.GetBase())
									: 0);
}

template <typename T>
bool ReadHandle(const void* object, size_t offset, T& out, const char* what)
{
	void* value = nullptr;
	if (!safemem::Read(static_cast<const uint8_t*>(object) + offset, &value, sizeof(value)) ||
		!LooksDispatchable(value))
	{
		Console::Warnf("vulkan: %s missing at +0x%zX (game update?)", what, offset);
		return false;
	}
	out = static_cast<T>(value);
	return true;
}

void ImageBarrier(VkImage image, uint32_t oldLayout, uint32_t newLayout, VkFlags srcAccess,
				  VkFlags dstAccess, VkFlags srcStage, VkFlags dstStage)
{
	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vk.CmdPipelineBarrier(g_commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
						  &barrier);
}

// --- Watching for the debugger's render target ------------------------------
//
// Vulkan binds attachments inside a command buffer, so there is no equivalent of
// OMSetRenderTargets to watch. The target is identified when it is *created*
// instead: panorama allocates a colour attachment the size of the window
// (rounded up to a multiple of 32) and pools it, so the set stays small.
VkResult __stdcall CreateImageDetour(VkDevice device, const VkImageCreateInfo* info,
									 const void* allocator, VkImage* image)
{
	const VkResult result = g_originalCreateImage(device, info, allocator, image);
	if (result != VK_SUCCESS || !info || !image)
		return result;

	// Only things that could be a panorama surface: a plain 2D colour attachment
	// that is also sampled, because the composite reads it back.
	const VkFlags wanted = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (info->imageType != VK_IMAGE_TYPE_2D || (info->usage & wanted) != wanted ||
		info->arrayLayers != 1 || info->extent.depth != 1)
		return result;

	std::lock_guard<std::mutex> lock(g_trackedMutex);
	if (g_trackedCount == kMaxTracked)
	{
		memmove(&g_tracked[0], &g_tracked[1], sizeof(g_tracked[0]) * (kMaxTracked - 1));
		--g_trackedCount;
	}
	g_tracked[g_trackedCount++] = {*image, info->extent.width, info->extent.height};
	return result;
}

void __stdcall DestroyImageDetour(VkDevice device, VkImage image, const void* allocator)
{
	{
		std::lock_guard<std::mutex> lock(g_trackedMutex);
		for (size_t i = 0; i < g_trackedCount; ++i)
		{
			if (g_tracked[i].image != image)
				continue;
			memmove(&g_tracked[i], &g_tracked[i + 1], sizeof(g_tracked[0]) * (g_trackedCount - i - 1));
			--g_trackedCount;
			break;
		}
	}
	g_originalDestroyImage(device, image, allocator);
}

// Newest match: exact size, then panorama's 32-rounded size, and nothing else.
// A "smallest that covers" fallback would match the game's own layers too.
VkImage FindDebuggerImage(uint32_t& width, uint32_t& height)
{
	const uint32_t roundedWidth = (g_extentWidth + 31) & ~31u;
	const uint32_t roundedHeight = (g_extentHeight + 31) & ~31u;

	std::lock_guard<std::mutex> lock(g_trackedMutex);
	for (size_t pass = 0; pass < 2; ++pass)
	{
		const uint32_t wantWidth = pass == 0 ? g_extentWidth : roundedWidth;
		const uint32_t wantHeight = pass == 0 ? g_extentHeight : roundedHeight;
		for (size_t i = g_trackedCount; i-- > 0;)
		{
			if (g_tracked[i].width != wantWidth || g_tracked[i].height != wantHeight)
				continue;
			width = g_tracked[i].width;
			height = g_tracked[i].height;
			return g_tracked[i].image;
		}
	}
	return VK_NULL_HANDLE_64;
}

bool InstallImageHooks()
{
	if (g_originalCreateImage)
		return true;

	const platform::Module module = platform::Module::FromName("rendersystemvulkan.dll");
	if (!module.IsValid())
		return false;

	g_createImageGlobal = reinterpret_cast<void**>(module.GetBase() + kGlobalCreateImage);
	g_destroyImageGlobal = reinterpret_cast<void**>(module.GetBase() + kGlobalDestroyImage);

	void* create = nullptr;
	void* destroy = nullptr;
	if (!safemem::Read(g_createImageGlobal, &create, sizeof(create)) ||
		!safemem::Read(g_destroyImageGlobal, &destroy, sizeof(destroy)) || !create || !destroy)
	{
		Console::Warnf("vulkan: vkCreateImage/vkDestroyImage globals are empty (game update?)");
		return false;
	}
	// The module filled these with vkGetDeviceProcAddr, so the same question on
	// the same device must give the same answers. That is the driver's
	// device-dispatch function, not the loader's exported trampoline: comparing
	// against the export fails even when the offsets are right.
	auto* expectedCreate = reinterpret_cast<void*>(vk.GetDeviceProcAddr(g_device, "vkCreateImage"));
	auto* expectedDestroy =
		reinterpret_cast<void*>(vk.GetDeviceProcAddr(g_device, "vkDestroyImage"));
	if (create != expectedCreate || destroy != expectedDestroy)
	{
		Console::Warnf("vulkan: image globals hold %p/%p, device dispatch says %p/%p -- not hooking",
					   create, destroy, expectedCreate, expectedDestroy);
		return false;
	}

	g_originalCreateImage = reinterpret_cast<PFN_vkCreateImage>(create);
	g_originalDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(destroy);

	DWORD previous = 0;
	if (VirtualProtect(g_createImageGlobal, sizeof(void*) * 2, PAGE_READWRITE, &previous))
	{
		*g_createImageGlobal = reinterpret_cast<void*>(&CreateImageDetour);
		*g_destroyImageGlobal = reinterpret_cast<void*>(&DestroyImageDetour);
		VirtualProtect(g_createImageGlobal, sizeof(void*) * 2, previous, &previous);
		return true;
	}

	g_originalCreateImage = nullptr;
	g_originalDestroyImage = nullptr;
	return false;
}

void RemoveImageHooks()
{
	if (!g_originalCreateImage)
		return;

	DWORD previous = 0;
	if (VirtualProtect(g_createImageGlobal, sizeof(void*) * 2, PAGE_READWRITE, &previous))
	{
		*g_createImageGlobal = reinterpret_cast<void*>(g_originalCreateImage);
		*g_destroyImageGlobal = reinterpret_cast<void*>(g_originalDestroyImage);
		VirtualProtect(g_createImageGlobal, sizeof(void*) * 2, previous, &previous);
	}

	g_originalCreateImage = nullptr;
	g_originalDestroyImage = nullptr;

	std::lock_guard<std::mutex> lock(g_trackedMutex);
	g_trackedCount = 0;
}

void DestroySwapchainObjects()
{
	if (!g_device)
		return;
	if (g_swapchain)
	{
		vk.DestroySwapchain(g_device, g_swapchain, nullptr);
		g_swapchain = VK_NULL_HANDLE_64;
	}
	if (g_surface && g_instance)
	{
		vk.DestroySurface(g_instance, g_surface, nullptr);
		g_surface = VK_NULL_HANDLE_64;
	}
	g_swapchainImageCount = 0;
}

// Draws our window, from the game's own render thread.
void PresentOwnWindow()
{
	if (!g_swapchain || !g_commandBuffer)
		return;

	uint32_t index = 0;
	if (vk.AcquireNextImage(g_device, g_swapchain, UINT64_MAX, g_acquired, VK_NULL_HANDLE_64,
							&index) != VK_SUCCESS ||
		index >= g_swapchainImageCount)
		return;

	vk.ResetCommandBuffer(g_commandBuffer, 0);

	VkCommandBufferBeginInfo begin = {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vk.BeginCommandBuffer(g_commandBuffer, &begin) != VK_SUCCESS)
		return;

	VkImage target = g_swapchainImages[index];
	uint32_t sourceWidth = 0;
	uint32_t sourceHeight = 0;
	const VkImage source = FindDebuggerImage(sourceWidth, sourceHeight);

	ImageBarrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
				 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				 VK_PIPELINE_STAGE_TRANSFER_BIT);

	if (source)
	{
		// The composite samples this target, so between frames it sits in
		// SHADER_READ_ONLY_OPTIMAL. Borrowed and handed straight back.
		ImageBarrier(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
					 VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					 VK_PIPELINE_STAGE_TRANSFER_BIT);

		// The window's size, not the target's: panorama rounds each dimension up
		// to a multiple of 32 and draws into the top-left, so blitting the whole
		// target squashes the image by the padding and input -- which passes raw
		// window coordinates -- then picks the wrong element.
		VkImageBlit blit = {};
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1] = {static_cast<int32_t>(sourceWidth < g_extentWidth ? sourceWidth
																			  : g_extentWidth),
							  static_cast<int32_t>(sourceHeight < g_extentHeight ? sourceHeight
																				: g_extentHeight),
							  1};
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[1] = {static_cast<int32_t>(g_extentWidth),
							  static_cast<int32_t>(g_extentHeight), 1};
		vk.CmdBlitImage(g_commandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

		ImageBarrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
					 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
					 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}
	else
	{
		// Nothing identified yet: a flat fill, so an empty window reads as "no
		// target found" rather than as a dead plugin.
		VkClearColorValue colour = {};
		colour.float32[0] = 0.10f;
		colour.float32[1] = 0.12f;
		colour.float32[2] = 0.16f;
		colour.float32[3] = 1.0f;
		VkImageSubresourceRange range = {};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.levelCount = 1;
		range.layerCount = 1;
		vk.CmdClearColorImage(g_commandBuffer, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							  &colour, 1, &range);
	}

	ImageBarrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
				 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	if (vk.EndCommandBuffer(g_commandBuffer) != VK_SUCCESS)
		return;

	const VkFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &g_acquired;
	submit.pWaitDstStageMask = &waitStage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &g_commandBuffer;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &g_blitted;
	if (vk.QueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE_64) != VK_SUCCESS)
		return;

	VkPresentInfoKHR present = {};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &g_blitted;
	present.swapchainCount = 1;
	present.pSwapchains = &g_swapchain;
	present.pImageIndices = &index;
	vk.QueuePresent(g_queue, &present);

	// ponytail: a full stall per frame instead of per-image fences. Swap for
	// fences only if it measurably costs frames.
	vk.QueueWaitIdle(g_queue);
}

using DevicePresentFn = int64_t(__fastcall*)(void* device, int64_t swapChain, void* a3, void* a4,
											 void* a5);

int64_t __fastcall DevicePresentDetour(void* device, int64_t swapChain, void* a3, void* a4,
									   void* a5)
{
	const int64_t result =
		g_devicePresentHook.GetOriginal<DevicePresentFn>()(device, swapChain, a3, a4, a5);

	if (g_presentOurs.load(std::memory_order_relaxed))
		PresentOwnWindow();

	return result;
}

} // namespace

bool VkOwnWindowIsActive()
{
	return platform::Module::FromName("rendersystemvulkan.dll").IsValid() &&
		   GetModuleHandleA("vulkan-1.dll") != nullptr;
}

bool VkOwnWindowInitialize(void* source2RenderDevice)
{
	if (g_device)
		return true;
	if (!source2RenderDevice || !LoadEntryPoints())
		return false;

	// rendersystemvulkan exports RenderDevice003 and answers with the same
	// static object engine2 was given; the offsets are only valid against it.
	const platform::Module module = platform::Module::FromName("rendersystemvulkan.dll");
	void* device = CompleteObjectFrom(module.FindInterface("RenderDevice003"));
	if (!device)
		device = CompleteObjectFrom(source2RenderDevice);
	if (!device)
	{
		Console::Warnf("vulkan: render device %p is not an object", source2RenderDevice);
		return false;
	}

	if (!ReadHandle(device, kDeviceVkDevice, g_device, "VkDevice") ||
		!ReadHandle(device, kDeviceVkPhysicalDevice, g_physicalDevice, "VkPhysicalDevice") ||
		!ReadHandle(device, kDeviceVkQueue, g_queue, "VkQueue"))
	{
		ReportObject("render device", device);
		DumpHandleFields(device);
		g_device = nullptr;
		return false;
	}

	if (!safemem::Read(static_cast<uint8_t*>(device) + kDeviceQueueFamily,
					   &g_queueFamily, sizeof(g_queueFamily)) ||
		g_queueFamily > 64)
	{
		Console::Warnf("vulkan: queue family index at +0x%zX is not plausible", kDeviceQueueFamily);
		g_device = nullptr;
		return false;
	}

	// The instance belongs to the manager, not the device.
	void* manager = CompleteObjectFrom(module.FindInterface("RenderDeviceMgr001"));
	if (!manager || !ReadHandle(manager, kMgrVkInstance, g_instance, "VkInstance"))
	{
		ReportObject("render device manager", manager);
		g_device = nullptr;
		return false;
	}

	// Now, not when the debugger opens: panorama pools its targets, so one
	// allocated before the hook is in place would never be seen.
	if (!InstallImageHooks())
		Console::Warnf("vulkan: render targets cannot be observed -- the window will stay flat");

	Console::Printf("vulkan: instance %p physical %p device %p queue %p family %u",
					static_cast<void*>(g_instance), static_cast<void*>(g_physicalDevice),
					static_cast<void*>(g_device), static_cast<void*>(g_queue), g_queueFamily);
	return true;
}

bool VkOwnWindowCreateSwapChain(void* hwnd, int width, int height)
{
	if (!g_device || !hwnd || width <= 0 || height <= 0)
		return false;

	// Everything in flight refers to the old swapchain's images.
	vk.DeviceWaitIdle(g_device);
	DestroySwapchainObjects();

	VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
	surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	surfaceInfo.hinstance = GetModuleHandleW(nullptr);
	surfaceInfo.hwnd = hwnd;
	if (vk.CreateWin32Surface(g_instance, &surfaceInfo, nullptr, &g_surface) != VK_SUCCESS)
	{
		Console::Warnf("vulkan: vkCreateWin32SurfaceKHR failed");
		return false;
	}

	uint32_t supported = 0;
	if (vk.GetSurfaceSupport(g_physicalDevice, g_queueFamily, g_surface, &supported) !=
			VK_SUCCESS ||
		!supported)
	{
		Console::Warnf("vulkan: queue family %u cannot present to this window", g_queueFamily);
		DestroySwapchainObjects();
		return false;
	}

	VkSurfaceCapabilitiesKHR caps = {};
	if (vk.GetSurfaceCapabilities(g_physicalDevice, g_surface, &caps) != VK_SUCCESS)
	{
		DestroySwapchainObjects();
		return false;
	}

	uint32_t formatCount = 0;
	VkSurfaceFormatKHR formats[32] = {};
	vk.GetSurfaceFormats(g_physicalDevice, g_surface, &formatCount, nullptr);
	if (formatCount == 0)
	{
		Console::Warnf("vulkan: the surface reports no formats");
		DestroySwapchainObjects();
		return false;
	}
	if (formatCount > 32)
		formatCount = 32;
	vk.GetSurfaceFormats(g_physicalDevice, g_surface, &formatCount, formats);

	// B8G8R8A8_UNORM (44) to match the game's targets, else whatever comes
	// first, which the specification guarantees is supported.
	VkSurfaceFormatKHR chosen = formats[0];
	for (uint32_t i = 0; i < formatCount; ++i)
	{
		if (formats[i].format == 44)
		{
			chosen = formats[i];
			break;
		}
	}

	g_extentWidth = caps.currentExtent.width == 0xFFFFFFFFu ? static_cast<uint32_t>(width)
															: caps.currentExtent.width;
	g_extentHeight = caps.currentExtent.height == 0xFFFFFFFFu ? static_cast<uint32_t>(height)
															  : caps.currentExtent.height;
	if (g_extentWidth == 0 || g_extentHeight == 0)
	{
		DestroySwapchainObjects();
		return false;
	}

	uint32_t imageCount = caps.minImageCount + 1;
	if (caps.maxImageCount != 0 && imageCount > caps.maxImageCount)
		imageCount = caps.maxImageCount;

	VkSwapchainCreateInfoKHR info = {};
	info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.surface = g_surface;
	info.minImageCount = imageCount;
	info.imageFormat = chosen.format;
	info.imageColorSpace = chosen.colorSpace;
	info.imageExtent.width = g_extentWidth;
	info.imageExtent.height = g_extentHeight;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = caps.currentTransform;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	// Immediate where offered: this runs on the game's render thread, so a FIFO
	// swapchain would gate the game's frame rate on our window's vsync. FIFO is
	// the fallback because it is the only guaranteed mode.
	uint32_t modeCount = 0;
	uint32_t modes[8] = {};
	vk.GetSurfacePresentModes(g_physicalDevice, g_surface, &modeCount, nullptr);
	if (modeCount > 8)
		modeCount = 8;
	vk.GetSurfacePresentModes(g_physicalDevice, g_surface, &modeCount, modes);

	info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	for (uint32_t i = 0; i < modeCount; ++i)
	{
		if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
		{
			info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			break;
		}
	}
	info.clipped = 1;
	if (vk.CreateSwapchain(g_device, &info, nullptr, &g_swapchain) != VK_SUCCESS)
	{
		Console::Warnf("vulkan: vkCreateSwapchainKHR failed");
		DestroySwapchainObjects();
		return false;
	}

	g_swapchainImageCount = 8;
	if (vk.GetSwapchainImages(g_device, g_swapchain, &g_swapchainImageCount, g_swapchainImages) !=
		VK_SUCCESS)
	{
		DestroySwapchainObjects();
		return false;
	}

	if (!g_commandPool)
	{
		VkCommandPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = g_queueFamily;
		if (vk.CreateCommandPool(g_device, &poolInfo, nullptr, &g_commandPool) != VK_SUCCESS)
		{
			DestroySwapchainObjects();
			return false;
		}

		VkCommandBufferAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = g_commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		if (vk.AllocateCommandBuffers(g_device, &allocInfo, &g_commandBuffer) != VK_SUCCESS)
		{
			DestroySwapchainObjects();
			return false;
		}

		VkSemaphoreCreateInfo semaphoreInfo = {};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		if (vk.CreateSemaphore(g_device, &semaphoreInfo, nullptr, &g_acquired) != VK_SUCCESS ||
			vk.CreateSemaphore(g_device, &semaphoreInfo, nullptr, &g_blitted) != VK_SUCCESS)
		{
			DestroySwapchainObjects();
			return false;
		}
	}

	Console::Printf("vulkan: swapchain %ux%u, format %u, %u images, present mode %u", g_extentWidth,
					g_extentHeight, chosen.format, g_swapchainImageCount, info.presentMode);
	return true;
}

bool VkOwnWindowInstallDevicePresentHook(void* source2RenderDevice)
{
	if (g_devicePresentHook.IsInstalled())
	{
		g_presentOurs.store(true, std::memory_order_relaxed);
		return true;
	}
	if (!source2RenderDevice || !safemem::HasPlausibleVTable(source2RenderDevice))
		return false;

	void** vtable = *static_cast<void***>(source2RenderDevice);
	if (!g_devicePresentHook.Install(vtable, kDevicePresentSlot,
									 reinterpret_cast<void*>(&DevicePresentDetour)))
		return false;

	g_presentOurs.store(true, std::memory_order_relaxed);
	return true;
}

void VkOwnWindowCloseWindow()
{
	g_presentOurs.store(false, std::memory_order_relaxed);
	g_devicePresentHook.Remove();

	if (!g_device || !g_loaded)
		return;

	vk.DeviceWaitIdle(g_device);
	DestroySwapchainObjects();
	if (g_acquired)
		vk.DestroySemaphore(g_device, g_acquired, nullptr);
	if (g_blitted)
		vk.DestroySemaphore(g_device, g_blitted, nullptr);
	if (g_commandPool)
		vk.DestroyCommandPool(g_device, g_commandPool, nullptr);

	g_acquired = VK_NULL_HANDLE_64;
	g_blitted = VK_NULL_HANDLE_64;
	g_commandPool = VK_NULL_HANDLE_64;
	g_commandBuffer = nullptr;
}

void VkOwnWindowShutdown()
{
	VkOwnWindowCloseWindow();
	RemoveImageHooks();

	g_instance = nullptr;
	g_physicalDevice = nullptr;
	g_device = nullptr;
	g_queue = nullptr;
	g_queueFamily = VK_QUEUE_FAMILY_IGNORED;
}

} // namespace panodbg
