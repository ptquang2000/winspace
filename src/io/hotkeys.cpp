// Hotkey adapter — I/O adapter, the sole owner of the Win32 MOD_*/VK_* coupling.
//
// Turns parsed Binds into live OS hotkeys via RegisterHotKey (never WH_KEYBOARD_LL
// — RegisterHotKey is kernel-delivered with no input-hook timeout) and turns each
// WM_HOTKEY back into an Event. Including <windows.h> keeps this in the app TU
// only, so the parser and Reducer it depends on stay linker-provably pure.
//
// A HotkeyTable must live on the Hotkey thread: RegisterHotKey binds each combo to
// the calling thread's queue, WM_HOTKEY is delivered there (with a null window, so
// the loop handles it inline), and the destructor unregisters on that same thread.
#pragma once

#include <windows.h>

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "io/error.cpp"          // lg:: levels — the shared I/O diagnostic sink
#include "winspace/config.cpp"   // Bind, Mod, Key, Dispatcher
#include "winspace/reducer.cpp"  // Event, WorkspaceSwitch, Quit

namespace winspace::io {

// ── Mod/Key → Win32 translation (the sole coupling point) ───────────────────

// Mod flags → RegisterHotKey's MOD_* bitmask. MOD_NOREPEAT so a held combo yields
// one Event, not an autorepeat storm at the Worker.
inline UINT toWin32Mods(Mod mods) {
    UINT out = MOD_NOREPEAT;
    if (contains(mods, Mod::Super)) out |= MOD_WIN;
    if (contains(mods, Mod::Alt)) out |= MOD_ALT;
    if (contains(mods, Mod::Ctrl)) out |= MOD_CONTROL;
    if (contains(mods, Mod::Shift)) out |= MOD_SHIFT;
    return out;
}

// Key → virtual-key code. Digit/letter/function runs are contiguous in both the
// Key enum and the VK space, so they map by offset; named keys use the switch.
inline UINT toWin32Vk(Key key) {
    const auto u = std::to_underlying(key);
    if (u >= std::to_underlying(Key::N0) && u <= std::to_underlying(Key::N9))
        return static_cast<UINT>('0' + (u - std::to_underlying(Key::N0)));
    if (u >= std::to_underlying(Key::A) && u <= std::to_underlying(Key::Z))
        return static_cast<UINT>('A' + (u - std::to_underlying(Key::A)));
    if (u >= std::to_underlying(Key::F1) && u <= std::to_underlying(Key::F24))
        return static_cast<UINT>(VK_F1 + (u - std::to_underlying(Key::F1)));
    switch (key) {
        case Key::Left: return VK_LEFT;
        case Key::Right: return VK_RIGHT;
        case Key::Up: return VK_UP;
        case Key::Down: return VK_DOWN;
        case Key::Home: return VK_HOME;
        case Key::End: return VK_END;
        case Key::PageUp: return VK_PRIOR;
        case Key::PageDown: return VK_NEXT;
        case Key::Insert: return VK_INSERT;
        case Key::Delete: return VK_DELETE;
        case Key::Return: return VK_RETURN;
        case Key::Space: return VK_SPACE;
        case Key::Tab: return VK_TAB;
        case Key::Escape: return VK_ESCAPE;
        case Key::Backspace: return VK_BACK;
        default: return 0;  // unreachable for a parsed Key; 0 fails registration loudly
    }
}

// ── Bind → Event ─────────────────────────────────────────────────────────────
// The dispatcher picks the alternative; a workspace Bind's target is in arg.
inline Event toEvent(const Bind& bind) {
    switch (bind.dispatcher) {
        case Dispatcher::Workspace: return WorkspaceSwitch{bind.arg};
        case Dispatcher::Quit: return Quit{};
    }
    return Quit{};  // unreachable
}

// ── human-readable combo naming (for diagnostics) ───────────────────────────
inline std::string describeKey(Key key) {
    const auto u = std::to_underlying(key);
    if (u >= std::to_underlying(Key::N0) && u <= std::to_underlying(Key::N9))
        return std::string(1, static_cast<char>('0' + (u - std::to_underlying(Key::N0))));
    if (u >= std::to_underlying(Key::A) && u <= std::to_underlying(Key::Z))
        return std::string(1, static_cast<char>('A' + (u - std::to_underlying(Key::A))));
    if (u >= std::to_underlying(Key::F1) && u <= std::to_underlying(Key::F24))
        return std::format("F{}", 1 + (u - std::to_underlying(Key::F1)));
    switch (key) {
        case Key::Left: return "Left";
        case Key::Right: return "Right";
        case Key::Up: return "Up";
        case Key::Down: return "Down";
        case Key::Home: return "Home";
        case Key::End: return "End";
        case Key::PageUp: return "PageUp";
        case Key::PageDown: return "PageDown";
        case Key::Insert: return "Insert";
        case Key::Delete: return "Delete";
        case Key::Return: return "Return";
        case Key::Space: return "Space";
        case Key::Tab: return "Tab";
        case Key::Escape: return "Escape";
        case Key::Backspace: return "Backspace";
        default: return "?";
    }
}

// e.g. "SUPER+ALT+2" — names the combo in registration diagnostics.
inline std::string describeCombo(const Bind& bind) {
    std::string out;
    const auto add = [&](Mod flag, const char* name) {
        if (!contains(bind.mods, flag)) return;
        if (!out.empty()) out += "+";
        out += name;
    };
    add(Mod::Super, "SUPER");
    add(Mod::Alt, "ALT");
    add(Mod::Ctrl, "CTRL");
    add(Mod::Shift, "SHIFT");
    if (!out.empty()) out += "+";
    return out + describeKey(bind.key);
}

// ── the registration + delivery table ───────────────────────────────────────

// Registers Binds as live OS hotkeys and maps each WM_HOTKEY back to its Event.
class HotkeyTable {
    // One live OS hotkey owned by a unique_ptr over its heap id; destroying it
    // calls UnregisterHotKey once, on the registering thread (where the table lives).
    using RegisteredHotkey = std::unique_ptr<int, decltype([](int* id) {
                                                 UnregisterHotKey(nullptr, *id);
                                                 delete id;
                                             })>;

public:
    // Registers each Bind on the CURRENT thread, so WM_HOTKEY lands in this thread's
    // queue. Every return value is checked; a failure names the combo and the rest
    // still register. Hotkey id == Bind index, so a WM_HOTKEY wParam indexes m_binds.
    explicit HotkeyTable(const std::vector<Bind>& binds) : m_binds(binds) {
        for (int id = 0; id < static_cast<int>(m_binds.size()); ++id) {
            const Bind& bind = m_binds[static_cast<size_t>(id)];
            if (RegisterHotKey(nullptr, id, toWin32Mods(bind.mods), toWin32Vk(bind.key))) {
                m_registered.emplace_back(new int(id));
            } else {
                const DWORD err = GetLastError();
                const std::string combo = describeCombo(bind);
                if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
                    lg::warn(
                        "hotkey {} is already registered by another app — skipping", combo);
                } else {
                    lg::error("failed to register hotkey {} (error {})", combo, err);
                }
            }
        }
    }

    // Move-only via the RegisteredHotkey member; destroying m_registered
    // unregisters every hotkey. No explicit destructor needed.

    // Map a delivered WM_HOTKEY id (its wParam) to the Event to post; nullopt for
    // an out-of-range id (a stray message that never named one of our Binds).
    std::optional<Event> eventFor(int id) const {
        if (id < 0 || id >= static_cast<int>(m_binds.size())) return std::nullopt;
        return toEvent(m_binds[static_cast<size_t>(id)]);
    }

    // How many Binds became live hotkeys (vs. skipped on a registration failure).
    size_t registeredCount() const { return m_registered.size(); }

private:
    std::vector<Bind> m_binds;                   // index == hotkey id
    std::vector<RegisteredHotkey> m_registered;  // RegisterHotKey accepted these
};

}  // namespace winspace::io
