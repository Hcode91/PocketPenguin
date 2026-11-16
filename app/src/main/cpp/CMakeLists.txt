cmake_minimum_required(VERSION 3.18)
project(pocketpenguin CXX)
set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/src)
add_library(pocketpenguin SHARED
    jni_bridge.cpp
    ${CORE}/arm926.cpp
    ${CORE}/linux.cpp
    ${CORE}/gp2x.cpp
    ${CORE}/fpa.cpp
    ${CORE}/glhle.cpp
    ${CORE}/gles.cpp)
target_include_directories(pocketpenguin PRIVATE ${CORE})
target_compile_options(pocketpenguin PRIVATE -O3 -fexceptions -Wall -Wno-misleading-indentation)
target_link_libraries(pocketpenguin log)
