cmake_minimum_required(VERSION 3.15)
project(BuildYourOwnRedis CXX)

# Set C++ standard to C++17 or C++20
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add Server Executable
add_executable(redis_server src/server.cpp src/conn.h "src/hashtable.h" "src/hashtable.cpp")

# Add Client Executable
add_executable(redis_client src/client.cpp src/conn.h "src/hashtable.h" "src/hashtable.cpp")