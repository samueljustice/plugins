cmake_minimum_required(VERSION 3.15)

# Set universal binary architectures BEFORE project()
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

project(Reversinator VERSION 1.0.0)

# Add JUCE subdirectory
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE JUCE)

# Define our plugin
juce_add_plugin(Reversinator
    PLUGIN_MANUFACTURER_CODE Sjus
    PLUGIN_CODE Revr
    FORMATS AU VST3
    PRODUCT_NAME "SammyJs Reversinator"
    COMPANY_NAME "Samuel Justice"
    COMPANY_COPYRIGHT "Copyright 2025 Samuel Justice"
    COMPANY_WEBSITE ""
    COMPANY_EMAIL ""
    BUNDLE_ID com.samueljustice.reversinator
    PLUGIN_MANUFACTURER_URL ""
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES "Fx" "Delay"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
)

# Generate JUCE header
juce_generate_juce_header(Reversinator)

# Add source files
target_sources(Reversinator
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/ReverseEngine.cpp
)

# Compile definitions
target_compile_definitions(Reversinator
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        PLUGIN_VERSION="${PROJECT_VERSION}"
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Link libraries
target_link_libraries(Reversinator
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Platform specific settings
if(APPLE)
    target_link_libraries(Reversinator PRIVATE "-framework Accelerate")
endif()

if(WIN32)
    # Windows specific settings for VST3
    set_target_properties(Reversinator_VST3 PROPERTIES
        SUFFIX ".vst3"
        PREFIX ""
    )
endif()

# Set C++ standard
target_compile_features(Reversinator PRIVATE cxx_std_17)

# Build universal binary on macOS
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures for macOS")
    set_target_properties(Reversinator PROPERTIES
        OSX_ARCHITECTURES "arm64;x86_64"
    )
endif()

# Post-build installation is handled by JUCE's COPY_PLUGIN_AFTER_BUILD flag

# CPack configuration for creating installers
set(CPACK_PACKAGE_NAME "Reversinator")
set(CPACK_PACKAGE_VENDOR "Samuel Justice")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Real-time audio reversing effect plugin")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_CONTACT "sam@sweetjusticesound.com")

if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Reversinator ${PROJECT_VERSION}")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "SammyJs Reversinator")
    set(CPACK_NSIS_HELP_LINK "https://www.sweetjusticesound.com")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://www.sweetjusticesound.com")
    set(CPACK_NSIS_CONTACT "sam@sweetjusticesound.com")
endif()

include(CPack)