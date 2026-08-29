#pragma once

// The slice of Vulkan the copy path uses, declared by hand so the build needs no
// Vulkan SDK. Everything here is ABI-fixed by the specification, but the layouts
// must still be exact.

#include <cstdint>

#define VK_DEFINE_HANDLE(name) typedef struct name##_T* name
VK_DEFINE_HANDLE(VkInstance);
VK_DEFINE_HANDLE(VkPhysicalDevice);
VK_DEFINE_HANDLE(VkDevice);
VK_DEFINE_HANDLE(VkQueue);
VK_DEFINE_HANDLE(VkCommandBuffer);
#undef VK_DEFINE_HANDLE

// Non-dispatchable handles are 64-bit on every platform we care about.
typedef uint64_t VkSurfaceKHR;
typedef uint64_t VkSwapchainKHR;
typedef uint64_t VkImage;
typedef uint64_t VkSemaphore;
typedef uint64_t VkFence;
typedef uint64_t VkCommandPool;

typedef uint32_t VkFlags;
typedef int32_t VkResult;

constexpr VkResult VK_SUCCESS = 0;
constexpr uint32_t VK_QUEUE_FAMILY_IGNORED = ~0u;
constexpr uint64_t VK_NULL_HANDLE_64 = 0;

// Only the structure types actually constructed below.
enum VkStructureType : uint32_t
{
	VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
	VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
	VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
	VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
	VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
	VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
	VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001,
	VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR = 1000009000,
	VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9,
};

constexpr uint32_t VK_IMAGE_LAYOUT_UNDEFINED = 0;
constexpr uint32_t VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6;
constexpr uint32_t VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7;
constexpr uint32_t VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002;
constexpr uint32_t VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5;

constexpr uint32_t VK_IMAGE_ASPECT_COLOR_BIT = 0x1;
constexpr uint32_t VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x2;
constexpr uint32_t VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x10;
constexpr uint32_t VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 0x1;
constexpr uint32_t VK_PIPELINE_STAGE_TRANSFER_BIT = 0x1000;
constexpr uint32_t VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 0x2000;
constexpr uint32_t VK_ACCESS_TRANSFER_READ_BIT = 0x800;
constexpr uint32_t VK_ACCESS_TRANSFER_WRITE_BIT = 0x1000;
constexpr uint32_t VK_ACCESS_MEMORY_READ_BIT = 0x8000;
constexpr uint32_t VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x1;
constexpr uint32_t VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x2;
constexpr uint32_t VK_SHARING_MODE_EXCLUSIVE = 0;
constexpr uint32_t VK_PRESENT_MODE_FIFO_KHR = 2;
constexpr uint32_t VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR = 0x1;
constexpr uint32_t VK_FILTER_LINEAR = 1;
constexpr uint32_t VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0;

struct VkExtent2D
{
	uint32_t width;
	uint32_t height;
};

struct VkExtent3D
{
	uint32_t width;
	uint32_t height;
	uint32_t depth;
};

struct VkOffset3D
{
	int32_t x;
	int32_t y;
	int32_t z;
};

struct VkWin32SurfaceCreateInfoKHR
{
	VkStructureType sType;
	const void* pNext;
	VkFlags flags;
	void* hinstance;
	void* hwnd;
};

struct VkSurfaceCapabilitiesKHR
{
	uint32_t minImageCount;
	uint32_t maxImageCount;
	VkExtent2D currentExtent;
	VkExtent2D minImageExtent;
	VkExtent2D maxImageExtent;
	uint32_t maxImageArrayLayers;
	VkFlags supportedTransforms;
	uint32_t currentTransform;
	VkFlags supportedCompositeAlpha;
	VkFlags supportedUsageFlags;
};

struct VkSurfaceFormatKHR
{
	uint32_t format;
	uint32_t colorSpace;
};

