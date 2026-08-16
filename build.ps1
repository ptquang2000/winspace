new-item -itemtype directory -force -path .\build  > $null
push-location .\build
cl.exe /nologo /MT /Zi /std:c++latest ..\win32.cpp User32.lib /Fe:winspace.exe
pop-location
