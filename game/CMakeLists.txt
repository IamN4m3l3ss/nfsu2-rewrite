# nfsu2_game.dll: the rewrite. Loaded by the probe core; rebuilding it hot-reloads it in the running game.
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(nfsu2_game SHARED ${GAME_SOURCES})
target_compile_definitions(nfsu2_game PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(nfsu2_game PRIVATE src ${CMAKE_SOURCE_DIR}/probe/common ${CMAKE_SOURCE_DIR}/third_party/json)
set_target_properties(nfsu2_game PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${OUT_DIR}")
