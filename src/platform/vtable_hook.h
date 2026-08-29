#pragma once

#include <cstddef>

namespace panodbg::platform
{

// Replaces one entry of an existing vtable. Every hook here is a virtual call,
// so patching the slot is enough -- no SourceHook, no inline-hook library.
class VTableHook
{
public:
	VTableHook() = default;
	~VTableHook();

	VTableHook(const VTableHook&) = delete;
	VTableHook& operator=(const VTableHook&) = delete;

	// Writes detour into vtable[index], remembering the original.
	bool Install(void** vtable, size_t index, void* detour);
	// Puts the original entry back. Safe to call more than once.
	void Remove();

	bool IsInstalled() const { return m_entry != nullptr; }

	template <typename Fn>
	Fn GetOriginal() const
	{
		return reinterpret_cast<Fn>(m_original);
	}

private:
	void** m_entry = nullptr;
	void* m_original = nullptr;
};

} // namespace panodbg::platform
