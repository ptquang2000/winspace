#include <windows.h>

#include <array>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <ranges>
#include <stop_token>
#include <string>

#define WM_KEYBOARD WM_USER

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

namespace win32
{
struct keyboard_input
{
    u32 time;
    u32 key;
    bool isPressed;
    bool isAltPressed;
};
}

namespace winspace
{
enum class err : u64
{
    LastError,
};
}

global std::stop_source g_running;
global std::deque<win32::keyboard_input> g_keyboardInputs;

template<winspace::err Error, typename Func, typename... Args>
internal auto
CallWithError(Func&& func, Args&&... args)
-> std::expected<std::invoke_result_t<Func, Args...>, winspace::err>
{
    using result_type = std::invoke_result_t<Func, Args...>;
    if constexpr (Error == winspace::err::LastError)
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
    else
    {
        static_assert(0, "unhandle error");
    }
}       

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
            g_keyboardInputs.push_back(win32::keyboard_input{ 
                    .time = static_cast<u32>(kbDllHook.time),
                    .key = static_cast<u32>(kbDllHook.vkCode),
                    .isPressed = ((flags & LLKHF_UP) == 0),
                    .isAltPressed = ((flags & LLKHF_ALTDOWN) != 0),
            });
            PostMessageA(nullptr, WM_KEYBOARD, 0, 0);
        } break;
        default:
        {
        } break;
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

namespace winspace
{
struct input_state
{
    int lastChangedTime;
    bool wasDown;
};

template<std::size_t MaxKeyCode>
struct controller_input
{
    std::array<input_state, MaxKeyCode> keys;
};
}

int APIENTRY
WinMain(HINSTANCE hPrevInstance,
        HINSTANCE hInstance,
        LPSTR lpCmdLine,
        int nShowCmd)
{
    auto hookExp = CallWithError<winspace::err::LastError>(
        SetWindowsHookExA,
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        GetModuleHandleA(nullptr),
        0);
    if (!hookExp.has_value())
    {
        OutputDebugStringA("Failed to hook SetWindowsHookExA:WH_KEYBOARD_LL");
        return 1;
    }

    auto controllerInput = winspace::controller_input<VK_OEM_CLEAR>{};
    auto running = g_running.get_token();

    while (!running.stop_requested())
    {
        MSG msg{};
        if (GetMessageA(&msg, 0, 0, 0) < 0)
        {
            g_running.request_stop();
            continue;
        }

        switch(msg.message)
        {
            case WM_QUIT:
            {
                g_running.request_stop();
            } break;

            case WM_KEYBOARD:
            {
                auto inputEvent = g_keyboardInputs.front();
                g_keyboardInputs.pop_front();
                auto &oldInputEvent = controllerInput.keys[inputEvent.key];
                if (inputEvent.isPressed == oldInputEvent.wasDown)
                {
                    continue;
                }
                OutputDebugStringA(std::format("{} is {} at {}\n", 
                            inputEvent.key,
                            inputEvent.isPressed ? "pressed" : "released",
                            inputEvent.time
                            ).c_str());
                
                oldInputEvent.wasDown = inputEvent.isPressed;
                oldInputEvent.lastChangedTime = inputEvent.time;
            } break;

            default:
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            } break;
        }
    }

    auto hook = hookExp.value();
    UnhookWindowsHookEx(hook);
    return 0;
}
