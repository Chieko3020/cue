add_executable(udp_sender sender.cpp)
target_compile_options(udp_sender PRIVATE -Wall -Wextra)
target_link_libraries(udp_sender PRIVATE Threads::Threads)

add_executable(udp_receiver receiver.cpp)
target_compile_options(udp_receiver PRIVATE -Wall -Wextra)
target_link_libraries(udp_receiver PRIVATE Threads::Threads)
