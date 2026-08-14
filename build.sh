#!/bin/bash
export PATH=~/msvc/opt/msvc/bin/x64:$PATH
mkdir -p build
pushd build
cl.exe /nologo /Zi /FC /std:c++latest ../win32.cpp User32.lib /Fe:winspace.exe
popd
