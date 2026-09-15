cmake_minimum_required(VERSION 3.14)
project(rate_limiter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 平台检测 ----
if(WIN32)
    message(STATUS "Building for Windows")
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
elseif(UNIX AND NOT APPLE)
    message(STATUS "Building for Linux")
    add_definitions(-DLINUX)
elseif(APPLE)
    message(STATUS "Building for macOS")
    add_definitions(-DMACOS)
endif()

# ---- hiredis (必须) ----
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)
if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "hiredis not found. Install: apt install libhiredis-dev / brew install hiredis / vcpkg install hiredis")
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

    # event_bus
    src/event_bus/event_producer.cpp
    src/event_bus/event_consumer.cpp

    # metrics
    src/metrics/metrics.cpp
    src/metrics/aggregator.cpp
    src/metrics/metrics_exporter.cpp

    # rate_limiter
    src/rate_limiter/rate_limiter.cpp
    src/rate_limiter/local_rate_limiter.cpp

    # local_cache
    src/local_cache/local_cache.cpp

    # service
    src/service/rate_limit_service.cpp

    # controller
    src/controller/rate_limit_controller.cpp
)

# ---- 平台特定库 ----
if(WIN32)
    set(PLATFORM_LIBS ws2_32 wsock32)
else()
    set(PLATFORM_LIBS pthread)
endif()

# ---- 主程序（非必须，用于快速验证） ----
add_executable(rate_limiter_demo
    ${SOURCES}
    src/main.cpp
)
target_link_libraries(rate_limiter_demo ${HIREDIS_LIBRARY} ${PLATFORM_LIBS})

# ---- 压测程序（Redis 版） ----
add_executable(benchmark
    ${SOURCES}
    test/benchmark.cpp
)
target_link_libraries(benchmark ${HIREDIS_LIBRARY} ${PLATFORM_LIBS})

# ---- 压测程序（本地版，无 Redis 依赖） ----
add_executable(benchmark_local
    ${SOURCES}
    test/benchmark_local.cpp
)
target_link_libraries(benchmark_local ${HIREDIS_LIBRARY} ${PLATFORM_LIBS})

# ---- 单元测试 ----
add_executable(unit_test
    ${SOURCES}
    test/unit_test.cpp
)
target_link_libraries(unit_test ${HIREDIS_LIBRARY} ${PLATFORM_LIBS})

# ---- 编译选项 ----
if(MSVC)
    target_compile_options(rate_limiter_demo PRIVATE /O2 /W3)
    target_compile_options(benchmark PRIVATE /O2 /W3)
    target_compile_options(unit_test PRIVATE /Od /Zi /W3)
else()
    target_compile_options(rate_limiter_demo PRIVATE -O2 -Wall -Wextra)
    target_compile_options(benchmark PRIVATE -O2 -Wall -Wextra)
    target_compile_options(unit_test PRIVATE -O0 -g -Wall -Wextra)
endif()