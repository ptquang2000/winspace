new-item -itemtype directory -force -path .\build  > $null
push-location .\build
cl.exe -WX -W4 -wd4100 -wd4189 -wd4201 -Gm- -nologo -MT -Zi -Od -FC -std:c++latest ../win32_winspace.cpp user32.lib -Fe:winspace.exe
pop-location
