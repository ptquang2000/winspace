# PRD 0002 — Window tracking + eligibility + fill-one-window

**Labels:** `ready-for-agent`
**Blocked by:** 01 (idiom from 11)
**References:** [ADR-0006](../adr/0006-window-tracking-probe-decide-seam.md), `CONTEXT.md`
(Window tracking), `DESIGN.md`

## Problem Statement

winspace can switch Workspaces from a hotkey, but it is blind to windows. When the user
opens an app it lands wherever Windows decides and at whatever size it feels like; winspace
does nothing with it. There is no notion of which windows are candidates for tiling and which
should be left alone, and no path that ever moves a window. Until winspace can *perceive* and
*place* a window, no tiling feature can exist.

## Solution

winspace watches the desktop for windows appearing and vanishing, classifies each one through
the Eligibility gate, and automatically fills the focused Display's work area with the active
Tileable window — pixel-flush to the edges, DPI-correct, taskbar left visible. Dialogs, tool
windows, system popups, cloaked UWP host windows, and fullscreen apps are recognized as
Ineligible and never touched. When the active tiled window closes, the previous one reclaims
the space. This slice deliberately fills exactly **one** window; it is the foundation that
BSP tiling (03) and multi-Display fill (04) build on.

## User Stories

1. As a desktop user, I want a newly opened app window to fill my monitor's usable area
   automatically, so that I don't have to reach for the maximize button every time.
2. As a desktop user, I want the fill to respect the work area (`rcWork`), so that my taskbar
   stays visible instead of being covered.
3. As a desktop user, I want the tiled window to sit pixel-flush against the screen edges
   with no visible gap, so that the layout looks clean despite Windows' invisible window
   drop-shadow border.
4. As a high-DPI user, I want the fill to be correct on my scaled display, so that windows
   are neither too small nor overflowing.
5. As a desktop user, I want Save-As and other dialogs to stay floating at their natural
   size, so that transient interactions aren't stretched across my screen.
6. As a desktop user, I want tool windows and floating palettes left alone, so that apps
   that rely on them keep working normally.
7. As a desktop user, I want UWP / Microsoft Store apps to tile correctly while their
   invisible cloaked host windows are ignored, so that I never get a phantom empty tile.
8. As a desktop user, I want fullscreen apps (games, video) left untouched, so that winspace
   never interferes with a fullscreen experience.
9. As a desktop user, when I close the active tiled window, I want the previous window to
   reclaim the space, so that I'm never left staring at an empty desktop with a usable window
   hidden underneath.
10. As a desktop user, I want winspace to react the instant a window appears rather than on a
    timer, so that tiling feels immediate and my battery isn't drained by polling.
11. As a desktop user on a busy machine, I want tiling to stay responsive under heavy CPU
    load, so that a loaded system never makes window placement lag (the hook runs
    out-of-context, off the input critical path).
12. As a desktop user, I want a window filled on the monitor it actually opened on, so that
    placement isn't surprising on a multi-monitor setup (single-fill this slice).
13. As a desktop user, I want winspace to adopt the windows already open when it starts, so
    that launching winspace immediately tiles my current window rather than doing nothing
    until I open a new one.
14. As a desktop user, I want a window that Windows shows more than once (repeated `SHOW`
    events) to be tiled once, not flicker or double-place, so that placement is stable.
15. As a desktop user, I want a window that uncloaks (e.g. returns from another Virtual
    Desktop) to be picked up as if it appeared, so that switching desktops keeps tiling
    coherent.
16. As a desktop user, I want winspace to shut down cleanly, unhooking from the desktop, so
    that quitting leaves no dangling system hook.
17. As a winspace maintainer, I want the eligibility rule and the layout to be pure and
    unit-testable from synthetic window data, so that I can prove classification and
    placement without a live desktop.
18. As a winspace maintainer, I want window identity and monitor identity to be strong,
    opaque types in the core, so that a window handle can never be silently confused with a
    Logical workspace number or a monitor.
19. As a winspace maintainer, I want the geometry the core computes to be logical and
    device-independent, with the drop-shadow and DPI compensation confined to the I/O layer,
    so that the core stays `windows.h`-free and portable to test.

## Implementation Decisions

Overarching principle (ADR-0006): every OS-touching concern is split into a **Probe**
(adapter gathers a window's live attributes into plain data) and a **policy** (the pure
Reducer decides over that plain data). The Reducer stays `windows.h`-free; the Worker owns
the `State` value and executes Effects with live compensation.

**New hook adapter, its own (third) thread.** An out-of-context `SetWinEventHook` adapter
mirrors the Hotkey thread: `WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS`, a single hook
over the object-event range, a `thread_local` worker HWND, heap `Event*` transport via the
existing `postEvent` contract, and `PostThreadMessage(WM_QUIT)` + `UnhookWinEvent` teardown.
Callback first-pass noise gate: `idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd`.

**Lifecycle edges, not raw CREATE.** Appeared = `EVENT_OBJECT_SHOW` / `EVENT_OBJECT_UNCLOAKED`
(probe here); Vanished = `EVENT_OBJECT_DESTROY` / `HIDE` / `CLOAKED`. Raw `CREATE` is dropped
— the window is half-born and misclassifies.

**Adoption of pre-existing windows.** At startup the adapter runs one `EnumWindows` sweep and
posts a synthetic `Appeared` for each currently-shown top-level window, through the same path
as live events — so winspace inherits the session (same principle as Virtual Desktop
Adoption). No separate machinery.

