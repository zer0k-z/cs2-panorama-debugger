#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace panodbg::platform
{

// A module already loaded in this process. Adapted from CS2ServerGUI's CModule,
// minus mounting by game-relative path, and with every lookup failing softly
// rather than calling Error().
class Module
{
public:
	Module() = default;

	// Base names are not unique (Metamod loads its own server.dll), so this
	// returns whichever the loader lists first. Prefer FromAddress.
	static Module FromName(const char* moduleName);
	// The module owning an address, or an invalid Module. Unambiguous even when
	// several loaded modules share a base name.
	static Module FromAddress(const void* address);
	// Wraps an already-resolved handle.
	static Module FromHandle(HMODULE handle);

	bool IsValid() const { return m_handle != nullptr; }
	// Base name, e.g. "server.dll". Not unique across loaded modules.
	const std::string& GetName() const { return m_name; }
	uint8_t* GetBase() const { return m_base; }
	size_t GetSize() const { return m_size; }
	HMODULE GetHandle() const { return m_handle; }

	// CreateInterface(version). nullptr when the export or the version is absent.
	void* FindInterface(const char* version) const;

	// Byte pattern search over the whole module. '?' bytes in mask are wildcards;
	// mask must be the same length as pattern, using 'x' for "must match".
	void* FindPattern(const uint8_t* pattern, const char* mask) const;

private:
	HMODULE m_handle = nullptr;
	std::string m_name;
	uint8_t* m_base = nullptr;
	size_t m_size = 0;
};

} // namespace panodbg::platform
