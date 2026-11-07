# Standalone test project for the spectral morph (SpectralMorph). Builds only the morph DSP and its test.
#   cmake -S Tests/morph -B build-morph -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
#   cmake --build build-morph -j 12 && ./build-morph/MorphTest_artefacts/Release/MorphTest [outputFolder]
cmake_minimum_required(VERSION 3.21)
project(SwooshbucklerMorphTest VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

set(SWOOSH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${SWOOSH_ROOT}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

juce_add_console_app(MorphTest PRODUCT_NAME "MorphTest")
target_sources(MorphTest PRIVATE MorphTest.cpp ${SWOOSH_ROOT}/Source/SpectralMorph.cpp)
target_include_directories(MorphTest PRIVATE ${SWOOSH_ROOT}/Source)
target_compile_definitions(MorphTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(MorphTest
    PRIVATE juce::juce_audio_formats juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
