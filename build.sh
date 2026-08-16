#!/bin/bash
export PATH=~/msvc/opt/msvc/bin/x64:$PATH
mkdir -p build
pushd build> /dev/null
cl.exe -WX -W4 -wd4100 -Gm- -nologo -MT -Zi -Od -FC -std:c++latest ../win32.cpp User32.lib -Fe:winspace.exe
popd> /dev/null
