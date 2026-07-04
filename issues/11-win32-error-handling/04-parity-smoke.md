# 11.04 — Behavior-parity smoke + build-clean

**Labels:** `ready-for-agent`
**Blocked by:** 11.02, 11.03

## What to build

Close the slice: prove the refactor changed structure, not behavior, and that the purity
guardrails held. No new code beyond fixes surfaced here — this is the verification gate.

Since `io::Error` is I/O-only (there is **no new test seam**), verification is build-clean
+ the existing slice-01 manual smoke, re-run as a **parity oracle**, plus a diagnostic-
quality check that exercises the new `formatError` path.

## Verification steps

1. **Build-clean:** app TU + test TU build under `/W4 /WX /permissive-` with `error.cpp`
   included first; no new warnings.
2. **Purity intact:** the pure test TU still links **no** WM libraries; the Reducer +
   config-parser Catch2 suites pass **unmodified** (proves the refactor stayed I/O-only).
3. **Behavior parity — the slice-01 six-step smoke, identical outcomes:**
   1. Windowless (no console / taskbar / Alt-Tab).
   2. Adoption: 3 desktops open, `$mod+2` → existing 2nd desktop.
   3. Create-on-demand: `$mod+5` → one new desktop at the tail; `$mod+4` → one more (no fill, no clamp).
   4. GUID-anchored stability: reorder in Task View, `$mod+5` → same desktop.
   5. Quit: `quit` bind → clean exit, no orphan desktops.
   6. Variant diagnostic: 24H2 log names the resolved IID.
4. **Diagnostic quality (new):** `WINSPACE_FORCE_VD_VARIANT=23h2-kb` (or another forced
   failure) emits a `formatError` line carrying **file:line + hex + system text** — richer
   than the pre-refactor message, and never garbage on an undocumented COM code.
5. **Degrade-don't-crash:** an already-registered hotkey is skipped-and-logged while the
   rest bind; a null bridge no-ops the switch. No crash on any single failed call.

## Acceptance criteria

- [ ] Both TUs build clean under `/W4 /WX /permissive-`; no new warnings
- [ ] Test TU links no WM libraries; Reducer + parser suites pass unmodified
- [ ] All six slice-01 smoke steps behave identically to pre-refactor
- [ ] A forced failure emits a `formatError` diagnostic with source location + hex + (when available) system text; never garbage
- [ ] Skip-and-log and null-bridge-no-op degrade paths confirmed; no crash on a single failed OS call
