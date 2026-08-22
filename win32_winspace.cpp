#include <windows.h>

#include <array>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <ranges>
#include <stop_token>
#include <string>
#include <thread>

#define internal	static
#define global		static

#define i8		int8_t
#define i6		int16_t
#define i2		int32_t
#define i4		int64_t
#define u8		uint8_t
#define u16		uint16_t
#define u32		uint32_t
#define u64		uint64_t

#define WM_KEYBOARD WM_USER

global DWORD g_mainThreadId;

namespace win32
{
enum class error : u64
{
    LastError,
    NullHandle,
};

namespace keyboard
{
struct input
{
    u32 time;
    u32 key;
    bool isPressed;
    bool isAltPressed;
};
global std::deque<input> g_keyboardInputs;

internal LRESULT CALLBACK
LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    switch (wParam)
    {
        case WM_SYSKEYDOWN: 
        case WM_SYSKEYUP:
        case WM_KEYDOWN:
        case WM_KEYUP:
        {
            auto kbDllHook = *reinterpret_cast<PKBDLLHOOKSTRUCT>(lParam);
            u32 flags = static_cast<u32>(kbDllHook.flags);
            g_keyboardInputs.push_back(input{ 
                    .time = static_cast<u32>(kbDllHook.time),
                    .key = static_cast<u32>(kbDllHook.vkCode),
                    .isPressed = ((flags & LLKHF_UP) == 0),
                    .isAltPressed = ((flags & LLKHF_ALTDOWN) != 0),
            });
            PostThreadMessageA(g_mainThreadId, WM_KEYBOARD, 0, 0);
        } break;
        default:
        {
        } break;
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
} // keyboard_lowlevel

template<error Error, typename Func, typename... Args>
internal auto
CallWithError(Func&& func, Args&&... args)
-> std::expected<std::invoke_result_t<Func, Args...>, win32::error>
{
    if constexpr (Error == win32::error::LastError)
    {
        if (auto r = std::invoke(
            func,
            std::forward<Args>(args)...
        ); r != nullptr)
        {
            return r;
        }
        LPSTR buffer = nullptr;
        if (auto bufSize = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM,
            nullptr, GetLastError(), 0, (LPSTR)&buffer, 0, nullptr
        ); bufSize != 0)
        {
            OutputDebugStringA(buffer);
            LocalFree(buffer);
        }
        return std::unexpected(Error);
    }
    else if constexpr (Error == win32::error::NullHandle)
    {
        if (auto handle = std::invoke(
            func,
            std::forward<Args>(args)...
        ); handle != 0)
        {
            return handle;
        }
        return std::unexpected(Error);
    }
    else
    {
        static_assert(0, "unhandle error");
    }
}

struct controller_input
{
    struct input_state
    {
        int lastChangedTime;
        bool wasDown;
    };
    std::array<input_state, VK_OEM_CLEAR> vkeys;
};

void
ProcessKeyboardInput(keyboard::input keyboardInput,
                    controller_input& controllerInput)
{
    auto &oldInput = controllerInput.vkeys[keyboardInput.key];
    if (keyboardInput.isPressed == oldInput.wasDown)
    {
        return;
    }
    OutputDebugStringA(std::format("{} is {} at {}\n", 
                keyboardInput.key,
                keyboardInput.isPressed ? "pressed" : "released",
                keyboardInput.time
                ).c_str());
    oldInput.wasDown = keyboardInput.isPressed;
    oldInput.lastChangedTime = keyboardInput.time;
}
} // win32

