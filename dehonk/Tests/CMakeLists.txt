# De-Honk offline harness: dehonk_render and dehonk_tests.
# Standalone (no JUCE). Compiles the DSP core from ../dsp directly.
#
#   cmake -S dehonk/Tests -B dehonk/Tests/build
#   cmake --build dehonk/Tests/build -j 8

cmake_minimum_required (VERSION 3.22)
project (DeHonkTests LANGUAGES CXX)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set (CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)
set (CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# The core under test. Point this elsewhere (-DDEHONK_DSP_DIR=...) to run the same harness on
# another implementation of DeHonkCore.h, such as a prototype or a mock.
set (DEHONK_DSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dsp" CACHE PATH "Folder holding DeHonkCore.h and the core's .cpp files")
# Every .cpp in that folder is part of the core, so the core can be split into more files
# without touching this one.
file (GLOB DEHONK_DSP_SOURCES CONFIGURE_DEPENDS "${DEHONK_DSP_DIR}/*.cpp")

if (MSVC)
    set (DEHONK_WARNINGS /W4 /permissive-)
    set (DEHONK_OPT $<$<CONFIG:Release>:/O2>)
    set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else ()
    set (DEHONK_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    set (DEHONK_OPT $<$<CONFIG:Release>:-O3>)
endif ()

add_library (dehonk_core STATIC ${DEHONK_DSP_SOURCES})
target_include_directories (dehonk_core PUBLIC "${DEHONK_DSP_DIR}")
target_compile_options (dehonk_core PRIVATE ${DEHONK_WARNINGS} ${DEHONK_OPT})

add_library (dehonk_harness STATIC
    src/Analysis.cpp
    src/Fft.cpp
    src/Measures.cpp
    src/Render.cpp
    src/Signals.cpp
    src/Wav.cpp)
target_include_directories (dehonk_harness PUBLIC src)
target_link_libraries (dehonk_harness PUBLIC dehonk_core)
target_compile_options (dehonk_harness PRIVATE ${DEHONK_WARNINGS} ${DEHONK_OPT})

add_executable (dehonk_render src/dehonk_render.cpp)
target_link_libraries (dehonk_render PRIVATE dehonk_harness)
target_compile_options (dehonk_render PRIVATE ${DEHONK_WARNINGS} ${DEHONK_OPT})

add_executable (dehonk_tests src/dehonk_tests.cpp)
target_link_libraries (dehonk_tests PRIVATE dehonk_harness)
target_compile_options (dehonk_tests PRIVATE ${DEHONK_WARNINGS} ${DEHONK_OPT})
# golden_mono_stereo reads its reference from Tests/golden/ in the source tree.
target_compile_definitions (dehonk_tests PRIVATE DEHONK_TESTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# Golden outputs. Two tests check the current core bit for bit against hashes kept with the
# sources in Tests/golden/ (one set of files per architecture), and fail if a file or a case in
# one is missing:
#   golden_mono_stereo  at the settings that must reproduce the older cores (Max Cut 8 dB, the
#                       release Speed used to set, Impact Guard off, and the old notch movement,
#                       Tuning::handOver off, which no control reaches):
#                       golden_ref_<arch>.txt, mono and stereo, from the core before surround, and
#                       golden_surround_<arch>.txt, 5.1 and 7.1.4, from the core before the three
#                       controls.
#   golden_defaults     at the plug-in's defaults: golden_defaults_<arch>.txt, from the current core.
# To record a reference on purpose: copy a core to <build>/ref-dsp/ (one from before surround) or
# <build>/ref-dsp2/ (one with layouts) and build; dehonk_golden_ref writes <build>/golden_ref.txt
# and dehonk_golden_ref2 writes <build>/golden_ref2.txt (with the surround cases), which go under
# the comment header of the file in Tests/golden/. "dehonk_golden --defaults out.txt" writes the
# hashes of the current core at its defaults, with the extra cases that make Max Cut and the
# impact guard work, for golden_defaults_<arch>.txt.
add_executable (dehonk_golden src/dehonk_golden.cpp)
target_include_directories (dehonk_golden PRIVATE src)
target_link_libraries (dehonk_golden PRIVATE dehonk_core)
target_compile_options (dehonk_golden PRIVATE ${DEHONK_WARNINGS} ${DEHONK_OPT})
target_compile_definitions (dehonk_golden PRIVATE DEHONK_GOLDEN_SURROUND=1)
target_compile_definitions (dehonk_tests PRIVATE DEHONK_GOLDEN_SURROUND=1)

foreach (REF IN ITEMS "ref" "ref2")
    if (REF STREQUAL "ref")
        set (DEHONK_REF_DSP_DIR "${CMAKE_CURRENT_BINARY_DIR}/ref-dsp")
        set (DEHONK_REF_ARGS "")
        set (DEHONK_REF_SURROUND 0)
    else ()
        set (DEHONK_REF_DSP_DIR "${CMAKE_CURRENT_BINARY_DIR}/ref-dsp2")
        set (DEHONK_REF_ARGS "--surround")
        set (DEHONK_REF_SURROUND 1)
    endif ()
    if (EXISTS "${DEHONK_REF_DSP_DIR}/DeHonkCore.h")
        file (GLOB DEHONK_REF_SOURCES CONFIGURE_DEPENDS "${DEHONK_REF_DSP_DIR}/*.cpp")
        add_library (dehonk_core_${REF} STATIC ${DEHONK_REF_SOURCES})
        target_include_directories (dehonk_core_${REF} PUBLIC "${DEHONK_REF_DSP_DIR}")
        target_compile_options (dehonk_core_${REF} PRIVATE ${DEHONK_WARNINGS} ${DEHONK_OPT})
        add_executable (dehonk_golden_${REF} src/dehonk_golden.cpp)
        target_include_directories (dehonk_golden_${REF} PRIVATE src)
        target_link_libraries (dehonk_golden_${REF} PRIVATE dehonk_core_${REF})
        target_compile_options (dehonk_golden_${REF} PRIVATE ${DEHONK_WARNINGS} ${DEHONK_OPT})
        target_compile_definitions (dehonk_golden_${REF} PRIVATE DEHONK_GOLDEN_SURROUND=${DEHONK_REF_SURROUND})
        add_custom_command (OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/golden_${REF}.txt"
                            COMMAND dehonk_golden_${REF} ${DEHONK_REF_ARGS} "${CMAKE_CURRENT_BINARY_DIR}/golden_${REF}.txt"
                            DEPENDS dehonk_golden_${REF}
                            COMMENT "Recording golden outputs of ${DEHONK_REF_DSP_DIR}")
        add_custom_target (golden_${REF} ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/golden_${REF}.txt")
    endif ()
endforeach ()
