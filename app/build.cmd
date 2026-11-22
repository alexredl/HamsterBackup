@echo off
rem Build HamsterBackup.exe with MSVC (run from a "x64 Native Tools" prompt, or after vcvars64.bat)
setlocal
cd /d "%~dp0"
rc /nologo hamster.rc || goto :fail
cl /nologo /std:c++17 /O1 /MT /W3 /EHsc /GS /Gy /DUNICODE /D_UNICODE hamster.cpp hamster.res ^
   /Fe:HamsterBackup.exe /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTINPUT:hamster.manifest || goto :fail
del hamster.obj hamster.res 2>nul
echo.
echo Built: %~dp0HamsterBackup.exe
for %%F in (HamsterBackup.exe) do echo Size:  %%~zF bytes
exit /b 0
:fail
echo BUILD FAILED
exit /b 1