**Strong identity types.** `WindowId` = `enum class WindowId : uint64_t {}` minted from
`HWND`; `MonitorId` likewise from `HMONITOR`.

**Core vocabulary (from the design prototype — the decision-bearing shape):**

```cpp
struct WindowAttrs {                       // plain-data Probe result, rides on Appeared
    WindowId id{}; MonitorId monitor{};
    bool topLevel, visible, thickFrame, caption, toolWindow, cloaked, fullscreen;
};
struct MonitorInfo { MonitorId id{}; Rect rcWork; };   // Rect = plain {int l,t,r,b}

// Events (add to the existing variant)
struct Appeared        { WindowAttrs attrs; };
struct Vanished        { WindowId id; };
struct MonitorsChanged { std::vector<MonitorInfo> monitors; };   // whole-topology snapshot

// Effect (add to the existing variant)
struct PositionWindow  { WindowId id; Rect rect; };              // logical rect; no shadow/DPI

// Eligibility policy — pure, unit-tested from synthetic WindowAttrs
constexpr bool isTileable(const WindowAttrs& w) {
    return w.topLevel && w.visible && w.thickFrame && w.caption
        && !w.toolWindow && !w.cloaked && !w.fullscreen;
}
```

**State additions and the layout function.** `State` gains `monitors` (a map
`MonitorId → rcWork`, **replaced wholesale** on `MonitorsChanged`) and the **Focus order** (a
priority-ordered list of Tileable `WindowId`, front = highest priority, approximated by
appearance order this slice). The layout recomputes on every `Appeared`/`Vanished`: fill
`focusOrder.front()` to its monitor's `rcWork` via `PositionWindow`; empty ⇒ no Effect;
`Vanished` of the head ⇒ the survivor fills (reclaim). `Appeared` is idempotent on `WindowId`
(already present ⇒ move-to-front, never duplicate).

**Ineligible windows are classified and left alone — not stored.** No passive floating set
(it would thrash on every tooltip/menu and leak stale entries). The snap-back **Detached**
set is a distinct concept that arrives with rules / drag-to-float / `togglefloat` in 06/07.

**Compensation lives in the Worker, at execute time.** `PositionWindow` → two live reads
(`GetWindowRect` + `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)`); grow the target
outward by the drop-shadow delta so the *visible* frame lands flush, then `SetWindowPos`.
Process is Per-Monitor-V2 DPI aware; all rects are physical pixels. The delta never enters
the core.

**Monitor topology enters as a whole-set snapshot.** Startup `EnumDisplayMonitors` sweep
posts one `MonitorsChanged`. Runtime `WM_DISPLAYCHANGE` reaction is deferred to 04.

## Testing Decisions

A good test here exercises **external behavior, not implementation detail**: it feeds Events
and asserts on the emitted Effects (and on State only where an Effect already reveals it).
This is the house style established in `reducer_test.cpp` and mandated by the `issues/README`
seam contract.

**Seam (1) — the pure Reducer — carries the whole brain and is unit-tested:**
- `isTileable` over synthetic `WindowAttrs`: each gate condition flips the verdict; UWP
  cloaked host and tool windows are excluded.
- Fill: a single eligible `Appeared` emits exactly `PositionWindow` to that monitor's
  `rcWork`; a second eligible `Appeared` fills over it; `Vanished` of the head re-emits a fill
  for the survivor (reclaim); the last `Vanished` emits nothing.
- Idempotency: two `Appeared` for the same `WindowId` ⇒ one Focus-order entry, one fill.
- Ineligible: an ineligible `Appeared` emits **no** Effect and leaves Focus order unchanged.
- Monitor model: `MonitorsChanged` replaces topology; an `Appeared` resolves its `MonitorId`
  to the right `rcWork`.
- Adoption: a batch of synthetic startup `Appeared` events yields exactly one fill (the head).

Prior art: `src/winspace/reducer_test.cpp` (`single_effect` helper, effect-equality asserts).

**Secondary — the existing VM Smoke seam (ADR-0005, harness in issue 12)** — only for the
live-only behaviors Seam (1) structurally cannot reach: the real hook firing on lifecycle,
flush `SetWindowPos` + frame-bounds compensation landing pixel-flush, and Probe accuracy
against real dialogs / tool windows / cloaked UWP hosts. No new seam is introduced; the hook
adapter holds no logic worth its own seam.

## Out of Scope

- Foreground / `EVENT_SYSTEM_FOREGROUND` focus reordering — Focus order is appearance-ordered
  here; real focus tracking arrives in 03/05.
- Multi-window BSP / dwindle layout and coalesced batched (`DeferWindowPos`) commits — 03.
- Multi-Display fill order, min-floor float overflow, `WM_DISPLAYCHANGE` re-tile — 04.
- The Detached set and `windowrule float` / drag-to-float / `togglefloat` snap-back — 06/07.
- Any persistence of window state — State is rebuilt on start, as ever.

## Further Notes

This slice's real deliverable is the **rails**: window identity in the pure core, the
lifecycle event stream, the Eligibility gate, the monitor model, and one end-to-end
compute-a-rect → position-flush path. 03 swaps the degenerate one-window layout body for BSP
arithmetic with **no change** to the trigger, the Effect vocabulary, or the reclaim behavior;
05 reorders the same Focus order list; 04 turns the single `rcWork` lookup into a per-Display
model. Sequenced after 11 so the new adapter copies the current `io::Error` idiom.
