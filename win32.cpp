#include <windows.h>

#include <expected>
#include <functional>
#include <stop_token>
#include <string>

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

global std::stop_source g_running;

enum class winspace_err : u64
{
	Success,
	LastError,
};

template<winspace_err Error, typename Func, typename... Args>
internal
	auto
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
		auto ec = GetLastError();
		LPSTR buffer = nullptr;
		if (auto bufSize = FormatMessageA(
					FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM,
					nullptr,
					0,
					0,
					buffer,
					0,
					nullptr
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

internal
LRESULT
CALLBACK
LowLevelKeyboardProc(
		int    nCode,
		WPARAM wParam,
		LPARAM lParam
		){
	auto vkCode = static_cast<int>(wParam);
	switch (vkCode)
	{
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYDOWN: 
		case WM_SYSKEYUP:
		default:
			break;
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

int 
APIENTRY 
WinMain(
		HINSTANCE hPrevInstance,
		HINSTANCE hInstance,
		LPSTR     lpCmdLine,
		int       nShowCmd
       ){

	auto running = g_running.get_token();

	if (auto hookExp = CallWithError<winspace_err::LastError>(
				SetWindowsHookExA,
				WH_KEYBOARD_LL,
				&LowLevelKeyboardProc,
				nullptr,
				GetCurrentThreadId()
				); hookExp)
	{
		auto hook = hookExp.value();
		while (!running.stop_requested())
		{
			MSG msg{};
			while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT)
				{
					g_running.request_stop();
				}
				TranslateMessage(&msg);
				DispatchMessageA(&msg);
			}
		}
		UnhookWindowsHookEx(hook);
		return 0;
	}

	OutputDebugStringA("Failed to hook SetWindowsHookExA:WH_KEYBOARD_LL");
	return 1;
}
