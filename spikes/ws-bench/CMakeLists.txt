add_executable(ws_bench_server server.cpp)
target_include_directories(ws_bench_server PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(ws_bench_server PRIVATE Threads::Threads Boost::system)

add_executable(ws_bench_client client.cpp)
target_include_directories(ws_bench_client PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(ws_bench_client PRIVATE Threads::Threads Boost::system)
