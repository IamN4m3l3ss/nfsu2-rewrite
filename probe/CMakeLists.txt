add_library(minhook STATIC
  ${CMAKE_SOURCE_DIR}/third_party/minhook/src/buffer.c
  ${CMAKE_SOURCE_DIR}/third_party/minhook/src/hook.c
  ${CMAKE_SOURCE_DIR}/third_party/minhook/src/trampoline.c
  ${CMAKE_SOURCE_DIR}/third_party/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${CMAKE_SOURCE_DIR}/third_party/minhook/include)

# Stable loader: loaded by the ASI loader, owns D3D hooks + keep_active + the log, (re)loads the core.
add_library(nfsu2_probe SHARED loader/loader.cpp)
target_include_directories(nfsu2_probe PRIVATE common ${CMAKE_SOURCE_DIR}/third_party/json)
target_compile_definitions(nfsu2_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(nfsu2_probe PRIVATE minhook bcrypt)
set_target_properties(nfsu2_probe PROPERTIES
  SUFFIX ".asi"
  RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${OUT_DIR}")

# Hot-reloadable core: server, input, car, schedules, screenshots. Rebuild -> the running game reloads it.
add_library(nfsu2_core SHARED
  core/core_main.cpp
  core/log.cpp
  core/mem.cpp
  core/d3d.cpp
  core/server.cpp
  core/patches.cpp
  core/input.cpp
  core/car.cpp
  core/rewrite.cpp
  core/overlay.cpp)
# Dear ImGui 1.93.0 for the in-game panel (F8), D3D9 + Win32 backends
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})
target_link_libraries(nfsu2_core PRIVATE imgui d3d9 dwmapi)
target_include_directories(nfsu2_core PRIVATE core common ${CMAKE_SOURCE_DIR}/third_party/json)
target_compile_definitions(nfsu2_core PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(nfsu2_core PRIVATE minhook ws2_32 dxguid)
set_target_properties(nfsu2_core PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${OUT_DIR}")
