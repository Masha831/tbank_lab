cmake_minimum_required(VERSION 3.16)
project(graphrank CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

add_compile_options(-Wall -Wextra)

add_executable(graphrank
    src/main.cpp
    src/csv_import.cpp
    src/grid_build.cpp
    src/leaderrank.cpp)

find_package(Threads REQUIRED)
target_link_libraries(graphrank PRIVATE Threads::Threads)
