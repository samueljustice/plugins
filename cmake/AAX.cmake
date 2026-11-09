# AAX for every plug-in. Include after add_subdirectory(JUCE) and before juce_add_plugin:
#
#   include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AAX.cmake)
#   sammyj_enable_aax(<PLUGIN>_FORMATS)
#
# -DSAMMYJ_AAX=ON adds AAX to the plug-in's formats (macOS and Windows only).
# -DAAX_SDK_PATH=<folder> builds against that AAX SDK (e.g. Avid's aax-sdk-2-10-0); without it, JUCE
# uses the AAX SDK it bundles. The SDK is never committed here: it is Avid's, under their licence or GPLv3.
#
# An AAX built this way runs only in Pro Tools Developer until it is signed with PACE's wraptool
# (see the release workflows).
option(SAMMYJ_AAX "Build the AAX format" OFF)
set(AAX_SDK_PATH "" CACHE PATH "AAX SDK folder (empty: the one bundled with JUCE)")

macro(sammyj_enable_aax formats_var)
    if(SAMMYJ_AAX AND (APPLE OR WIN32))
        if(AAX_SDK_PATH)
            juce_set_aax_sdk_path("${AAX_SDK_PATH}")
            message(STATUS "AAX: building with the SDK at ${AAX_SDK_PATH}")
        else()
            message(STATUS "AAX: building with the SDK bundled with JUCE")
        endif()
        if(NOT "AAX" IN_LIST ${formats_var})
            list(APPEND ${formats_var} AAX)
        endif()
    endif()
endmacro()
