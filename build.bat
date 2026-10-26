@echo off
rem Build one DLL with the Visual Studio C++ tools + MinHook sources in minhook\ (the workflow fetches the pinned commit).
rem   build.bat <name> [x86]   (<name>.cpp -> <name>.dll; x86 = a 32-bit game: <name>_x86.dll with hde32)
setlocal
cd /d %~dp0
set NAME=%1
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
if "%VS%"=="" set VS=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools
set VC=vcvars64.bat
set HDE=hde64
set OUT=%NAME%
if /i "%2"=="x86" (set VC=vcvars32.bat& set HDE=hde32& set OUT=%NAME%_x86)
call "%VS%\VC\Auxiliary\Build\%VC%" >nul || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /LD /Iminhook\include %NAME%.cpp minhook\src\buffer.c minhook\src\hook.c minhook\src\trampoline.c minhook\src\hde\%HDE%.c /Fe:%OUT%.dll /link /DLL
