cmake_minimum_required(VERSION 3.20)
project(cue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
find_package(Boost 1.83 REQUIRED COMPONENTS system)

# ---- proto：纯头文件，编解码规则集中在一处 ----
add_library(cue_proto INTERFACE)
target_include_directories(cue_proto INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- cued：媒体管线（WS 接收 → 重组 → UDP 发送）----
add_executable(cued apps/cued.cpp)
target_link_libraries(cued PRIVATE cue_proto Boost::system Threads::Threads)
target_include_directories(cued PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- cue-probe：下行接收端（UDP 重组 + 指标）----
add_executable(cue-probe apps/cue-probe.cpp)
target_link_libraries(cue-probe PRIVATE cue_proto Threads::Threads)
target_include_directories(cue-probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- 静态资源目录（供本地 http server 使用）----
add_custom_target(web ALL SOURCES web/index.html web/app.js)

# ---- 测试 ----
enable_testing()
add_executable(test_wire tests/test_wire.cpp)
target_link_libraries(test_wire PRIVATE cue_proto)
add_test(NAME wire COMMAND test_wire)
