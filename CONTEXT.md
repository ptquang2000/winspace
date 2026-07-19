# winspace

A Windows 11 workspace + focus manager that leans on OS-native facilities and stays off
the input critical path. It switches Workspaces and switches keyboard focus between
windows; it owns **no window geometry** — it never moves or sizes a window (see
[ADR-0007](docs/adr/0007-drop-tiling-no-window-geometry.md)). This glossary fixes the
ubiquitous language shared by the ADRs (`docs/adr/`) and the code.

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
session rather than resetting it. This is a **Workspace**-level concept — the desktops, not
the windows on them. _Distinct from_ **Rule adoption** (the window-level startup sweep).

**Reaping**:
Destroying an empty, unfocused Workspace so junk desktops don't accumulate. A wanted
feature, still deferred: it needs per-Workspace occupancy tracking the hook does not yet
maintain (the lifecycle stream is in master, an occupancy model is not); the GUID-anchored
mapping is what makes it safe.

**Display**:
One physical monitor. All Displays switch Workspace together (global switch); each window
stays on its own Display across a switch.
_Avoid_: Monitor, Screen.

**Empty Display**:
A Display carrying **zero Eligible windows on the current Workspace**. "Empty" is about
Eligible windows only — wallpaper, taskbar, and Ineligible windows never count, and a window
parked on another Virtual Desktop does not occupy the Display while that Workspace is hidden.
The target concept for the **Spread** WindowRule action: a matching window is relocated onto an
Empty Display on `Appeared`, and when none remains the action is a no-op (**overflow → no
placement** — the window is left Eligible and untouched wherever the OS opened it).
_Avoid_: Free monitor, empty screen, Floating (there is no layout to float above — that term
died with tiling, ADR-0007).

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
treats as a focus candidate (and a rule-match target). winspace never moves or
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
a timer. The hook adapter also Probes on a lifecycle edge (an `Appeared`).
_Avoid_: Poll, scan, snapshot-loop (there is no interval).

**Appeared / Vanished** *(PRD 06)*:
The window-lifecycle edges delivered by a `SetWinEventHook` adapter. **Appeared** =
`EVENT_OBJECT_SHOW` / `EVENT_OBJECT_UNCLOAKED` (never raw `CREATE` — the window is half-born
there and misclassifies, per [ADR-0006](docs/adr/0006-window-tracking-probe-decide-seam.md));
**Vanished** = `EVENT_OBJECT_DESTROY` / `HIDE` / `CLOAKED`. `Appeared` carries a Probed
`WindowAttrs` (the Eligibility facts) **and** a `WindowIdentity` (exe / class / title for
matching); `Vanished` carries just a `WindowId`. Removed from master with tiling, then
reintroduced by PRD 06 (window rules) — the hook's first genuine consumer — so both are in
master now.

**Rule adoption**:
The window-level startup sweep: once its hooks are live, the adapter runs `EnumWindows` and
posts a synthetic **Appeared** for every top-level window, so a window **rule** pins apps that
were already open when winspace launched — the startup counterpart of a live `SHOW`. _Distinct
from_ **Adoption**, which is the Workspace/Virtual-Desktop-level startup binding; Rule adoption
inherits the *windows*, Adoption inherits the *desktops*.

**WindowRule**:
A parsed `windowrule` carrying one **action** — **Place** (`workspace N`, pin a matching app to
a target Workspace on `Appeared`), **Ignore** (exclude a matching window from Spatial focus), or
**Spread** (`spread`, relocate a matching app onto an **Empty Display** on `Appeared`; see below).
One rule names **one** match field and a pattern (`exe:…`, `class:…`, or `title:…`); `exe`/`class`
match case-insensitively and exactly, `title` is a regex. Rules are evaluated in the fixed
field precedence **exe → class → title** (config order breaks within-field ties), first match
wins — and the winning rule's *action* is what applies (a window matching both an Ignore and a
Place rule resolves by that same first-match order). Place, Ignore and Spread are all keyed on
`WindowIdentity`, fired on `Appeared`, and **place-once**. Spread is the **lone** action that
writes geometry — the single bounded exception to ADR-0007 ([ADR-0015](docs/adr/0015-spread-bounded-geometry-write-to-empty-display.md)).
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

