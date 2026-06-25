cmake_minimum_required(VERSION 3.19)
project(MatrixDB CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build configuration to enable the CPU mock engine
option(MATRIX_USE_CPU_MOCK "Compile with CPU-based mock execution engine" ON)

find_package(Threads REQUIRED)

# Detect host architecture and set platform-specific target optimizations
if(APPLE)
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        message(STATUS "MatrixDB Config: Native Apple Silicon target detected.")
        # Apply mcpu optimizations targeting Apple Silicon cores
        add_compile_options("-mcpu=apple-m1" "-O3" "-fomit-frame-pointer" "-flto")
    else()
        message(STATUS "MatrixDB Config: Intel macOS target detected.")
        add_compile_options("-march=native" "-O3" "-flto")
    endif()
else()
    message(STATUS "MatrixDB Config: Standard Linux x86_64 target detected.")
    add_compile_options("-march=native" "-O3" "-pthread" "-fomit-frame-pointer" "-flto")
endif()

# Define the primary executable target (CPU backend; for the CUDA build see README.md)
add_executable(matrixdb_proto
    main.cpp
    types.hpp
    ring_buffer.hpp
    compute.hpp
    compute_mock.cpp
)

target_link_libraries(matrixdb_proto PRIVATE Threads::Threads)

if(MATRIX_USE_CPU_MOCK)
    target_compile_definitions(matrixdb_proto PRIVATE MATRIX_USE_CPU_MOCK=1)
endif()
