# Window tracking: adapter probes plain-data facts, the pure Reducer decides and computes logical rects

Issue 02 is the first slice that makes the pure core reason about windows and geometry —
both inherently Windows concepts. We keep the Reducer `windows.h`-free by splitting every
OS-touching concern into two halves: a **Probe** (adapter side — gathers a window's live
attributes into plain data) and a **policy** (Reducer side — a pure decision over that plain
data). Concretely: an out-of-context `SetWinEventHook` adapter on its own thread probes
window attributes into a plain-data snapshot carried on an `Appeared` Event; the Reducer
holds the Eligibility predicate and the Focus order and emits **logical** target rects; the
Worker applies `DWMWA_EXTENDED_FRAME_BOUNDS` + DPI compensation at execute time. Lifecycle
keys off the `SHOW`/`UNCLOAKED` (appeared) and `DESTROY`/`HIDE`/`CLOAKED` (vanished) edges,
never raw `CREATE`.

## Considered Options

- **Adapter decides eligibility (emit `TileableWindowOpened`).** Rejected: it moves the
  behavioral gate into the `windows.h` I/O layer, where only the VM harness can exercise it,
  and contradicts the AC "reducer tests assert eligibility."
- **Symbolic position Effect (`FillWorkArea{id, monitor}`, adapter computes the rect).**
  Rejected: issue 03's BSP split is pure rect arithmetic that *must* live in the Reducer, so
  a symbolic Effect would be yanked back into the core one issue later.
- **Piggyback `rcWork` on `Appeared`, no monitor model.** Rejected in favor of a
  whole-topology `MonitorsChanged` snapshot the Reducer folds into `State.monitors`, which
  04's "monitor layout-change re-tiles" needs anyway.
- **Probe once, at raw `CREATE`.** Rejected: the window is half-born (styles/visibility not
  final, UWP host still cloaked) and misclassifies; the `SHOW`/`UNCLOAKED` edge is stable.

## Consequences

- Eligibility and layout are pure, so reducer tests cover them by feeding synthetic Events —
  no live desktop.
- The drop-shadow delta and DPI never enter the core; the Reducer speaks logical rects only.
- Probing is reactive, never on a timer — the adapter is idle in `GetMessage` when the
  desktop is quiet, consistent with the "work with the scheduler" philosophy.
- One tileable window is touched three times per placement (classify at hook time, then two
  reads — window rect + frame bounds — at execute time). Deliberate: classification and
  measurement happen on different threads at different instants and are not fungible.
- "Own" is split: the Worker *owns* the `State` value (lifetime); the Reducer, a pure free
  function, *authors* its transitions. `State.monitors` and the Focus order are data in that
  value, not members of the Reducer.