**Ignore-set**:
The bounded `ignored` set of `WindowId` in `State` that enforces the **Ignore** WindowRule
action. An id enters on its first Eligible `Appeared` when the window matches an Ignore rule and
is erased on `Vanished`, mirroring `placed`. Spatial focus consults it: `resolveFocus` drops any
Candidate whose id is in the set, so an Ignored window is never a focus target. Kept as state —
rather than re-matched live at keypress — so the WindowIdentity read (a `title` match needs
`GetWindowText` → `WM_GETTEXT`, which can block on a hung window) stays on the hook thread and
never freezes focus navigation. Like Place-once, it is **not re-asserted on reload**: an Ignore
rule added at reload only takes effect for a matching window when it next `Appears`.
_Avoid_: unmanaged, blacklist, Ineligible (an Ignored window IS Eligible — it is excluded by
rule, not by the Eligibility gate).

**Spread** *(the action)*:
The third **WindowRule** action (`windowrule = exe:…, spread`): on a matching window's first
Eligible `Appeared`, relocate it **once** onto an **Empty Display**. Parallel to **Place** but
targeting a Display, not a Workspace, and — uniquely — it **writes geometry**. Empty Display is
resolved **statelessly**: a Probe round-trip (mirroring Spatial focus, ADR-0008) enumerates the
Displays that already carry an Eligible window on the current Workspace, and the Reducer picks one
that carries none (the subject window excluded from the count). It is **place-once** (recorded in
`placed`, erased on `Vanished`, never re-asserted on uncloak or reload — a dragged window is not
yanked back). Two boundaries: **overflow → no placement** (no Empty Display left → emit nothing;
the window is left where the OS opened it, never forced onto the focused Display), and
**best-effort under bursts** (no persisted occupancy, so a simultaneous burst of matching windows
may collide on one Empty Display — accepted, since overflow is a benign no-op). The move is the
lone `PositionWindow` Effect — a single `SetWindowPos`, never a `layout()` — the bounded reversal
of ADR-0007 that [ADR-0015](docs/adr/0015-spread-bounded-geometry-write-to-empty-display.md)
records, the geometry twin of Place-once's bounded-state reversal (ADR-0009).
_Avoid_: Tile, fill, distribute, balance (Spread relocates one window per Empty Display on
`Appeared` — it is not a continuous layout that rebalances as windows come and go).

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

### Config & reload

**Reload** *(the trigger)*:
The `reload` Dispatcher — a Bind target that re-reads and re-parses the config file live, with
no restart. Distinct from **Reloaded** (below): `reload` fires from a hotkey and *causes* the
re-parse; `Reloaded` is the post-parse Event that only re-launches `exec` entries. The flow is
I/O-orchestrated because the Reducer is pure and cannot read files: `Reload` Event → the Reducer
emits a `ReloadConfig` Effect → the Worker re-reads + re-parses (the parse lives on the Worker
thread) → on success it swaps the new `rules`/`execs` into State, hands the new Binds to the
Hotkey thread to re-register, and posts `Reloaded` to itself.
_Avoid_: hot-swap, restart.

**Reloaded** *(the Event)*:
The post-parse lifecycle Event that makes the Reducer emit a `LaunchApp` for each `exec` (not
`exec-once`) entry. Carries no config — the parse already happened in the I/O layer and the new
`execs` are already in State. Not the trigger; see **Reload**.

