cmake_minimum_required(VERSION 3.21)

project(AuroraPlayer
    VERSION 0.1.0
    DESCRIPTION "AuroraPlayer (晨曦影音) — a pro-grade media player built on libmpv + Qt"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
    # Gate these on C/CXX only: nvcc otherwise sees /W4, /permissive-, etc.
    # as positional input-file args and fails with "A single input file is
    # required for a non-link phase when an outputfile is specified".
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:/W4>
        $<$<COMPILE_LANGUAGE:C,CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:C,CXX>:/Zc:__cplusplus>
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:C,CXX>:/EHsc>
    )
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-Wall -Wextra -Wpedantic>)
endif()

# Always emit a compile_commands.json for IDEs / clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Qt6 — install via online installer or vcpkg
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network)
qt_standard_project_setup()

# libmpv — provide headers in third_party/mpv/include and libmpv-2.lib + libmpv-2.dll
#          OR install via vcpkg (`vcpkg install mpv`) and let the toolchain handle it.
find_package(Mpv REQUIRED)

# ---------------------------------------------------------------------------
# whisper.cpp — AI subtitle generation (offline). Pulled via FetchContent so
# users don't have to run `git submodule update` manually. The library is
# small (~500 KB build); the heavyweight piece is the model file (~1 GB).
#
# CUDA is auto-detected. On systems with a working CUDA toolkit + nvcc on
# PATH we enable GGML_CUDA for ~20× speedup on RTX-class cards. Otherwise
# whisper.cpp falls back to CPU (AVX2 / AVX-512) seamlessly.
# ---------------------------------------------------------------------------
include(FetchContent)

# Probe for CUDA without making it a hard error.
# Visual Studio's dev-shell wrapper strips %CUDA_PATH%\bin from PATH, so
# CMake's auto-probe fails even when CUDA Toolkit is installed. We fall
# back to the well-known CUDA_PATH env var the installer sets up.
#
# Escape hatch: set AURORAPLAYER_DISABLE_CUDA=1 in the environment, or pass
# -DAURORAPLAYER_DISABLE_CUDA=ON to cmake, to skip CUDA entirely (useful when
# nvcc's host_config refuses your MSVC version, or cudafe++ segfaults on
# a too-new MSVC STL).
include(CheckLanguage)
option(AURORAPLAYER_DISABLE_CUDA "Force CPU-only whisper, ignore any CUDA install" OFF)
if(DEFINED ENV{AURORAPLAYER_DISABLE_CUDA} AND NOT "$ENV{AURORAPLAYER_DISABLE_CUDA}" STREQUAL "0")
    set(AURORAPLAYER_DISABLE_CUDA ON CACHE BOOL "" FORCE)
endif()
if(NOT AURORAPLAYER_DISABLE_CUDA
        AND NOT DEFINED CMAKE_CUDA_COMPILER
        AND DEFINED ENV{CUDA_PATH})
    set(_cuda_nvcc "$ENV{CUDA_PATH}/bin/nvcc.exe")
    if(EXISTS "${_cuda_nvcc}")
        set(CMAKE_CUDA_COMPILER "${_cuda_nvcc}" CACHE FILEPATH "Path to nvcc" FORCE)
        message(STATUS "Found nvcc via CUDA_PATH: ${_cuda_nvcc}")
    endif()
endif()
if(AURORAPLAYER_DISABLE_CUDA)
    message(STATUS "AURORAPLAYER_DISABLE_CUDA=ON — whisper.cpp will run on CPU only")
    unset(CMAKE_CUDA_COMPILER CACHE)
endif()
if(NOT AURORAPLAYER_DISABLE_CUDA)
        check_language(CUDA)
