cmake_minimum_required(VERSION 3.20)
project(cue_spikes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

# 默认 -O2 -g（RelWithDebInfo）；压测需要真实优化下的行为
find_package(Threads REQUIRED)
find_package(Boost 1.83 REQUIRED COMPONENTS system)

add_subdirectory(ws-bench)
add_subdirectory(udp-loss)
