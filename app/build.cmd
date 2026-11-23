@echo off
rem Build HamsterBackup.exe with MSVC (run from a "x64 Native Tools" prompt, or after vcvars64.bat).
rem The exe is written to the project's base folder, next to Backup.ps1 (same layout as the release zip).
setlocal
cd /d "%~dp0"
set OUT=%~dp0..\HamsterBackup.exe
rc /nologo hamster.rc || goto :fail
cl /nologo /std:c++17 /O1 /MT /W3 /EHsc /GS /Gy /DUNICODE /D_UNICODE hamster.cpp hamster.res ^
   /Fe:"%OUT%" /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTINPUT:hamster.manifest || goto :fail
del hamster.obj hamster.res 2>nul
for %%F in ("%OUT%") do (
    echo.
    echo Built: %%~fF
    echo Size:  %%~zF bytes
)
exit /b 0
:fail
del hamster.obj hamster.res 2>nul
echo BUILD FAILED
echo (If the linker says it cannot open HamsterBackup.exe: Hamster is running - exit it from the tray menu first.)
exit /b 1