endif()
set(AURORAPLAYER_WHISPER_CUDA OFF)
if(CMAKE_CUDA_COMPILER AND NOT AURORAPLAYER_DISABLE_CUDA)
    # nvcc's host_config.h refuses any MSVC newer than what CUDA shipped with
    # (e.g. CUDA 12.8 vs MSVC 14.51 = error C1189). In practice the runtime
    # works fine — override the version check rather than downgrading MSVC.
    set(CMAKE_CUDA_FLAGS_INIT "-allow-unsupported-compiler" CACHE STRING "")
    if(NOT CMAKE_CUDA_FLAGS MATCHES "allow-unsupported-compiler")
        set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -allow-unsupported-compiler"
            CACHE STRING "" FORCE)
    endif()
    # Target the user's GPU architecture BEFORE enable_language — CMake
    # latches CMAKE_CUDA_ARCHITECTURES at language-enable time. "native"
    # auto-detects from the host card; explicitly set 89 for RTX 4080 if
    # the runtime detection misfires.
    set(CMAKE_CUDA_ARCHITECTURES "89" CACHE STRING "CUDA arch (89=Ada/4080)" FORCE)
    enable_language(CUDA)
    set(AURORAPLAYER_WHISPER_CUDA ON)
    message(STATUS "CUDA detected at ${CMAKE_CUDA_COMPILER} — whisper.cpp will build with GPU support (arch=${CMAKE_CUDA_ARCHITECTURES})")
else()
    message(STATUS "CUDA not detected — whisper.cpp will run on CPU only")
endif()

# Configure whisper.cpp's build before fetching.
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(GGML_CUDA              ${AURORAPLAYER_WHISPER_CUDA} CACHE BOOL "" FORCE)

# Try multiple mirrors in order — first hit wins. Tarball goes through a
# GitHub-proxy first (works reliably from China) before falling back to the
# upstream URL. Drop the .tar.gz into third_party/ to skip network entirely.
set(_whisper_tag "v1.8.4")
set(_whisper_local_cache "${CMAKE_SOURCE_DIR}/third_party/whisper.cpp-${_whisper_tag}.tar.gz")
if(EXISTS "${_whisper_local_cache}")
    message(STATUS "Using cached whisper.cpp tarball: ${_whisper_local_cache}")
    set(_whisper_urls "${_whisper_local_cache}")
else()
    set(_whisper_urls
        "https://mirror.ghproxy.com/https://github.com/ggml-org/whisper.cpp/archive/refs/tags/${_whisper_tag}.tar.gz"
        "https://ghproxy.net/https://github.com/ggml-org/whisper.cpp/archive/refs/tags/${_whisper_tag}.tar.gz"
        "https://gh-proxy.com/https://github.com/ggml-org/whisper.cpp/archive/refs/tags/${_whisper_tag}.tar.gz"
        "https://github.com/ggml-org/whisper.cpp/archive/refs/tags/${_whisper_tag}.tar.gz"
    )
endif()

FetchContent_Declare(
    whisper_cpp
    URL ${_whisper_urls}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(whisper_cpp)

# whisper.cpp / ggml inherit our global /W4 + /permissive- which produces a
# torrent of harmless warnings (third-party headers). Silence them so the
# real diagnostics from our own code stay visible.
if(MSVC)
    foreach(_t whisper ggml ggml-base ggml-cpu ggml-cuda ggml-vulkan)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE
                $<$<COMPILE_LANGUAGE:C,CXX>:/W0 /wd4244 /wd4267 /wd4305 /wd4996>)
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.cpp

    src/core/MpvPlayer.cpp
    src/core/MpvPlayer.h

    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/VideoWidget.cpp
    src/ui/VideoWidget.h
    src/ui/SeekBar.cpp
    src/ui/SeekBar.h
    src/ui/ControlBar.cpp
    src/ui/ControlBar.h
    src/ui/ThumbnailWorker.cpp
    src/ui/ThumbnailWorker.h
    src/ui/TimelineStrip.cpp
    src/ui/TimelineStrip.h
    src/ui/PlaylistPanel.cpp
    src/ui/PlaylistPanel.h
    src/ui/QualityAdvancedDialog.cpp
    src/ui/QualityAdvancedDialog.h

    src/ai/VadParams.h
    src/ai/ModelManager.cpp
    src/ai/ModelManager.h
    src/ai/AudioExtractor.cpp
    src/ai/AudioExtractor.h
    src/ai/WhisperEngine.cpp
    src/ai/WhisperEngine.h
    src/ai/SubtitleStream.cpp
    src/ai/SubtitleStream.h
    src/ai/TranscriptionJob.cpp
    src/ai/TranscriptionJob.h
    src/ai/TranscriptionDialog.cpp
    src/ai/TranscriptionDialog.h
    src/ai/TranslationEngine.cpp
    src/ai/TranslationEngine.h
    src/ai/TranslationJob.cpp
    src/ai/TranslationJob.h
    src/ai/TranslationDialog.cpp
    src/ai/TranslationDialog.h

    src/vr/VrController.cpp
    src/vr/VrController.h
    src/vr/VrRenderer.cpp
    src/vr/VrRenderer.h
)

