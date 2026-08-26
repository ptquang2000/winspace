new-item -itemtype directory -force -path .\build  > $null
$warn = "-WX -W4 -wd4100 -wd4189 -wd4201 -wd4505"
$debug = "-MTd -Zi -Od -FC"
$out = "-Fo:build/ -Fd:build/ -Fe:build/winspace.exe"
$libs = "user32.lib dwmapi.lib"
$elapsed = (Measure-Command {
    cl.exe -nologo -std:c++latest $warn $debug win32_winspace.cpp $libs $out
}).TotalSeconds
Write-Host ("Compilation finished in {0:N3}s" -f $elapsed)
