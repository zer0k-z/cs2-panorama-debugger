#pragma once

namespace panodbg
{

// CS2 ships the real Panorama debugger but never builds it: engine2's
// CPanoramaEngineHandler::Init passes 0 for the debugger-enable argument and
// registers none of Dota's CreateDebuggerWindow handlers. This reconstructs that
// caller and toggles it with F6. See README.md.

// Resolves everything and installs the frame hook. Retries only what is still
// missing, so it is safe to call until it returns true.
bool Initialize();

// IVEngineServer2::ServerCommand, supplied by the plugin. Used once from the
// first frame, for kStartupCommands.
void SetConsoleCommandSink(void (*sink)(const char* command));

// Removes every hook. Must run before the DLL unloads.
void Shutdown();

// Queued from the window procedure, applied on the game thread.
void RequestOwnWindowResize(int width, int height);
void NotifyForeignMouseDown();
void RequestDebuggerClose();

// The debugger window's CUIWindowInput. Null until the window exists.
void* GetOwnWindowInput();
void SetOwnWindowMousePosition(float x, float y);

// Moves mouse and keyboard from gameplay to the UI, via the same
// AddGameInputHandler chat and the main menu use.
void RequestGameInputCapture(bool enabled);

} // namespace panodbg
