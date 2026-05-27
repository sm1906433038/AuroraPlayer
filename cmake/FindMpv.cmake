# FindMpv.cmake
# -------------
# Locates libmpv (the client library, mpv/client.h + mpv/render*.h).
#
# Resolution order:
#   1. MPV_ROOT cache/env var (recommended for a manual SDK drop).
#   2. <project>/third_party/mpv  (auto-detected).
#   3. pkg-config (Linux / MSYS2 / vcpkg).
#
# Produces an IMPORTED target: Mpv::Mpv

include(FindPackageHandleStandardArgs)

set(_mpv_search_roots "")
if(DEFINED MPV_ROOT)
    list(APPEND _mpv_search_roots "${MPV_ROOT}")
endif()
if(DEFINED ENV{MPV_ROOT})
    list(APPEND _mpv_search_roots "$ENV{MPV_ROOT}")
endif()
list(APPEND _mpv_search_roots "${CMAKE_SOURCE_DIR}/third_party/mpv")

find_path(Mpv_INCLUDE_DIR
    NAMES mpv/client.h
    HINTS ${_mpv_search_roots}
    PATH_SUFFIXES include
)

find_library(Mpv_LIBRARY
    NAMES mpv libmpv libmpv-2 mpv-2
    HINTS ${_mpv_search_roots}
    PATH_SUFFIXES lib lib64
)

# Try pkg-config as a fallback (Linux, MSYS2)
if(NOT Mpv_INCLUDE_DIR OR NOT Mpv_LIBRARY)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(_pc_mpv QUIET mpv)
        if(_pc_mpv_FOUND)
            if(NOT Mpv_INCLUDE_DIR)
                set(Mpv_INCLUDE_DIR "${_pc_mpv_INCLUDE_DIRS}")
            endif()
            if(NOT Mpv_LIBRARY)
                find_library(Mpv_LIBRARY NAMES ${_pc_mpv_LIBRARIES} HINTS ${_pc_mpv_LIBRARY_DIRS})
            endif()
        endif()
    endif()
endif()

# On Windows we also want to track the actual runtime DLL so we can copy it.
set(Mpv_RUNTIME_DLL "")
if(WIN32 AND Mpv_LIBRARY)
    get_filename_component(_mpv_lib_dir "${Mpv_LIBRARY}" DIRECTORY)
    foreach(_candidate
            "${_mpv_lib_dir}/libmpv-2.dll"
            "${_mpv_lib_dir}/mpv-2.dll"
            "${_mpv_lib_dir}/mpv-1.dll"
            "${_mpv_lib_dir}/../bin/libmpv-2.dll"
            "${_mpv_lib_dir}/../bin/mpv-2.dll")
        if(EXISTS "${_candidate}")
            set(Mpv_RUNTIME_DLL "${_candidate}")
            break()
        endif()
    endforeach()
endif()

find_package_handle_standard_args(Mpv
    REQUIRED_VARS Mpv_LIBRARY Mpv_INCLUDE_DIR
)

if(Mpv_FOUND AND NOT TARGET Mpv::Mpv)
    if(WIN32 AND Mpv_RUNTIME_DLL)
        add_library(Mpv::Mpv SHARED IMPORTED)
        set_target_properties(Mpv::Mpv PROPERTIES
            IMPORTED_LOCATION "${Mpv_RUNTIME_DLL}"
            IMPORTED_IMPLIB   "${Mpv_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Mpv_INCLUDE_DIR}"
        )
    else()
        add_library(Mpv::Mpv UNKNOWN IMPORTED)
        set_target_properties(Mpv::Mpv PROPERTIES
            IMPORTED_LOCATION "${Mpv_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Mpv_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(Mpv_INCLUDE_DIR Mpv_LIBRARY Mpv_RUNTIME_DLL)
