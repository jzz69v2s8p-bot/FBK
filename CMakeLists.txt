cmake_minimum_required(VERSION 3.22)

project(FeedbackGuard VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# macOS: Universal Binary (Apple Silicon + Intel), ab macOS 11
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimale macOS-Version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architekturen")
endif()

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.6
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

# Formate: AU und VST3 auf dem Mac. Auf anderen Systemen nur VST3.
if(APPLE)
    set(FG_FORMATS AU VST3)
else()
    set(FG_FORMATS VST3)
endif()

juce_add_plugin(FeedbackGuard
    COMPANY_NAME                "Feedback Guard"
    PLUGIN_MANUFACTURER_CODE    Fgrd
    PLUGIN_CODE                 Fbgd
    FORMATS                     ${FG_FORMATS}
    PRODUCT_NAME                "Feedback Guard"
    AU_MAIN_TYPE                kAudioUnitType_Effect
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    COPY_PLUGIN_AFTER_BUILD     TRUE
    VST3_CATEGORIES             Fx)

target_sources(FeedbackGuard PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/FeedbackEngine.h)

target_compile_definitions(FeedbackGuard PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(FeedbackGuard
    PRIVATE
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Engine-Test (ohne JUCE):  cmake -DFG_BUILD_TESTS=ON ..
option(FG_BUILD_TESTS "Engine-Test bauen" OFF)
if(FG_BUILD_TESTS)
    add_executable(fg_test Tests/test_engine.cpp)
    target_include_directories(fg_test PRIVATE Source)
    target_compile_options(fg_test PRIVATE -O2)
endif()
