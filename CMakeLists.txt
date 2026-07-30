cmake_minimum_required(VERSION 3.15)
project(BuildYourOwnRedis CXX)

# Set C++ standard to C++17 or C++20
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add Server Executable
add_executable(redis_server src/server.cpp
    "src/hashtable.h" "src/hashtable.cpp"
    "src/avl.h" "src/avl.cpp"
    "src/zset.h" "src/zset.cpp")

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

# server and client end to end test drives the real binaries over the socket
add_test(
    NAME integration
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_integration.sh
            $<TARGET_FILE:redis_server> $<TARGET_FILE:redis_client>
)
