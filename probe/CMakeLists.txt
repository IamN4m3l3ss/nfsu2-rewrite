add_library(minhook STATIC
  ${CMAKE_SOURCE_DIR}/third_party/minhook/src/buffer.c
  ${CMAKE_SOURCE_DIR}/third_party/minhook/src/hook.c
  ${CMAKE_SOURCE_DIR}/third_party/minhook/src/trampoline.c
  ${CMAKE_SOURCE_DIR}/third_party/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${CMAKE_SOURCE_DIR}/third_party/minhook/include)

add_library(nfsu2_probe SHARED
  src/main.cpp
  src/log.cpp
  src/mem.cpp
  src/d3d.cpp
  src/server.cpp)
target_include_directories(nfsu2_probe PRIVATE src ${CMAKE_SOURCE_DIR}/third_party/json)
target_compile_definitions(nfsu2_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(nfsu2_probe PRIVATE minhook ws2_32 bcrypt)
set_target_properties(nfsu2_probe PROPERTIES
  SUFFIX ".asi"
  RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${OUT_DIR}")
