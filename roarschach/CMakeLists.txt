cmake_minimum_required(VERSION 3.22)

# Minimum macOS. Without this the binaries take the build machine's SDK version as their
# minimum and JUCE's runtime OS checks (@available) are compiled out. Must come before project().
# (FORCE only when empty, so an explicit -DCMAKE_OSX_DEPLOYMENT_TARGET=... still wins.)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Universal binary.
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

# The one place the version lives. Release builds pass -DROARSCHACH_VERSION=x.y.z (from the
# roarschach-vx.y.z tag); it ends up in the About panel, the bundles and the installers.
set(ROARSCHACH_VERSION "1.0.1" CACHE STRING "Roarschach version (major.minor.patch)")
project(Roarschach VERSION ${ROARSCHACH_VERSION})

# The copyright year is the year it was built, so it never goes stale.
string(TIMESTAMP COPYRIGHT_YEAR "%Y")
add_compile_definitions(COPYRIGHT_YEAR="${COPYRIGHT_YEAR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Windows: link the C++ runtime statically, so the plug-in doesn't depend on the
# VC++ redistributable being installed (or on an older msvcp140.dll a host ships).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
    PLUGIN_VERSION="${PROJECT_VERSION}"
    PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    JUCE_VST3_CAN_REPLACE_VST2=0
)

# Build timestamp for the About panel (passed from CI; absent in local builds).
if(BUILD_TIMESTAMP)
    add_compile_definitions(BUILD_TIMESTAMP="${BUILD_TIMESTAMP}")
endif()

# Plugin formats; pass -DROARSCHACH_FORMATS="AU;VST3;Standalone" for a standalone app.
set(ROARSCHACH_FORMATS "AU;VST3" CACHE STRING "Plugin formats to build")
option(ROARSCHACH_TESTS "Build the offline tests" OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
sammyj_enable_aax(ROARSCHACH_FORMATS)

# The manufacturer/plugin codes and bundle ID must never change: hosts use them
# to find existing instances in saved sessions.
juce_add_plugin(Roarschach
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "Sammy J"
    COMPANY_COPYRIGHT "Copyright ${COPYRIGHT_YEAR} Samuel Justice"
    COMPANY_WEBSITE "https://sweetjusticesound.com"
    BUNDLE_ID "com.samueljustice.roarschach"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE SJus
    PLUGIN_CODE Rrsc
    FORMATS ${ROARSCHACH_FORMATS}
    PRODUCT_NAME "Roarschach"
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx "Pitch Shift"
    # Only used by a Standalone build, which takes its input from a microphone.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Roarschach processes the audio input in real time."
)

if(APPLE)
    set_target_properties(Roarschach PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
        XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

target_sources(Roarschach
    PRIVATE
    Source/RoarschachLookAndFeel.cpp
    Source/AboutWindow.cpp
    Source/PresetManager.cpp
    Source/FactoryPresets.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/SignalFlowStrip.cpp
    Source/MonsterView.cpp
    Source/CreatureEvolution.cpp
    Source/LogoSdf.cpp
    Source/dsp/PitchDetector.cpp
    Source/dsp/CreatureChain.cpp
    Source/dsp/common/HilbertPair.cpp
    Source/dsp/common/Oversampler2x.cpp
    Source/dsp/common/VoiceStream.cpp
    Source/dsp/modules/InModule.cpp
    Source/dsp/modules/PitchModule.cpp
    Source/dsp/modules/CleanModule.cpp
    Source/dsp/modules/DeessModule.cpp
    Source/dsp/modules/DetailModule.cpp
    Source/dsp/modules/VocoderModule.cpp
    Source/dsp/modules/RingModule.cpp
    Source/dsp/modules/ShimmerModule.cpp
    Source/dsp/modules/GrowlModule.cpp
    Source/dsp/modules/SwirlModule.cpp
    Source/dsp/modules/RoarModule.cpp
    Source/dsp/modules/EnsembleModule.cpp
    Source/dsp/modules/TameModule.cpp
    Source/dsp/modules/SubModule.cpp
    Source/dsp/modules/OvertoneModule.cpp
    Source/dsp/modules/OutputModule.cpp
)

# The DSP modules compare floats exactly on purpose (settled glides, unchanged settings).
file(GLOB ROARSCHACH_DSP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/modules/*.cpp)
# Clang/GCC only: MSVC rejects the flag (D8021) and has no such warning anyway.
if(NOT MSVC)
    set_source_files_properties(${ROARSCHACH_DSP_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-float-equal")
endif()

target_compile_definitions(Roarschach
    PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

# The licence text for the About panel, the typeface and the logo.
juce_add_binary_data(RoarschachBinaryData
    SOURCES
    LICENSE
    Resources/Fonts/AzeretMono-Regular.ttf
    Resources/Fonts/AzeretMono-Medium.ttf
    Resources/Fonts/AzeretMono-Bold.ttf
    Resources/sj-logo.svg
)

juce_generate_juce_header(Roarschach)

target_link_libraries(Roarschach
    PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_opengl
    RoarschachBinaryData
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Offline tests: drive the real processor at several sample rates and block sizes (see
# Tests/RoarschachTests.cpp). The test links the plug-in's shared code library (the
# Roarschach target itself, which the AU/VST3 wrappers link too), so it tests exactly the code
# that ships, compiled once, with the same JucePlugin_* definitions. That library links the JUCE
# modules privately, so their headers and the generated JuceHeader.h are added here by hand;
# the module sources are not compiled a second time.
if(ROARSCHACH_TESTS)
    add_executable(RoarschachTests Tests/RoarschachTests.cpp)
    target_include_directories(RoarschachTests
        PRIVATE
        Source
        "$<TARGET_PROPERTY:Roarschach,JUCE_GENERATED_SOURCES_DIRECTORY>"
        "$<TARGET_PROPERTY:juce::juce_core,INTERFACE_INCLUDE_DIRECTORIES>"
    )
    # The shared code target exports only the JucePlugin_* values; the per-format switches
    # stay private to it. The test is none of the formats.
    target_compile_definitions(RoarschachTests
        PRIVATE
        JucePlugin_Build_VST=0
        JucePlugin_Build_VST3=0
        JucePlugin_Build_AU=0
        JucePlugin_Build_AUv3=0
        JucePlugin_Build_AAX=0
        JucePlugin_Build_Standalone=0
        JucePlugin_Build_Unity=0
        JucePlugin_Build_LV2=0
    )
    add_dependencies(RoarschachTests Roarschach)
    target_link_libraries(RoarschachTests
        PRIVATE
        Roarschach
        RoarschachBinaryData
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
endif()
