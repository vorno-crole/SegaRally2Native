@echo off
rem Builds the capture viewer (SegaRally2Viewer.exe, docs\VIEWER.md) with MSVC.
rem It links none of the recompiled code, so it needs no ROM: the same tools as
rem build.bat (run install.bat first if they are missing), and the same build
rem folder, so after build.bat only the viewer's own files are compiled.
setlocal
cd /d "%~dp0"
rem Started with a double click (cmd /c): keep the window open at the end so the
rem message can be read
set "PAUSE_AT_END="
for %%a in (%cmdcmdline%) do if /i "%%~a"=="/c" set PAUSE_AT_END=1
if defined SR2_NO_PAUSE set "PAUSE_AT_END="

rem CMakeLists.txt reads the list of recompiled files: an empty one where
rem build.bat has not run yet (build.bat writes the real one every time)
if not exist build\gen\files.cmake (
    if not exist build\gen mkdir build\gen
    >build\gen\files.cmake echo set^(GEN_SOURCES ""^)
)
rem SDL3 and miniz: the viewer uses neither, but CMake sets the whole project up
call tools\get_deps.bat || goto fail

call tools\find_vs.bat
if errorlevel 2 goto no_sdk
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto no_vs
echo [1/2] Setting up Visual Studio ^("%VSDIR%"^)
rem (the three steps below are build.bat's: see the reasons there)
set "PATH=%PATH:"=%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >"%TEMP%\sr2_vcvars.log" 2>&1 || (
    type "%TEMP%\sr2_vcvars.log"
    echo Visual Studio setup failed. Check that the "Windows 11 SDK" component is installed.
    goto fail
)
chcp 65001 >nul
set "OLD_CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%OLD_CMAKE%" goto no_vs
set "CMAKE="
call :try_cmake "%OLD_CMAKE%"
if defined CMAKE goto cmake_ok
for /f "delims=" %%i in ('where cmake 2^>nul') do if not defined CMAKE call :try_cmake "%%i"
if not defined CMAKE goto old_cmake
echo The CMake of Visual Studio is too old, using "%CMAKE%"
:cmake_ok
echo [2/2] Building
"%CMAKE%" -S . -B build\cmake -G Ninja -DCMAKE_BUILD_TYPE=Release || goto fail
"%CMAKE%" --build build\cmake --target SegaRally2Viewer || goto fail
copy /y build\cmake\SegaRally2Viewer.exe SegaRally2Viewer.exe >nul
echo.
echo Done: SegaRally2Viewer.exe ^(press F9 in the game to save a snapshot, then run it^)
if defined PAUSE_AT_END pause
exit /b 0

:fail
echo.
echo Build failed: see the message above.
if defined PAUSE_AT_END pause
exit /b 1

:try_cmake
rem Sets CMAKE to the cmake.exe in %1 if it is at least the version
rem tools\cmake_version.cmake asks for
"%~1" -P tools\cmake_version.cmake >nul 2>&1
if not errorlevel 1 set "CMAKE=%~1"
exit /b 0

:old_cmake
"%OLD_CMAKE%" -P tools\cmake_version.cmake
echo The CMake of Visual Studio ^("%VSDIR%"^) is too old: this needs CMake 3.27 or newer.
echo Update Visual Studio ^(Visual Studio Installer, Update button^) or run install.bat, which updates it.
goto fail

:no_vs
echo Visual Studio 2022 with the x64 C++ tools and CMake not found.
echo Run install.bat: it installs them. Or install Build Tools for Visual Studio 2022
echo with the "Desktop development with C++" workload yourself.
goto fail
:no_sdk
echo Visual Studio is in "%VSDIR%" but there is no Windows SDK. Run install.bat: it adds
echo it. Or add "Windows 11 SDK" in the Visual Studio Installer ^(Individual components^).
goto fail
