cmake_minimum_required(VERSION 3.20)

project(
    runtime_monitor
    VERSION 0.1.0
    LANGUAGES C
)

# ---------------------------------------------------------------------------
# C configuration
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
)

# ---------------------------------------------------------------------------
# Threads
# ---------------------------------------------------------------------------

set(THREADS_PREFER_PTHREAD_FLAG TRUE)

find_package(
    Threads
    REQUIRED
)

# ---------------------------------------------------------------------------
# Output directories
# ---------------------------------------------------------------------------

set(
    CMAKE_RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin"
)

set(
    CMAKE_LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/lib"
)

set(
    CMAKE_ARCHIVE_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/lib"
)

# ===========================================================================
# Monitor core
# ===========================================================================

add_library(
    monitor_core
    STATIC

    src/ring_buffer.c
    src/monitor_runtime.c
    src/monitor_verifier.c
)

target_include_directories(
    monitor_core

    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    monitor_core

    PUBLIC
    Threads::Threads
)

set_target_properties(
    monitor_core

    PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# ===========================================================================
# LD_PRELOAD monitor
# ===========================================================================

add_library(
    monitor
    SHARED

    src/interceptor.c
)

target_include_directories(
    monitor

    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    monitor

    PRIVATE
    monitor_core
    ${CMAKE_DL_LIBS}
    Threads::Threads
)

if(UNIX AND NOT APPLE)

    target_link_options(
        monitor

        PRIVATE
        "-Wl,-z,defs"
    )

endif()

set_target_properties(
    monitor

    PROPERTIES
    OUTPUT_NAME "monitor"
)

# ===========================================================================
# Correct workload
# ===========================================================================

add_executable(
    correct_mutex

    targets/correct/correct_mutex.c
)

target_link_libraries(
    correct_mutex

    PRIVATE
    Threads::Threads
)

# ===========================================================================
# Circular deadlock
# ===========================================================================

add_executable(
    circular_deadlock

    targets/circular_deadlock/circular_deadlock.c
)

target_link_libraries(
    circular_deadlock

    PRIVATE
    Threads::Threads
)

# ===========================================================================
# Non-owner unlock
# ===========================================================================

add_executable(
    non_owner_unlock

    targets/non_owner_unlock/non_owner_unlock.c
)

target_link_libraries(
    non_owner_unlock

    PRIVATE
    Threads::Threads
)

# ===========================================================================
# Self-deadlock candidate
# ===========================================================================

add_executable(
    self_deadlock

    targets/self_deadlock/self_deadlock.c
)

target_link_libraries(
    self_deadlock

    PRIVATE
    Threads::Threads
)

# ===========================================================================
# Lock held at thread exit
# ===========================================================================

add_executable(
    held_at_exit

    targets/held_at_exit/held_at_exit.c
)

target_link_libraries(
    held_at_exit

    PRIVATE
    Threads::Threads
)

# ===========================================================================
# Hold-time violation
# ===========================================================================

add_executable(
    hold_time

    targets/hold_time/hold_time.c
)

target_link_libraries(
    hold_time

    PRIVATE
    Threads::Threads
)

# ===========================================================================
# L5 configurable benchmark
# ===========================================================================

add_executable(
    benchmark_mutex

    targets/benchmark_mutex/benchmark_mutex.c
)

target_link_libraries(
    benchmark_mutex

    PRIVATE
    Threads::Threads
)

# ===========================================================================
# Tests
# ===========================================================================

enable_testing()

add_executable(
    test_ring_buffer

    tests/test_ring_buffer.c
)

target_link_libraries(
    test_ring_buffer

    PRIVATE
    monitor_core
    Threads::Threads
)

add_test(
    NAME ring_buffer

    COMMAND test_ring_buffer
)

add_test(
    NAME benchmark_smoke

    COMMAND
        benchmark_mutex
        --threads 2
        --iterations 1000
        --work 0
)