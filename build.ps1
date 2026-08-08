new-item -itemtype directory -force -path .\build  > $null
push-location .\build
cl.exe /nologo /Zi /std:c++20 ..\win32.cpp /Fe:winspace.exe
pop-location
