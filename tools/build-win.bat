@echo off
rem Windows build: direct clang-cl invocations. clang-cl is the supported
rem Windows toolchain — MSVC's C11 atomics remain experimental and the
rem lock-free paths carry no second atomics implementation. clang-cl is
rem preinstalled on GitHub Windows runners (LLVM).
rem
rem Usage: tools\build-win.bat [test]
rem   (no args)  build rei.lib (static) and rei.dll (shared)
rem   test       also build and run the unit tier

setlocal enabledelayedexpansion
cd /d "%~dp0\.."

rem MSVC environment: clang-cl auto-detects the toolchain to compile,
rem but lib.exe/link.exe want vcvars on PATH. vswhere ships with any
rem VS2017+ installer.
for /f "usebackq delims=" %%v in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%v
if not defined VSINSTALL (echo error: Visual Studio C++ tools not found & exit /b 1)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul

rem /W4, not -Wall: clang-cl maps -Wall to /Wall (-Weverything), which
rem misfires -Wpre-c11-compat on C11 constructs even with the standard
rem set; /W4 maps to clang's -Wall -Wextra.
set CFLAGS=-nologo /std:c11 -O2 /W4 -Werror -Iinclude -Isrc -D_CRT_SECURE_NO_WARNINGS
set DLLFLAGS=-nologo /std:c11 -O2 /W4 -Werror -Iinclude -Isrc -DREI_SHARED -DREI_BUILDING -D_CRT_SECURE_NO_WARNINGS
set SOURCES=src\api.c src\bytes.c src\channel.c src\err.c src\err_tls.c src\liveness.c src\parker.c src\pool.c src\preamble.c src\rng_jump.c src\shm.c src\shm_rw.c src\spill.c src\tune.c src\wait_linux.c src\wait_macos.c src\wait_win32.c

rem Every TU is platform-guarded internally; a foreign platform's file
rem compiles empty.

if exist build rmdir /s /q build
mkdir build

for %%f in (%SOURCES%) do (
  clang-cl %CFLAGS% -c %%f -Fobuild\ || exit /b 1
)

lib -nologo -out:build\rei.lib build\*.obj || exit /b 1

rem The DLL needs its own object set: dllexport is a compile-time
rem attribute (REI_API defaults to empty — objects compiled without
rem REI_SHARED carry no exports), and the link's default import library
rem name would clobber the static rei.lib.
mkdir build\dll
for %%f in (%SOURCES%) do (
  clang-cl %DLLFLAGS% -c %%f -Fobuild\dll\ || exit /b 1
)
clang-cl -nologo -LD build\dll\*.obj -Fe:build\rei.dll -link -IMPLIB:build\rei-dll.lib || exit /b 1

if "%~1"=="test" (
  for %%t in (tests\unit\*.c) do (
    clang-cl %CFLAGS% %%t build\rei.lib -Fe:build\%%~nf.exe || exit /b 1
    build\%%~nf.exe || exit /b 1
  )
)

echo build-win: ok
endlocal
