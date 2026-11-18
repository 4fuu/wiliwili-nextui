# FindSDL2.cmake — locate SDL2 for the tg5040 cross build.
#
# borealis calls find_package(SDL2 REQUIRED) when USE_SYSTEM_SDL2 is on. CMake
# has no built-in SDL2 finder, and the vendor SDK sysroot ships sdl2.pc rather
# than an SDL2 config package, so resolve SDL2 through pkg-config first and
# fall back to a plain header/library search.
#
# Defines SDL2_FOUND, SDL2_INCLUDE_DIRS, SDL2_LIBRARIES and SDL2_VERSION.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_SDL2 QUIET sdl2)
endif()

find_path(SDL2_INCLUDE_DIR
    NAMES SDL.h
    HINTS ${PC_SDL2_INCLUDE_DIRS}
    PATH_SUFFIXES SDL2)

find_library(SDL2_LIBRARY
    NAMES SDL2
    HINTS ${PC_SDL2_LIBRARY_DIRS})

set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
set(SDL2_LIBRARIES ${SDL2_LIBRARY})
if(PC_SDL2_VERSION)
    set(SDL2_VERSION ${PC_SDL2_VERSION})
elseif(EXISTS "${SDL2_INCLUDE_DIR}/SDL_version.h")
    file(STRINGS "${SDL2_INCLUDE_DIR}/SDL_version.h" _sdl2_version_lines
        REGEX "#define SDL_(MAJOR|MINOR|PATCH)NUM")
    string(REGEX REPLACE ".*SDL_MAJOR_NUM[ \t]+([0-9]+).*" "\\1" _sdl2_major "${_sdl2_version_lines}")
    string(REGEX REPLACE ".*SDL_MINOR_NUM[ \t]+([0-9]+).*" "\\1" _sdl2_minor "${_sdl2_version_lines}")
    string(REGEX REPLACE ".*SDL_PATCH_NUM[ \t]+([0-9]+).*" "\\1" _sdl2_patch "${_sdl2_version_lines}")
    set(SDL2_VERSION "${_sdl2_major}.${_sdl2_minor}.${_sdl2_patch}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SDL2
    REQUIRED_VARS SDL2_LIBRARY SDL2_INCLUDE_DIR
    VERSION_VAR SDL2_VERSION)

mark_as_advanced(SDL2_INCLUDE_DIR SDL2_LIBRARY)
