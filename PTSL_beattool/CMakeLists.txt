cmake_minimum_required(VERSION 3.16)
project(PTSLBeatTool VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")  # Updated for better compatibility

# Find required packages
find_package(OpenGL REQUIRED)

# Download and configure GLM
include(FetchContent)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

# Add Beat-and-Tempo-Tracking library
add_library(btt STATIC
    ../SDK/Beat-and-Tempo-Tracking/src/BTT.c
    ../SDK/Beat-and-Tempo-Tracking/src/DFT.c
    ../SDK/Beat-and-Tempo-Tracking/src/Filter.c
    ../SDK/Beat-and-Tempo-Tracking/src/STFT.c
    ../SDK/Beat-and-Tempo-Tracking/src/Statistics.c
    ../SDK/Beat-and-Tempo-Tracking/src/fastsin.c
)
target_include_directories(btt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../SDK/Beat-and-Tempo-Tracking
    ${CMAKE_CURRENT_SOURCE_DIR}/../SDK/Beat-and-Tempo-Tracking/src
)

# Main application
add_executable(ptsl_beat_tool MACOSX_BUNDLE
    Source/gui_native.mm
    Source/BeatTracker.cpp
    Source/AudioFileReader.cpp
    Source/PyPTSL.cpp
    Source/OpenGLView.mm
    Source/WaveformVisualizer.cpp
    Source/ScalableView.mm
)

target_include_directories(ptsl_beat_tool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${OPENGL_INCLUDE_DIR}
)

target_link_libraries(ptsl_beat_tool PRIVATE
    btt
    ${OPENGL_LIBRARIES}
    glm::glm
    "-framework Cocoa"
    "-framework Foundation"
    "-framework OpenGL"
    "-framework CoreVideo"
)

# macOS specific settings
set_target_properties(ptsl_beat_tool PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Resources/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "PTSL Beat Tool"
    MACOSX_BUNDLE_EXECUTABLE_NAME "PTSL Beat Tool"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_COPYRIGHT "Copyright © 2025 Samuel Justice"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.samueljustice.ptslbeattool"
    OUTPUT_NAME "PTSL Beat Tool"
)

# Bundle Python with py-ptsl
add_custom_command(TARGET ptsl_beat_tool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Creating Python bundle with py-ptsl..."
    COMMAND ${CMAKE_COMMAND} -E make_directory 
        "$<TARGET_BUNDLE_DIR:ptsl_beat_tool>/Contents/Resources/python"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/ptsl_client.py"
        "$<TARGET_BUNDLE_DIR:ptsl_beat_tool>/Contents/Resources/python/"
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bundle_pytsl.sh
        "$<TARGET_BUNDLE_DIR:ptsl_beat_tool>/Contents/Resources/python"
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bundle_python_framework.sh
        "$<TARGET_BUNDLE_DIR:ptsl_beat_tool>/.."
    COMMENT "Bundling Python with py-ptsl"
)

# Code sign the app bundle
if(APPLE)
    add_custom_command(TARGET ptsl_beat_tool POST_BUILD
        COMMAND /usr/bin/codesign --force --deep --sign "Developer ID Application: Sweet Justice Sound Ltd (85DN8C9FPR)"
            "$<TARGET_BUNDLE_DIR:ptsl_beat_tool>"
        COMMENT "Code signing app bundle"
        VERBATIM
    )
endif()

# CLI tool for testing
add_executable(ptsl-beat-cli
    Source/main.cpp
    Source/BeatTracker.cpp
    Source/AudioFileReader.cpp
    Source/PyPTSL.cpp
)

target_include_directories(ptsl-beat-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(ptsl-beat-cli PRIVATE
    btt
)

# Bundle Python with py-ptsl for CLI tool
add_custom_command(TARGET ptsl-beat-cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/ptsl_client.py"
        "$<TARGET_FILE_DIR:ptsl-beat-cli>/ptsl_client.py"
    COMMAND ${CMAKE_COMMAND} -E make_directory 
        "$<TARGET_FILE_DIR:ptsl-beat-cli>/python"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/ptsl_client.py"
        "$<TARGET_FILE_DIR:ptsl-beat-cli>/python/ptsl_client.py"
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bundle_pytsl.sh
        "$<TARGET_FILE_DIR:ptsl-beat-cli>/python"
    COMMENT "Bundling Python with py-ptsl for CLI tool"
)