internal VOID CALLBACK
Wineventproc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject,
            LONG idChild, DWORD idEventThread, DWORD dwmsEventTime)
{
    switch(event)
    {
        case EVENT_SYSTEM_SOUND:
        {
            OutputDebugStringA("system sound\n");
        } break;
        case EVENT_SYSTEM_ALERT:
        {
            OutputDebugStringA("system alert\n");
        } break;
        case EVENT_SYSTEM_FOREGROUND:
        {
            // TODO: early indicator???
            OutputDebugStringA("system foreground\n");
        } break;
        case EVENT_SYSTEM_MENUSTART:
        {
            OutputDebugStringA("menu start\n");
        } break;
        case EVENT_SYSTEM_MENUEND:
        {
            OutputDebugStringA("menu end\n");
        } break;
        case EVENT_SYSTEM_MENUPOPUPSTART:
        {
            OutputDebugStringA("menu popup start\n");
        } break;
        case EVENT_SYSTEM_MENUPOPUPEND:
        {
            OutputDebugStringA("menu popup end\n");
        } break;
        case EVENT_SYSTEM_CAPTURESTART:
        {
            // TODO: onMousePressed
            // OutputDebugStringA("capture start\n");
        } break;
        case EVENT_SYSTEM_CAPTUREEND:
        {
            // TODO: onMouseReleased
            // OutputDebugStringA("capture end\n");
        } break;
        case EVENT_SYSTEM_MOVESIZESTART:
        {
            // TODO: window moved
            // OutputDebugStringA("move size start\n");
        } break;
        case EVENT_SYSTEM_MOVESIZEEND:
        {
            // TODO: window moved
            // OutputDebugStringA("move size end\n");
        } break;
        case EVENT_SYSTEM_CONTEXTHELPSTART:
        {
            OutputDebugStringA("context help start\n");
        } break;
        case EVENT_SYSTEM_CONTEXTHELPEND:
        {
            OutputDebugStringA("context help end\n");
        } break;
        case EVENT_SYSTEM_DRAGDROPSTART:
        {
            OutputDebugStringA("drag drop start\n");
        } break;
        case EVENT_SYSTEM_DRAGDROPEND:
        {
            OutputDebugStringA("drag drop end\n");
        } break;
        case EVENT_SYSTEM_DIALOGSTART:
        {
            OutputDebugStringA("dialog start\n");
        } break;
        case EVENT_SYSTEM_DIALOGEND:
        {
            OutputDebugStringA("dialog end\n");
        } break;
        case EVENT_SYSTEM_SCROLLINGSTART:
        {
            OutputDebugStringA("scrolling start\n");
        } break;
        case EVENT_SYSTEM_SCROLLINGEND:
        {
            OutputDebugStringA("scrolling end\n");
        } break;
        case EVENT_SYSTEM_SWITCHSTART:
        {
            OutputDebugStringA("switch start\n");
        } break;
        case EVENT_SYSTEM_SWITCHEND:
        {
            OutputDebugStringA("switch end\n");
        } break;
        case EVENT_SYSTEM_MINIMIZESTART:
        {
            OutputDebugStringA("minize start\n");
        } break;
        case EVENT_SYSTEM_MINIMIZEEND:
        {
            OutputDebugStringA("minize end\n");
        } break;
        case EVENT_SYSTEM_DESKTOPSWITCH:
        {
            OutputDebugStringA("desktop switch\n");
        } break;
        case EVENT_SYSTEM_SWITCHER_APPGRABBED:
        {
            OutputDebugStringA("switcher app grabbed\n");
        } break;
        case EVENT_SYSTEM_SWITCHER_APPOVERTARGET:
        {
            OutputDebugStringA("switcher app over target\n");
        } break;
        case EVENT_SYSTEM_SWITCHER_APPDROPPED:
        {
            OutputDebugStringA("switcher app dropped\n");
        } break;
        case EVENT_SYSTEM_SWITCHER_CANCELLED:
        {
            OutputDebugStringA("switcher cancelled\n");
        } break;
        case EVENT_SYSTEM_IME_KEY_NOTIFICATION:
        {
            OutputDebugStringA("ime key notification\n");
        } break;
        case EVENT_CONSOLE_CARET:
        {
            OutputDebugStringA("console caret\n");
        } break;
        case EVENT_CONSOLE_UPDATE_REGION:
        {
            OutputDebugStringA("console update region\n");
        } break;
        case EVENT_CONSOLE_UPDATE_SIMPLE:
        {
            OutputDebugStringA("console update simple\n");
        } break;
        case EVENT_CONSOLE_UPDATE_SCROLL:
        {
            OutputDebugStringA("console update scroll\n");
        } break;
        case EVENT_CONSOLE_LAYOUT:
        {
            OutputDebugStringA("console layout\n");
        } break;
        case EVENT_CONSOLE_START_APPLICATION:
        {
            OutputDebugStringA("console start application\n");
        } break;
        case EVENT_CONSOLE_END_APPLICATION:
        {
            OutputDebugStringA("console end application\n");
        } break;
        case EVENT_OBJECT_CREATE:
        {
            // XXX: noise
            // OutputDebugStringA("object create\n");
        } break;
        case EVENT_OBJECT_DESTROY:
        {
            // XXX: noise
            // OutputDebugStringA("object destroy\n");
        } break;
        case EVENT_OBJECT_SHOW:
        {
            // XXX: noise
            // OutputDebugStringA("object show\n");
        } break;
        case EVENT_OBJECT_HIDE:
        {
            // XXX: noise
            // OutputDebugStringA("object hide\n");
        } break;
        case EVENT_OBJECT_REORDER:
        {
            // TODO: ShellDesktopView???
            // OutputDebugStringA("object reorder\n");
        } break;
        case EVENT_OBJECT_FOCUS:
        {
            // XXX: little noise
            // OutputDebugStringA("object focus\n");
        } break;
        case EVENT_OBJECT_SELECTION:
        {
            // TODO: enter ???
            OutputDebugStringA("object selection\n");
        } break;
        case EVENT_OBJECT_SELECTIONADD:
        {
            OutputDebugStringA("object selection add\n");
        } break;
        case EVENT_OBJECT_SELECTIONREMOVE:
        {
            // TODO: quit ???
            OutputDebugStringA("object selection remove\n");
        } break;
        case EVENT_OBJECT_SELECTIONWITHIN:
        {
            OutputDebugStringA("object selection within\n");
        } break;
        case EVENT_OBJECT_STATECHANGE:
        {
            // XXX: noise
            // OutputDebugStringA("state change\n");
        } break;
        case EVENT_OBJECT_LOCATIONCHANGE:
        {
            // TODO: onMouseMoved???
            // OutputDebugStringA("object location change\n");
        } break;
        case EVENT_OBJECT_NAMECHANGE:
        {
            // XXX: ???
            // OutputDebugStringA("object name change\n");
        } break;
        case EVENT_OBJECT_DESCRIPTIONCHANGE:
        {
            OutputDebugStringA("object description change\n");
        } break;
        case EVENT_OBJECT_VALUECHANGE:
        {
            OutputDebugStringA("object value change\n");
        } break;
        case EVENT_OBJECT_PARENTCHANGE:
        {
            // XXX: ???
            // OutputDebugStringA("object parent change\n");
        } break;
        case EVENT_OBJECT_HELPCHANGE:
        {
            OutputDebugStringA("object help change\n");
        } break;
        case EVENT_OBJECT_DEFACTIONCHANGE:
        {
            OutputDebugStringA("object defaction change\n");
        } break;
        case EVENT_OBJECT_ACCELERATORCHANGE:
        {
            OutputDebugStringA("object accelerator change\n");
        } break;
        case EVENT_OBJECT_INVOKED:
        {
            OutputDebugStringA("object invoked\n");
        } break;
        case EVENT_OBJECT_TEXTSELECTIONCHANGED:
        {
            OutputDebugStringA("object text selection changed\n");
        } break;
        case EVENT_OBJECT_CONTENTSCROLLED:
        {
            OutputDebugStringA("object contents scrolled\n");
        } break;
        case EVENT_SYSTEM_ARRANGMENTPREVIEW:
        {
            OutputDebugStringA("arrangement preview\n");
        } break;
        case EVENT_OBJECT_CLOAKED:
        {
            // TODO: start menu 
            // OutputDebugStringA("object cloaked\n");
        } break;
        case EVENT_OBJECT_UNCLOAKED:
        {
            // TODO: start menu 
            // OutputDebugStringA("object uncloaked\n");
        } break;
        case EVENT_OBJECT_LIVEREGIONCHANGED:
        {
            OutputDebugStringA("object live region changed\n");
        } break;
        case EVENT_OBJECT_HOSTEDOBJECTSINVALIDATED:
        {
            OutputDebugStringA("object hosted objects invalidated\n");
        } break;
        case EVENT_OBJECT_DRAGSTART:
        {
            OutputDebugStringA("object drag start\n");
        } break;
        case EVENT_OBJECT_DRAGCANCEL:
        {
            OutputDebugStringA("object drag cancel\n");
        } break;
        case EVENT_OBJECT_DRAGCOMPLETE:
        {
            OutputDebugStringA("object drag complete\n");
        } break;
        case EVENT_OBJECT_DRAGENTER:
        {
            OutputDebugStringA("object drag enter\n");
        } break;
        case EVENT_OBJECT_DRAGLEAVE:
        {
            OutputDebugStringA("object drag leave\n");
        } break;
        case EVENT_OBJECT_DRAGDROPPED:
        {
            OutputDebugStringA("object drag dropped\n");
        } break;
        case EVENT_OBJECT_IME_SHOW:
        {
            OutputDebugStringA("object ime show\n");
        } break;
        case EVENT_OBJECT_IME_HIDE:
        {
            OutputDebugStringA("object ime hide\n");
        } break;
        case EVENT_OBJECT_IME_CHANGE:
        {
            OutputDebugStringA("object ime change\n");
        } break;
        case EVENT_OBJECT_TEXTEDIT_CONVERSIONTARGETCHANGED:
        {
            OutputDebugStringA("object textedit conversion target changed\n");
        } break;
        default:
        {
        } break;
    }
}


