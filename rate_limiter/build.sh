#!/bin/bash
# ============================================================================
# build.sh — Linux/macOS 构建脚本
#
# 用法：
#   ./build.sh              # 默认编译（Release）
#   ./build.sh debug        # Debug 编译
#   ./build.sh run          # 编译并运行
#   ./build.sh test         # 编译并运行单元测试
#   ./build.sh bench        # 编译并运行压测
# ============================================================================

set -e

BUILD_TYPE="${1:-release}"
BUILD_DIR="build"

echo "=== Building Rate Limiter (${BUILD_TYPE}) ==="

# 创建构建目录
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# CMake 配置
if [ "${BUILD_TYPE}" = "debug" ]; then
    cmake .. -DCMAKE_BUILD_TYPE=Debug
else
    cmake .. -DCMAKE_BUILD_TYPE=Release
fi

# 编译
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo ""
echo "=== Build Complete ==="

# 根据参数执行
case "${2}" in
    run)
        echo ""
        echo "=== Running Demo ==="
        ./rate_limiter_demo
        ;;
    test)
        echo ""
        echo "=== Running Unit Tests ==="
        ./unit_test
        ;;
    bench)
        echo ""
        echo "=== Running Benchmark ==="
        ./benchmark
        ;;
esac