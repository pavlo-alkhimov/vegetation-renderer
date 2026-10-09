@echo off
rem Unity builds with clang on Windows (docs/01). Untested so far; mirrors build.sh.
rem   build.bat [debug|release]
rem Needs: clang (LLVM), VULKAN_SDK (provides headers, vulkan-1.lib, slangc), SDL3_DIR (SDL3 VC dev package).
setlocal
cd /d "%~dp0"
set MODE=%1
if "%MODE%"=="" set MODE=debug
set COMMON=-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function -D_CRT_SECURE_NO_WARNINGS
if "%MODE%"=="debug" (set OPT=-g -O0 -DVR_DEBUG) else (set OPT=-g -O2)
if "%VULKAN_SDK%"=="" (echo VULKAN_SDK not set & exit /b 1)
if "%SDL3_DIR%"=="" (echo SDL3_DIR not set ^(SDL3-devel-*-VC.zip unpacked^) & exit /b 1)
if not exist build\shaders mkdir build\shaders
"%VULKAN_SDK%\Bin\slangc.exe" shaders\terrain.slang -target spirv -o build\shaders\terrain.spv || exit /b 1
clang++ %COMMON% %OPT% -I"%VULKAN_SDK%\Include" -I"%SDL3_DIR%\include" src\vr.cpp -o build\vr.exe ^
    -L"%VULKAN_SDK%\Lib" -L"%SDL3_DIR%\lib\x64" -lvulkan-1 -lSDL3 -Xlinker /subsystem:console || exit /b 1
copy /y "%SDL3_DIR%\lib\x64\SDL3.dll" build\ >nul
clang++ %COMMON% -g -O2 src\tools\cook_terrain.cpp -o build\cook_terrain.exe || exit /b 1
echo built (%MODE%)
