# winspace

A Windows 11 workspace + auto-tiling window manager that leans on OS-native facilities
and stays off the input critical path. This glossary fixes the ubiquitous language shared
by `PRD.md`, `DESIGN.md`, the `issues/`, and the code.

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
feature, deferred until window tracking can detect "empty" (issue 02); the GUID-anchored
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
Virtual Desktop, position a window, exit). The Reducer emits Effects; it never performs them.

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

### Window tracking

**Eligibility gate**:
The rule that decides whether a window is Tileable or Floating: top-level & unowned,
`WS_VISIBLE` + `WS_THICKFRAME` + `WS_CAPTION`, not `WS_EX_TOOLWINDOW`, not DWM-cloaked, not
fullscreen. The *policy* (the AND of these facts) is a pure predicate in the Reducer; the
*Probe* that gathers the facts is the adapter's job.

**Tileable window**:
A window that passes the Eligibility gate and so participates in the layout. winspace owns
its geometry.
_Avoid_: Managed, tiled (adjective is fine; the noun for the set is "tileable").

**Ineligible window**:
A window that fails the Eligibility gate (dialog, tool window, context menu, cloaked UWP
host, fullscreen). winspace never tiles it and — as of issue 02 — does not store it either;
it is classified and left alone. "Floating" the noun, when unqualified, means this.
_Avoid_: Ignored, unmanaged.

**Detached window**:
A window that *passes* the gate (genuinely manageable) but is deliberately outside the
layout right now — dragged out, matched by a `windowrule float`, or toggled off. Unlike an
Ineligible window it can be *snapped back* into Focus order. The Detached set is empty until
06/07 give it content (rules, drag-to-float, a `togglefloat` Dispatcher); do not conflate it
with Ineligible.

**Probe**:
A one-shot synchronous read of a single window's live attributes (styles, cloak state, rect)
taken *reactively* when a hook fires — never on a timer. Push (the hook) says *when*; the
Probe says *what*, because the win-event callback carries no attributes.
_Avoid_: Poll, scan, snapshot-loop (there is no interval).

**Appeared / Vanished**:
The two lifecycle edges the Reducer reacts to. Appeared = `EVENT_OBJECT_SHOW` /
`EVENT_OBJECT_UNCLOAKED` (probe here); Vanished = `EVENT_OBJECT_DESTROY` / `HIDE` /
`CLOAKED`. Raw `EVENT_OBJECT_CREATE` is ignored — the window is half-born and misclassifies.

**Focus order**:
The Tileable windows kept as a priority-ordered list, front = most-recently-focused. The
layout function acts on the head (the window filled, or in later slices the tile split). Its
priority signal is focus recency; until foreground tracking lands it is approximated by
appearance order (a newly shown window takes focus, so it front-inserts).
_Avoid_: MRU, stack, z-order (z-order is OS stacking — a different thing).

### Threads

**Hotkey thread**:
The thread that owns `RegisterHotKey` and its message loop; it turns keystrokes into
Events and hands them to the Worker thread. Does no domain logic.

**Worker thread**:
The single thread that owns the State, runs the Reducer, executes the emitted Effects, and
is the sole owner of the COM Virtual Desktop bridge (STA). All later window-tracking and
tiling logic lands here.
_Avoid_: Main thread, UI thread (winspace is windowless — there is no UI thread).

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
