cmake_minimum_required(VERSION 3.21)

# Minimum macOS. Without this the binaries take the build machine's SDK version as their
# minimum and JUCE's runtime OS checks (@available) are compiled out. Must come before project().
# (FORCE only when empty, so an explicit -DCMAKE_OSX_DEPLOYMENT_TARGET=... still wins.)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(Subbertone VERSION 1.3.1)

# The copyright year is the year it was built, so it never goes stale.
string(TIMESTAMP COPYRIGHT_YEAR "%Y")
add_compile_definitions(COPYRIGHT_YEAR="${COPYRIGHT_YEAR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Windows: link the C++ runtime statically, so the plug-in doesn't depend on the
# VC++ redistributable being installed (or on an older msvcp140.dll a host ships).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Universal Binary Support
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

# Version Definitions
add_compile_definitions(
    PLUGIN_VERSION="${PROJECT_VERSION}"
    PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    JUCE_VST3_CAN_REPLACE_VST2=0
)

# Build timestamp (passed from CI or defaults to compile time)
if(BUILD_TIMESTAMP)
    add_compile_definitions(BUILD_TIMESTAMP="${BUILD_TIMESTAMP}")
endif()

# Plugin formats; pass -DSUBBERTONE_FORMATS="AU;VST3;Standalone" for a standalone app.
set(SUBBERTONE_FORMATS "AU;VST3" CACHE STRING "Plugin formats to build")

# Add the JUCE library subdirectory
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
sammyj_enable_aax(SUBBERTONE_FORMATS)

# JUCE-native pitch detection using autocorrelation

# Main plugin target
juce_add_plugin(Subbertone
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "Sammy J"
    BUNDLE_ID "com.samueljustice.subbertone"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE SJus
    PLUGIN_CODE Subb
    FORMATS ${SUBBERTONE_FORMATS}
    PRODUCT_NAME "SammyJs Subbertone"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "This plugin needs access to your microphone to process real-time audio input"
)

# Platform specific settings
if(APPLE)
    set_target_properties(Subbertone PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
        XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

# Source files
target_sources(Subbertone
    PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PitchDetector.cpp
    Source/PresetManager.cpp
    Source/HarmonicStrip.cpp
    Source/LogoOutline.cpp
    Source/MasterProcessor.cpp
    Source/SubharmonicEngine.cpp
    Source/WaveformVisualizer.cpp
    Source/AboutWindow.cpp
    Source/SubbertoneLookAndFeel.cpp
)

# Plugin unique ID is set in juce_add_plugin above

# Generate JuceHeader.h
juce_generate_juce_header(Subbertone)

# Link libraries
target_link_libraries(Subbertone
    PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_opengl
    SubbertoneBinaryData
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Binary data
juce_add_binary_data(SubbertoneBinaryData
    SOURCES
    LICENSE
    Resources/Fonts/AzeretMono-Regular.ttf
    Resources/Fonts/AzeretMono-Medium.ttf
    Resources/Fonts/AzeretMono-Bold.ttf
    Resources/sj-logo.svg
)