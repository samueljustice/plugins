cmake_minimum_required(VERSION 3.22)

# Minimum macOS. Without this the binaries take the build machine's SDK version as their
# minimum and JUCE's runtime OS checks (@available) are compiled out. Must come before project().
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")

set(TONALRECALL_VERSION "0.2.0" CACHE STRING "Tonal Recall version (major.minor.patch)")
project(TonalRecall VERSION ${TONALRECALL_VERSION})

# The copyright year is the year it was built, so it never goes stale.
string(TIMESTAMP COPYRIGHT_YEAR "%Y")
add_compile_definitions(COPYRIGHT_YEAR="${COPYRIGHT_YEAR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Windows: link the C++ runtime statically (no VC++ redistributable needed).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
    PLUGIN_VERSION="${PROJECT_VERSION}"
    JUCE_VST3_CAN_REPLACE_VST2=0
)

set(TONALRECALL_FORMATS "AU;VST3" CACHE STRING "Plugin formats to build")
option(TONALRECALL_TESTS "Build the offline tests" OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

# The pitch shifter (prebuilt static library in the shared SDK folder).
set(RUBBERBAND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../SDK/rubberband-4.0.0")
set(RUBBERBAND_LIB "${RUBBERBAND_ROOT}/lib/librubberband.a")

# The analysis and Stamp core: plain C++, shared by the plug-in and the core tests.
set(TONALRECALL_CORE_SOURCES
    Source/core/Analysis.cpp
    Source/core/HindsightTracker.cpp
    Source/core/RubberBandDriver.cpp
    Source/core/Stamper.cpp
)

# The UI typeface (SIL Open Font License 1.1), read from the copy already in the repository.
juce_add_binary_data(TonalRecallBinaryData
    HEADER_NAME TonalRecallBinaryData.h
    NAMESPACE TonalRecallBinaryData
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../pitchflattener/Resources/Fonts/AzeretMono-Regular.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/../pitchflattener/Resources/Fonts/AzeretMono-Medium.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/../pitchflattener/Resources/Fonts/AzeretMono-Bold.ttf
)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
sammyj_enable_aax(TONALRECALL_FORMATS)

# The manufacturer/plugin codes and bundle ID must never change: hosts use them to find
# existing instances in saved sessions. AudioSuite is on (JUCE's default).
juce_add_plugin(TonalRecall
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "Sammy J"
    COMPANY_COPYRIGHT "Copyright ${COPYRIGHT_YEAR} Samuel Justice"
    COMPANY_WEBSITE "https://sweetjusticesound.com"
    BUNDLE_ID "com.samueljustice.tonalrecall"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE SJus
    PLUGIN_CODE Tnrc
    FORMATS ${TONALRECALL_FORMATS}
    PRODUCT_NAME "SammyJs Tonal Recall"
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx "Pitch Shift"
    AAX_IDENTIFIER com.samueljustice.tonalrecall
    AAX_CATEGORY AAX_ePlugInCategory_PitchShift
)

if(APPLE)
    set_target_properties(TonalRecall PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
        XCODE_ATTRIBUTE_HARDENED_RUNTIME_ENABLED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

target_sources(TonalRecall
    PRIVATE
    Source/Audition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/SammyJUpdateCheck.cpp
    Source/CaptureStore.cpp
    Source/DiagnosticLog.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/MatchHub.cpp
    ${TONALRECALL_CORE_SOURCES}
)

target_include_directories(TonalRecall
    PRIVATE
    Source
    ${RUBBERBAND_ROOT}
    ${RUBBERBAND_ROOT}/src
)

# The update check shared by every Sammy J plug-in (common/).
target_include_directories(TonalRecall PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_compile_definitions(TonalRecall
    PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

juce_generate_juce_header(TonalRecall)

target_link_libraries(TonalRecall
    PRIVATE
    juce::juce_audio_utils
    TonalRecallBinaryData
    ${RUBBERBAND_LIB}
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

if(APPLE)
    target_link_libraries(TonalRecall PRIVATE "-framework Accelerate")
endif()

# Offline tests: real processor instances driven with a fake play head (Tests/TonalRecallTests.cpp).
# The test links the plug-in's shared code library, so it tests exactly the code that ships. That
# library links the JUCE modules privately, so their headers and the generated JuceHeader.h are
# added here by hand; the module sources are not compiled a second time.
if(TONALRECALL_TESTS)
    add_executable(TonalRecallTests Tests/TonalRecallTests.cpp)
    target_include_directories(TonalRecallTests
        PRIVATE
        Source
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
        "$<TARGET_PROPERTY:TonalRecall,JUCE_GENERATED_SOURCES_DIRECTORY>"
        "$<TARGET_PROPERTY:juce::juce_core,INTERFACE_INCLUDE_DIRECTORIES>"
    )
    # The shared code target exports only the JucePlugin_* values; the test is none of the formats.
    target_compile_definitions(TonalRecallTests
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
    add_dependencies(TonalRecallTests TonalRecall)
    target_link_libraries(TonalRecallTests
        PRIVATE
        TonalRecall
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )

    # The analysis and Stamp core on its own: no JUCE, synthetic material with known answers.
    add_executable(TonalRecallCoreTests Tests/CoreTests.cpp ${TONALRECALL_CORE_SOURCES})
    target_include_directories(TonalRecallCoreTests PRIVATE Source ${RUBBERBAND_ROOT} ${RUBBERBAND_ROOT}/src)
    target_compile_features(TonalRecallCoreTests PRIVATE cxx_std_20)
    target_link_libraries(TonalRecallCoreTests
        PRIVATE
        ${RUBBERBAND_LIB}
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
    if(APPLE)
        target_link_libraries(TonalRecallCoreTests PRIVATE "-framework Accelerate")
    endif()
    # Command-line tools for listening tests on real files (no host, no JUCE): what the analysis
    # makes of a music file and an effect, and the effect stamped against a batch of music files.
    foreach(tool Analyse Stamp)
        add_executable(TonalRecall${tool} Tools/${tool}.cpp
            Source/core/Analysis.cpp Source/core/HindsightTracker.cpp Source/core/Stamper.cpp Source/core/RubberBandDriver.cpp)
        target_compile_features(TonalRecall${tool} PRIVATE cxx_std_20)
        target_include_directories(TonalRecall${tool} PRIVATE Source ${RUBBERBAND_ROOT} ${RUBBERBAND_ROOT}/src)
        target_link_libraries(TonalRecall${tool} PRIVATE ${RUBBERBAND_LIB})
        if(APPLE)
            target_link_libraries(TonalRecall${tool} PRIVATE "-framework Accelerate")
        endif()
    endforeach()
endif()
