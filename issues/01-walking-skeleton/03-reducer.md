# 01.03 — Pure Reducer (Seam 1)

**Labels:** `ready-for-agent`
**Blocked by:** 01.01

## What to build

The central seam: a pure Reducer in core, no `windows.h`. This is **Seam 1**, tested via
the Effects it emits — never by inspecting State internals.

```
struct ReduceResult { State state; std::vector<Effect> effects; };
ReduceResult reduce(const State& s, const Event& e);

Event  = WorkspaceSwitch{ int n } | Quit{}          // std::variant of aggregates
Effect = SwitchToWorkspace{ int logical } | Exit{}  // std::variant of aggregates
State  = { int current_workspace; bool running; }
```

`reduce` dispatches with an overloaded visitor and returns fresh State by value (State is
tiny; purity buys testability). Note the effect is `SwitchToWorkspace{logical}` — the
Reducer reasons only in Logical workspace numbers and never sees a Virtual Desktop GUID
(the `logical→GUID` map lives in the bridge, task 06).

## Acceptance criteria

- [ ] `reduce(s, WorkspaceSwitch{n})` emits exactly `SwitchToWorkspace{n}` and sets `current_workspace = n`
- [ ] `reduce(s, Quit{})` emits `Exit{}` and clears `running`
- [ ] `reduce` is pure — same inputs, same outputs; no I/O, no globals, no Windows calls
- [ ] `Event`/`Effect` are `std::variant`s of aggregate structs; visitor is exhaustive
- [ ] Compiles into the test TU with no WM libraries linked
- [ ] Catch2 tests assert on emitted Effects (and State only as an Effect would reveal), never on internal structure
