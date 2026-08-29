#include "platform/vtable_hook.h"

#include <windows.h>

#include "core/safemem.h"

namespace panodbg::platform
{

VTableHook::~VTableHook()
{
	Remove();
}

bool VTableHook::Install(void** vtable, size_t index, void* detour)
{
	if (!vtable || !detour || IsInstalled())
		return false;

	void** entry = vtable + index;
	if (!safemem::IsReadable(entry, sizeof(void*)))
		return false;

	// Vtables live in read-only data, so the page has to be made writable for the
	// single pointer store and then put back.
	DWORD oldProtect = 0;
	if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
		return false;

	m_original = *entry;
	InterlockedExchangePointer(entry, detour);

	DWORD restored = 0;
	VirtualProtect(entry, sizeof(void*), oldProtect, &restored);

	m_entry = entry;
	return true;
}

void VTableHook::Remove()
{
	if (!m_entry)
		return;

	DWORD oldProtect = 0;
	if (VirtualProtect(m_entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
	{
		InterlockedExchangePointer(m_entry, m_original);

		DWORD restored = 0;
		VirtualProtect(m_entry, sizeof(void*), oldProtect, &restored);
	}

	m_entry = nullptr;
	m_original = nullptr;
}

} // namespace panodbg::platform
