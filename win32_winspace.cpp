#include <windows.h>

#include <array>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#define internal    static
#define global      static
#define persistent  static constexpr

#define i8		int8_t
#define i16		int16_t
#define i32		int32_t
#define i64		int64_t
#define u8		uint8_t
#define u16		uint16_t
#define u32		uint32_t
#define u64		uint64_t

#define WM_KEYBOARD WM_USER
#define WM_WINEVENT WM_USER + 1

global DWORD g_mainThreadId;

namespace win32
{
enum class error : u64
{
  LastError,
  NullHandle,
};

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

template<typename... Args>
internal void
PrintDebug(std::format_string<Args...> formatString, Args&&... args)
{
  auto message = std::format(formatString, std::forward<Args>(args)...) + '\n';
  OutputDebugStringA(message.c_str());
}

namespace keyboard
{
struct input
{
  u32 time;
  u32 key;
  bool isPressed;
  bool isAltPressed;
};

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
      auto newInput = reinterpret_cast<WPARAM>(new input{ 
          .time = static_cast<u32>(kbDllHook.time),
          .key = static_cast<u32>(kbDllHook.vkCode),
          .isPressed = ((flags & LLKHF_UP) == 0),
          .isAltPressed = ((flags & LLKHF_ALTDOWN) != 0),
          });
      PostThreadMessageA(g_mainThreadId, WM_KEYBOARD, newInput, 0);
    } break;

    default:
    {
    } break;
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
} // keyboard_lowlevel

struct controller_input
{
  struct state
  {
    int lastChangedTime;
    bool wasDown;
  };
  std::array<state, VK_OEM_CLEAR> vkeys;
};

void
ProcessKeyboardInput(keyboard::input *keyboardInput,
    controller_input& controllerInput)
{
  auto key = keyboardInput->key;
  auto &oldInput = controllerInput.vkeys[key];
  if (keyboardInput->isPressed == oldInput.wasDown)
  {
    return;
  }
  oldInput.wasDown = keyboardInput->isPressed;
  oldInput.lastChangedTime = keyboardInput->time;
}