struct VkSwapchainCreateInfoKHR
{
	VkStructureType sType;
	const void* pNext;
	VkFlags flags;
	VkSurfaceKHR surface;
	uint32_t minImageCount;
	uint32_t imageFormat;
	uint32_t imageColorSpace;
	VkExtent2D imageExtent;
	uint32_t imageArrayLayers;
	VkFlags imageUsage;
	uint32_t imageSharingMode;
	uint32_t queueFamilyIndexCount;
	const uint32_t* pQueueFamilyIndices;
	uint32_t preTransform;
	uint32_t compositeAlpha;
	uint32_t presentMode;
	uint32_t clipped;
	VkSwapchainKHR oldSwapchain;
};

struct VkCommandPoolCreateInfo
{
	VkStructureType sType;
	const void* pNext;
	VkFlags flags;
	uint32_t queueFamilyIndex;
};

struct VkCommandBufferAllocateInfo
{
	VkStructureType sType;
	const void* pNext;
	VkCommandPool commandPool;
	uint32_t level;
	uint32_t commandBufferCount;
};

struct VkCommandBufferBeginInfo
{
	VkStructureType sType;
	const void* pNext;
	VkFlags flags;
	const void* pInheritanceInfo;
};

struct VkImageSubresourceRange
{
	VkFlags aspectMask;
	uint32_t baseMipLevel;
	uint32_t levelCount;
	uint32_t baseArrayLayer;
	uint32_t layerCount;
};

struct VkImageSubresourceLayers
{
	VkFlags aspectMask;
	uint32_t mipLevel;
	uint32_t baseArrayLayer;
	uint32_t layerCount;
};

struct VkImageMemoryBarrier
{
	VkStructureType sType;
	const void* pNext;
	VkFlags srcAccessMask;
	VkFlags dstAccessMask;
	uint32_t oldLayout;
	uint32_t newLayout;
	uint32_t srcQueueFamilyIndex;
	uint32_t dstQueueFamilyIndex;
	VkImage image;
	VkImageSubresourceRange subresourceRange;
};

struct VkImageBlit
{
	VkImageSubresourceLayers srcSubresource;
	VkOffset3D srcOffsets[2];
	VkImageSubresourceLayers dstSubresource;
	VkOffset3D dstOffsets[2];
};

struct VkSubmitInfo
{
	VkStructureType sType;
	const void* pNext;
	uint32_t waitSemaphoreCount;
	const VkSemaphore* pWaitSemaphores;
	const VkFlags* pWaitDstStageMask;
	uint32_t commandBufferCount;
	const VkCommandBuffer* pCommandBuffers;
	uint32_t signalSemaphoreCount;
	const VkSemaphore* pSignalSemaphores;
};

struct VkPresentInfoKHR
{
	VkStructureType sType;
	const void* pNext;
	uint32_t waitSemaphoreCount;
	const VkSemaphore* pWaitSemaphores;
	uint32_t swapchainCount;
	const VkSwapchainKHR* pSwapchains;
	const uint32_t* pImageIndices;
	VkResult* pResults;
};

struct VkSemaphoreCreateInfo
{
	VkStructureType sType;
	const void* pNext;
	VkFlags flags;
};

using PFN_vkVoidFunction = void(__stdcall*)();
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(__stdcall*)(VkDevice, const char*);
using PFN_vkCreateWin32SurfaceKHR = VkResult(__stdcall*)(VkInstance,
														 const VkWin32SurfaceCreateInfoKHR*,
														 const void*, VkSurfaceKHR*);
using PFN_vkGetPhysicalDeviceSurfaceSupportKHR = VkResult(__stdcall*)(VkPhysicalDevice, uint32_t,
																	  VkSurfaceKHR, uint32_t*);
using PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
	VkResult(__stdcall*)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
using PFN_vkGetPhysicalDeviceSurfaceFormatsKHR = VkResult(__stdcall*)(VkPhysicalDevice, VkSurfaceKHR,
																	  uint32_t*,
																	  VkSurfaceFormatKHR*);
using PFN_vkCreateSwapchainKHR = VkResult(__stdcall*)(VkDevice, const VkSwapchainCreateInfoKHR*,
													  const void*, VkSwapchainKHR*);
using PFN_vkDestroySwapchainKHR = void(__stdcall*)(VkDevice, VkSwapchainKHR, const void*);
using PFN_vkGetSwapchainImagesKHR = VkResult(__stdcall*)(VkDevice, VkSwapchainKHR, uint32_t*,
														 VkImage*);
