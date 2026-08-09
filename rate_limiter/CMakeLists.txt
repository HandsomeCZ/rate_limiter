cmake_minimum_required(VERSION 3.14)
project(rate_limiter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- hiredis (必须) ----
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)
if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "hiredis not found. Install: apt install libhiredis-dev / brew install hiredis")
endif()

# ---- 头文件路径 ----
set(INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
include_directories(${INCLUDE_DIR} ${HIREDIS_INCLUDE_DIR})

# ---- 源文件（按模块组织） ----
set(SOURCES
    # config_manager
    src/config_manager/config_manager.cpp

    # redis_client
    src/redis_client/redis_client.cpp

    # rule_engine
    src/rule_engine/rule_handler.cpp
    src/rule_engine/rule_engine.cpp

    # decision_engine
    src/decision_engine/decision_engine.cpp
    src/decision_engine/feature_extractor.cpp

    # event_bus (NEW)
    src/event_bus/event_producer.cpp
    src/event_bus/event_consumer.cpp

    # metrics (NEW)
    src/metrics/metrics.cpp
    src/metrics/aggregator.cpp
    src/metrics/metrics_exporter.cpp

    # rate_limiter
    src/rate_limiter/rate_limiter.cpp

    # local_cache
    src/local_cache/local_cache.cpp

    # service
    src/service/rate_limit_service.cpp

    # controller
    src/controller/rate_limit_controller.cpp
)

# ---- 主程序（非必须，用于快速验证） ----
add_executable(rate_limiter_demo
    ${SOURCES}
    src/main.cpp
)
target_link_libraries(rate_limiter_demo ${HIREDIS_LIBRARY} pthread)

# ---- 压测程序 ----
add_executable(benchmark
    ${SOURCES}
    test/benchmark.cpp
)
target_link_libraries(benchmark ${HIREDIS_LIBRARY} pthread)

# ---- 单元测试 ----
add_executable(unit_test
    ${SOURCES}
    test/unit_test.cpp
)
target_link_libraries(unit_test ${HIREDIS_LIBRARY} pthread)

# ---- 编译选项 ----
target_compile_options(rate_limiter_demo PRIVATE -O2 -Wall -Wextra)
target_compile_options(benchmark PRIVATE -O2 -Wall -Wextra)
target_compile_options(unit_test PRIVATE -O0 -g -Wall -Wextra)
