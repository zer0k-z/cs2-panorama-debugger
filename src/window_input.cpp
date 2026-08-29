#include "window_input.h"

#include <windows.h>

// GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h.
#include <windowsx.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include "core/console.h"
#include "core/safemem.h"
#include "debugger.h"
#include "platform/vtable_hook.h"

namespace panodbg
{

namespace
{

HWND g_hwnd = nullptr;
WNDPROC g_originalProc = nullptr;

// CUIWindowInput::HandleInputEvent -- slot 0 of the shared vtable.
constexpr size_t kInputHandleEvent = 0;
using HandleInputEventFn = int64_t(__fastcall*)(void* self, void* event, char flag);

platform::VTableHook g_inputHook;

// The captured prefix of a real event, kept as a header template.
constexpr size_t kEventBytes = 64;

// Type 4, not 5: type 5 carries 0x7FFC-high pointers that would be stale.
//
// panorama::InputMessage_t, confirmed against the live event log:
//
//   +0   EInputType m_eInputType
//   +4   float      m_flInputTime
//   +8   uint8      m_eSource        (upper 3 bytes are union padding)
//   +12  MouseCode  m_MouseCode      MOUSE_LEFT 0, MOUSE_RIGHT 1, MOUSE_MIDDLE 2
//   +16  uint32     m_Modifiers
//   +20  uint8      m_RepeatCount
//   +24  int        m_Delta
//   +28  float      m_XPos
//   +32  float      m_YPos
//
// EInputType: 1 KeyDown, 2 KeyUp, 3 KeyChar, 4 MouseDown, 5 MouseUp,
// 6 MouseMove, 7 MouseDoubleClick, 8 MouseTripleClick, 9 MouseWheel,
// 10 MouseEnter, 11 MouseLeave.
constexpr uint32_t kMouseEventType = 4;
constexpr size_t kEventType = 0;
constexpr size_t kEventMouseCode = 12;
constexpr size_t kEventModifiers = 16;
constexpr size_t kEventRepeatCount = 20;
constexpr size_t kEventDelta = 24;
constexpr size_t kEventMouseX = 28;
constexpr size_t kEventMouseY = 32;

constexpr uint32_t kMouseLeft = 0;
constexpr uint32_t kMouseRight = 1;

// KeyData_t shares the union, starting at the same +8:
//   +12  KeyCode m_KeyCode
//   +16  uint8   m_RepeatCount
//   +17  bool    m_bFirstDown
//   +20  uchar32 m_UniChar
//   +24  uint32  m_Modifiers
constexpr size_t kEventKeyCode = 12;
constexpr size_t kEventRepeat = 16;
constexpr size_t kEventFirstDown = 17;
constexpr size_t kEventUniChar = 20;
constexpr size_t kEventKeyModifiers = 24;

constexpr int kTypeKeyDown = 1;
constexpr int kTypeKeyUp = 2;
constexpr int kTypeKeyChar = 3;

// panorama::ModifierCode
constexpr uint32_t kModLControl = 0x01;
constexpr uint32_t kModLAlt = 0x04;
constexpr uint32_t kModLShift = 0x10;

// panorama::KeyCode: KEY_NONE 0, KEY_0..KEY_9, KEY_A..KEY_Z contiguous, then
// a lookup for the rest.
constexpr uint32_t kKeyZero = 1;
constexpr uint32_t kKeyA = 11;

uint32_t PanoramaKeyCode(WPARAM vk)
{
	if (vk >= '0' && vk <= '9')
		return kKeyZero + static_cast<uint32_t>(vk - '0');
	if (vk >= 'A' && vk <= 'Z')
		return kKeyA + static_cast<uint32_t>(vk - 'A');

	// KEY_PAD_0 follows KEY_Z at 37; punctuation and control keys follow it.
	switch (vk)
	{
	case VK_OEM_4: return 53;    // KEY_LBRACKET
	case VK_OEM_6: return 54;    // KEY_RBRACKET
	case VK_OEM_1: return 55;    // KEY_SEMICOLON
	case VK_OEM_7: return 56;    // KEY_APOSTROPHE
	case VK_OEM_3: return 57;    // KEY_BACKQUOTE
	case VK_OEM_COMMA: return 58;
	case VK_OEM_PERIOD: return 59;
	case VK_OEM_2: return 60;    // KEY_SLASH
	case VK_OEM_5: return 61;    // KEY_BACKSLASH
	case VK_OEM_MINUS: return 62;
	case VK_OEM_PLUS: return 63;  // KEY_EQUAL
	case VK_RETURN: return 64;    // KEY_ENTER
	case VK_SPACE: return 65;
	case VK_BACK: return 66;      // KEY_BACKSPACE
	case VK_TAB: return 67;
	case VK_CAPITAL: return 68;
	case VK_NUMLOCK: return 69;
	case VK_ESCAPE: return 70;
	case VK_SCROLL: return 71;
	case VK_INSERT: return 72;
	case VK_DELETE: return 73;
	case VK_HOME: return 74;
	case VK_END: return 75;
	case VK_PRIOR: return 76;     // KEY_PAGEUP
	case VK_NEXT: return 77;      // KEY_PAGEDOWN
	case VK_PAUSE: return 78;     // KEY_BREAK
	case VK_SHIFT: case VK_LSHIFT: return 79;
	case VK_RSHIFT: return 80;
	case VK_MENU: case VK_LMENU: return 81;
	case VK_RMENU: return 82;
	case VK_CONTROL: case VK_LCONTROL: return 83;
	case VK_RCONTROL: return 84;
	case VK_LWIN: return 85;
	case VK_RWIN: return 86;
	case VK_APPS: return 87;
	case VK_UP: return 88;
	case VK_LEFT: return 89;
	case VK_DOWN: return 90;
	case VK_RIGHT: return 91;
	default: break;
	}

	// KEY_F1 follows KEY_RIGHT at 92, up to KEY_F12 at 103. The debugger's own
	// shortcuts need these -- F5 reloads styles after an edit.
	if (vk >= VK_F1 && vk <= VK_F12)
		return 92 + static_cast<uint32_t>(vk - VK_F1);

	return 0; // KEY_NONE
}

uint32_t CurrentModifiers()
{
	uint32_t modifiers = 0;
	if (GetKeyState(VK_CONTROL) < 0)
		modifiers |= kModLControl;
	if (GetKeyState(VK_MENU) < 0)
		modifiers |= kModLAlt;
	if (GetKeyState(VK_SHIFT) < 0)
		modifiers |= kModLShift;
	return modifiers;
}

// The callee reads a whole event struct, larger than the prefix we capture, so
// the buffer is padded and zeroed.
constexpr size_t kEventBufferBytes = 256;

std::mutex g_templateMutex;
uint8_t g_mouseTemplate[kEventBytes] = {};
char g_templateFlag = 0;
bool g_haveTemplate = false;

// EInputType values, read from the live event log.
constexpr int kTypeMove = 6;  // k_eMouseMove
constexpr int kTypeDown = 4;  // k_eMouseDown
constexpr int kTypeUp = 5;    // k_eMouseUp
constexpr int kTypeWheel = 9; // k_eMouseWheel

void RecordEvent(void* event, char flag)
{
	uint8_t bytes[kEventBytes] = {};
	if (!safemem::Read(event, bytes, sizeof(bytes)))
		return;

	uint32_t type = 0;
	memcpy(&type, bytes, sizeof(type));

	if (type != kMouseEventType)
		return;

	std::lock_guard<std::mutex> templateLock(g_templateMutex);
	memcpy(g_mouseTemplate, bytes, sizeof(g_mouseTemplate));
	g_templateFlag = flag;
	g_haveTemplate = true;
}

int64_t __fastcall HandleInputEventDetour(void* self, void* event, char flag)
{
	if (event)
	{
		RecordEvent(event, flag);

		// A mouse-down on someone else's input object is a pick in the game.
		// The shared vtable is the only place we can see one.
		uint32_t type = 0;
		if (safemem::Read(event, &type, sizeof(type)) && type == kMouseEventType &&
			self != GetOwnWindowInput())
		{
			NotifyForeignMouseDown();
		}
	}
	return g_inputHook.GetOriginal<HandleInputEventFn>()(self, event, flag);
}

// Built, not replayed: only the header the engine stamps (input time and
// source) comes from a captured event, and every known field is written. A
// wholesale replay of a template captured mid-click drags forever. Setting the
// position field alone is not enough either -- hover is decided by the
// mouse-tracking pass, which runs only when an event is handled.
bool SendMouseEvent(int type, float x, float y, uint32_t mouseCode, int delta)
{
	if (type < 0)
		return false;

	void* input = GetOwnWindowInput();
	if (!input)
		return false;

	uint8_t buffer[kEventBufferBytes] = {};
	char flag = 0;
	{
		std::lock_guard<std::mutex> lock(g_templateMutex);
		if (!g_haveTemplate)
			return false; // no header seen yet -- click once in a working window
		memcpy(buffer, g_mouseTemplate, kEventBytes);
		flag = g_templateFlag;
	}

	const uint32_t eventType = static_cast<uint32_t>(type);
	const uint32_t zero = 0;
	memcpy(buffer + kEventType, &eventType, sizeof(eventType));
	memcpy(buffer + kEventMouseCode, &mouseCode, sizeof(mouseCode));
	memcpy(buffer + kEventModifiers, &zero, sizeof(zero));
	memcpy(buffer + kEventRepeatCount, &zero, sizeof(zero));
	memcpy(buffer + kEventDelta, &delta, sizeof(delta));
	memcpy(buffer + kEventMouseX, &x, sizeof(x));
	memcpy(buffer + kEventMouseY, &y, sizeof(y));

	void** vtable = *static_cast<void***>(input);
	reinterpret_cast<HandleInputEventFn>(vtable[kInputHandleEvent])(input, buffer, flag);
	return true;
}

// Same construction as the mouse events, over the KeyData_t half of the union.
bool SendKeyEvent(int type, uint32_t keyCode, uint32_t uniChar, bool firstDown)
{
	void* input = GetOwnWindowInput();
	if (!input)
		return false;

	uint8_t buffer[kEventBufferBytes] = {};
	char flag = 0;
	{
		std::lock_guard<std::mutex> lock(g_templateMutex);
		if (!g_haveTemplate)
			return false;
		memcpy(buffer, g_mouseTemplate, kEventBytes);
		flag = g_templateFlag;
	}

	const uint32_t eventType = static_cast<uint32_t>(type);
	const uint32_t zero = 0;
	const uint32_t modifiers = CurrentModifiers();
	const uint8_t first = firstDown ? 1 : 0;
	memcpy(buffer + kEventType, &eventType, sizeof(eventType));
	memcpy(buffer + kEventKeyCode, &keyCode, sizeof(keyCode));
	memcpy(buffer + kEventRepeat, &zero, sizeof(zero));
	memcpy(buffer + kEventFirstDown, &first, sizeof(first));
	memcpy(buffer + kEventUniChar, &uniChar, sizeof(uniChar));
	memcpy(buffer + kEventKeyModifiers, &modifiers, sizeof(modifiers));

	void** vtable = *static_cast<void***>(input);
	reinterpret_cast<HandleInputEventFn>(vtable[kInputHandleEvent])(input, buffer, flag);
	return true;
}

void SendMouseMove(float x, float y)
{
	SendMouseEvent(kTypeMove, x, y, kMouseLeft, 0);
}

LRESULT CALLBACK OwnWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message)
	{
	case WM_MOUSEMOVE:
	{
		// Already client coordinates, which is the window's surface space.
		const float x = static_cast<float>(GET_X_LPARAM(lparam));
		const float y = static_cast<float>(GET_Y_LPARAM(lparam));
		SetOwnWindowMousePosition(x, y);
		SendMouseMove(x, y);
		break;
	}
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	{
		const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN;
		const float x = static_cast<float>(GET_X_LPARAM(lparam));
		const float y = static_cast<float>(GET_Y_LPARAM(lparam));
		SetOwnWindowMousePosition(x, y);
		const bool right = message == WM_RBUTTONDOWN || message == WM_RBUTTONUP;
		SendMouseEvent(down ? kTypeDown
							: kTypeUp,
					   x, y, right ? kMouseRight : kMouseLeft, 0);
		break;
	}
	case WM_CLOSE:
		// Hidden, exactly as F6 does it. Letting DefWindowProc destroy the window
		// would leave the panorama window and its view pointing at a dead HWND.
		RequestDebuggerClose();
		return 0;
	case WM_SETFOCUS:
	case WM_MOUSEACTIVATE:
		// The game hides and re-centres the OS cursor while it holds capture,
		// so re-take it on focus. Also undoes the release that follows a pick.
		RequestGameInputCapture(true);
		break;
	case WM_SIZE:
		// Client size: the panorama window, swapchain and mouse coordinates are
		// all in client pixels.
		RequestOwnWindowResize(LOWORD(lparam), HIWORD(lparam));
		break;
	case WM_MOUSEWHEEL:
	{
		POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
		ScreenToClient(hwnd, &point); // wheel coordinates are screen-relative
		SendMouseEvent(kTypeWheel, static_cast<float>(point.x), static_cast<float>(point.y), kMouseLeft,
					   GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA);
		break;
	}
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		SendKeyEvent(kTypeKeyDown, PanoramaKeyCode(wparam), 0, (lparam & (1 << 30)) == 0);
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		SendKeyEvent(kTypeKeyUp, PanoramaKeyCode(wparam), 0, false);
		break;
	case WM_CHAR:
		// What a text entry reads; KeyDown alone gives a key but no text.
		SendKeyEvent(kTypeKeyChar, 0, static_cast<uint32_t>(wparam), true);
		break;
	default:
		break;
	}

