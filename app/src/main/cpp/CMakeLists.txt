cmake_minimum_required(VERSION 3.18)
project(gp2xdroid CXX)
set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/src)
add_library(gp2xdroid SHARED
    jni_bridge.cpp
    ${CORE}/arm926.cpp
    ${CORE}/linux.cpp
    ${CORE}/gp2x.cpp
    ${CORE}/fpa.cpp)
target_include_directories(gp2xdroid PRIVATE ${CORE})
target_compile_options(gp2xdroid PRIVATE -O3 -fexceptions -Wall -Wno-misleading-indentation)
target_link_libraries(gp2xdroid log)
