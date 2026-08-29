#include "core/safemem.h"

#include <windows.h>

namespace panodbg::safemem
{

namespace
{

constexpr DWORD kReadableProtect = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
								   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
								   PAGE_EXECUTE_WRITECOPY;

constexpr DWORD kExecutableProtect =
	PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

constexpr DWORD kNoAccess = PAGE_NOACCESS | PAGE_GUARD;

bool RegionAllows(const void* address, size_t size, DWORD allowedProtect)
{
	auto cursor = reinterpret_cast<uintptr_t>(address);
	const uintptr_t end = cursor + size;

	while (cursor < end)
	{
		MEMORY_BASIC_INFORMATION info = {};
		if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &info, sizeof(info)) == 0)
			return false;

		if (info.State != MEM_COMMIT)
			return false;
		if (info.Protect & kNoAccess)
			return false;
		if ((info.Protect & allowedProtect) == 0)
			return false;

		const uintptr_t regionEnd =
			reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
		if (regionEnd <= cursor)
			return false;

		cursor = regionEnd;
	}

	return true;
}

} // namespace

bool IsReadable(const void* address, size_t size)
{
	if (!address || size == 0)
		return false;

	return RegionAllows(address, size, kReadableProtect);
}

bool IsExecutable(const void* address)
{
	if (!address)
		return false;

	return RegionAllows(address, 1, kExecutableProtect);
}

bool Read(const void* address, void* out, size_t size)
{
	if (!address || size == 0)
		return false;

	// ReadProcessMemory on our own process returns ERROR_PARTIAL_COPY on a bad
	// address rather than raising, so no VirtualQuery pre-check is needed.
	SIZE_T read = 0;
	if (!ReadProcessMemory(GetCurrentProcess(), address, out, size, &read))
		return false;

	return read == size;
}

void* ReadPointer(const void* address)
{
	void* value = nullptr;
	if (!Read(address, &value, sizeof(value)))
		return nullptr;
	return value;
}

bool HasPlausibleVTable(const void* object)
{
	if (!object || (reinterpret_cast<uintptr_t>(object) & 0x7) != 0)
		return false;

	void* vtable = ReadPointer(object);
	if (!vtable || (reinterpret_cast<uintptr_t>(vtable) & 0x7) != 0)
		return false;

	// The first slot of a real vtable is a function pointer into executable code.
	void* firstSlot = ReadPointer(vtable);
	return IsExecutable(firstSlot);
}

} // namespace panodbg::safemem
