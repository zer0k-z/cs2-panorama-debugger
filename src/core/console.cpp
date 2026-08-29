#include "core/console.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "tier0/dbg.h"

namespace panodbg
{

namespace
{

constexpr const char* kPrefix = "[panorama-debugger] ";

// Also to the debugger output, because the console is not up yet while the
// plugin resolves its patterns during load.
void Emit(const char* format, va_list args, bool warning)
{
	char buffer[1024];
	_vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);

	const std::string text = kPrefix + std::string(buffer) + "\n";
	OutputDebugStringA(text.c_str());

	if (warning)
		Warning("%s", text.c_str());
	else
		Msg("%s", text.c_str());
}

} // namespace

void Console::Printf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	Emit(format, args, false);
	va_end(args);
}

void Console::Warnf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	Emit(format, args, true);
	va_end(args);
}

} // namespace panodbg
