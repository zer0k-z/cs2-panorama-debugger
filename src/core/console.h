#pragma once

namespace panodbg
{

// The game console is the only output. Every resolution step and every refusal
// prints, because a plugin that silently stops working after a game update is
// indistinguishable from a dead key.
//
// printf-style only, so the format string must always be a literal.
class Console
{
public:
	static void Printf(const char* format, ...);
	// Same, but coloured as a warning: for "this build no longer matches the
	// binaries" rather than "this did not happen".
	static void Warnf(const char* format, ...);
};

} // namespace panodbg