int APIENTRY
WinMain(HINSTANCE hPrevInstance,
        HINSTANCE hInstance,
        LPSTR lpCmdLine,
        int nShowCmd)
{
    g_mainThreadId = GetCurrentThreadId();

    auto winEventHookExp = win32::CallWithError<win32::error::NullHandle>(
        SetWinEventHook, 
        EVENT_MIN, EVENT_MAX, nullptr, Wineventproc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!winEventHookExp.has_value())
    {
        OutputDebugStringA("Failed to hook SetWinEventHook");
        return 1;
    }

    std::jthread keyboardHookThread([](std::stop_token running){
        auto keyboardHookExp = win32::CallWithError<win32::error::LastError>(
                SetWindowsHookExA,
                WH_KEYBOARD_LL, win32::keyboard::LowLevelKeyboardProc,
                GetModuleHandleA(nullptr), 0);
        if (!keyboardHookExp.has_value())
        {
            OutputDebugStringA(
                    "Failed to hook SetWindowsHookExA:WH_KEYBOARD_LL");
            return;
        }

        while (!running.stop_requested())
        {
            MSG msg{};
            if (GetMessageA(&msg, 0, 0, 0) < 0)
            {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        UnhookWindowsHookEx(keyboardHookExp.value());
    });

    auto controllerInput = win32::controller_input{};
    bool running = true;
    while (running)
    {
        MSG msg{};
        if (GetMessageA(&msg, 0, 0, 0) <= 0)
        {
            UnhookWinEvent(winEventHookExp.value());
            keyboardHookThread.request_stop();
            running = false;
            continue;
        }

        switch(msg.message)
        {
            using namespace win32;
            case WM_KEYBOARD:
            {
                ProcessKeyboardInput(
                        keyboard::g_keyboardInputs.front(), controllerInput);
                keyboard::g_keyboardInputs.pop_front();
            } break;

            default:
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            } break;
        }
    }
    return 0;
}
