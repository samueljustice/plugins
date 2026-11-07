cmake_minimum_required(VERSION 3.21)

# The one place the version lives. Release builds pass -DSOUNDADVICE_VERSION=x.y.z (from the
# soundadvice-vx.y.z tag); it ends up in the bundles, the installers and the footer.
set(SOUNDADVICE_VERSION "1.0.4" CACHE STRING "Sound Advice version (major.minor.patch)")
project(SoundAdvice VERSION ${SOUNDADVICE_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Universal binary, and run on macOS 11 and later rather than whatever the CI runner has.
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

add_compile_definitions(
    PLUGIN_VERSION="${PROJECT_VERSION}"
    JUCE_VST3_CAN_REPLACE_VST2=0
)

option(SOUNDADVICE_TESTS "Build the quote library test" ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

# AU and VST3 for DAWs; the standalone app is handy for a second screen.
set(SOUNDADVICE_FORMATS "AU;VST3;Standalone" CACHE STRING "Plugin formats to build")

juce_add_plugin(SoundAdvice
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "Sweet Justice"
    COMPANY_WEBSITE "https://sweetjusticesound.com"
    BUNDLE_ID "com.samueljustice.soundadvice"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE SJus
    PLUGIN_CODE SAdv
    FORMATS ${SOUNDADVICE_FORMATS}
    PRODUCT_NAME "Sound Advice"
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Tools
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"   # app icon (.icns / .ico are made from it); see tools/Preview.cpp --icon
)

if(APPLE)
    set_target_properties(SoundAdvice PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
        XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

target_sources(SoundAdvice
    PRIVATE
    Source/QuoteLibrary.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/QuoteView.cpp
)

# Our own standalone app (Source/StandaloneApp.cpp): the page in a native window, no audio device.
target_sources(SoundAdvice_Standalone PRIVATE Source/StandaloneApp.cpp)

target_compile_definitions(SoundAdvice
    PUBLIC
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

# The quote database and the typeface. Resources/quotes.json is built from data/*.json by
# tools/build_quotes.py; edit the sources, then run the script.
juce_add_binary_data(SoundAdviceBinaryData
    SOURCES
    Resources/quotes.json
    Resources/Fonts/CormorantGaramond-Medium.ttf
    Resources/Fonts/CormorantGaramond-MediumItalic.ttf
    Resources/Fonts/CormorantGaramond-SemiBold.ttf
)

juce_generate_juce_header(SoundAdvice)

target_link_libraries(SoundAdvice
    PRIVATE
    juce::juce_audio_utils
    SoundAdviceBinaryData
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Offline test: loads the embedded database, checks every entry and category, and exercises
# the no-repeat picker.
if(SOUNDADVICE_TESTS)
    juce_add_console_app(SoundAdviceTests PRODUCT_NAME "SoundAdviceTests")
    juce_generate_juce_header(SoundAdviceTests)
    target_sources(SoundAdviceTests PRIVATE Tests/QuoteLibraryTest.cpp Source/QuoteLibrary.cpp)
    target_include_directories(SoundAdviceTests PRIVATE Source)
    target_compile_definitions(SoundAdviceTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(SoundAdviceTests
        PRIVATE juce::juce_core juce::juce_events SoundAdviceBinaryData
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()

# Offscreen preview renderer (off by default): writes PNGs of the editor. See tools/Preview.cpp.
option(SOUNDADVICE_TOOLS "Build the preview renderer" OFF)
if(SOUNDADVICE_TOOLS)
    juce_add_console_app(SoundAdvicePreview PRODUCT_NAME "SoundAdvicePreview")
    juce_generate_juce_header(SoundAdvicePreview)
    target_sources(SoundAdvicePreview PRIVATE tools/Preview.cpp Source/QuoteLibrary.cpp Source/PluginProcessor.cpp
                   Source/PluginEditor.cpp Source/QuoteView.cpp)
    target_include_directories(SoundAdvicePreview PRIVATE Source)
    target_compile_definitions(SoundAdvicePreview PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="Sound Advice")
    target_link_libraries(SoundAdvicePreview
        PRIVATE juce::juce_audio_utils SoundAdviceBinaryData
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
