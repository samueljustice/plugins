cmake_minimum_required(VERSION 3.15)

# Set universal binary architectures BEFORE project()
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

project(PitchFlattener VERSION 2.3.1)

# The copyright year is the year it was built, so it never goes stale.
string(TIMESTAMP COPYRIGHT_YEAR "%Y")
add_compile_definitions(COPYRIGHT_YEAR="${COPYRIGHT_YEAR}")

# Add JUCE subdirectory
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE JUCE)

# Find Rubberband
set(RUBBERBAND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../SDK/rubberband-4.0.0")
set(RUBBERBAND_INCLUDE_DIR "${RUBBERBAND_ROOT}/rubberband")
set(RUBBERBAND_LIB "${RUBBERBAND_ROOT}/lib/librubberband.a")

# Plugin formats; pass -DPITCHFLATTENER_FORMATS="AU;VST3;Standalone" for a standalone app.
set(PITCHFLATTENER_FORMATS "AU;VST3" CACHE STRING "Plugin formats to build")
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
sammyj_enable_aax(PITCHFLATTENER_FORMATS)
option(PITCHFLATTENER_BUILD_TESTS "Build the offline flattening test harness" OFF)

# Embedded UI fonts (Azeret Mono, SIL OFL 1.1)
juce_add_binary_data(PitchFlattenerBinaryData
    SOURCES
        Resources/Fonts/AzeretMono-Regular.ttf
        Resources/Fonts/AzeretMono-Medium.ttf
        Resources/Fonts/AzeretMono-Bold.ttf
)

# DSP core shared by the plugin and the test harness
set(PF_DSP_SOURCES
    Source/dsp/Detectors.cpp
    Source/dsp/PitchTracker.cpp
    Source/dsp/RubberBandDriver.cpp
    Source/dsp/FlattenerCore.cpp
    Source/dsp/SourceAdvisor.cpp
)

option(PITCHFLATTENER_COPY_AFTER_BUILD "Copy plugins to the system folders after building" ON)

# Define our plugin
juce_add_plugin(PitchFlattener
    PLUGIN_MANUFACTURER_CODE Sjus
    PLUGIN_CODE Pftf
    FORMATS ${PITCHFLATTENER_FORMATS}
    PRODUCT_NAME "SammyJs Pitch Flattener"
    COMPANY_NAME "Sammy J"
    COMPANY_COPYRIGHT "Copyright ${COPYRIGHT_YEAR} Samuel Justice"
    COMPANY_WEBSITE ""
    COMPANY_EMAIL ""
    BUNDLE_ID com.samueljustice.pitchflattener
    PLUGIN_MANUFACTURER_URL ""
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${PITCHFLATTENER_COPY_AFTER_BUILD}
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
        Source/PitchFlattenerLookAndFeel.cpp
        Source/PresetManager.cpp
        Source/SpectrogramVisualizer.cpp
        Source/AboutWindow.cpp
        Source/UpdatePopup.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/SammyJUpdateCheck.cpp
        ${PF_DSP_SOURCES}
)

# Compile definitions
target_compile_definitions(PitchFlattener
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        PLUGIN_VERSION="${PROJECT_VERSION}"
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Build timestamp (passed from CI or defaults to compile time)
if(BUILD_TIMESTAMP)
    target_compile_definitions(PitchFlattener PUBLIC BUILD_TIMESTAMP="${BUILD_TIMESTAMP}")
endif()

# Include directories
target_include_directories(PitchFlattener
    PRIVATE
        ${RUBBERBAND_ROOT}
        ${RUBBERBAND_ROOT}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

# Link libraries
target_link_libraries(PitchFlattener
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        PitchFlattenerBinaryData
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

# Offline test harness: renders synthetic sirens / voices / engines through the
# DSP core and measures how flat the output pitch is.
if(PITCHFLATTENER_BUILD_TESTS)
    juce_add_console_app(PitchFlattenerTests PRODUCT_NAME "PitchFlattenerTests")
    target_sources(PitchFlattenerTests PRIVATE tests/FlattenTests.cpp ${PF_DSP_SOURCES})
    target_include_directories(PitchFlattenerTests PRIVATE ${RUBBERBAND_ROOT} ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_compile_definitions(PitchFlattenerTests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(PitchFlattenerTests PRIVATE juce::juce_core juce::juce_audio_basics juce::juce_audio_formats juce::juce_dsp ${RUBBERBAND_LIB}
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    target_compile_features(PitchFlattenerTests PRIVATE cxx_std_17)
    if(APPLE)
        target_link_libraries(PitchFlattenerTests PRIVATE "-framework Accelerate")
    endif()
endif()

# Post-build installation is handled by JUCE's COPY_PLUGIN_AFTER_BUILD flag

# CPack configuration for creating installers
set(CPACK_PACKAGE_NAME "PitchFlattener")
set(CPACK_PACKAGE_VENDOR "Samuel Justice")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Real-time pitch flattening audio plugin")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_CONTACT "sam@sweetjusticesound.com")

if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "PitchFlattener ${PROJECT_VERSION}")
    set(CPACK_DMG_BACKGROUND_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/Resources/dmg_background.png")
    set(CPACK_DMG_DS_STORE "${CMAKE_CURRENT_SOURCE_DIR}/Resources/DS_Store")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "SammyJs Pitch Flattener")
    set(CPACK_NSIS_HELP_LINK "https://www.sweetjusticesound.com")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://www.sweetjusticesound.com")
    set(CPACK_NSIS_CONTACT "sam@sweetjusticesound.com")
endif()

include(CPack)