cmake_minimum_required(VERSION 3.21)

# Universal Binary Support - MUST be set before project()
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

project(StretchArmstrong VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Version Definitions
add_compile_definitions(
    PLUGIN_VERSION="${PROJECT_VERSION}"
    PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    JUCE_VST3_CAN_REPLACE_VST2=0
)

# Add the JUCE library subdirectory
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

# Find Rubberband for high-quality time-stretching
set(RUBBERBAND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../SDK/rubberband-4.0.0")
set(RUBBERBAND_INCLUDE_DIR "${RUBBERBAND_ROOT}/rubberband")
set(RUBBERBAND_LIB "${RUBBERBAND_ROOT}/lib/librubberband.a")

# Main plugin target
juce_add_plugin(StretchArmstrong
    VERSION 1.0.0
    COMPANY_NAME "Samuel Justice"
    BUNDLE_ID "com.samueljustice.stretcharmstrong"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE SJus
    PLUGIN_CODE StAr
    FORMATS AU VST3
    PRODUCT_NAME "SammyJs Stretch Armstrong"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "This plugin needs access to your microphone to process real-time audio input"
    VST3_CATEGORIES "Fx" "Tools"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
)

# Platform specific settings
if(APPLE)
    set_target_properties(StretchArmstrong PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
        XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

# Source files
target_sources(StretchArmstrong
    PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/StretchEngine.cpp
    Source/WaveformVisualizer.cpp
    Source/PresetManager.cpp
    Source/AboutWindow.cpp
)

# Include directories
target_include_directories(StretchArmstrong
    PRIVATE
        ${RUBBERBAND_ROOT}
        ${RUBBERBAND_ROOT}/src
)

# Generate JuceHeader.h
juce_generate_juce_header(StretchArmstrong)

# Link libraries
target_link_libraries(StretchArmstrong
    PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_opengl
    StretchArmstrongBinaryData
    ${RUBBERBAND_LIB}
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Platform specific libraries
if(APPLE)
    target_link_libraries(StretchArmstrong PRIVATE "-framework Accelerate")
endif()

# Binary data
juce_add_binary_data(StretchArmstrongBinaryData
    SOURCES
    LICENSE
)