using PFN_vkAcquireNextImageKHR = VkResult(__stdcall*)(VkDevice, VkSwapchainKHR, uint64_t,
													   VkSemaphore, VkFence, uint32_t*);
using PFN_vkQueuePresentKHR = VkResult(__stdcall*)(VkQueue, const VkPresentInfoKHR*);
using PFN_vkCreateCommandPool = VkResult(__stdcall*)(VkDevice, const VkCommandPoolCreateInfo*,
													 const void*, VkCommandPool*);
using PFN_vkAllocateCommandBuffers = VkResult(__stdcall*)(VkDevice,
														  const VkCommandBufferAllocateInfo*,
														  VkCommandBuffer*);
using PFN_vkBeginCommandBuffer = VkResult(__stdcall*)(VkCommandBuffer,
													  const VkCommandBufferBeginInfo*);
using PFN_vkEndCommandBuffer = VkResult(__stdcall*)(VkCommandBuffer);
using PFN_vkCmdPipelineBarrier = void(__stdcall*)(VkCommandBuffer, VkFlags, VkFlags, VkFlags,
												  uint32_t, const void*, uint32_t, const void*,
												  uint32_t, const VkImageMemoryBarrier*);
using PFN_vkCmdBlitImage = void(__stdcall*)(VkCommandBuffer, VkImage, uint32_t, VkImage, uint32_t,
											uint32_t, const VkImageBlit*, uint32_t);
using PFN_vkQueueSubmit = VkResult(__stdcall*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
using PFN_vkQueueWaitIdle = VkResult(__stdcall*)(VkQueue);
using PFN_vkDeviceWaitIdle = VkResult(__stdcall*)(VkDevice);
using PFN_vkCreateSemaphore = VkResult(__stdcall*)(VkDevice, const VkSemaphoreCreateInfo*,
												   const void*, VkSemaphore*);
using PFN_vkDestroySemaphore = void(__stdcall*)(VkDevice, VkSemaphore, const void*);
using PFN_vkDestroySurfaceKHR = void(__stdcall*)(VkInstance, VkSurfaceKHR, const void*);
using PFN_vkDestroyCommandPool = void(__stdcall*)(VkDevice, VkCommandPool, const void*);

union VkClearColorValue
{
	float float32[4];
	int32_t int32[4];
	uint32_t uint32[4];
};

using PFN_vkCmdClearColorImage = void(__stdcall*)(VkCommandBuffer, VkImage, uint32_t,
												  const VkClearColorValue*, uint32_t,
												  const VkImageSubresourceRange*);
using PFN_vkResetCommandBuffer = VkResult(__stdcall*)(VkCommandBuffer, VkFlags);
using PFN_vkGetPhysicalDeviceSurfacePresentModesKHR =
	VkResult(__stdcall*)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, uint32_t*);

constexpr uint32_t VK_PRESENT_MODE_IMMEDIATE_KHR = 0;

constexpr uint32_t VK_IMAGE_TYPE_2D = 1;
constexpr uint32_t VK_IMAGE_USAGE_SAMPLED_BIT = 0x4;
constexpr uint32_t VK_ACCESS_SHADER_READ_BIT = 0x20;
constexpr uint32_t VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT = 0x80;
constexpr VkStructureType VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = VkStructureType(14);

struct VkImageCreateInfo
{
	VkStructureType sType;
	const void* pNext;
	VkFlags flags;
	uint32_t imageType;
	uint32_t format;
	VkExtent3D extent;
	uint32_t mipLevels;
	uint32_t arrayLayers;
	uint32_t samples;
	uint32_t tiling;
	VkFlags usage;
	uint32_t sharingMode;
	uint32_t queueFamilyIndexCount;
	const uint32_t* pQueueFamilyIndices;
	uint32_t initialLayout;
};

using PFN_vkCreateImage = VkResult(__stdcall*)(VkDevice, const VkImageCreateInfo*, const void*,
											   VkImage*);
using PFN_vkDestroyImage = void(__stdcall*)(VkDevice, VkImage, const void*);
