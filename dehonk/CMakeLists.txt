cmake_minimum_required(VERSION 3.22)

# Must be set before project(). Without an explicit deployment target the binary would require the
# macOS version of whichever machine built it. Forced only when empty, so an explicit
# -DCMAKE_OSX_DEPLOYMENT_TARGET=... still wins.
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# The version lives here and nowhere else; the release workflow checks the tag against it.
project(DeHonk VERSION 0.0.4)

# The copyright year is the year it was built, so it never goes stale.
string(TIMESTAMP COPYRIGHT_YEAR "%Y")
add_compile_definitions(COPYRIGHT_YEAR="${COPYRIGHT_YEAR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Windows: link the C++ runtime statically, so the plug-in doesn't depend on the host machine
# having a recent enough Visual C++ redistributable (the installer ships none).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Plug-in formats. AAX is added by -DSAMMYJ_AAX=ON (cmake/AAX.cmake).
set(DEHONK_FORMATS "AU;VST3;Standalone" CACHE STRING "Plugin formats to build")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
sammyj_enable_aax(DEHONK_FORMATS)

# The manufacturer/plug-in codes and bundle ID must never change: hosts use them to find
# existing instances in saved sessions.
juce_add_plugin(DeHonk
    VERSION ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE SJus
    PLUGIN_CODE DHnk
    FORMATS ${DEHONK_FORMATS}
    # The name hosts list it under and the bundles' file name, like the other Sammy J plug-ins. The
    # first test build was "De-Honk": the codes and the bundle ID below are the same, so its sessions
    # still open, and the installers remove the bundles with the old name.
    PRODUCT_NAME "SammyJs De-Honk"
    COMPANY_NAME "Sammy J"
    COMPANY_COPYRIGHT "Copyright ${COPYRIGHT_YEAR} Samuel Justice"
    COMPANY_WEBSITE "https://sweetjusticesound.com"
    BUNDLE_ID com.samueljustice.dehonk
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES "Fx" "EQ"
    # Where Pro Tools lists it in its plug-in menus (without this it goes under "Other").
    AAX_CATEGORY AAX_ePlugInCategory_EQ
    AU_MAIN_TYPE "kAudioUnitType_Effect"
    # The Standalone app needs an audio input, which macOS grants only with a usage description.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "De-Honk processes the audio input in real time."
)

if(APPLE)
    set_target_properties(DeHonk PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
        XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

juce_generate_juce_header(DeHonk)

target_sources(DeHonk
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/PresetManager.cpp
        Source/DeHonkLookAndFeel.cpp
        Source/Widgets.cpp
        Source/Overlays.cpp
        # The update check shared by every Sammy J plug-in (common/).
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/SammyJUpdateCheck.cpp
        Source/DisplayComponent.cpp
        Source/scene/BandView.cpp
        # The pond picture, drawn in software (Source/scene/PondScene.h is its contract).
        Source/scene/PondScene.cpp
        # The DSP core: plain C++, shared with the offline harness in Tests/.
        dsp/DeHonkCore.cpp
)

target_include_directories(DeHonk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dsp ${CMAKE_CURRENT_SOURCE_DIR}/../common)

juce_add_binary_data(DeHonkBinaryData
    SOURCES
        LICENSE
        Resources/Fonts/AzeretMono-Regular.ttf
        Resources/Fonts/AzeretMono-Medium.ttf
        Resources/Fonts/AzeretMono-Bold.ttf
)

target_compile_definitions(DeHonk
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# Build timestamp (passed from CI, otherwise unset).
if(BUILD_TIMESTAMP)
    target_compile_definitions(DeHonk PUBLIC BUILD_TIMESTAMP="${BUILD_TIMESTAMP}")
endif()

target_link_libraries(DeHonk
    PRIVATE
        DeHonkBinaryData
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# A console host for the plug-in's processor (Tests/host/HostCheck.cpp): accepted layouts, the
# LFE per layout, odd buffers, layout changes and the AAX stem formats. Not part of the default
# build; build and run it with
#   cmake --build build --config Release --target DeHonkHostCheck
add_executable(DeHonkHostCheck EXCLUDE_FROM_ALL Tests/host/HostCheck.cpp)
target_link_libraries(DeHonkHostCheck PRIVATE DeHonk)
target_include_directories(DeHonkHostCheck PRIVATE $<TARGET_PROPERTY:DeHonk,INCLUDE_DIRECTORIES>)
target_compile_definitions(DeHonkHostCheck PRIVATE $<TARGET_PROPERTY:DeHonk,COMPILE_DEFINITIONS>)
set_target_properties(DeHonkHostCheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/DeHonkHostCheck_artefacts")
