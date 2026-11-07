# Standalone test project for master-sample fingerprints (Fingerprint). Builds only Fingerprint.cpp and its test.
#   cmake -S Tests/fingerprint -B build-fingerprint -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
#   cmake --build build-fingerprint -j 12 && ./build-fingerprint/FingerprintTest_artefacts/Release/FingerprintTest
cmake_minimum_required(VERSION 3.21)
project(SwooshbucklerFingerprintTest VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

set(SWOOSH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${SWOOSH_ROOT}/../SDK/JUCE ${CMAKE_CURRENT_BINARY_DIR}/SDK/JUCE)

juce_add_console_app(FingerprintTest PRODUCT_NAME "FingerprintTest")
target_sources(FingerprintTest PRIVATE FingerprintTest.cpp ${SWOOSH_ROOT}/Source/Fingerprint.cpp)
target_include_directories(FingerprintTest PRIVATE ${SWOOSH_ROOT}/Source)
target_compile_definitions(FingerprintTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(FingerprintTest
    PRIVATE juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
