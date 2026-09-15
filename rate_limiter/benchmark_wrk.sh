#!/bin/bash
# ============================================================================
# benchmark_wrk.sh — wrk 压测脚本
#
# 用法：
#   ./benchmark_wrk.sh              # 默认压测（12线程，400连接，30秒）
#   ./benchmark_wrk.sh 8 200 60     # 自定义：8线程，200连接，60秒
# ============================================================================

THREADS=${1:-12}
CONNECTIONS=${2:-400}
DURATION=${3:-30}
URL=${4:-"http://localhost:8080/api/test"}

echo "=== wrk Benchmark ==="
echo "Threads:     ${THREADS}"
echo "Connections: ${CONNECTIONS}"
echo "Duration:    ${DURATION}s"
echo "URL:         ${URL}"
echo ""

wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s ${URL}

echo ""
echo "=== Benchmark Complete ==="