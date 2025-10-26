cmake_minimum_required(VERSION 3.20)
project(auto_website_c C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

add_executable(github_stats src/github_stats.c)

target_link_libraries(github_stats PRIVATE CURL::libcurl)
