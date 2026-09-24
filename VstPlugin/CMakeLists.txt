# VstPlugin/CMakeLists.txt
#
# Standalone CMake project for Patina FX, the Windows 11 VST3 insert
# effect that runs Sources/Core's real-time signal path (see
# Sources/Core/RealtimeChannel.h) live on a DAW channel. Deliberately its
# OWN top-level project -- the repo root CMakeLists.txt (which builds
# Sources/Core + Tests/CoreTests only) is untouched by this file's
# existence, and this file is what a developer points cmake -S at
# directly:
#
#   cmake -S VstPlugin -B VstPlugin/build
#   cmake --build VstPlugin/build --config Release
#
# Builds on macOS too (VST3/AU/Standalone) for local development --
# Windows 11 VST3 is the shipping target, but nothing here is Windows-
# specific except the MSVC-only compile definitions below.

cmake_minimum_required(VERSION 3.22)
project(PatinaFX VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static runtime on MSVC (JUCE's own default via
# JUCE_STATIC_CRT_HANDLING once juce_add_plugin is called, kept explicit
# here too) -- ships without needing the VC++ redistributable installed.
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# JUCE_SOURCE_DIR lets an offline build point at a pre-fetched JUCE
# checkout (e.g. copied onto a machine with no network access) instead
# of git-cloning it. Leave unset for the normal online flow.
set(JUCE_SOURCE_DIR "" CACHE PATH "Path to a local JUCE 8.0.15 checkout; leave empty to fetch from GitHub")

include(FetchContent)
if (JUCE_SOURCE_DIR)
    FetchContent_Declare(
        JUCE
        SOURCE_DIR ${JUCE_SOURCE_DIR}
    )
else()
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.15
        GIT_SHALLOW    TRUE
    )
endif()
FetchContent_MakeAvailable(JUCE)

# The DSP core this plugin wraps -- Sources/Core is otherwise only built
# by the repo root CMakeLists.txt (standalone core + tests) and
# Package.swift (the macOS app); this is a THIRD, independent consumer
# of the same C-API boundary (Sources/Core/include/AkaizerCore.h), same
# discipline the Swift app already follows. Adds no files or targets to
# either of those -- this add_subdirectory call is the only place
# Sources/Core is reached from here.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../Sources/Core ${CMAKE_CURRENT_BINARY_DIR}/AkaizerCore)

# Sources/Core uses M_PI/M_LN2-style POSIX math macros (see
# FilterStages.h/RateStages.h), which <cmath> only defines on MSVC when
# _USE_MATH_DEFINES is set before including it. Applied here rather than
# editing Sources/Core itself, since every other consumer (the standalone
# CMake build, the Swift package) builds with a compiler that defines
# these unconditionally.
if (MSVC)
    target_compile_definitions(AkaizerCore PUBLIC _USE_MATH_DEFINES)
endif()

juce_add_plugin(PatinaFX
    COMPANY_NAME "Patina"
    PLUGIN_MANUFACTURER_CODE Ptna
    PLUGIN_CODE Pfx1
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "Patina FX"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES "Fx" "Filter"
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(PatinaFX PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/MachineControls.cpp
    Source/MachineControls.h
)

target_compile_definitions(PatinaFX PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(PatinaFX PRIVATE
    AkaizerCore
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
