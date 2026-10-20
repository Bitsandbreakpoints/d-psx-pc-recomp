@echo off
rem Builds the PC port: downloads the tools on first use, then configure + ninja.
rem   build.bat           release build (build\d.exe)
rem   build.bat --debug   debug build
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo Python 3 is needed to build ^(https://www.python.org^).
  exit /b 1
)
if not exist "tools\llvm-mingw\bin" python tools\setup.py || exit /b 1
if not exist build.ninja python configure.py %* || exit /b 1
"tools\ninja\ninja.exe" || exit /b 1
echo.
echo Built build\d.exe
