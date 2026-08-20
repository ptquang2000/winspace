new-item -itemtype directory -force -path .\build  > $null
push-location .\build
fl.exe -WX -W4 -wd4100 -wd4189 -Gm- -nologo -MT -Zi -Od -FC -std:c++latest ../win32.cpp User32.lib -Fe:winspace.exe
pop-location
