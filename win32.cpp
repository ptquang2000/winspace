#include <windows.h>

#include <deque>
#include <expected>
#include <format>
#include <functional>
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

struct KeyboardInputEvent
{
    u32 time;
    u32 key;
    bool isPressed;
    bool isAltPressed;
};

enum class winspace_err : u64
{
    LastError,
};

global std::stop_source g_running;
global std::deque<KeyboardInputEvent> g_keyboardInputEvents;

template<winspace_err Error, typename Func, typename... Args>
internal auto
CallWithError(Func&& func, Args&&... args)
-> std::expected<std::invoke_result_t<Func, Args...>, winspace_err>
{
    using result_type = std::invoke_result_t<Func, Args...>;
    if constexpr (Error == winspace_err::LastError)
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
            g_keyboardInputEvents.push_back(KeyboardInputEvent{ 
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

int APIENTRY
WinMain(HINSTANCE hPrevInstance,
        HINSTANCE hInstance,
        LPSTR lpCmdLine,
        int nShowCmd)
{
    auto hookExp = CallWithError<winspace_err::LastError>(
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

    auto hook = hookExp.value();
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
                auto inputEvent = g_keyboardInputEvents.front();
                OutputDebugStringA(std::format("{} is {}\n", 
                        inputEvent.key,
                        inputEvent.isPressed ? "pressed" : "released"
                    ).c_str());
                g_keyboardInputEvents.pop_front();
            } break;

            default:
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            } break;
        }
    }
    UnhookWindowsHookEx(hook);
    return 0;
}
