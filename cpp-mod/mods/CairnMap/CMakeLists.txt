cmake_minimum_required(VERSION 3.22)

set(TARGET CairnMap)
project(${TARGET})

add_library(${TARGET} SHARED
    src/dllmain.cpp
)
target_include_directories(${TARGET} PRIVATE src)
target_compile_features(${TARGET} PRIVATE cxx_std_20)
target_link_libraries(${TARGET} PRIVATE UE4SS)
