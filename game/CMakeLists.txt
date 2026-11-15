# nfsu2_game.dll: the rewrite. Loaded by the probe core; rebuilding it hot-reloads it in the running game.
#   framework/  rewrite registration, register-preserving stubs, diff harness, x87-lift helpers
#   systems/    rewritten game code, one folder per system (each function next to its diff test)
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS framework/*.cpp systems/*.cpp)
add_library(nfsu2_game SHARED ${GAME_SOURCES})
target_compile_definitions(nfsu2_game PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(nfsu2_game PRIVATE framework ${CMAKE_SOURCE_DIR}/probe/common ${CMAKE_SOURCE_DIR}/third_party/json)
set_target_properties(nfsu2_game PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${OUT_DIR}")

# nfsu2-gcpc (CC0) rewritten code, vendored in third_party/nfsu2-gcpc: built as its own library because it needs
# C++17 (MSVC's C++20 <cmath> breaks with an 80-bit long double) and an 80-bit long double (their maths mirrors x87
# extended precision). Registered at run time by game/systems/frontend/gcpc_hooks.cpp.
set(GCPC ${CMAKE_SOURCE_DIR}/third_party/nfsu2-gcpc)
file(GLOB_RECURSE GCPC_SOURCES CONFIGURE_DEPENDS ${GCPC}/game/*.cpp)
add_library(nfsu2_gcpc STATIC ${GCPC_SOURCES} ${GCPC}/bridge_win32.cpp ${GCPC}/replacements.cpp)
set_target_properties(nfsu2_gcpc PROPERTIES CXX_STANDARD 17)
target_include_directories(nfsu2_gcpc PRIVATE ${GCPC}/game ${GCPC})
target_compile_definitions(nfsu2_gcpc PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(nfsu2_gcpc PRIVATE -Wno-everything /clang:-mlong-double-80 /clang:-fno-math-errno)
target_link_libraries(nfsu2_game PRIVATE nfsu2_gcpc)
