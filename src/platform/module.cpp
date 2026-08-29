#include "platform/module.h"

#include <psapi.h>

#include <cstring>

namespace panodbg::platform
{

namespace
{

using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);

std::string BaseName(const std::string& path)
{
	const size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

Module Module::FromHandle(HMODULE handle)
{
	Module module;

	if (!handle)
		return module;

	MODULEINFO info = {};
	if (!GetModuleInformation(GetCurrentProcess(), handle, &info, sizeof(info)))
		return module;

	char path[MAX_PATH] = {};
	if (GetModuleFileNameA(handle, path, sizeof(path)) != 0)
		module.m_name = BaseName(path);

	module.m_handle = handle;
	module.m_base = static_cast<uint8_t*>(info.lpBaseOfDll);
	module.m_size = info.SizeOfImage;
	return module;
}

Module Module::FromName(const char* moduleName)
{
	// Note that several loaded modules can share a base name -- Metamod ships its
	// own server.dll -- and this returns whichever the loader lists first. Prefer
	// FromAddress whenever an address is available.
	return FromHandle(GetModuleHandleA(moduleName));
}

Module Module::FromAddress(const void* address)
{
	HMODULE handle = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
								GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							static_cast<LPCSTR>(address), &handle))
	{
		return Module();
	}

	// The handle is used directly. Round-tripping it through its base name would
	// resolve to the wrong image whenever two loaded modules share that name,
	// which is exactly the case for server.dll under Metamod.
	return FromHandle(handle);
}

void* Module::FindInterface(const char* version) const
{
	if (!IsValid())
		return nullptr;

	const auto createInterface =
		reinterpret_cast<CreateInterfaceFn>(GetProcAddress(m_handle, "CreateInterface"));
	if (!createInterface)
		return nullptr;

	int returnCode = 0;
	return createInterface(version, &returnCode);
}

void* Module::FindPattern(const uint8_t* pattern, const char* mask) const
{
	if (!IsValid() || !pattern || !mask)
		return nullptr;

	const uint8_t* start = m_base;
	const size_t size = m_size;
	const size_t patternSize = strlen(mask);

	if (patternSize == 0 || size < patternSize)
		return nullptr;

	for (size_t offset = 0; offset <= size - patternSize; ++offset)
	{
		bool matched = true;
		for (size_t i = 0; i < patternSize; ++i)
		{
			if (mask[i] == 'x' && start[offset + i] != pattern[i])
			{
				matched = false;
				break;
			}
		}

		if (matched)
			return const_cast<uint8_t*>(start + offset);
	}

	return nullptr;
}

} // namespace panodbg::platform
