cmake_minimum_required(VERSION 3.15)

# Set universal binary architectures BEFORE project()
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

project(PitchFlattener VERSION 1.0.0)

# Add JUCE subdirectory
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE JUCE)

# Find Rubberband
set(RUBBERBAND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../SDK/rubberband-4.0.0")
set(RUBBERBAND_INCLUDE_DIR "${RUBBERBAND_ROOT}/rubberband")
set(RUBBERBAND_LIB "${RUBBERBAND_ROOT}/lib/librubberband.a")

# Find WORLD
set(WORLD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../SDK/World")
set(WORLD_INCLUDE_DIR "${WORLD_ROOT}/src")
set(WORLD_SOURCES
    "${WORLD_ROOT}/src/dio.cpp"
    "${WORLD_ROOT}/src/common.cpp"
    "${WORLD_ROOT}/src/matlabfunctions.cpp"
    "${WORLD_ROOT}/src/fft.cpp"
)

# Define our plugin
juce_add_plugin(PitchFlattener
    PLUGIN_MANUFACTURER_CODE Sjus
    PLUGIN_CODE Pftf
    FORMATS AU VST3
    PRODUCT_NAME "SammyJs Pitch Flattener"
    COMPANY_NAME "Samuel Justice"
    COMPANY_COPYRIGHT "Copyright 2025 Samuel Justice"
    COMPANY_WEBSITE ""
    COMPANY_EMAIL ""
    BUNDLE_ID com.samueljustice.pitchflattener
    PLUGIN_MANUFACTURER_URL ""
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES "Fx" "Pitch"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "This plugin needs access to your microphone for real-time pitch detection"
)

# Generate JUCE header
juce_generate_juce_header(PitchFlattener)

# Add source files
target_sources(PitchFlattener
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/PitchDetector.cpp
        Source/PitchFlattenerEngine.cpp
        Source/PresetManager.cpp
        ${WORLD_SOURCES}
)

# Compile definitions
target_compile_definitions(PitchFlattener
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# Include directories
target_include_directories(PitchFlattener
    PRIVATE
        ${RUBBERBAND_ROOT}
        ${RUBBERBAND_ROOT}/src
        ${WORLD_INCLUDE_DIR}
)

# Link libraries
target_link_libraries(PitchFlattener
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        ${RUBBERBAND_LIB}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Platform specific settings
if(APPLE)
    target_link_libraries(PitchFlattener PRIVATE "-framework Accelerate")
endif()

if(WIN32)
    # Windows specific settings for VST3
    set_target_properties(PitchFlattener_VST3 PROPERTIES
        SUFFIX ".vst3"
        PREFIX ""
    )
endif()

# Set C++ standard
target_compile_features(PitchFlattener PRIVATE cxx_std_17)

# Build universal binary on macOS
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures for macOS")
    set_target_properties(PitchFlattener PROPERTIES
        OSX_ARCHITECTURES "arm64;x86_64"
    )
endif()

# Post-build installation is handled by JUCE's COPY_PLUGIN_AFTER_BUILD flag