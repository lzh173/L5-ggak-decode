cmake_minimum_required(VERSION 3.16)
project(ggak_decode LANGUAGES CXX)

add_executable(ggak_decode ggak_decode.cpp)
target_compile_features(ggak_decode PRIVATE cxx_std_17)

if (MSVC)
    target_compile_options(ggak_decode PRIVATE /O2 /W4 /permissive-)
else()
    target_compile_options(ggak_decode PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
