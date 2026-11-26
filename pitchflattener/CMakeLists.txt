cmake_minimum_required(VERSION 3.15)

# macOS 11 is the oldest system the plug-in is built for.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Set universal binary architectures BEFORE project()
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

project(PitchFlattener VERSION 3.1.1)

# The C++ runtime is linked in on Windows, so the installer needs no redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The copyright year is the year it was built, so it never goes stale.
string(TIMESTAMP COPYRIGHT_YEAR "%Y")
add_compile_definitions(COPYRIGHT_YEAR="${COPYRIGHT_YEAR}")

# Add JUCE subdirectory
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE JUCE)

# Plugin formats; pass -DPITCHFLATTENER_FORMATS="AU;VST3;Standalone" for a standalone app.
set(PITCHFLATTENER_FORMATS "AU;VST3" CACHE STRING "Plugin formats to build")
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
sammyj_enable_aax(PITCHFLATTENER_FORMATS)
option(PITCHFLATTENER_BUILD_TESTS "Build the tests (PitchFlattenerTests, PitchFlattenerCoreTests)" OFF)

# Embedded UI fonts (Azeret Mono, SIL OFL 1.1)
juce_add_binary_data(PitchFlattenerBinaryData
    SOURCES
        Resources/Fonts/AzeretMono-Regular.ttf
        Resources/Fonts/AzeretMono-Medium.ttf
        Resources/Fonts/AzeretMono-Bold.ttf
)

# The flattening core: plain C++, no JUCE (shared by the plug-in, its tests and the core tests).
# The time stretch that keeps a flattened sound's length (prebuilt static library in the shared SDK folder).
set(RUBBERBAND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../SDK/rubberband-4.0.0")
set(RUBBERBAND_LIB "${RUBBERBAND_ROOT}/lib/librubberband.a")

set(PF_CORE_SOURCES
    Source/core/Flatten.cpp
    Source/core/HarmonicTracker.cpp
    Source/core/HindsightTracker.cpp
    Source/core/RidgeTracker.cpp
    Source/core/KeepLength.cpp
)

# Learn, locate and play: plain C++ too, around the core.
set(PF_ENGINE_SOURCES
    Source/Engine.cpp
    Source/Locator.cpp
    Source/SoundRegistry.cpp
)

option(PITCHFLATTENER_COPY_AFTER_BUILD "Copy plugins to the system folders after building" ON)

# Define our plugin. The codes and the bundle ID never change: hosts find the plug-in by them.
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
    # Where Pro Tools lists it in its plug-in menus (without this it goes under "Other").
    AAX_CATEGORY AAX_ePlugInCategory_PitchShift
    AU_MAIN_TYPE "kAudioUnitType_Effect"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "This plugin listens to its input to learn a sound"
)

# Generate JUCE header
juce_generate_juce_header(PitchFlattener)

target_sources(PitchFlattener
    PRIVATE
        Source/PluginProcessor.cpp
        Source/DiagnosticLog.cpp
        Source/SoundCodec.cpp
        Source/PluginEditor.cpp
        Source/PitchDisplay.cpp
        Source/FlightView.cpp
        Source/Banner.cpp
        Source/Hud.cpp
        Source/Framebuffer.cpp
        Source/HudControls.cpp
        Source/PitchFlattenerLookAndFeel.cpp
        Source/AboutWindow.cpp
        Source/UpdatePopup.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/SammyJUpdateCheck.cpp
        ${PF_CORE_SOURCES}
        ${PF_ENGINE_SOURCES}
)

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

target_include_directories(PitchFlattener
    PRIVATE
        ${RUBBERBAND_ROOT}
        ${RUBBERBAND_ROOT}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_link_libraries(PitchFlattener
    PRIVATE
        juce::juce_audio_utils
        PitchFlattenerBinaryData
    PUBLIC
        ${RUBBERBAND_LIB}
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if(WIN32)
    # Windows specific settings for VST3
    set_target_properties(PitchFlattener_VST3 PROPERTIES
        SUFFIX ".vst3"
        PREFIX ""
    )
endif()

target_compile_features(PitchFlattener PRIVATE cxx_std_20)

# Build universal binary on macOS
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures for macOS")
    set_target_properties(PitchFlattener PROPERTIES
        OSX_ARCHITECTURES "arm64;x86_64"
    )
endif()

if(PITCHFLATTENER_BUILD_TESTS)
    # The plug-in's tests (Tests/PitchFlattenerTests.cpp, Tests/EngineTests.cpp): the locator, the
    # engine driven through real process() calls, the processor and the editor. They link the
    # plug-in's shared code library, so they test exactly the code that ships; that library links
    # the JUCE modules privately, so their headers and the generated JuceHeader.h are added here by
    # hand. The program lands where a console app would (PitchFlattenerTests_artefacts/<config>).
    add_executable(PitchFlattenerTests Tests/PitchFlattenerTests.cpp Tests/EngineTests.cpp)
    target_include_directories(PitchFlattenerTests
        PRIVATE
            Source
            Tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../common
            "$<TARGET_PROPERTY:PitchFlattener,JUCE_GENERATED_SOURCES_DIRECTORY>"
            "$<TARGET_PROPERTY:juce::juce_core,INTERFACE_INCLUDE_DIRECTORIES>"
            "$<TARGET_PROPERTY:PitchFlattenerBinaryData,INTERFACE_INCLUDE_DIRECTORIES>"
    )
    # The shared code target exports only the JucePlugin_* values; the test is none of the formats.
    target_compile_definitions(PitchFlattenerTests
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
    add_dependencies(PitchFlattenerTests PitchFlattener)
    target_link_libraries(PitchFlattenerTests
        PRIVATE
            PitchFlattener
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
    target_compile_features(PitchFlattenerTests PRIVATE cxx_std_20)
    set_target_properties(PitchFlattenerTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/PitchFlattenerTests_artefacts/$<CONFIG>")

    # The core on its own: no JUCE, sounds whose pitch is known.
    add_executable(PitchFlattenerCoreTests Tests/CoreTests.cpp ${PF_CORE_SOURCES})
    target_include_directories(PitchFlattenerCoreTests PRIVATE Source ${RUBBERBAND_ROOT} ${RUBBERBAND_ROOT}/src)
    target_link_libraries(PitchFlattenerCoreTests PRIVATE ${RUBBERBAND_LIB})
    if(APPLE)
        target_link_libraries(PitchFlattenerCoreTests PRIVATE "-framework Accelerate")
    endif()
    target_compile_features(PitchFlattenerCoreTests PRIVATE cxx_std_20)
    # Its own warning set, as in Tests/build_core_tests.sh (the core is checked against that).
    if(MSVC)
        target_compile_options(PitchFlattenerCoreTests PRIVATE /W4)
    else()
        target_compile_options(PitchFlattenerCoreTests PRIVATE -Wall -Wextra)
    endif()
endif()

# Post-build installation is handled by JUCE's COPY_PLUGIN_AFTER_BUILD flag

# The stretch library uses the system FFT on macOS.
if(APPLE)
    target_link_libraries(PitchFlattener PUBLIC "-framework Accelerate")
endif()
