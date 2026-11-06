@echo off
rem build.bat -- build drifto.exe (the game client) on Windows: MSVC x64 Release + vcpkg.
rem Run from anywhere (double-click works). Needs Visual Studio 2022 with the
rem "Desktop development with C++" workload (MSVC, CMake, vcpkg), and Git on PATH.
rem   tools\windows\build.bat            configure (first run: vcpkg builds glfw3,
rem                                      openssl, cyclonedds -- ~15-30 min once) + build
rem   tools\windows\build.bat clean      wipe build\windows first
rem Output: build\windows\Release\drifto.exe (+ shaders, net\config, certs, assets, DLLs next to it)
setlocal EnableExtensions
cd /d "%~dp0..\.."
set "ROOT=%CD%"

if /i "%~1"=="clean" if exist build\windows rmdir /s /q build\windows

rem --- find Visual Studio (prefer 2022 = MSVC 14.4x) -----------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -version [17.0^,18.0^) -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
  echo [build] Visual Studio 2022 with C++ tools not found ^(looked in "%VSDIR%"^)
  exit /b 1
)
echo [build] Visual Studio: %VSDIR%
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%ROOT%"
if not defined VCPKG_ROOT set "VCPKG_ROOT=%VSDIR%\VC\vcpkg"
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
  echo [build] vcpkg not found at %VCPKG_ROOT% ^(install the vcpkg component or set VCPKG_ROOT^)
  exit /b 1
)
echo [build] vcpkg: %VCPKG_ROOT%

cmake -S . -B build\windows -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows || exit /b 1
cmake --build build\windows --config Release --parallel || exit /b 1
echo [build] OK: %ROOT%\build\windows\Release\drifto.exe
endlocal