set(RESOURCES
    resources/resources.qrc
)

# ---------------------------------------------------------------------------
# Windows: embed a multi-size .ico into the .exe so Explorer / taskbar /
# Alt-Tab / File-Properties show the application icon. We:
#   1) (Re)generate resources/icons/app.ico from app.png via scripts/make-icon.ps1
#      whenever app.ico is missing or older than app.png. PowerShell + GDI+;
#      no external tools required.
#   2) Compile resources/icons/app.rc into the executable as a Win32 RC
#      resource (CMake handles .rc → .res automatically with MSVC).
# ---------------------------------------------------------------------------
if(WIN32)
    set(APP_PNG "${CMAKE_SOURCE_DIR}/resources/icons/app.png")
    set(APP_ICO "${CMAKE_SOURCE_DIR}/resources/icons/app.ico")
    set(APP_RC  "${CMAKE_SOURCE_DIR}/resources/icons/app.rc")

    if(EXISTS "${APP_PNG}")
        set(_regen_ico NO)
        if(NOT EXISTS "${APP_ICO}")
            set(_regen_ico YES)
        elseif("${APP_PNG}" IS_NEWER_THAN "${APP_ICO}")
            set(_regen_ico YES)
        elseif("${CMAKE_SOURCE_DIR}/scripts/make-icon.ps1" IS_NEWER_THAN "${APP_ICO}")
            # Script logic changed (e.g. crop / resampling fix); force rebuild.
            set(_regen_ico YES)
        endif()
        if(_regen_ico)
            message(STATUS "Generating ${APP_ICO} from ${APP_PNG}")
            execute_process(
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                        -File "${CMAKE_SOURCE_DIR}/scripts/make-icon.ps1"
                        -SourcePng "${APP_PNG}"
                        -OutputIco "${APP_ICO}"
                RESULT_VARIABLE _ico_result
            )
            if(NOT _ico_result EQUAL 0)
                message(WARNING
                    "scripts/make-icon.ps1 failed (rc=${_ico_result}); "
                    ".exe icon will be missing this build")
            endif()
        endif()
    else()
        message(STATUS "${APP_PNG} not found; .exe icon will not be embedded")
    endif()

    if(EXISTS "${APP_ICO}")
        # Make rc.exe recompile when the .ico changes.
        set_source_files_properties("${APP_RC}" PROPERTIES OBJECT_DEPENDS "${APP_ICO}")
        list(APPEND RESOURCES "${APP_RC}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
qt_add_executable(AuroraPlayer WIN32 MACOSX_BUNDLE
    ${SOURCES}
    ${RESOURCES}
)

target_include_directories(AuroraPlayer PRIVATE src)

target_link_libraries(AuroraPlayer PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Network
    Mpv::Mpv
    whisper
)

if(AURORAPLAYER_WHISPER_CUDA)
    target_compile_definitions(AuroraPlayer PRIVATE GGML_USE_CUDA)
endif()

set_target_properties(AuroraPlayer PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "AuroraPlayer"
)

# ---------------------------------------------------------------------------
# Windows: copy libmpv DLL next to the produced exe
# ---------------------------------------------------------------------------
if(WIN32 AND TARGET Mpv::Mpv)
    get_target_property(_mpv_dll Mpv::Mpv IMPORTED_LOCATION)
    if(_mpv_dll AND EXISTS "${_mpv_dll}")
        add_custom_command(TARGET AuroraPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mpv_dll}"
                "$<TARGET_FILE_DIR:AuroraPlayer>"
            COMMENT "Copying libmpv runtime DLL")
    endif()
endif()

# ---------------------------------------------------------------------------
# Windows deployment: run windeployqt after build (Debug & Release)
# ---------------------------------------------------------------------------
if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT6_INSTALL_PREFIX}/bin")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET AuroraPlayer POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    "$<TARGET_FILE:AuroraPlayer>"
            COMMENT "Running windeployqt")
    endif()
endif()
