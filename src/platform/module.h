#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace panodbg::platform
{

struct Section
{
	std::string name;
	uint8_t* base = nullptr;
	size_t size = 0;
	uint32_t characteristics = 0;

	bool Contains(const void* address) const
	{
		const auto value = reinterpret_cast<uintptr_t>(address);
		return value >= reinterpret_cast<uintptr_t>(base) &&
			   value < reinterpret_cast<uintptr_t>(base) + size;
	}

	bool IsExecutable() const { return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0; }
};

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
	// Full path on disk, which is what actually distinguishes two modules that
	// share a base name.
	const std::string& GetPath() const { return m_path; }
	uint8_t* GetBase() const { return m_base; }
	size_t GetSize() const { return m_size; }
	HMODULE GetHandle() const { return m_handle; }

	const Section* GetSection(const char* name) const;
	// Preferred over GetSection(".rdata"): which read-only section holds the
	// vtables is a linker detail.
	const Section* FindSectionContaining(const void* address) const;

	// CreateInterface(version). nullptr when the export or the version is absent.
	void* FindInterface(const char* version) const;

	// Byte pattern search. '?' bytes in mask are wildcards; mask must be the same
	// length as pattern, using 'x' for "must match".
	void* FindPattern(const uint8_t* pattern, const char* mask, const Section* section = nullptr)
		const;

	// Class name -> vtable, via the MSVC RTTI tables. offset picks the base
	// sub-object (0 for the primary one).
	void* FindVirtualTable(const std::string& className, int32_t offset = 0) const;

private:
	void InitializeSections();

	HMODULE m_handle = nullptr;
	std::string m_name;
	std::string m_path;
	uint8_t* m_base = nullptr;
	size_t m_size = 0;
	std::vector<Section> m_sections;
};

} // namespace panodbg::platform