	return CallWindowProcW(g_originalProc, hwnd, message, wparam, lparam);
}

} // namespace

bool OwnWindowInputAttach(void* hwnd)
{
	if (!hwnd)
		return false;
	if (g_hwnd == hwnd && g_originalProc)
		return true;

	OwnWindowInputDetach();

	g_hwnd = static_cast<HWND>(hwnd);
	g_originalProc = reinterpret_cast<WNDPROC>(
		SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OwnWindowProc)));
	if (!g_originalProc)
	{
		g_hwnd = nullptr;
		return false;
	}

	Console::Printf("panorama input: subclassed the debugger window");
	return true;
}

void OwnWindowInputDetach()
{
	if (g_hwnd && g_originalProc)
	{
		SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalProc));
	}
	g_hwnd = nullptr;
	g_originalProc = nullptr;
}

void OwnWindowInputRemoveLogger()
{
	g_inputHook.Remove();
}

bool OwnWindowInputInstallLogger(void* anyWindowInput)
{
	if (g_inputHook.IsInstalled())
		return true;
	if (!anyWindowInput || !safemem::HasPlausibleVTable(anyWindowInput))
		return false;

	auto** vtable = *static_cast<void***>(anyWindowInput);
	return g_inputHook.Install(vtable, kInputHandleEvent,
							   reinterpret_cast<void*>(&HandleInputEventDetour));
}

} // namespace panodbg
