cmake_minimum_required(VERSION 3.22)
project(Channel73 VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
endif()

# JUCE: use a local checkout in third_party/JUCE if present, otherwise fetch it.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    add_subdirectory(third_party/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.10
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(Channel73
    COMPANY_NAME "Channel73"
    PLUGIN_MANUFACTURER_CODE C73a
    PLUGIN_CODE C73p
    PRODUCT_NAME "Channel73"
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx EQ Distortion
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Channel73 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(Channel73 PRIVATE Source)

target_compile_definitions(Channel73 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Channel73
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Optional: DSP measurement suite (no JUCE needed)
option(C73_BUILD_TESTS "Build the DSP measurement suite" ON)
if(C73_BUILD_TESTS)
    add_executable(c73_dsp_tests tests/dsp_tests.cpp)
    target_include_directories(c73_dsp_tests PRIVATE Source)
endif()
