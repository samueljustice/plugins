cmake_minimum_required(VERSION 3.21)
project(Subbertone VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

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

# Add the JUCE library subdirectory
add_subdirectory("/Users/ernest/JUCE" "${CMAKE_CURRENT_BINARY_DIR}/JUCE-build")

# JUCE-native pitch detection using autocorrelation

# Main plugin target
juce_add_plugin(Subbertone
    VERSION 1.0.0
    COMPANY_NAME "Samuel Justice"
    BUNDLE_ID "com.samueljustice.subbertone"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE SJus
    PLUGIN_CODE Subb
    FORMATS AU VST3
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
    Source/SubharmonicEngine.cpp
    Source/WaveformVisualizer.cpp
    Source/AboutWindow.cpp

    Source/PluginProcessor.h
    Source/PluginEditor.h
    Source/PitchDetector.h
    Source/SubharmonicEngine.h
    Source/WaveformVisualizer.h
    Source/AboutWindow.h
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
)
