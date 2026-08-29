#pragma once

#include <string>

namespace panodbg
{

// The game console is the only output. Every resolution step and every refusal
// prints, because a plugin that silently stops working after a game update is
// indistinguishable from a dead key.
class Console
{
public:
	static void Print(const std::string& line);
	static void Printf(const char* format, ...);

	// Same, but coloured as a warning. Used for the things that mean "this build
	// no longer matches the binaries" rather than "this did not happen".
	static void Warn(const std::string& line);
	static void Warnf(const char* format, ...);
};

} // namespace panodbg
