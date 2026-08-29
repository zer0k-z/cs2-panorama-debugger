#pragma once

#include <cstddef>
#include <cstdint>

namespace panodbg::safemem
{

// Every dereference of an engine-owned pointer goes through here. Uses
// VirtualQuery / ReadProcessMemory rather than SEH, so it is safe in frames
// holding C++ objects with destructors.

bool IsReadable(const void* address, size_t size);
bool IsExecutable(const void* address);

// Copies size bytes, returning false without touching out on a bad address.
bool Read(const void* address, void* out, size_t size);

template <typename T>
bool ReadValue(const void* address, T& out)
{
	return Read(address, &out, sizeof(T));
}

// Reads a pointer-sized value. Returns nullptr if the address is unreadable.
void* ReadPointer(const void* address);

// True when *object looks like a vtable pointer: readable, aligned, and
// pointing at readable function pointers.
bool HasPlausibleVTable(const void* object);

// Copies a NUL-terminated string, capped at maxLength. Returns false if the
// string is unreadable or unterminated within the cap.
bool ReadString(const void* address, char* out, size_t maxLength);

} // namespace panodbg::safemem
