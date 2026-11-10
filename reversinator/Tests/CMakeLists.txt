# Offline test runner: renders audio through the processor, captures the editor
# to PNG and runs a robustness suite. Enable with -DREVERSINATOR_BUILD_TESTS=ON.

juce_add_console_app(ReversinatorTests PRODUCT_NAME "ReversinatorTests")
juce_generate_juce_header(ReversinatorTests)

target_sources(ReversinatorTests
    PRIVATE
        TestRunner.cpp
        ../Source/PluginProcessor.cpp
        ../Source/PluginEditor.cpp
        ../Source/Controls.cpp
        ../Source/SnippetDisplay.cpp
        ../Source/AboutPanel.cpp
        ../Source/UpdatePopup.cpp
        ../Source/ReverseEngine.cpp
        ../../common/SammyJUpdateCheck.cpp
)

target_include_directories(ReversinatorTests PRIVATE ../Source ../../common)

target_compile_definitions(ReversinatorTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        REVERSINATOR_NO_UPDATE_CHECK=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=1
        JucePlugin_Name="SammyJs Reversinator"
        JucePlugin_VersionString="${PROJECT_VERSION}"
        JucePlugin_IsSynth=0
        JucePlugin_WantsMidiInput=0
        JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0
)

target_link_libraries(ReversinatorTests
    PRIVATE
        ReversinatorBinaryData
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