**Keep-last-good**:
On reload, the running config stays live if the new file does not parse cleanly. Reload is
**atomic**: any Diagnostic rejects the *whole* file — the Worker keeps the currently-running
Binds, rules, and execs and logs the errors; nothing is partially applied. Contrast **startup**,
which degrades **per-line** (keeps the good lines, drops the bad) because its only fallback is
the destructive built-in default, whereas reload's fallback is the user's own working config.
The asymmetry is deliberate: fall back to the least-destructive thing available.
_Avoid_: rollback, last-known-good (this is config, not State).

**Settings** *(the flat tail of the grammar)*:
General `key = value` options that are neither Binds, rules, nor Launch entries. winspace has
**no `section { … }` grammar and no workspace rules** — both are Hyprland constructs that only
ever carried geometry, which winspace does not own (ADR-0007). The lone surviving setting is
**`start_at_login`** — a bool (`true`/`false`, case-insensitive; anything else is a Diagnostic)
parsed into `Config` and carried into `State` (`bool startAtLogin`, seeded at Worker
construction beside `rules`/`execs`, reseeded on reload). Slice 09 only *parses* it and holds
it in State; the **Logon task** it controls is registered/removed by slice 10, which reads
`State.startAtLogin` and emits a **SyncAutostart** Effect. A removed tiling setting
(`min_tile_width`, `min_tile_height`) or dispatcher (`movewindow`, `maximize`, `resizeactive`,
`togglefloat`, `movetomonitor`) is a **targeted** "removed with tiling" Diagnostic, distinct
from the generic unknown-directive message, so a ported Hyprland config reads as *scoped*, not
broken.
_Avoid_: section, category, general{} (there are no blocks).

### Autostart

**Logon task**:
The OS artifact that makes winspace start with the user session — a **Task Scheduler task with a
logon trigger**, registered per-user at `\winspace\<username>` (a dedicated `winspace` task
folder, one task per user, so two accounts on one machine never collide). It launches the
windowless `winspace.exe` at sign-in with **restart-on-failure** and a **limited** (LUA, never
elevated) run level. Deliberately **not a Windows service** (a Session-0 service cannot touch the
interactive desktop, Virtual Desktops, or hotkeys) and **not a userspace watchdog** (that would
own restart-on-failure — a thing Windows gives for free — and its supervisor would itself need
autostarting): the OS scheduler *is* the supervisor and winspace is the worker it launches into
the session (see [ADR-0013](docs/adr/0013-autostart-per-user-logon-task.md)). The worker cannot
manage windows before the session exists, so a logon task reaches the first manageable moment as
early as anything can — and **Adoption** absorbs any window that opened first. Controlled by the
**`start_at_login`** Setting via the **SyncAutostart** Effect.
_Avoid_: service, watchdog, supervisor process, Run key, Startup folder.

**SyncAutostart** *(the Effect)*:
The Effect that makes the OS **Logon task** match `State.startAtLogin` — **declarative**, not a
transition command: the Reducer always emits `SyncAutostart{startAtLogin}` and never decides
register-vs-remove. The Worker executes it on its existing STA apartment via COM `ITaskService`
(never `schtasks`/PowerShell): `enabled` → `RegisterTaskDefinition` with `TASK_CREATE_OR_UPDATE`
(idempotent — no duplicate on repeated enable, and re-writing the exe path each launch self-heals
a moved binary); `!enabled` → delete the task, counting `ERROR_FILE_NOT_FOUND` as success so a
repeated disable is a clean no-op. Emitted from **Started** (initial start) and **Reloaded** (so
toggling `start_at_login` + `reload` registers/removes the task live), mirroring how `LaunchApp`
falls out of those same two lifecycle Events. Failures degrade-and-log (ADR-0004); autostart
never blocks or crashes the running WM.
_Avoid_: RegisterAutostart/RemoveAutostart (it is one declarative Effect, not a transition pair).

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
cloak-move round-trip). Removed from master with tiling, reintroduced when PRD 06 (window
rules) landed; in master now.
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
