cmake_minimum_required(VERSION 3.22)

# Must be set before project(). Without an explicit deployment target the binary
# would require the macOS version of whichever machine built it. Forced only when empty:
# build trees configured before this existed cache an empty value, which would otherwise
# win; an explicit -DCMAKE_OSX_DEPLOYMENT_TARGET=... still does.
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(Reversinator VERSION 2.0.3)

# The copyright year is the year it was built, so it never goes stale.
string(TIMESTAMP COPYRIGHT_YEAR "%Y")
add_compile_definitions(COPYRIGHT_YEAR="${COPYRIGHT_YEAR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Windows: link the C++ runtime statically, so the plugin doesn't depend on the host
# machine having a recent enough Visual C++ redistributable (the installer ships none).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Plugin formats; pass -DREVERSINATOR_FORMATS="AU;VST3;Standalone" for a standalone app.
set(REVERSINATOR_FORMATS "AU;VST3" CACHE STRING "Plugin formats to build")
option(REVERSINATOR_BUILD_TESTS "Build the offline DSP test runner" OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
sammyj_enable_aax(REVERSINATOR_FORMATS)

# The manufacturer/plugin codes and bundle ID must never change: hosts use them
# to find existing instances in saved sessions.
juce_add_plugin(Reversinator
    VERSION ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE Sjus
    PLUGIN_CODE Revr
    FORMATS ${REVERSINATOR_FORMATS}
    PRODUCT_NAME "SammyJs Reversinator"
    COMPANY_NAME "Sammy J"
    COMPANY_COPYRIGHT "Copyright ${COPYRIGHT_YEAR} Samuel Justice"
    COMPANY_WEBSITE "https://www.sweetjusticesound.com"
    COMPANY_EMAIL "sam@sweetjusticesound.com"
    BUNDLE_ID com.samueljustice.reversinator
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES "Fx" "Delay"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
)

juce_generate_juce_header(Reversinator)

target_sources(Reversinator
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Controls.cpp
        Source/SnippetDisplay.cpp
        Source/AboutPanel.cpp
        Source/ReverseEngine.cpp
)

juce_add_binary_data(ReversinatorBinaryData
    SOURCES
        LICENSE
        Resources/Fonts/AzeretMono-Regular.ttf
        Resources/Fonts/AzeretMono-Medium.ttf
        Resources/Fonts/AzeretMono-Bold.ttf
)

target_compile_definitions(Reversinator
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# Build timestamp (passed from CI, otherwise the About box shows the compile time)
if(BUILD_TIMESTAMP)
    target_compile_definitions(Reversinator PUBLIC BUILD_TIMESTAMP="${BUILD_TIMESTAMP}")
endif()

target_link_libraries(Reversinator
    PRIVATE
        ReversinatorBinaryData
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if(REVERSINATOR_BUILD_TESTS)
    add_subdirectory(Tests)
endif()