namespace window
{
struct event
{
  HWND handle;
  DWORD time;
  bool isFocused;
  bool isShown;
  bool isDestroyed;
};

internal std::string
GetWindowTitle(HWND handle)
{
  std::string title(256, 0);
  title.resize(GetWindowTextA(
        handle,
        title.data(),
        static_cast<int>(title.size())
        ));
  return title;
}

internal std::string
GetWindowExecutablePath(HWND hwnd)
{    
  DWORD processId;
  GetWindowThreadProcessId(hwnd, &processId);
  if (HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
      hProcess != nullptr)
  {
    std::string path(MAX_PATH, 0);
    DWORD size = MAX_PATH;
    QueryFullProcessImageNameA(hProcess, 0, path.data(), &size);
    path.resize(size);
    CloseHandle(hProcess);
    return path;
  }
  return "";
}

bool isRealWindow(HWND hwnd, LONG idObject, LONG idChild)
{
  bool isWindowObject = idObject == OBJID_WINDOW && idChild == CHILDID_SELF;
  // bool isVisible = IsWindow(hwnd) && IsWindowVisible(hwnd);
  bool isOwner = GetWindow(hwnd, GW_OWNER) == NULL;
  bool isRoot = GetAncestor(hwnd, GA_ROOT) == hwnd;
  LONG_PTR exStyle = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
  bool isTool = (exStyle & WS_EX_TOOLWINDOW) != 0;
  std::string className(256, 0);
  bool isDesktop = GetClassNameA(
      hwnd, className.data(), static_cast<int>(className.size())) && 
    (className == "Progman" || className == "WorkerW");
  return isWindowObject && isOwner && isRoot && !isTool && !isDesktop;
}

internal VOID CALLBACK
WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject,
    LONG idChild, DWORD idEventThread, DWORD dwmsEventTime)
{
  if (!isRealWindow(hwnd, idObject, idChild))
  {
    return;
  }
  window::event windowEvent = {
    .handle = hwnd,
    .time = dwmsEventTime,
  };
  const auto cloneEvent = [&windowEvent](){
    return reinterpret_cast<WPARAM>(new window::event{windowEvent});
  };
  switch(event)
  {
    case EVENT_SYSTEM_FOREGROUND:
    {
      // NOTE: this window is on top
      windowEvent.isFocused = true;
      PostThreadMessageA(g_mainThreadId, WM_WINEVENT, cloneEvent(), 0);
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

    case EVENT_OBJECT_CREATE:
    {
    // XXX: a window is created
    // OutputDebugStringA("object create\n");
    } break;

    case EVENT_OBJECT_DESTROY:
    {
      // NOTE: may not apply for crash/kill.
      windowEvent.isDestroyed = true;
      PostThreadMessageA(g_mainThreadId, WM_WINEVENT, cloneEvent(), 0);
    } break;

    case EVENT_OBJECT_SHOW:
    {
      // NOTE: this window is shown
      windowEvent.isShown = true;
      PostThreadMessageA(g_mainThreadId, WM_WINEVENT, cloneEvent(), 0);
    } break;

    case EVENT_OBJECT_HIDE:
    {
      // XXX: noise
      OutputDebugStringA("object hide\n");
    } break;

    case EVENT_OBJECT_REORDER:
    {
      // TODO: ShellDesktopView???
      // OutputDebugStringA("object reorder\n");
    } break;

    case EVENT_OBJECT_FOCUS:
    {
      // XXX: child element has keyboard focus
      // OutputDebugStringA("object focus\n");
    } break;

    case EVENT_OBJECT_SELECTION:
    {
      // TODO: enter ???
      // OutputDebugStringA("object selection\n");
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
      // OutputDebugStringA("object description change\n");
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

    default:
    {
    } break;
  }
}
} // window

struct window_state
{
  HWND topWindow;
  struct status
  {
    u32 lastUpdatedTime;
    std::string windowTitle;
    std::string executablePath;
  };
  std::unordered_map<HWND, status> windows;
};

internal void
ProcessWindowEvent(window::event *newEvent, window_state &state)
{
  auto handle = newEvent->handle;
  if (newEvent->isDestroyed)
  {
    state.windows.erase(handle);
    return;
  }
  auto [it, isWindowNew] = state.windows.try_emplace(handle);
  auto &[_, status] = *it;
  status.lastUpdatedTime = static_cast<u32>(newEvent->time);
  if (newEvent->isFocused)
  {
    state.topWindow = handle;
  }
  else if (newEvent->isShown)
  {
    status.windowTitle = window::GetWindowTitle(handle);
    status.executablePath = window::GetWindowExecutablePath(handle);
  }
}

namespace winspace
{
enum class command : u32
{
  StartProcess,
  CloseWindow,
};

struct dispatch_command
{
  command type;
  union
  {
    std::string_view commandLine;
  };
};

void
ExecuteCommand(dispatch_command command, HWND hwnd)
{
  switch (command.type)
  {
    case command::StartProcess:
    {
      STARTUPINFOA startupInfo{};
      PROCESS_INFORMATION processInformation{};
      CreateProcessA(
          nullptr, const_cast<LPSTR>(command.commandLine.data()), nullptr, 
          nullptr, false, NORMAL_PRIORITY_CLASS, nullptr, nullptr,
          &startupInfo, &processInformation);
    } break;

    case command::CloseWindow:
    {
      PostMessageA(reinterpret_cast<HWND>(hwnd), WM_CLOSE, 0, 0);
    } break;

    default:
    {
    } break;
  }
}

persistent auto k_openExplorer = std::to_array<u32>({VK_LMENU, 'E'});
persistent auto k_openPowershell = std::to_array<u32>({VK_LMENU, VK_RETURN});
persistent auto k_closeWindow = std::to_array<u32>({VK_LMENU, 'W'});
persistent auto k_defaultKeybindings = std::to_array<std::span<const u32>>({
    k_openExplorer,
    k_openPowershell,
    k_closeWindow,
    });

internal auto k_defaultCommands = std::to_array<winspace::dispatch_command>({
    {
    .type = winspace::command::StartProcess,
    .commandLine = "C:\\Windows\\explorer.exe",
    },
    {
    .type = winspace::command::StartProcess,
    .commandLine = 
    "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
    },
    {
    .type= winspace::command::CloseWindow,
    },
    });

void
DispatchCommand(controller_input& controllerInput, window_state &windowState)
{
  for (size_t keyBindingIndex = 0;
      keyBindingIndex < k_defaultKeybindings.size();
      keyBindingIndex++)
  {
    bool shouldExecute = true;
    for (auto key : k_defaultKeybindings[keyBindingIndex])
    {
      shouldExecute &= controllerInput.vkeys[key].wasDown;
    }
    if (shouldExecute)
    {
      ExecuteCommand(k_defaultCommands[keyBindingIndex], windowState.topWindow);
    }
  }
}
} // winspace
} // win32

int APIENTRY
WinMain(HINSTANCE hPrevInstance,
    HINSTANCE hInstance,
    LPSTR lpCmdLine,
    int nShowCmd)
{
  using namespace win32;
  g_mainThreadId = GetCurrentThreadId();

  auto winEventHookExp = win32::CallWithError<win32::error::NullHandle>(
      SetWinEventHook, 
      EVENT_MIN, EVENT_MAX, nullptr, window::WinEventProc, 0, 0,
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
  auto windowStates = win32::window_state{};
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
        auto keyboardInput = reinterpret_cast<keyboard::input *>(msg.wParam);
        ProcessKeyboardInput(keyboardInput, controllerInput);
        winspace::DispatchCommand(controllerInput, windowStates);
        delete keyboardInput;
      } break;

      case WM_WINEVENT:
      {
        auto newWindowEvent = reinterpret_cast<window::event *>(msg.wParam);
        ProcessWindowEvent(newWindowEvent, windowStates);
        delete newWindowEvent;
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
