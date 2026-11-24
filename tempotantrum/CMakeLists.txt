cmake_minimum_required(VERSION 3.22)

# Minimum macOS. Without this the binaries take the build machine's SDK version as their
# minimum and JUCE's runtime OS checks (@available) are compiled out. Must come before project().
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

set(TEMPOTANTRUM_VERSION "0.1.0" CACHE STRING "Tempo Tantrum version (major.minor.patch)")
project(TempoTantrum VERSION ${TEMPOTANTRUM_VERSION})

# The copyright year is the year it was built, so it never goes stale.
string(TIMESTAMP COPYRIGHT_YEAR "%Y")
add_compile_definitions(COPYRIGHT_YEAR="${COPYRIGHT_YEAR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

option(TEMPOTANTRUM_TESTS "Build the core tests and the command-line tool" OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

# ---------------------------------------------------------------------------------------------
# The Pro Tools Scripting link (PTSL): Write to Pro Tools reads the session and moves the cursor
# through it. Avid's Pro Tools Scripting SDK is confidential and is never in this repository: give
# its unpacked folder as TEMPOTANTRUM_PTSL_SDK. Left unset, a folder ../SDK/PTSL_SDK_CPP.* beside
# the repository root is used if there is one. Set it empty to build without the link.
#
# With the SDK, Tools/ptsl_generate.py writes the numbers the client needs (field numbers,
# command numbers, the call's path) into a header in the build folder, and the client talks
# gRPC over HTTP/2 with nghttp2. Without it, a stand-in client says the build has no link, and
# Write to Pro Tools works through Accessibility alone.
if(NOT DEFINED TEMPOTANTRUM_PTSL_SDK)
    file(GLOB _tt_ptsl_found LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/../SDK/PTSL_SDK_CPP.*")
    list(SORT _tt_ptsl_found)
    list(REVERSE _tt_ptsl_found)
    set(_tt_ptsl_default "")

    foreach(_dir IN LISTS _tt_ptsl_found)
        if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/Source/PTSL.proto")
            set(_tt_ptsl_default "${_dir}")
            break()
        endif()
    endforeach()

    set(TEMPOTANTRUM_PTSL_SDK "${_tt_ptsl_default}" CACHE PATH "The unpacked Pro Tools Scripting SDK folder (empty: build without the link)")
endif()

set(TEMPOTANTRUM_HAS_PTSL 0)

if(TEMPOTANTRUM_PTSL_SDK)
    get_filename_component(TEMPOTANTRUM_PTSL_SDK "${TEMPOTANTRUM_PTSL_SDK}" ABSOLUTE)

    if(NOT EXISTS "${TEMPOTANTRUM_PTSL_SDK}/Source/PTSL.proto")
        message(FATAL_ERROR "TEMPOTANTRUM_PTSL_SDK is set to ${TEMPOTANTRUM_PTSL_SDK}, which holds no Source/PTSL.proto")
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(TEMPOTANTRUM_PTSL_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/PtslGenerated)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/Tools/ptsl_generate.py"
                "${TEMPOTANTRUM_PTSL_SDK}" "${TEMPOTANTRUM_PTSL_GENERATED_DIR}/PtslGenerated.h"
        RESULT_VARIABLE _tt_ptsl_result)

    if(NOT _tt_ptsl_result EQUAL 0)
        message(FATAL_ERROR "Could not read the Pro Tools Scripting SDK in ${TEMPOTANTRUM_PTSL_SDK}")
    endif()

    # Generated again when the SDK's definitions or the script change.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${TEMPOTANTRUM_PTSL_SDK}/Source/PTSL.proto"
        "${TEMPOTANTRUM_PTSL_SDK}/Source/PTSL_Versions.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Tools/ptsl_generate.py")

    # nghttp2 (MIT), a plain C HTTP/2 library, from its release tarball. Only its library sources
    # are compiled, as a static library here, so it builds for every architecture the app does.
    include(FetchContent)
    FetchContent_Declare(tt_nghttp2
        URL https://github.com/nghttp2/nghttp2/releases/download/v1.64.0/nghttp2-1.64.0.tar.gz
        URL_HASH SHA256=20e73f3cf9db3f05988996ac8b3a99ed529f4565ca91a49eb0550498e10621e8
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR no-cmake-here)
    FetchContent_MakeAvailable(tt_nghttp2)

    set(_tt_ng ${tt_nghttp2_SOURCE_DIR}/lib)
    set(_tt_ng_version_dir ${CMAKE_CURRENT_BINARY_DIR}/nghttp2-includes)
    set(PACKAGE_VERSION "1.64.0")
    set(PACKAGE_VERSION_NUM "0x014000")
    configure_file(${_tt_ng}/includes/nghttp2/nghttp2ver.h.in ${_tt_ng_version_dir}/nghttp2/nghttp2ver.h @ONLY)

    add_library(TempoTantrumNghttp2 STATIC
        ${_tt_ng}/nghttp2_pq.c ${_tt_ng}/nghttp2_map.c ${_tt_ng}/nghttp2_queue.c ${_tt_ng}/nghttp2_frame.c
        ${_tt_ng}/nghttp2_buf.c ${_tt_ng}/nghttp2_stream.c ${_tt_ng}/nghttp2_outbound_item.c
        ${_tt_ng}/nghttp2_session.c ${_tt_ng}/nghttp2_submit.c ${_tt_ng}/nghttp2_helper.c
        ${_tt_ng}/nghttp2_alpn.c ${_tt_ng}/nghttp2_hd.c ${_tt_ng}/nghttp2_hd_huffman.c
        ${_tt_ng}/nghttp2_hd_huffman_data.c ${_tt_ng}/nghttp2_version.c ${_tt_ng}/nghttp2_priority_spec.c
        ${_tt_ng}/nghttp2_option.c ${_tt_ng}/nghttp2_callbacks.c ${_tt_ng}/nghttp2_mem.c
        ${_tt_ng}/nghttp2_http.c ${_tt_ng}/nghttp2_rcbuf.c ${_tt_ng}/nghttp2_extpri.c
        ${_tt_ng}/nghttp2_ratelim.c ${_tt_ng}/nghttp2_time.c ${_tt_ng}/nghttp2_debug.c ${_tt_ng}/sfparse.c)
    target_include_directories(TempoTantrumNghttp2
        PUBLIC ${_tt_ng}/includes ${_tt_ng_version_dir}
        PRIVATE ${_tt_ng})
    target_compile_definitions(TempoTantrumNghttp2
        PUBLIC NGHTTP2_STATICLIB
        PRIVATE BUILDING_NGHTTP2 HAVE_ARPA_INET_H=1 HAVE_NETINET_IN_H=1 HAVE_CLOCK_GETTIME=1 HAVE_DECL_CLOCK_MONOTONIC=1)
    set_target_properties(TempoTantrumNghttp2 PROPERTIES C_VISIBILITY_PRESET hidden)

    set(TEMPOTANTRUM_HAS_PTSL 1)
    message(STATUS "Tempo Tantrum: Pro Tools Scripting link ON, built from the SDK in ${TEMPOTANTRUM_PTSL_SDK}")
else()
    message(STATUS "Tempo Tantrum: Pro Tools Scripting link OFF (no SDK given in TEMPOTANTRUM_PTSL_SDK). "
                   "Write to Pro Tools works through Accessibility only")
endif()

# ---------------------------------------------------------------------------------------------
# The listening engine: finding hits, mapping sections, the beat grid and the click.
#
# It is a static library of the engine's own code only. The JUCE module code it calls (juce_dsp
# and what juce_dsp needs) is compiled once by each program that links it, so the app, the tests
# and the tool never carry two copies. The engine gets the module headers, the same definitions
# a JUCE target gets, and a JuceHeader.h of its own that includes just those modules.
set(TEMPOTANTRUM_CORE_SOURCES
    Source/core/Onsets.cpp
    Source/core/SectionMapper.cpp
    Source/core/BeatGrid.cpp
    Source/core/Click.cpp
)

set(TEMPOTANTRUM_CORE_HEADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/CoreJuceHeader)
file(CONFIGURE OUTPUT ${TEMPOTANTRUM_CORE_HEADER_DIR}/JuceHeader.h
    CONTENT [=[
#pragma once

// The JUCE modules the listening engine uses. Generated by tempotantrum/CMakeLists.txt.
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#if ! DONT_SET_USING_JUCE_NAMESPACE
using namespace juce;
#endif
]=]
    @ONLY)

add_library(TempoTantrumCore STATIC ${TEMPOTANTRUM_CORE_SOURCES})
target_compile_features(TempoTantrumCore PUBLIC cxx_std_20)
target_include_directories(TempoTantrumCore
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/core
    ${TEMPOTANTRUM_CORE_HEADER_DIR}
    "$<TARGET_PROPERTY:juce::juce_core,INTERFACE_INCLUDE_DIRECTORIES>"
)
target_compile_definitions(TempoTantrumCore
    PRIVATE
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_MODULE_AVAILABLE_juce_core=1
    JUCE_MODULE_AVAILABLE_juce_audio_basics=1
    JUCE_MODULE_AVAILABLE_juce_audio_formats=1
    JUCE_MODULE_AVAILABLE_juce_dsp=1
    $<$<CONFIG:Debug>:DEBUG=1>
    $<$<CONFIG:Debug>:_DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:_NDEBUG=1>
)
target_link_libraries(TempoTantrumCore
    PRIVATE
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ---------------------------------------------------------------------------------------------
# The UI typeface (SIL Open Font License 1.1), read from the copy already in the repository.
juce_add_binary_data(TempoTantrumBinaryData
    HEADER_NAME TempoTantrumBinaryData.h
    NAMESPACE TempoTantrumBinaryData
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../pitchflattener/Resources/Fonts/AzeretMono-Regular.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/../pitchflattener/Resources/Fonts/AzeretMono-Medium.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/../pitchflattener/Resources/Fonts/AzeretMono-Bold.ttf
)

# ---------------------------------------------------------------------------------------------
# The app. The bundle ID must never change: macOS keeps the app's settings and permissions by it.
juce_add_gui_app(TempoTantrum
    PRODUCT_NAME "Tempo Tantrum"
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "Sammy J"
    COMPANY_COPYRIGHT "Copyright ${COPYRIGHT_YEAR} Samuel Justice"
    BUNDLE_ID "com.samueljustice.tempotantrum"
    MICROPHONE_PERMISSION_ENABLED FALSE
    HARDENED_RUNTIME_ENABLED TRUE
)

if(APPLE)
    set_target_properties(TempoTantrum PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
        XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

target_sources(TempoTantrum
    PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/Document.cpp
    Source/Player.cpp
    Source/MapFile.cpp
    Source/ProToolsTestPanel.cpp
    Source/ProToolsWrite.cpp
    Source/protools/IdentifyBeatTest.cpp
    Source/protools/IdentifyBeatWrite.cpp
    Source/protools/PtslSession.cpp
    Source/protools/PtslSessionMaths.cpp
    Source/ptsl/PtslClient.cpp
    Source/ui/Theme.cpp
    Source/ui/WaveformView.cpp
    Source/ui/Controls.cpp
    # The app's side of the AudioSuite capture: it listens for the plug-in's captures.
    Source/plugin/CaptureHandOff.cpp
    Source/plugin/AppCaptureListener.cpp
)

# Music captured from Pro Tools (Source/capture): every source file there is built into the app.
file(GLOB TEMPOTANTRUM_CAPTURE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/capture/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/capture/*.mm)

if(TEMPOTANTRUM_CAPTURE_SOURCES)
    target_sources(TempoTantrum PRIVATE ${TEMPOTANTRUM_CAPTURE_SOURCES})
endif()

# Writing to Pro Tools, and its test, drive Pro Tools through the macOS Accessibility API and keyboard events.
# Elsewhere a stand-in is built whose every step fails and says so.
if(APPLE)
    target_sources(TempoTantrum PRIVATE Source/protools/ProToolsDriver.mm)
    target_link_libraries(TempoTantrum PRIVATE "-framework AppKit" "-framework ApplicationServices" "-framework CoreFoundation")
else()
    target_sources(TempoTantrum PRIVATE Source/protools/ProToolsDriverStub.cpp)
endif()

target_include_directories(TempoTantrum PRIVATE Source)

if(TEMPOTANTRUM_HAS_PTSL)
    target_sources(TempoTantrum PRIVATE Source/ptsl/PtslWire.cpp)
    target_include_directories(TempoTantrum PRIVATE ${TEMPOTANTRUM_PTSL_GENERATED_DIR})
    target_link_libraries(TempoTantrum PRIVATE TempoTantrumNghttp2)
endif()

target_compile_definitions(TempoTantrum
    PRIVATE
    TEMPOTANTRUM_HAS_PTSL=${TEMPOTANTRUM_HAS_PTSL}
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:TempoTantrum,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:TempoTantrum,JUCE_VERSION>"
)

juce_generate_juce_header(TempoTantrum)

target_link_libraries(TempoTantrum
    PRIVATE
    TempoTantrumCore
    TempoTantrumBinaryData
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# ---------------------------------------------------------------------------------------------
# Tempo Tantrum Capture: an AudioSuite plug-in for Pro Tools that sends the selected audio to the
# app (Source/plugin). It passes the audio through unchanged and records what an AudioSuite Render
# streams through it. Pro Tools is its only host, so it is built as AAX only: AAX is on by default
# here (macOS and Windows; -DSAMMYJ_AAX=OFF leaves the plug-in out). With no AAX_SDK_PATH it builds
# with the AAX SDK bundled with JUCE. Unsigned, it loads only in Pro Tools Developer; releases are
# PACE-signed by hand (bundle/sign-aax.sh).
#
# The plug-in code and bundle ID must never change: Pro Tools finds the plug-in by them.
if((APPLE OR WIN32) AND NOT DEFINED CACHE{SAMMYJ_AAX})
    set(SAMMYJ_AAX ON CACHE BOOL "Build the AAX format")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
set(TEMPOTANTRUM_CAPTURE_FORMATS "")
sammyj_enable_aax(TEMPOTANTRUM_CAPTURE_FORMATS)

if(TEMPOTANTRUM_CAPTURE_FORMATS)
    juce_add_plugin(TempoTantrumCapture
        VERSION ${PROJECT_VERSION}
        PRODUCT_NAME "Tempo Tantrum Capture"
        COMPANY_NAME "Sammy J"
        COMPANY_COPYRIGHT "Copyright ${COPYRIGHT_YEAR} Samuel Justice"
        BUNDLE_ID "com.samueljustice.tempotantrumcapture"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        PLUGIN_MANUFACTURER_CODE SJus
        PLUGIN_CODE Ttcp
        FORMATS ${TEMPOTANTRUM_CAPTURE_FORMATS}
        AAX_IDENTIFIER com.samueljustice.tempotantrumcapture
        # "Other" in Pro Tools' AudioSuite menu.
        AAX_CATEGORY AAX_ePlugInCategory_Effect
    )

    if(APPLE)
        set_target_properties(TempoTantrumCapture PROPERTIES
            XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
            XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
            XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
        )
        target_link_libraries(TempoTantrumCapture PRIVATE "-framework CoreFoundation")
    endif()

    target_sources(TempoTantrumCapture
        PRIVATE
        Source/plugin/CaptureHandOff.cpp
        Source/plugin/CaptureRecorder.cpp
        Source/plugin/CaptureProcessor.cpp
        Source/plugin/CaptureEditor.cpp
        Source/ui/Theme.cpp
    )

    target_include_directories(TempoTantrumCapture PRIVATE Source)

    target_compile_definitions(TempoTantrumCapture
        PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
    )

    juce_generate_juce_header(TempoTantrumCapture)

    target_link_libraries(TempoTantrumCapture
        PRIVATE
        TempoTantrumBinaryData
        juce::juce_audio_utils
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
    )
else()
    message(STATUS "Tempo Tantrum Capture (AudioSuite) is not built: it is AAX only, and AAX is off or not available here")
endif()

# ---------------------------------------------------------------------------------------------
# The engine's tests (Tests/CoreTests.cpp: main() returns non-zero on a failure) and tt-cli, a
# command-line tool that maps a music file without the window. Both are console programs that
# link the engine library and compile the JUCE module code it needs.
if(TEMPOTANTRUM_TESTS)
    juce_add_console_app(TempoTantrumCoreTests PRODUCT_NAME "TempoTantrumCoreTests")
    target_sources(TempoTantrumCoreTests PRIVATE Tests/CoreTests.cpp)

    juce_add_console_app(TempoTantrumCli PRODUCT_NAME "tt-cli")
    target_sources(TempoTantrumCli PRIVATE Tools/tt-cli.cpp)
    set_target_properties(TempoTantrumCli PROPERTIES OUTPUT_NAME "tt-cli")

    foreach(tool TempoTantrumCoreTests TempoTantrumCli)
        target_include_directories(${tool} PRIVATE Source)
        target_compile_definitions(${tool} PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
        juce_generate_juce_header(${tool})
        target_link_libraries(${tool}
            PRIVATE
            TempoTantrumCore
            juce::juce_audio_formats
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
        )
    endforeach()

    # The hand-written protocol buffer and gRPC code of the Pro Tools Scripting client, tested
    # with byte sequences of its own, and the sums the write makes with what the link reads (the
    # tempo comparison, multi-mono clips). It needs neither the SDK nor JUCE.
    add_executable(TempoTantrumPtslTests Tests/PtslWireTests.cpp Source/ptsl/PtslWire.cpp Source/protools/PtslSessionMaths.cpp)
    target_include_directories(TempoTantrumPtslTests PRIVATE Source)
    target_compile_features(TempoTantrumPtslTests PRIVATE cxx_std_20)

    # The AudioSuite capture plug-in's processor driven directly, as a Render would drive it
    # (Tests/CaptureHostTests.cpp): odd block sizes offline, then the end of the Render; the WAV it
    # writes and its output must both equal the input exactly. Also the placing of a capture on the
    # timeline. It links the plug-in's shared code library, so it tests the code that ships; that
    # library links the JUCE modules privately, so their headers are added here by hand.
    if(TARGET TempoTantrumCapture)
        add_executable(TempoTantrumCaptureTests Tests/CaptureHostTests.cpp)
        target_include_directories(TempoTantrumCaptureTests
            PRIVATE
            Source
            "$<TARGET_PROPERTY:TempoTantrumCapture,JUCE_GENERATED_SOURCES_DIRECTORY>"
            "$<TARGET_PROPERTY:juce::juce_core,INTERFACE_INCLUDE_DIRECTORIES>"
        )
        # The shared code target exports only the JucePlugin_* values; the test is none of the formats.
        target_compile_definitions(TempoTantrumCaptureTests
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
        add_dependencies(TempoTantrumCaptureTests TempoTantrumCapture)
        target_link_libraries(TempoTantrumCaptureTests
            PRIVATE
            TempoTantrumCapture
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
        )
    endif()
endif()
