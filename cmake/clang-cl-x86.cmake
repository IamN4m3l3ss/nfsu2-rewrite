# Toolchain: clang-cl targeting 32-bit Windows (MSVC ABI), with the MSVC CRT + Windows SDK from xwin.
#   xwin --accept-license --arch x86 splat --output ~/.local/share/xwin
#   cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-x86.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET i686-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET i686-pc-windows-msvc)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)

if(NOT XWIN_DIR)
  if(DEFINED ENV{XWIN_DIR})
    set(XWIN_DIR "$ENV{XWIN_DIR}")
  else()
    set(XWIN_DIR "$ENV{HOME}/.local/share/xwin")
  endif()
endif()
if(NOT EXISTS "${XWIN_DIR}/crt/include")
  message(FATAL_ERROR "xwin splat not found at ${XWIN_DIR}")
endif()
# make XWIN_DIR visible in try_compile projects
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)

set(_inc "/imsvc ${XWIN_DIR}/crt/include /imsvc ${XWIN_DIR}/sdk/include/ucrt /imsvc ${XWIN_DIR}/sdk/include/um /imsvc ${XWIN_DIR}/sdk/include/shared")
set(CMAKE_C_FLAGS_INIT "${_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_inc}")

set(_lib "/libpath:${XWIN_DIR}/crt/lib/x86 /libpath:${XWIN_DIR}/sdk/lib/um/x86 /libpath:${XWIN_DIR}/sdk/lib/ucrt/x86")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}")

# static release CRT in every config: nothing extra to install in the Wine prefix, and xwin's splat
# has no debug CRT (libcmtd) unless --include-debug-libs
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
