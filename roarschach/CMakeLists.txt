cmake_minimum_required(VERSION 3.22)

# Minimum macOS. Without this the binaries take the build machine's SDK version as their
# minimum and JUCE's runtime OS checks (@available) are compiled out. Must come before project().
# (FORCE only when empty, so an explicit -DCMAKE_OSX_DEPLOYMENT_TARGET=... still wins.)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Universal binary.
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

# The one place the version lives. Release builds pass -DROARSCHACH_VERSION=x.y.z (from the
# roarschach-vx.y.z tag); it ends up in the About panel, the bundles and the installers.
set(ROARSCHACH_VERSION "1.1.0" CACHE STRING "Roarschach version (major.minor.patch)")
project(Roarschach VERSION ${ROARSCHACH_VERSION})

# The copyright year is the year it was built, so it never goes stale.
string(TIMESTAMP COPYRIGHT_YEAR "%Y")
add_compile_definitions(COPYRIGHT_YEAR="${COPYRIGHT_YEAR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Windows: link the C++ runtime statically, so the plug-in doesn't depend on the
# VC++ redistributable being installed (or on an older msvcp140.dll a host ships).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
    PLUGIN_VERSION="${PROJECT_VERSION}"
    PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    JUCE_VST3_CAN_REPLACE_VST2=0
)

# Build timestamp for the About panel (passed from CI; absent in local builds).
if(BUILD_TIMESTAMP)
    add_compile_definitions(BUILD_TIMESTAMP="${BUILD_TIMESTAMP}")
endif()

