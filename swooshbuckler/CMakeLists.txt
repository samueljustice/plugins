cmake_minimum_required(VERSION 3.21)

# The one place the version lives. Release builds pass -DSWOOSHBUCKLER_VERSION=x.y.z (from the
# swooshbuckler-vx.y.z tag); it ends up in the About panel, the bundles, the installers and the app.
set(SWOOSHBUCKLER_VERSION "1.4.2" CACHE STRING "Swooshbuckler version (major.minor.patch)")
project(Swooshbuckler VERSION ${SWOOSHBUCKLER_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Universal Binary Support
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

add_compile_definitions(
    PLUGIN_VERSION="${PROJECT_VERSION}"
    JUCE_VST3_CAN_REPLACE_VST2=0
)

if(BUILD_TIMESTAMP)
    add_compile_definitions(BUILD_TIMESTAMP="${BUILD_TIMESTAMP}")
endif()

# Short commit hash for the About panel (empty outside a git checkout).
if(NOT BUILD_COMMIT)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        OUTPUT_VARIABLE BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
endif()
if(BUILD_COMMIT)
    add_compile_definitions(BUILD_COMMIT="${BUILD_COMMIT}")
endif()

# Standalone app first; AU and VST3 for DAWs.
set(SWOOSHBUCKLER_FORMATS "Standalone;AU;VST3" CACHE STRING "Plugin formats to build")
option(SWOOSHBUCKLER_TESTS "Build the offline tests" ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
sammyj_enable_aax(SWOOSHBUCKLER_FORMATS)

juce_add_plugin(Swooshbuckler
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "Sammy J"
    COMPANY_WEBSITE "https://sweetjusticesound.com"
    BUNDLE_ID "com.samueljustice.swooshbuckler"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE SJus
    PLUGIN_CODE Swsh
    FORMATS ${SWOOSHBUCKLER_FORMATS}
    PRODUCT_NAME "Swooshbuckler"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"   # app icon (.icns / .ico are made from it)
)

if(APPLE)
    set_target_properties(Swooshbuckler PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
        XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

set(SWOOSHBUCKLER_DSP_SOURCES
    Source/Cargo.cpp
    Source/MasterProcessor.cpp
    Source/TapeRecorder.cpp
    Source/SampleClassifier.cpp
    Source/RigEngine.cpp
    Source/SpectralMorph.cpp
    Source/Modulators.cpp
    Source/PitchShifter.cpp
    Source/SpaceReverb.cpp
)

target_sources(Swooshbuckler
    PRIVATE
    ${SWOOSHBUCKLER_DSP_SOURCES}
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/LookAndFeel.cpp
    Source/SeaVisualiser.cpp
    Source/CargoHoldView.cpp
    Source/MorphPad.cpp
    Source/LogoOutline.cpp
    Source/ModPanel.cpp
    Source/UpdatePopup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/SammyJUpdateCheck.cpp
)

# The shared "new version available" check (common/).
target_include_directories(Swooshbuckler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)

# Our own standalone app (Source/StandaloneApp.cpp): a normal native window without JUCE's Options bar.
target_sources(Swooshbuckler_Standalone PRIVATE Source/StandaloneApp.cpp)

target_compile_definitions(Swooshbuckler
    PUBLIC
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_MP3AUDIOFORMAT=1
)

juce_add_binary_data(SwooshbucklerBinaryData
    SOURCES
    Resources/Fonts/PirataOne-Regular.ttf
    Resources/Fonts/IBMPlexSansCondensed-Regular.ttf
    Resources/Fonts/IBMPlexSansCondensed-Medium.ttf
    Resources/Fonts/IBMPlexSansCondensed-SemiBold.ttf
    Resources/sj-logo.svg
)

juce_generate_juce_header(Swooshbuckler)

target_link_libraries(Swooshbuckler
    PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_opengl
    SwooshbucklerBinaryData
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Offline tests (rig, cargo, classifier).
if(SWOOSHBUCKLER_TESTS)
    # Cargo test: sample tracks, editing, mixer, sessions, the tape recorder, presets.
    juce_add_console_app(SwooshbucklerCargo PRODUCT_NAME "SwooshbucklerCargo")
    juce_generate_juce_header(SwooshbucklerCargo)
    target_sources(SwooshbucklerCargo PRIVATE Tests/CargoTest.cpp ${SWOOSHBUCKLER_DSP_SOURCES})
    target_include_directories(SwooshbucklerCargo PRIVATE Source)
    target_compile_definitions(SwooshbucklerCargo PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1)
    target_link_libraries(SwooshbucklerCargo
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # Rig test: the whoosh rig with synthetic tracks for 60 s.
    juce_add_console_app(SwooshbucklerRig PRODUCT_NAME "SwooshbucklerRig")
    juce_generate_juce_header(SwooshbucklerRig)
    target_sources(SwooshbucklerRig PRIVATE Tests/RigTest.cpp ${SWOOSHBUCKLER_DSP_SOURCES})
    target_include_directories(SwooshbucklerRig PRIVATE Source)
    target_compile_definitions(SwooshbucklerRig PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1)
    target_link_libraries(SwooshbucklerRig
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # Classifier test: synthesises one sample per role and checks the detected role and label.
    juce_add_console_app(SwooshbucklerClassify PRODUCT_NAME "SwooshbucklerClassify")
    juce_generate_juce_header(SwooshbucklerClassify)
    target_sources(SwooshbucklerClassify PRIVATE Tests/ClassifierTest.cpp ${SWOOSHBUCKLER_DSP_SOURCES})
    target_include_directories(SwooshbucklerClassify PRIVATE Source)
    target_compile_definitions(SwooshbucklerClassify PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1)
    target_link_libraries(SwooshbucklerClassify
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # Pitch test: the per-track pitch shifter (pitch, level, length, transients, bypass, chunking, speed).
    juce_add_console_app(SwooshbucklerPitch PRODUCT_NAME "SwooshbucklerPitch")
    juce_generate_juce_header(SwooshbucklerPitch)
    target_sources(SwooshbucklerPitch PRIVATE Tests/PitchTest.cpp Source/PitchShifter.cpp)
    target_include_directories(SwooshbucklerPitch PRIVATE Source)
    target_compile_definitions(SwooshbucklerPitch PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(SwooshbucklerPitch
        PRIVATE juce::juce_dsp juce::juce_audio_formats
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # Master test: the output stage (Inflate, Push, the true-peak limiter and its release, the gain reduction
    # hold) and the downsampler of the monitor path. Plain C++: neither class uses JUCE.
    add_executable(SwooshbucklerMaster Tests/MasterTest.cpp Source/MasterProcessor.cpp)
    target_include_directories(SwooshbucklerMaster PRIVATE Source)
    target_compile_features(SwooshbucklerMaster PRIVATE cxx_std_20)
    set_target_properties(SwooshbucklerMaster PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/SwooshbucklerMaster_artefacts/$<CONFIG>")
endif()

# Artwork tools (off by default): renders Resources/icon.png. See tools/IconRender.cpp.
option(SWOOSHBUCKLER_TOOLS "Build the artwork tools" OFF)
if(SWOOSHBUCKLER_TOOLS)
    juce_add_console_app(SwooshbucklerIcon PRODUCT_NAME "SwooshbucklerIcon")
    juce_generate_juce_header(SwooshbucklerIcon)
    target_sources(SwooshbucklerIcon PRIVATE tools/IconRender.cpp)
    target_compile_definitions(SwooshbucklerIcon PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(SwooshbucklerIcon
        PRIVATE juce::juce_gui_basics
        PUBLIC juce::juce_recommended_config_flags)

    # Knob audit: sweeps a parameter through the real plug-in offline and measures each step (tools/KnobAudit.cpp).
    juce_add_console_app(SwooshbucklerAudit PRODUCT_NAME "SwooshbucklerAudit")
    target_sources(SwooshbucklerAudit PRIVATE tools/KnobAudit.cpp)
    target_include_directories(SwooshbucklerAudit PRIVATE Source ${CMAKE_CURRENT_BINARY_DIR}/Swooshbuckler_artefacts/JuceLibraryCode $<TARGET_PROPERTY:Swooshbuckler,INCLUDE_DIRECTORIES>)
    target_compile_definitions(SwooshbucklerAudit PRIVATE $<TARGET_PROPERTY:Swooshbuckler,COMPILE_DEFINITIONS>)
    target_link_libraries(SwooshbucklerAudit PRIVATE Swooshbuckler)
endif()
