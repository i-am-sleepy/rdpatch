cmake_minimum_required(VERSION 3.22)
project(vkshim C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# everything (shim + adrenotools hook libs) lands in one folder
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
add_subdirectory(libadrenotools)   # cloned by the workflow
add_library(vkshim SHARED src/shim.cpp src/tramp.S)
target_compile_options(vkshim PRIVATE -fvisibility=hidden -O2 -g0 -fno-omit-frame-pointer)
target_link_libraries(vkshim PRIVATE adrenotools log dl)
target_link_options(vkshim PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)
