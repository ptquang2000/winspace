new-item -itemtype directory -force -path .\build  > $null
push-location .\build
cl.exe -WX -W4 -wd4100 -Gm- -nologo -MT -Zi -Od -FC -std:c++latest ../win32.cpp User32.lib -Fe:winspace.exe
pop-location
