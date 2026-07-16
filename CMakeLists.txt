cmake_minimum_required(VERSION 3.20)

# Detect the actual macOS SDK path before project() initialises the toolchain,
# so CMake never warns about a stale/missing CMAKE_OSX_SYSROOT value.
if(APPLE)
  execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE _detected_sysroot
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(_detected_sysroot)
    set(CMAKE_OSX_SYSROOT "${_detected_sysroot}" CACHE PATH "macOS SDK path" FORCE)
  endif()
endif()

project(ccm C)

set(CCM_VERSION "0.0.0")

if(${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_BINARY_DIR})
  message(FATAL_ERROR "In-source builds are not allowed. Build from a subdir:

     cmake -S . -B build
     cmake --build build
")
endif()

include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

if(APPLE)
  set(CMAKE_MACOSX_RPATH ON)
endif()

# Same C dialect gtcaca itself compiles with. gnu99 (not strict c99) keeps glibc
# from hiding the POSIX functions this code uses (strdup, usleep, strcasecmp);
# under strict c99 GCC implicitly declares strdup as int-returning, which
# TRUNCATES THE RETURNED POINTER TO 32 BITS on x86-64 and corrupts every string.
if(NOT WIN32)
  add_compile_options(-std=gnu99 -fPIC)
endif()

# Make the two halves of that truncation hard errors — never benign here.
add_compile_options(
  -Werror=implicit-function-declaration
  -Werror=int-conversion
)

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  add_definitions(-DMACOS=1)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  add_definitions(-DLINUX=1)
endif()

# ── dependency: the gtcaca toolkit (found via pkg-config) ────────────────────
# ccm is a consumer of gtcaca like any third party would be: install gtcaca
# first (`make install`, its .pkg, or `brew install gtcaca`), which drops
# gtcaca.pc on the pkg-config path. gtcaca.pc already Requires: caca, so libcaca
# comes in transitively; we pkg-check it too for its include dir and clarity.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTCACA REQUIRED gtcaca)
pkg_check_modules(LIBCACA REQUIRED caca)
message(STATUS "gtcaca:  ${GTCACA_LDFLAGS}")
message(STATUS "libcaca: ${LIBCACA_LDFLAGS}")

add_executable(ccm
  src/cacamacs.c src/games.c
  src/edit.c src/search.c src/keymap.c src/lang.c src/ui.c
  src/spell.c src/theme.c)

target_include_directories(ccm PRIVATE
  src
  ${GTCACA_INCLUDE_DIRS}
  ${LIBCACA_INCLUDE_DIRS})

target_link_directories(ccm PRIVATE
  ${GTCACA_LIBRARY_DIRS}
  ${LIBCACA_LIBRARY_DIRS})

# GTCACA_LDFLAGS already carries -lcaca (gtcaca.pc Requires: caca), so linking
# LIBCACA_LDFLAGS again would only trip a "duplicate library" warning.
target_link_libraries(ccm
  ${GTCACA_LDFLAGS}
  ${CMAKE_DL_LIBS})

# Let the installed binary find libgtcaca.dylib wherever pkg-config located it,
# without relying on a system-wide DYLD path.
if(APPLE)
  set_target_properties(ccm PROPERTIES
    INSTALL_RPATH "${GTCACA_LIBRARY_DIRS};${LIBCACA_LIBRARY_DIRS}")
endif()

install(TARGETS ccm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The colour-theme sample, so a fresh install has something to seed ~/.ccm/theme.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/ccm-theme.example"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/ccm")
