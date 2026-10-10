@echo off
rem Unity builds with clang on Windows (docs/01); mirrors build.sh.
rem   build.bat [debug|release]
rem Needs: clang (LLVM) + Visual Studio C++ tools (headers, linker), VULKAN_SDK (headers, slangc; the Vulkan loader
rem comes with the GPU driver and is opened at runtime by SDL), SDL3_DIR (SDL3-devel-*-VC.zip unpacked).
setlocal
cd /d "%~dp0"
set MODE=%1
if "%MODE%"=="" set MODE=debug
set COMMON=-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function -D_CRT_SECURE_NO_WARNINGS
if "%MODE%"=="debug" (set OPT=-g -O0 -DVR_DEBUG) else (set OPT=-g -O2)
if "%VULKAN_SDK%"=="" (echo VULKAN_SDK not set & exit /b 1)
if "%SDL3_DIR%"=="" (echo SDL3_DIR not set ^(SDL3-devel-*-VC.zip unpacked^) & exit /b 1)
if not exist build\shaders mkdir build\shaders
set SLANGC="%VULKAN_SDK%\Bin\slangc.exe"
%SLANGC% shaders\terrain.slang -target spirv -o build\shaders\terrain.spv || exit /b 1
%SLANGC% shaders\overlay.slang -target spirv -o build\shaders\overlay.spv || exit /b 1
%SLANGC% shaders\taa.slang -target spirv -o build\shaders\taa.spv || exit /b 1
rem Task shaders in their own modules (driver issue with task + mesh in one module, see vk.cpp PipelineDesc).
for %%v in (trees) do (
    %SLANGC% shaders\vegetation.slang -target spirv -fvk-use-entrypoint-name -entry as_%%v -stage amplification ^
        -o build\shaders\veg_%%v_task.spv || exit /b 1
    %SLANGC% shaders\vegetation.slang -target spirv -fvk-use-entrypoint-name -entry ms_%%v -stage mesh ^
        -entry fs_%%v -stage fragment -entry fs_%%v_shadow -stage fragment -o build\shaders\veg_%%v.spv || exit /b 1
)
%SLANGC% shaders\ground_cover.slang -target spirv -fvk-use-entrypoint-name -entry as_gc -stage amplification ^
    -o build\shaders\veg_gc_task.spv || exit /b 1
%SLANGC% shaders\ground_cover.slang -target spirv -fvk-use-entrypoint-name -entry ms_gc -stage mesh ^
    -entry fs_gc -stage fragment -entry fs_gc_shadow -stage fragment -o build\shaders\veg_gc.spv || exit /b 1
clang++ %COMMON% %OPT% -I"%VULKAN_SDK%\Include" -I"%SDL3_DIR%\include" src\vr.cpp -o build\vr.exe ^
    -L"%SDL3_DIR%\lib\x64" -lSDL3 -Xlinker /subsystem:console || exit /b 1
copy /y "%SDL3_DIR%\lib\x64\SDL3.dll" build\ >nul
clang++ %COMMON% -g -O2 src\tools\cook_terrain.cpp -o build\cook_terrain.exe || exit /b 1
clang++ %COMMON% -g -O2 src\tools\cook_ground_cover.cpp -o build\cook_ground_cover.exe || exit /b 1
echo built (%MODE%)
