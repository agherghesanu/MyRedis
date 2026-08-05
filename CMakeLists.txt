cmake_minimum_required(VERSION 3.15)
project(BuildYourOwnRedis CXX)

# Set C++ standard to C++17 or C++20
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add Server Executable
add_executable(redis_server src/server.cpp
    "src/hashtable.h" "src/hashtable.cpp"
    "src/avl.h" "src/avl.cpp"
    "src/zset.h" "src/zset.cpp"
    "src/dlist.h"
    "src/heap.h" "src/heap.cpp"
    "src/thread_pool.h" "src/thread_pool.cpp")

# the thread pool needs pthreads
find_package(Threads REQUIRED)
target_link_libraries(redis_server PRIVATE Threads::Threads)

# concurrent load generator many client threads hammering one server
add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE Threads::Threads)

# Add Client Executable
add_executable(redis_client src/client.cpp)

# unit tests live in tests and include headers from src
enable_testing()

# avl tree unit test
add_executable(test_avl tests/test_avl.cpp "src/avl.h" "src/avl.cpp")
target_include_directories(test_avl PRIVATE src)
add_test(NAME avl COMMAND test_avl)

# hashtable unit test covering insert lookup delete foreach across resizes
add_executable(test_hashtable tests/test_hashtable.cpp "src/hashtable.h" "src/hashtable.cpp")
target_include_directories(test_hashtable PRIVATE src)
add_test(NAME hashtable COMMAND test_hashtable)

# sorted set unit test covering add update lookup delete seek and rank
add_executable(test_zset tests/test_zset.cpp
    "src/zset.h" "src/zset.cpp" "src/avl.cpp" "src/hashtable.cpp")
target_include_directories(test_zset PRIVATE src)
add_test(NAME zset COMMAND test_zset)

# min heap unit test covering push update pop and the tracked ref index
add_executable(test_heap tests/test_heap.cpp "src/heap.h" "src/heap.cpp")
target_include_directories(test_heap PRIVATE src)
add_test(NAME heap COMMAND test_heap)

# thread pool unit test many tasks must all run exactly once
add_executable(test_thread_pool tests/test_thread_pool.cpp
    "src/thread_pool.h" "src/thread_pool.cpp")
target_include_directories(test_thread_pool PRIVATE src)
target_link_libraries(test_thread_pool PRIVATE Threads::Threads)
add_test(NAME thread_pool COMMAND test_thread_pool)

# server and client end to end test drives the real binaries over the socket
add_test(
    NAME integration
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_integration.sh
            $<TARGET_FILE:redis_server> $<TARGET_FILE:redis_client>
)

# idle connection timeout test uses a short timeout via an env var
add_test(
    NAME timer
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_timer.sh
            $<TARGET_FILE:redis_server>
)

# async big value deletion test exercises the thread pool path end to end
add_test(
    NAME bigdel
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bigdel.sh
            $<TARGET_FILE:redis_server>
)
