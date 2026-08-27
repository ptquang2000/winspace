#!/bin/bash
export PATH=~/msvc/opt/msvc/bin/x64:$PATH
mkdir -p build
warn="-WX -W4 -wd4100 -wd4189 -wd4201"
debug="-MTd -Zi -Od -FC"
out="-Fo:build/ -Fd:build/ -Fe:build/winspace.exe"
libs="user32.lib dwmapi.lib"
TIMEFORMAT="Compilation finished in %Rs"; time \
  cl.exe -nologo $warn $debug -std:c++latest win32_winspace.cpp $libs $out
