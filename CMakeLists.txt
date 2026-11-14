cmake_minimum_required(VERSION 3.25)
project(nfsu2_rewrite LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "32-bit build required (use -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-x86.cmake)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OUT_DIR "${CMAKE_SOURCE_DIR}/out")

add_compile_options(/EHsc /Z7 /W3 -Wno-unused-command-line-argument)
add_link_options(/DEBUG:FULL /MANIFEST:NO)

add_subdirectory(probe)
add_subdirectory(game)
add_subdirectory(tests/abi)
