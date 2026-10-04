cmake_minimum_required(VERSION 3.15)

project(Ferric
    VERSION 0.2.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h. Set
# globally rather than per-file: the next file to include windows.h hits it too,
# and the error gives no hint what happened.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(FERRIC_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# The FFGL plugin, the FFGL SDK submodule and GLEW are one dependency group, and
# the OpenFX plugin belongs to none of it. Turning this OFF builds the OFX plugin
# alone, with no GL loader anywhere in the configure -- which is what the Linux
# job does, because the FFGL SDK has no Linux path and its own
# find_package(GLEW REQUIRED) would otherwise fail at CONFIGURE time on a
# machine building something that never calls GL.
option(FERRIC_BUILD_FFGL "Build the FFGL plugin (needs the FFGL SDK and, off macOS, GLEW)" ON)

# ---------------------------------------------------------------------------
# The parts with no GL and no host in them at all: the transport and its error
# signal, the compander, the reaction, every control curve and the preset table.
#
# A separate library from the FFGL core because every build needs it and only
# one of them has a GL context. Splitting it is what lets the OFX build link the
# SAME arithmetic rather than a second transcription of it.
# ---------------------------------------------------------------------------
add_library(ferric_dsp OBJECT
    source/Transport.h
    source/Transport.cpp
    source/Compander.h
    source/Compander.cpp
    source/Drive.h
    source/Drive.cpp
    source/Controls.h
    source/Controls.cpp
    source/Presets.h
)
target_include_directories(ferric_dsp PUBLIC source)
set_target_properties(ferric_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The CPU mirror of the three GLSL passes. Its own library because the FFGL
# bundle has no use for it -- the GPU does that work there -- while the OpenFX
# plugin renders with it and the harness needs it to hold it against the GPU.
add_library(ferric_cpu OBJECT
    source/CpuPasses.h
    source/CpuPasses.cpp
)
target_link_libraries(ferric_cpu PUBLIC ferric_dsp)
set_target_properties(ferric_cpu PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(FERRIC_BUILD_FFGL)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins. See AGENTS.md for what is wrong with it.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin and the harness share, as an OBJECT library.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor and nothing ever references that object by name, so in
# an archive the linker is entitled to drop the whole translation unit -- and the
# result is a bundle that loads, exports plugMain, and reports that it contains
# no plugins. tools/verify.sh checks both failure directions.
# ---------------------------------------------------------------------------
add_library(ferric_core OBJECT
    source/Diag.h
    source/Diag.cpp
    source/PassBuffer.h
    source/PassBuffer.cpp
    source/Shaders.h
    source/shaders/Vertex.cpp
    source/shaders/Tbe.cpp
    source/shaders/Compand.cpp
    source/shaders/Passes.cpp
    source/Ferric.h
    source/Ferric.cpp
)

target_include_directories(ferric_core PUBLIC source)
target_link_libraries(ferric_core PUBLIC ffgl::sdk ferric_dsp)
target_compile_definitions(ferric_core PUBLIC FERRIC_VERSION="${PROJECT_VERSION}")
set_target_properties(ferric_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers we include pull in <GL/glew.h>. The SDK links
# GLEW privately, so consumers need it too -- for the include path, and for the
# GLEW_STATIC define the static vcpkg triplet propagates. macOS uses the system
# OpenGL framework and needs none of this.
#
# GLEW arrives through vcpkg.json, which nothing in this file mentions. Deleting
# that manifest breaks the Windows job at CONFIGURE time while every macOS build
# stays green.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(ferric_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin. One FFGL effect; PluginEntry.cpp is listed directly rather than in
# the core library so the harness does not carry the registration.
# ---------------------------------------------------------------------------
add_library(FerricPlugin MODULE source/PluginEntry.cpp)
# ferric_dsp is named again here, and on every other final target, because an
# OBJECT library's objects do NOT travel transitively through a second OBJECT
# library. ferric_core linking it gives this target the include paths and
# nothing to link, and the failure is a pile of undefined symbols at the very
# last step of an otherwise clean build.
target_link_libraries(FerricPlugin PRIVATE ferric_core ferric_dsp)

if(APPLE)
    set_target_properties(FerricPlugin PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        OUTPUT_NAME "Ferric"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Ferric"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.ferric"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
    target_link_libraries(FerricPlugin PRIVATE
        "-framework OpenGL"
        "-framework Carbon"
        "-framework AppKit"
    )
else()
    set_target_properties(FerricPlugin PROPERTIES OUTPUT_NAME "Ferric")
endif()

# ---------------------------------------------------------------------------
# Offline harness. Where the plugin is actually verified rather than merely
# previewed: it drives the real plugin class through the real FFGL sequence in a
# headless core-profile context, including SetTime and a synthetic spectrum. See
# tools/frtest.
# ---------------------------------------------------------------------------
if(FERRIC_BUILD_TOOLS AND APPLE)
    add_executable(frtest tools/frtest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    target_link_libraries(frtest PRIVATE ferric_core ferric_dsp ferric_cpu z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# `cmake --install` drops the bundle straight into the user's Resolume plugin
# folder. Override with -DCMAKE_INSTALL_PREFIX=...
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # honour an explicit prefix
elseif(APPLE)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

endif()  # FERRIC_BUILD_FFGL

# ---------------------------------------------------------------------------
# The OpenFX build, for Resolve/Nuke/Natron/Vegas. CPU render against the
# vendored OFX SDK subset under external/openfx (C headers + the official C++
# Support library, BSD-3).
#
# ferric_dsp links straight in, so the error signal, the compander's gain law
# and stage table, the reaction arithmetic and every control curve are the same
# code as the FFGL build. Drive.cpp comes too even though OFX has neither beat
# information nor an FFT: the plugin fills a zeroed drive::Input, gets scale 1
# and no lurch, and renders the manual transport through the same arithmetic
# rather than a second reduced copy. Only the per-pixel stage is written twice
# -- CpuPasses.cpp against the GLSL -- and `frtest --cpu` holds the two together.
# ---------------------------------------------------------------------------
option(BUILD_OFX "Build the OpenFX plugin" ON)

if(BUILD_OFX)
    set(OFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx")

    add_library(FerricOFX MODULE
        source/ofx/FerricOFX.cpp
        ${OFX_SDK}/Support/Library/ofxsCore.cpp
        ${OFX_SDK}/Support/Library/ofxsImageEffect.cpp
        ${OFX_SDK}/Support/Library/ofxsInteract.cpp
        ${OFX_SDK}/Support/Library/ofxsLog.cpp
        ${OFX_SDK}/Support/Library/ofxsMultiThread.cpp
        ${OFX_SDK}/Support/Library/ofxsParams.cpp
        ${OFX_SDK}/Support/Library/ofxsProperty.cpp
        ${OFX_SDK}/Support/Library/ofxsPropertyValidation.cpp
    )
    # PRIVATE on an OBJECT library gives both the objects and the usage
    # requirements -- and both libraries are named, because OBJECT libraries'
    # objects do not travel transitively. `$<TARGET_OBJECTS:...>` would give
    # the objects alone
    # -- no include directories, no compile definitions -- and the failure is
    # platform-specific enough to cost a day.
    target_link_libraries(FerricOFX PRIVATE ferric_cpu ferric_dsp)
    target_include_directories(FerricOFX PRIVATE
        source
        "${OFX_SDK}/include"
        "${OFX_SDK}/Support/include"
    )
    target_compile_definitions(FerricOFX PRIVATE
        FERRIC_VERSION="${PROJECT_VERSION}"
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    )

    # Threads, explicitly. The OFX Support library's multi-thread suite calls
    # pthread_create, and on Rocky 8 -- glibc 2.28, the floor Resolve supports --
    # that still lives in libpthread; libpthread was only folded into libc in
    # glibc 2.34. Leave it out and the plugin compiles, links, passes a symbol
    # check and a glibc-version check, and then fails at dlopen with "undefined
    # symbol: pthread_create". macOS and Windows never show it.
    find_package(Threads REQUIRED)
    target_link_libraries(FerricOFX PRIVATE Threads::Threads)

    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Ferric.ofx.bundle")
    set_target_properties(FerricOFX PROPERTIES
        PREFIX ""
        SUFFIX ".ofx"
        OUTPUT_NAME "Ferric"
        LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/MacOS"
    )
    if(APPLE)
        # Universal like the FFGL bundle, through the same CMAKE_OSX_ARCHITECTURES
        # set at the top of this file -- Resolve ships for both architectures.
        set(OFX_BUNDLE_IDENTIFIER "com.stoatworks.ferric.ofx")
        configure_file(cmake/InfoOFX.plist.in
            "${OFX_BUNDLE_DIR}/Contents/Info.plist" @ONLY)
    elseif(WIN32)
        set_target_properties(FerricOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            # MSVC's .exp/.lib for this target; its default directory collides
            # with the FFGL target's, which shares the output base name, and a
            # parallel build then races on the file (LNK1104).
            ARCHIVE_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/importlib"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/importlib")
    else()
        set_target_properties(FerricOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Linux-x86-64")
    endif()
endif()

if(FERRIC_BUILD_FFGL)
install(TARGETS FerricPlugin
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)
endif()  # FERRIC_BUILD_FFGL
