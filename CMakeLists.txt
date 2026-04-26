cmake_minimum_required(VERSION 3.20)

project(LocalDocSearch VERSION 1.0.0 LANGUAGES CXX)

add_executable(docsearch
    src/main.cpp
)

target_compile_features(docsearch PRIVATE cxx_std_20)

if (MSVC)
    target_compile_options(docsearch PRIVATE /utf-8 /W4)
else()
    target_compile_options(docsearch PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (WIN32)
    target_link_libraries(docsearch PRIVATE shell32)
endif()
