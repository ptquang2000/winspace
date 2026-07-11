# winspace

A Windows 11 workspace + focus manager that leans on OS-native facilities and stays off
the input critical path. It switches Workspaces and switches keyboard focus between
windows; it owns **no window geometry** — it never moves or sizes a window (see
[ADR-0007](docs/adr/0007-drop-tiling-no-window-geometry.md)). This glossary fixes the
ubiquitous language shared by `DESIGN.md`, the `issues/`, and the code.

## Language

### Workspaces & displays

**Workspace**:
A named set of windows the user switches between as a unit. In winspace a workspace *is*
an OS Virtual Desktop — not a userspace show/hide illusion.
_Avoid_: Desktop (ambiguous with the Windows shell desktop), Space, Tag.

**Virtual Desktop**:
The OS-level primitive (`IVirtualDesktopManagerInternal`) that backs a Workspace. It is a
**dense, ordered array identified by GUID** — no gaps, and deleting one shifts every later
position. Use this term only for the Windows mechanism; use Workspace for the user-facing concept.

**Logical workspace number**:
The stable 1, 2, 3… label the user types and binds keys to. Mapped to a Virtual Desktop
**by GUID, never by array position**, so the label survives reordering and (later) reaping.
The reducer reasons only in logical numbers; the `logical→GUID` map lives in the bridge.

**Adoption**:
At startup, binding the Virtual Desktops that already exist to logical numbers `1..N` (by
GUID), and seeding the current workspace from the active desktop — so winspace inherits the
session rather than resetting it.

**Reaping**:
Destroying an empty, unfocused Workspace so junk desktops don't accumulate. A wanted
feature, deferred until window tracking (PRD 06) can detect "empty"; the GUID-anchored
mapping is what makes it safe.

**Display**:
One physical monitor. All Displays switch Workspace together (global switch); each window
stays on its own Display across a switch.
_Avoid_: Monitor, Screen.

### The core seam

**Reducer**:
The pure function at winspace's center: `reduce(state, event) → (newState, effects)`. Holds
all behavioral logic; performs no I/O and touches no Windows API.

**Event**:
A plain-data input to the Reducer describing something that happened (a keybind fired, a
window opened, the config reloaded). Named in the past/imperative, never carries behavior.

**Effect**:
A plain-data output from the Reducer describing something the I/O layer must do (switch
Virtual Desktop, set the foreground window, exit). The Reducer emits Effects; it never
performs them. No Effect ever moves or sizes a window — winspace owns no geometry.

**State**:
The authoritative in-memory model the Reducer transforms. Rebuilt on start — never persisted.

**Dispatcher**:
A user-invokable command bound to a hotkey (`workspace`, `movetoworkspace`, `focus`,
`quit`, …). A fired Dispatcher becomes an Event.
_Avoid_: Command, Action, Verb.

**Bind**:
A config line mapping a modifier + key to a Dispatcher and its arguments
(`bind = MODS, KEY, dispatcher, args`).
_Avoid_: Keybinding, Shortcut, Mapping.

### Windows & focus

**Eligibility gate**:
The rule that decides whether a window is Eligible or Ineligible: top-level & unowned,
`WS_VISIBLE` + `WS_THICKFRAME` + `WS_CAPTION`, not `WS_EX_TOOLWINDOW`, not DWM-cloaked, not
fullscreen. The *policy* (the AND of these facts, `isEligible`) is a pure predicate in the
Reducer; the *Probe* that gathers the facts is the adapter's job.
_Note_: `WS_THICKFRAME` is a hold-over from tiling (only resizable windows could tile) and
is a candidate to loosen for focus candidacy later; kept as-is for now.

**Eligible window**:
A window that passes the Eligibility gate — a real top-level application window winspace
treats as a focus candidate (and, in PRD 06, a rule-match target). winspace never moves or
sizes it; "Eligible" is about *whether winspace considers it*, not about geometry.
_Avoid_: Tileable (dead — nothing is tiled), Managed.

**Ineligible window**:
A window that fails the Eligibility gate (dialog, tool window, context menu, cloaked UWP
host, fullscreen). The focus sweep skips it; winspace does not track or store it — it is
classified and left entirely alone.
_Avoid_: Ignored, unmanaged, Floating.

