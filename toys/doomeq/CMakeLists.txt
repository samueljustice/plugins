cmake_minimum_required(VERSION 3.22)

# Must be set before project(). Forced only when empty, so an explicit
# -DCMAKE_OSX_DEPLOYMENT_TARGET=... still wins.
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# The version lives here and nowhere else; the release workflow checks the tag against it.
# 0.0.x releases are pre-releases (test builds).
project(DoomEQ VERSION 0.0.1 LANGUAGES C CXX)

string(TIMESTAMP COPYRIGHT_YEAR "%Y")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Plug-in formats. AAX is added by -DSAMMYJ_AAX=ON (cmake/AAX.cmake).
set(DOOMEQ_FORMATS "AU;VST3;Standalone" CACHE STRING "Plugin formats to build")

set(SAMMYJ_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${SAMMYJ_ROOT}/SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

include(${SAMMYJ_ROOT}/cmake/AAX.cmake)
sammyj_enable_aax(DOOMEQ_FORMATS)

#==============================================================================
# Freedoom Phase 1 IWAD (BSD licence). Resources/freedoom1.wad is git-ignored;
# if it isn't there we fetch the official release once (checked against its hash).
set(DOOMEQ_WAD ${CMAKE_CURRENT_SOURCE_DIR}/Resources/freedoom1.wad)
if(NOT EXISTS ${DOOMEQ_WAD})
    set(FREEDOOM_ZIP ${CMAKE_CURRENT_BINARY_DIR}/freedoom-0.13.0.zip)
    if(NOT EXISTS ${FREEDOOM_ZIP})
        message(STATUS "Downloading Freedoom 0.13.0")
        file(DOWNLOAD https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip
             ${FREEDOOM_ZIP}
             EXPECTED_HASH SHA256=3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59
             STATUS FREEDOOM_STATUS)
        list(GET FREEDOOM_STATUS 0 FREEDOOM_CODE)
        if(NOT FREEDOOM_CODE EQUAL 0)
            file(REMOVE ${FREEDOOM_ZIP})
            message(FATAL_ERROR "Couldn't download Freedoom: ${FREEDOOM_STATUS}. Put freedoom1.wad in Resources/ instead.")
        endif()
    endif()
    file(ARCHIVE_EXTRACT INPUT ${FREEDOOM_ZIP} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/freedoom PATTERNS "*/freedoom1.wad")
    set(DOOMEQ_WAD ${CMAKE_CURRENT_BINARY_DIR}/freedoom/freedoom-0.13.0/freedoom1.wad)
endif()

# Zip it before embedding: ~28 MB of WAD becomes a ~10 MB binary blob.
set(DOOMEQ_WAD_ZIP ${CMAKE_CURRENT_BINARY_DIR}/wad/freedoom1.zip)
if(NOT EXISTS ${DOOMEQ_WAD_ZIP})
    get_filename_component(DOOMEQ_WAD_DIR ${DOOMEQ_WAD} DIRECTORY)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/wad)
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf ${DOOMEQ_WAD_ZIP} --format=zip freedoom1.wad
                    WORKING_DIRECTORY ${DOOMEQ_WAD_DIR} RESULT_VARIABLE ZIP_RESULT)
    if(NOT ZIP_RESULT EQUAL 0)
        message(FATAL_ERROR "Couldn't zip ${DOOMEQ_WAD}")
    endif()
endif()

#==============================================================================
# doomgeneric (GPL-2.0), built as C. Every exit() is routed to dg_exit so DOOM can never
# terminate the host, and sound goes to the plug-in's mixer (Source/DoomSoundModule.c).
file(GLOB DOOM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/doomgeneric/*.c)
add_library(doomgeneric STATIC ${DOOM_SOURCES} Source/DoomShim.c Source/DoomSoundModule.c)
target_include_directories(doomgeneric PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/doomgeneric)
target_compile_definitions(doomgeneric PRIVATE exit=dg_exit FEATURE_SOUND)
if(WIN32)
    target_compile_definitions(doomgeneric PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    target_compile_definitions(doomgeneric PRIVATE NORMALUNIX _DEFAULT_SOURCE)
endif()
set_target_properties(doomgeneric PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
if(MSVC)
    target_compile_options(doomgeneric PRIVATE /w /O2)
else()
    target_compile_options(doomgeneric PRIVATE -w -O2)
endif()

#==============================================================================
# The manufacturer/plug-in codes and bundle ID must never change: hosts use them to find
# existing instances in saved sessions.
juce_add_plugin(DoomEQ
    VERSION ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE SJus
    PLUGIN_CODE DmEQ
    FORMATS ${DOOMEQ_FORMATS}
    PRODUCT_NAME "SammyJs DOOM EQ"
    COMPANY_NAME "Sammy J"
    COMPANY_COPYRIGHT "Copyright ${COPYRIGHT_YEAR} Samuel Justice"
    BUNDLE_ID com.samueljustice.doomeq
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES "Fx" "EQ"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "DOOM EQ processes the audio input in real time."
)

if(APPLE)
    set_target_properties(DoomEQ PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
        XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

target_sources(DoomEQ
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/DoomEngine.cpp
        Source/DoomAudio.cpp
        Source/WadArt.cpp
)

juce_generate_juce_header(DoomEQ)

juce_add_binary_data(DoomEQBinaryData
    SOURCES
        ${DOOMEQ_WAD_ZIP}
)

target_compile_definitions(DoomEQ
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        PLUGIN_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(DoomEQ
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_opengl
        DoomEQBinaryData
        doomgeneric
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
