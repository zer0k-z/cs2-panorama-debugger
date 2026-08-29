#include "core/console.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

#include "tier0/dbg.h"

namespace panodbg
{

namespace
{

constexpr const char* kPrefix = "[panorama-debugger] ";

// Also to the debugger output, because the console is not up yet while the
// plugin resolves its patterns during load.
void Emit(const char* text, bool warning)
{
	OutputDebugStringA(text);

	if (warning)
		Warning("%s", text);
	else
		Msg("%s", text);
}

} // namespace

void Console::Print(const std::string& line)
{
	const std::string text = kPrefix + line + "\n";
	Emit(text.c_str(), false);
}

void Console::Warn(const std::string& line)
{
	const std::string text = kPrefix + line + "\n";
	Emit(text.c_str(), true);
}

void Console::Printf(const char* format, ...)
{
	char buffer[1024];

	va_list args;
	va_start(args, format);
	_vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
	va_end(args);

	Print(buffer);
}

void Console::Warnf(const char* format, ...)
{
	char buffer[1024];

	va_list args;
	va_start(args, format);
	_vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
	va_end(args);

	Warn(buffer);
}

} // namespace panodbg
