#include "platform/module.h"

#include <psapi.h>

#include <cstring>

#include "core/console.h"
#include "core/safemem.h"

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

// Searches haystack for an exact byte run. Used for the RTTI table walks, where
// the needles are type-descriptor names and 4-byte RVAs.
const uint8_t* FindBytes(const uint8_t* haystack, size_t haystackSize, const uint8_t* needle,
						 size_t needleSize, const uint8_t* startAt)
{
	if (needleSize == 0 || haystackSize < needleSize)
		return nullptr;

	const uint8_t* cursor = startAt ? startAt : haystack;
	const uint8_t* end = haystack + haystackSize - needleSize;

	for (; cursor <= end; ++cursor)
	{
		if (memcmp(cursor, needle, needleSize) == 0)
			return cursor;
	}

	return nullptr;
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
	{
		module.m_path = path;
		module.m_name = BaseName(module.m_path);
	}

	module.m_handle = handle;
	module.m_base = static_cast<uint8_t*>(info.lpBaseOfDll);
	module.m_size = info.SizeOfImage;
	module.InitializeSections();
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

void Module::InitializeSections()
{
	const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(m_base);
	if (!safemem::IsReadable(dosHeader, sizeof(IMAGE_DOS_HEADER)) ||
		dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
	{
		return;
	}

	const auto* ntHeader =
		reinterpret_cast<const IMAGE_NT_HEADERS64*>(m_base + dosHeader->e_lfanew);
	if (!safemem::IsReadable(ntHeader, sizeof(IMAGE_NT_HEADERS64)) ||
		ntHeader->Signature != IMAGE_NT_SIGNATURE)
	{
		return;
	}

	const IMAGE_SECTION_HEADER* sectionHeader = IMAGE_FIRST_SECTION(ntHeader);
	const int count = ntHeader->FileHeader.NumberOfSections;

	m_sections.reserve(static_cast<size_t>(count));

	for (int i = 0; i < count; ++i)
	{
		// Section names are 8 bytes and are not NUL-terminated when full.
		char name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
		memcpy(name, sectionHeader[i].Name, IMAGE_SIZEOF_SHORT_NAME);

		Section section;
		section.name = name;
		section.characteristics = sectionHeader[i].Characteristics;
		section.base = m_base + sectionHeader[i].VirtualAddress;
		// VirtualSize is the mapped size; SizeOfRawData can be smaller or larger.
		section.size = sectionHeader[i].Misc.VirtualSize != 0
						   ? sectionHeader[i].Misc.VirtualSize
						   : sectionHeader[i].SizeOfRawData;

		m_sections.push_back(std::move(section));
	}
}

const Section* Module::GetSection(const char* name) const
{
	for (const Section& section : m_sections)
	{
		if (section.name == name)
			return &section;
	}
	return nullptr;
}

const Section* Module::FindSectionContaining(const void* address) const
{
	for (const Section& section : m_sections)
	{
		if (section.Contains(address))
			return &section;
	}
	return nullptr;
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

void* Module::FindPattern(const uint8_t* pattern, const char* mask, const Section* section) const
{
	if (!IsValid() || !pattern || !mask)
		return nullptr;

	const uint8_t* start = section ? section->base : m_base;
	const size_t size = section ? section->size : m_size;
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

void* Module::FindVirtualTable(const std::string& className, int32_t offset) const
{
	if (!IsValid())
		return nullptr;

	const Section* data = GetSection(".data");
	const Section* rdata = GetSection(".rdata");
	if (!data || !rdata)
		return nullptr;

	// MSVC stores the decorated name inside the type descriptor, which itself
	// starts 0x10 bytes earlier (vftable pointer + spare).
	const std::string decorated = ".?AV" + className + "@@";

	const uint8_t* nameLocation =
		FindBytes(data->base, data->size, reinterpret_cast<const uint8_t*>(decorated.c_str()),
				  decorated.size() + 1, nullptr);
	if (!nameLocation)
		return nullptr;

	const uint8_t* typeDescriptor = nameLocation - 0x10;
	const auto typeDescriptorRva =
		static_cast<uint32_t>(reinterpret_cast<uintptr_t>(typeDescriptor) -
							  reinterpret_cast<uintptr_t>(m_base));

	// Every complete object locator referring to this type stores that RVA at
	// +0x0C, so a match points 0x0C into a candidate locator.
	const uint8_t* cursor = nullptr;
	while ((cursor = FindBytes(rdata->base, rdata->size,
							   reinterpret_cast<const uint8_t*>(&typeDescriptorRva),
							   sizeof(typeDescriptorRva), cursor)) != nullptr)
	{
		const uint8_t* locator = cursor - 0x0C;
		cursor += sizeof(typeDescriptorRva);

		if (!safemem::IsReadable(locator, 0x18))
			continue;

		// Signature is 1 for 64-bit images.
		if (*reinterpret_cast<const int32_t*>(locator) != 1)
			continue;

		// Which base sub-object this vtable belongs to.
		if (*reinterpret_cast<const int32_t*>(locator + 0x04) != offset)
			continue;

		// The vtable's slot -1 holds a pointer to the locator, so finding that
		// pointer in .rdata locates the vtable itself one slot later.
		const uint8_t* locatorReference =
			FindBytes(rdata->base, rdata->size, reinterpret_cast<const uint8_t*>(&locator),
					  sizeof(locator), nullptr);
		if (!locatorReference)
			continue;

		return const_cast<uint8_t*>(locatorReference + sizeof(void*));
	}

	return nullptr;
}

} // namespace panodbg::platform
