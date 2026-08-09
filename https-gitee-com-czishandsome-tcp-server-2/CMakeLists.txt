cmake_minimum_required(VERSION 3.14)
project(tcp_server_http VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# === hiredis ===
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)
if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(WARNING "hiredis not found, Redis features disabled - running in degraded mode")
    set(HAS_REDIS FALSE)
else()
    set(HAS_REDIS TRUE)
    add_definitions(-DHAS_REDIS)
endif()

# === 限流风控项目（相对路径：与 tcp_server_http 同级） ===
set(RL_ROOT    "${CMAKE_SOURCE_DIR}/../rate_limiter")
set(RL_INCLUDE "${RL_ROOT}/include")
set(RL_SRC     "${RL_ROOT}/src")

include_directories(${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/http ${RL_INCLUDE})
if(HAS_REDIS)
    include_directories(${HIREDIS_INCLUDE_DIR})
endif()

# === Rate Limiter 源文件 ===
set(RL_SOURCES
    ${RL_SRC}/config_manager/config_manager.cpp
    ${RL_SRC}/redis_client/redis_client.cpp
    ${RL_SRC}/rule_engine/rule_handler.cpp
    ${RL_SRC}/rule_engine/rule_engine.cpp
    ${RL_SRC}/decision_engine/decision_engine.cpp
    ${RL_SRC}/decision_engine/feature_extractor.cpp
    ${RL_SRC}/event_bus/event_producer.cpp
    ${RL_SRC}/event_bus/event_consumer.cpp
    ${RL_SRC}/metrics/metrics.cpp
    ${RL_SRC}/metrics/aggregator.cpp
    ${RL_SRC}/metrics/metrics_exporter.cpp
    ${RL_SRC}/rate_limiter/rate_limiter.cpp
    ${RL_SRC}/local_cache/local_cache.cpp
    ${RL_SRC}/service/rate_limit_service.cpp
    ${RL_SRC}/controller/rate_limit_controller.cpp
)

# === 主程序 ===
add_executable(tcp_server_http src/main.cpp ${RL_SOURCES})

target_compile_options(tcp_server_http PRIVATE -O2 -Wall -Wextra)
target_link_libraries(tcp_server_http pthread)
if(HAS_REDIS)
    target_link_libraries(tcp_server_http ${HIREDIS_LIBRARY})
endif()

message(STATUS "Rate Limiter root: ${RL_ROOT}")
message(STATUS "Redis enabled: ${HAS_REDIS}")
