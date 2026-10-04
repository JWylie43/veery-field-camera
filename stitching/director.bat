@echo off
REM director.bat - double-click launcher for the Director (virtual-camera editor) on Windows.
REM
REM Builds the binary on first run, then launches it; the editor opens in the browser.
REM
REM Requires (one-time): Visual Studio 2022 (C++ workload), CMake, OpenCV, and
REM optionally FFmpeg on PATH. See the README "Prerequisites" section.

cd /d "%~dp0"

if not exist "build\Release\Director.exe" (
  echo Building Director ^(first run; this may take a minute^)...
  cmake -S . -B build
  cmake --build build --config Release --target Director
)

if not exist "build\Release\Director.exe" (
  echo.
  echo Build did not produce build\Release\Director.exe - see messages above.
  echo A common cause is antivirus blocking the linker; add an exception for this folder.
  pause
  goto :eof
)

"build\Release\Director.exe"

echo.
echo Program exited. Press any key to close this window.
pause >nul
