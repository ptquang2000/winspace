# 01.01 — Build harness & vendored Catch2

**Labels:** `ready-for-agent`
**Blocked by:** none

## What to build

The handmade build that every other task compiles through. One `build.ps1` invoking
`cl.exe` directly — no CMake, no vcpkg, no package manager. It assumes an x64 Native Tools
dev prompt (`cl` already on `PATH`) and fails fast with a single line if `cl` is absent.

Two unity translation units:
- **App TU** — includes core + I/O adapters + `WinMain`; links the WM import libraries
  (`user32 ole32 oleaut32 dwmapi shcore`); built `/SUBSYSTEM:WINDOWS` (windowless).
- **Test TU** — includes **core only** + vendored Catch2; links **none** of the WM
  libraries; built `/SUBSYSTEM:CONSOLE`.

The linker-level purity guarantee is the point: because the test TU links no WM import
libs, any accidental OS call from core is a link error, not a runtime surprise.

Vendor Catch2 v3 (amalgamated). Check in a static `compile_commands.json` (two entries, cl
driver mode) for clangd. Always-clean full rebuild into a gitignored `build/<config>/`.

## Acceptance criteria

- [ ] `build.ps1` builds a windowless no-op app TU and a test TU with a trivial passing test
- [ ] Shared flags: `/nologo /std:c++latest /EHsc /W4 /WX /permissive- /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN`; debug `/Od /Zi /RTC1 /MDd` + `/DEBUG`; release `/O2 /DNDEBUG /MD`
- [ ] Params: `-Config debug|release` (default debug), `-Test` (build+run the test binary), `-Clean`
- [ ] Missing `cl` → one-line failure, non-zero exit
- [ ] A deliberate `#include <windows.h>` (or WM call) added to a core file makes the **test TU fail to link** — demonstrating the guarantee — then reverted
- [ ] Minimal timed console output: one line per TU, colored PASS/FAIL, total elapsed
- [ ] `compile_commands.json` present; clangd resolves MSVC/SDK headers from the dev prompt