# Plugin formats; pass -DROARSCHACH_FORMATS="AU;VST3;Standalone" for a standalone app.
set(ROARSCHACH_FORMATS "AU;VST3" CACHE STRING "Plugin formats to build")
option(ROARSCHACH_TESTS "Build the offline tests" OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
sammyj_enable_aax(ROARSCHACH_FORMATS)

# The manufacturer/plugin codes and bundle ID must never change: hosts use them
# to find existing instances in saved sessions.
juce_add_plugin(Roarschach
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "Sammy J"
    COMPANY_COPYRIGHT "Copyright ${COPYRIGHT_YEAR} Samuel Justice"
    COMPANY_WEBSITE "https://sweetjusticesound.com"
    BUNDLE_ID "com.samueljustice.roarschach"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE SJus
    PLUGIN_CODE Rrsc
    FORMATS ${ROARSCHACH_FORMATS}
    PRODUCT_NAME "Roarschach"
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx "Pitch Shift"
    # Only used by a Standalone build, which takes its input from a microphone.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Roarschach processes the audio input in real time."
)

if(APPLE)
    set_target_properties(Roarschach PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
        XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

target_sources(Roarschach
    PRIVATE
    Source/RoarschachLookAndFeel.cpp
    Source/AboutWindow.cpp
    Source/PresetManager.cpp
    Source/Dice.cpp
    Source/FactoryPresets.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/SignalFlowStrip.cpp
    Source/MonsterView.cpp
    Source/MonsterRenderer.cpp
    Source/MonsterShaders.cpp
    Source/CreatureMotion.cpp
    Source/NoiseTexture.cpp
    Source/CreatureEvolution.cpp
    Source/LogoSdf.cpp
    Source/dsp/PitchDetector.cpp
    Source/dsp/CreatureChain.cpp
    Source/dsp/common/HilbertPair.cpp
    Source/dsp/common/Oversampler2x.cpp
    Source/dsp/common/VoiceStream.cpp
    Source/dsp/common/ZeroLatencyConvolver.cpp
    Source/dsp/modules/InModule.cpp
    Source/dsp/modules/PitchModule.cpp
    Source/dsp/modules/CleanModule.cpp
    Source/dsp/modules/DeessModule.cpp
    Source/dsp/modules/DetailModule.cpp
    Source/dsp/modules/VocoderModule.cpp
    Source/dsp/modules/RingModule.cpp
    Source/dsp/modules/ShimmerModule.cpp
    Source/dsp/modules/GrowlModule.cpp
    Source/dsp/modules/SwirlModule.cpp
    Source/dsp/modules/RoarModule.cpp
    Source/dsp/modules/EnsembleModule.cpp
    Source/dsp/modules/TameModule.cpp
    Source/dsp/modules/SubModule.cpp
    Source/dsp/modules/OvertoneModule.cpp
    Source/dsp/modules/ChamberModule.cpp
    Source/dsp/modules/ChamberRooms.cpp
    Source/dsp/modules/OutputModule.cpp
)

# The DSP modules compare floats exactly on purpose (settled glides, unchanged settings).
file(GLOB ROARSCHACH_DSP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/modules/*.cpp)
# Clang/GCC only: MSVC rejects the flag (D8021) and has no such warning anyway.
if(NOT MSVC)
    set_source_files_properties(${ROARSCHACH_DSP_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-float-equal")
endif()

target_compile_definitions(Roarschach
    PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

# The licence text for the About panel, the typeface and the logo.
juce_add_binary_data(RoarschachBinaryData
    SOURCES
    LICENSE
    Resources/Fonts/AzeretMono-Regular.ttf
    Resources/Fonts/AzeretMono-Medium.ttf
    Resources/Fonts/AzeretMono-Bold.ttf
    Resources/sj-logo.svg
)

juce_generate_juce_header(Roarschach)

# CHAMBER's bundled rooms (optional). The room captures are not part of this repository: they are
# read from -DROARSCHACH_IR_DIR at build time, processed and encrypted by RoarschachIrPack with the
# key in the ROARSCHACH_IR_KEY environment variable (64 hex digits), and only the encrypted blob is
# embedded; the plug-in decrypts it in memory. Without both, CHAMBER uses its modelled rooms (the
# same list, so presets still load).
#
# The key is never kept in CMakeCache.txt: it is read from the environment (a -DROARSCHACH_IR_KEY
# given anyway is used once and removed from the cache). It is written only to
# <build>/chamber_irs/ir.key (for the packer and for later re-configures of this build folder) and,
# masked, to <build>/chamber_irs/RoarschachIrKey.h; CI deletes both after its last build step.
# A re-configure after the captures folder is gone reuses the blob packed earlier in this build
# folder; with neither the key nor that blob, configure fails rather than quietly switching rooms.
set(ROARSCHACH_IR_DIR "" CACHE PATH "Folder with CHAMBER's room captures (never committed); empty = modelled rooms")
set(ROARSCHACH_IR_OUT "${CMAKE_CURRENT_BINARY_DIR}/chamber_irs")
set(ROARSCHACH_IR_BLOB "${ROARSCHACH_IR_OUT}/chamber_irs.bin")
set(ROARSCHACH_IR_HEADER "${ROARSCHACH_IR_OUT}/RoarschachIrKey.h")

set(_roarschach_ir_key "$ENV{ROARSCHACH_IR_KEY}")
if(DEFINED CACHE{ROARSCHACH_IR_KEY})
    if(NOT _roarschach_ir_key)
        set(_roarschach_ir_key "$CACHE{ROARSCHACH_IR_KEY}")
    endif()
    unset(ROARSCHACH_IR_KEY CACHE)
endif()
if(NOT _roarschach_ir_key AND ROARSCHACH_IR_DIR AND EXISTS "${ROARSCHACH_IR_OUT}/ir.key")
    file(READ "${ROARSCHACH_IR_OUT}/ir.key" _roarschach_ir_key)   # an earlier configure of this folder
    string(STRIP "${_roarschach_ir_key}" _roarschach_ir_key)
endif()

set(ROARSCHACH_BUNDLES_ROOMS OFF)
if(ROARSCHACH_IR_DIR AND _roarschach_ir_key)
    string(LENGTH "${_roarschach_ir_key}" _roarschach_ir_key_length)
    if(NOT _roarschach_ir_key_length EQUAL 64 OR NOT _roarschach_ir_key MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "ROARSCHACH_IR_KEY must be 64 hex digits")   # (never print the key)
    endif()
    file(MAKE_DIRECTORY "${ROARSCHACH_IR_OUT}")

    # The key, for the packer (a file, so it is not on the packer's command line or in a log), and
    # masked for the plug-in (irpack::keyMask; the packer checks the two agree). Build folder only.
    file(WRITE "${ROARSCHACH_IR_OUT}/ir.key.tmp" "${_roarschach_ir_key}")
    configure_file("${ROARSCHACH_IR_OUT}/ir.key.tmp" "${ROARSCHACH_IR_OUT}/ir.key" COPYONLY)
    file(REMOVE "${ROARSCHACH_IR_OUT}/ir.key.tmp")
    string(TOLOWER "${_roarschach_ir_key}" _roarschach_ir_key_lower)
    set(_roarschach_ir_key_bytes "")
    foreach(i RANGE 0 31)
        math(EXPR at "${i} * 2")
        string(SUBSTRING "${_roarschach_ir_key_lower}" ${at} 2 byte)
        math(EXPR masked "0x${byte} ^ ((${i} * 167 + 91) % 256)" OUTPUT_FORMAT HEXADECIMAL)
        string(APPEND _roarschach_ir_key_bytes "${masked}, ")
    endforeach()
    file(WRITE "${ROARSCHACH_IR_HEADER}.tmp"
         "#pragma once\n// Generated from the ROARSCHACH_IR_KEY environment variable at configure time. Never commit.\n"
         "static const unsigned char roarschach_ir_key_masked[32] = { ${_roarschach_ir_key_bytes}};\n")
    configure_file("${ROARSCHACH_IR_HEADER}.tmp" "${ROARSCHACH_IR_HEADER}" COPYONLY)
    file(REMOVE "${ROARSCHACH_IR_HEADER}.tmp")

    if(IS_DIRECTORY "${ROARSCHACH_IR_DIR}")
        add_executable(RoarschachIrPack Tools/RoarschachIrPack.cpp)
        target_include_directories(RoarschachIrPack PRIVATE Source)
        target_compile_features(RoarschachIrPack PRIVATE cxx_std_17)

        file(GLOB ROARSCHACH_IR_CAPTURES CONFIGURE_DEPENDS
             "${ROARSCHACH_IR_DIR}/*.wav" "${ROARSCHACH_IR_DIR}/*.WAV" "${ROARSCHACH_IR_DIR}/manifest.txt"
             "${ROARSCHACH_IR_DIR}/irs/*.wav" "${ROARSCHACH_IR_DIR}/irs/*.WAV" "${ROARSCHACH_IR_DIR}/irs/manifest.txt")
        add_custom_command(OUTPUT "${ROARSCHACH_IR_BLOB}"
            COMMAND RoarschachIrPack --dir "${ROARSCHACH_IR_DIR}" --key-file "${ROARSCHACH_IR_OUT}/ir.key"
                    --masked-key "${ROARSCHACH_IR_HEADER}" --out "${ROARSCHACH_IR_BLOB}"
            DEPENDS RoarschachIrPack ${ROARSCHACH_IR_CAPTURES} "${ROARSCHACH_IR_OUT}/ir.key" "${ROARSCHACH_IR_HEADER}"
            COMMENT "Packing CHAMBER's rooms (encrypted)"
            VERBATIM)
    elseif(EXISTS "${ROARSCHACH_IR_BLOB}")
        message(WARNING "ROARSCHACH_IR_DIR is not a folder any more: reusing the rooms packed earlier in this build folder")
    else()
        message(FATAL_ERROR "ROARSCHACH_IR_DIR is not a folder (and no rooms were packed in this build folder before)")
    endif()
    set(ROARSCHACH_BUNDLES_ROOMS ON)
elseif(ROARSCHACH_IR_DIR AND EXISTS "${ROARSCHACH_IR_BLOB}" AND EXISTS "${ROARSCHACH_IR_HEADER}")
    message(WARNING "No ROARSCHACH_IR_KEY: reusing the rooms packed earlier in this build folder")
    set(ROARSCHACH_BUNDLES_ROOMS ON)
elseif(ROARSCHACH_IR_DIR)
    message(FATAL_ERROR "ROARSCHACH_IR_DIR is set but the key is not (set the ROARSCHACH_IR_KEY environment "
                        "variable, or clear ROARSCHACH_IR_DIR to build the modelled rooms). In CI, every build "
                        "step must run before the step that removes the room captures and key.")
endif()
unset(_roarschach_ir_key)
unset(_roarschach_ir_key_lower)
unset(_roarschach_ir_key_bytes)

if(ROARSCHACH_BUNDLES_ROOMS)
    juce_add_binary_data(RoarschachIrData
        NAMESPACE RoarschachIrData
        HEADER_NAME RoarschachIrData.h
        SOURCES "${ROARSCHACH_IR_BLOB}")
    target_link_libraries(Roarschach PRIVATE RoarschachIrData)
    target_include_directories(Roarschach PRIVATE "${ROARSCHACH_IR_OUT}")
    target_compile_definitions(Roarschach PRIVATE ROARSCHACH_HAS_IR_BLOB=1)
    message(STATUS "Roarschach: CHAMBER: bundled rooms (encrypted)")
else()
    message(STATUS "Roarschach: CHAMBER: modelled rooms (set ROARSCHACH_IR_DIR and the ROARSCHACH_IR_KEY environment variable to bundle the captures)")
endif()

target_link_libraries(Roarschach
    PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_opengl
    RoarschachBinaryData
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Offline tests: drive the real processor at several sample rates and block sizes (see
# Tests/RoarschachTests.cpp). The test links the plug-in's shared code library (the
# Roarschach target itself, which the AU/VST3 wrappers link too), so it tests exactly the code
# that ships, compiled once, with the same JucePlugin_* definitions. That library links the JUCE
# modules privately, so their headers and the generated JuceHeader.h are added here by hand;
# the module sources are not compiled a second time.
if(ROARSCHACH_TESTS)
    add_executable(RoarschachTests Tests/RoarschachTests.cpp)
    target_include_directories(RoarschachTests
        PRIVATE
        Source
        "$<TARGET_PROPERTY:Roarschach,JUCE_GENERATED_SOURCES_DIRECTORY>"
        "$<TARGET_PROPERTY:juce::juce_core,INTERFACE_INCLUDE_DIRECTORIES>"
    )
    # The shared code target exports only the JucePlugin_* values; the per-format switches
    # stay private to it. The test is none of the formats.
    target_compile_definitions(RoarschachTests
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
    add_dependencies(RoarschachTests Roarschach)
    target_link_libraries(RoarschachTests
        PRIVATE
        Roarschach
        RoarschachBinaryData
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
    # A build that embeds the rooms must decode them: the tests assert it (a key or layout
    # mismatch would otherwise fall back to the modelled rooms without a sound).
    if(ROARSCHACH_BUNDLES_ROOMS)
        target_compile_definitions(RoarschachTests PRIVATE ROARSCHACH_EXPECT_BUNDLED_ROOMS=1)
    endif()
endif()
