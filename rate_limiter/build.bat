@echo off
REM ============================================================================
REM build.bat — Windows 构建脚本
REM
REM 用法：
REM   build.bat              REM 默认编译（Release）
REM   build.bat debug        REM Debug 编译
REM   build.bat run          REM 编译并运行
REM   build.bat test         REM 编译并运行单元测试
REM   build.bat bench        REM 编译并运行压测
REM ============================================================================

setlocal

set BUILD_TYPE=%1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=release
set BUILD_DIR=build

echo === Building Rate Limiter (%BUILD_TYPE%) ===

REM 创建构建目录
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd "%BUILD_DIR%"

REM CMake 配置（使用 Visual Studio 生成器）
cmake .. -G "Visual Studio 17 2022" -A x64

REM 编译
cmake --build . --config Release

echo.
echo === Build Complete ===

REM 根据参数执行
if "%2"=="run" (
    echo.
    echo === Running Demo ===
    .\Release\rate_limiter_demo.exe
) else if "%2"=="test" (
    echo.
    echo === Running Unit Tests ===
    .\Release\unit_test.exe
) else if "%2"=="bench" (
    echo.
    echo === Running Benchmark ===
    .\Release\benchmark.exe
)

endlocal