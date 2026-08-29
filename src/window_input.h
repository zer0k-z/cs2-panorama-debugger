#pragma once

namespace panodbg
{

// engine2 pumps and routes input only for windows it created, and neither of
// Dota's routes reaches ours (AttachToWindow only moves OS focus; an input
// context of our own makes engine2 route the *game* window's mouse into ours).
// So the window procedure is subclassed and panorama::InputMessage_t events are
// fed to the window's CUIWindowInput directly.

// Subclasses the window procedure; Detach restores it.
bool OwnWindowInputAttach(void* hwnd);
void OwnWindowInputDetach();

// Hooks CUIWindowInput::HandleInputEvent on the class's shared vtable, so it
// sees every window. Needed for two things: a real event to copy m_flInputTime
// and m_eSource from, and detecting a mouse-down on someone else's input object
// (a pick in the game, which hands the cursor back). Any live window arms it.
bool OwnWindowInputInstallLogger(void* anyWindowInput);
void OwnWindowInputRemoveLogger();

} // namespace panodbg
