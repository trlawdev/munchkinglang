@echo off
setlocal

where cl >nul 2>&1
if errorlevel 1 (
  echo Build munxc with MSVC from a Developer Command Prompt, or use CMake:
  echo   cmake -S . -B build
  echo   cmake --build build --config Release
  exit /b 1
)

cl /nologo /std:c++20 /EHsc /O2 /I include src\main.cpp /Fe:munxc.exe ws2_32.lib
exit /b %ERRORLEVEL%
