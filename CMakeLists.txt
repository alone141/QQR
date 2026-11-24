cmake_minimum_required(VERSION 3.20)
project(qqr LANGUAGES CXX)

# Airgapped build: every dependency is vendored under third_party.
#   googletest 1.18.0 (third_party/googletest), tests only

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(src)
add_subdirectory(app)

enable_testing()
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
add_subdirectory(tests)