**Spatial focus**:
The resolution of a `focus left|right|up|down` Dispatcher. **Stateless**: at keypress the
adapter enumerates the windows and Probes their rects, and the pure Reducer picks the
nearest Candidate in the requested Direction from the Origin across the virtual screen
(crossing monitors naturally), emitting a `SetForegroundWindow` Effect. No focus order is
persisted and no geometry is stored — the rects are read fresh each time, so they are never
stale.
_Avoid_: Directional tiling move, `movewindow` (the tile-swap dispatcher died with tiling).

**Direction**:
The `left | right | up | down` argument of a `focus` request — the axis and sense the
resolution travels. Reasoned about in virtual-screen coordinates (Win32 convention: x grows
right, y grows **down**, so `down` is +y and `up` is −y).
_Avoid_: Arrow, way, side.

**Origin**:
The current foreground window at the moment a `focus` fires — the reference point the
resolution measures *from*. Its Eligibility is irrelevant (an Ineligible window can still be
an Origin) and it is never its own target. No Origin (nothing focused) → the request is a
no-op.
_Avoid_: Source, current, active.

**Candidate**:
An Eligible window, other than the Origin, that the resolution considers as a focus target.
The Reducer keeps only Candidates lying ahead in the requested Direction; of those it prefers
ones sharing the Origin's cross-axis band, then the nearest by centre distance.
_Avoid_: Target (that's the single chosen one), option.

**Probe**:
A one-shot synchronous read of a window's live attributes (styles, cloak state, rect) taken
*reactively* at the moment they are needed — on a `focus` keypress (Spatial focus), never on
a timer. In PRD 06 the parked hook adapter will also Probe on a lifecycle edge.
_Avoid_: Poll, scan, snapshot-loop (there is no interval).

**Appeared / Vanished** *(PRD 06)*:
The window-lifecycle edges delivered by a `SetWinEventHook` adapter. **Appeared** =
`EVENT_OBJECT_SHOW` / `EVENT_OBJECT_UNCLOAKED` (never raw `CREATE` — the window is half-born
there and misclassifies, per [ADR-0006](docs/adr/0006-window-tracking-probe-decide-seam.md));
**Vanished** = `EVENT_OBJECT_DESTROY` / `HIDE` / `CLOAKED`. Removed from master with tiling;
PRD 06 (window rules) — the hook's first genuine consumer — reintroduces them. `Appeared`
carries a Probed `WindowAttrs` (the Eligibility facts) **and** a `WindowIdentity` (exe / class
/ title for matching); `Vanished` carries just a `WindowId`. Not yet in master — the code
lands with PRD 06.

**WindowRule**:
A parsed `windowrule` that pins a matching app to a target Workspace on `Appeared`. One rule
names **one** match field and a pattern (`exe:…`, `class:…`, or `title:…`); `exe`/`class`
match case-insensitively and exactly, `title` is a regex. Rules are evaluated in the fixed
field precedence **exe → class → title** (config order breaks within-field ties), first match
wins. **Place-once** (see below) and Workspace-only — never geometry.
_Avoid_: windowrulev2, layer rule.

**WindowIdentity**:
The string half of a window Probe — `exe` (process image basename), `windowClass`, `title` —
gathered only on the `Appeared` path (rules need it; the focus sweep never does, so it stays
off that hot path). Plain UTF-8 `std::string`, narrowed at the adapter so the core stays
`wchar_t`-free. Distinct from `WindowAttrs`, which carries the Eligibility bools + rect.
_Avoid_: WindowInfo, metadata.

**Place-once**:
The rule that a `WindowRule` assigns a window a Workspace **exactly once in its lifetime** and
never re-asserts it — a window the user later moves is not yanked back. Enforced by a bounded
`placed` set of `WindowId` in `State`: an id is inserted on its first `Appeared` **once
Eligible** (matched or not; an Ineligible edge inserts nothing, so it is re-evaluated when it
later becomes Eligible) and erased on `Vanished`. This is the deliberate, bounded reintroduction of window state
that [ADR-0009](docs/adr/0009-window-rules-place-once-state.md) records against ADR-0007's
otherwise stateless window side.
_Avoid_: continuous enforcement, pinning-forever.

### Launcher

**Launch entry**:
A parsed `exec` or `exec-once` line — a **verbatim command line** winspace runs at start via
`CreateProcessW`. The command is stored unparsed (no `$var` expansion) and handed to Win32
as-is. A Launch entry carries **no target Workspace** and does **no** window matching: it only
starts a process (and detaches — winspace tracks nothing about the child). To place a launched
app on a Workspace, pair it with a **WindowRule** matching the app's `exe`. The originally
specified PID-match placement was dropped before it was built — see
[ADR-0011](docs/adr/0011-launcher-launch-only-placement-via-windowrule.md).
_Avoid_: PID match, placement (the launcher places nothing — WindowRule does).

**exec / exec-once**:
The two kinds of Launch entry. **exec-once** runs only at the initial start; **exec** runs at
start **and** on every later config reload. The distinction is stateless — it is purely which
lifecycle Event the Reducer is handling (a start emits every entry; a reload emits only the
`exec` ones), never a remembered "already launched" flag or a process-list check. Reload is
the `reload` path from a later slice; until it lands, only the start fires.
_Avoid_: autostart (that is the OS logon task that starts winspace itself), run, spawn-rule.

### Threads

**Hotkey thread**:
The thread that owns `RegisterHotKey` and its message loop; it turns keystrokes into
Events and hands them to the Worker thread. Does no domain logic.

**Worker thread**:
The single thread that owns the State, runs the Reducer, executes the emitted Effects, and
is the sole owner of the COM Virtual Desktop bridge (STA). It also runs the Spatial focus
Probe sweep. It does **not** own the hook adapter — that is its own thread (below), which only
produces `Appeared` / `Vanished` Events for the Worker to reduce.
_Avoid_: Main thread, UI thread (winspace is windowless — there is no UI thread).

**Hook thread** *(PRD 06)*:
The dedicated thread that owns the `SetWinEventHook` adapter (`WINEVENT_OUTOFCONTEXT`),
mirroring the Hotkey thread: its callback runs the noise gate, Probes the window, and posts an
`Appeared` / `Vanished` Event to the Worker — no domain logic, no State. Kept off the Worker
thread so hook delivery never queues behind a blocking Effect (a `SwitchDesktop` or the
cloak-move round-trip). Reintroduced with PRD 06; removed from master with tiling.
_Avoid_: Event thread, watcher thread.

### VM seam testing

Terms for the unattended VM harness that automates the manual smoke scripts. See
[ADR-0005](docs/adr/0005-vm-seam-test-harness.md). Note: **Seam** (above) is the pure
interface boundary — a **Smoke seam** is a live behavior tested through it, a different thing.

**Smoke seam**:
A single live I/O-layer behavior only the running binary can exhibit — adoption,
create-on-demand, clean quit, the variant diagnostic, degrade-don't-crash — provable by
driving winspace on a real or virtual desktop, never by a unit test. The unit of a Manual
smoke script and of the VM harness.
_Avoid_: Test seam (reads as the core Seam), scenario, case.

**Manual smoke script**:
The ordered list of Smoke seams that is an I/O-layer slice's definition of done (slice-01's
six steps, slice-11's parity steps). "Manual" is historical — the VM harness now runs it.

**Trigger**:
Causing a Dispatcher to fire during a test by synthesizing real keyboard input into the live
guest session (`SendInput`), so the OS hotkey path runs exactly as under a physical press.
_Avoid_: Inject, keypress, sendkey.

**Oracle**:
The independent source of truth a Smoke seam asserts against. OS state (Virtual Desktop
registry, window enumeration, process list) is the primary Oracle; winspace's own stderr log
is a secondary one, used only where no external observable exists.
_Avoid_: Expectation, golden.

**Guest runner**:
The one interactive process inside the VM that executes the whole harness in a single
logged-in session and emits the result report.
_Avoid_: Agent, driver.

**Host orchestrator**:
The host-side controller that stages the VM around a Guest runner invocation — revert the
snapshot, deploy the build, launch the runner, collect and summarise its results.
