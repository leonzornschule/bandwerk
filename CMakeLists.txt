cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
    endif()
endif()

project(Bandwerk VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: either a local checkout (-DJUCE_DIR=...) or fetched from GitHub
if(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.10
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(BANDWERK_FORMATS VST3)
if(APPLE)
    list(APPEND BANDWERK_FORMATS AU)
endif()

juce_add_plugin(Bandwerk
    COMPANY_NAME "Leon"
    BUNDLE_ID "de.leon.bandwerk"
    PLUGIN_MANUFACTURER_CODE Leon
    PLUGIN_CODE Bwmb
    PRODUCT_NAME "Bandwerk"
    FORMATS ${BANDWERK_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Dynamics)

set(BANDWERK_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/DSP/Engine.cpp
    Source/UI/BandDisplay.cpp)

target_sources(Bandwerk PRIVATE ${BANDWERK_SOURCES})
target_include_directories(Bandwerk PRIVATE Source)

set(BANDWERK_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_compile_definitions(Bandwerk PUBLIC ${BANDWERK_DEFS})

target_link_libraries(Bandwerk
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# Messprogramm (nur für Entwicklung, nicht nötig für das Plugin)
option(BANDWERK_TESTS "Build measurement tests" OFF)
if(BANDWERK_TESTS)
    juce_add_console_app(BandwerkTests PRODUCT_NAME "BandwerkTests")
    target_sources(BandwerkTests PRIVATE Tests/TestMain.cpp ${BANDWERK_SOURCES})
    target_include_directories(BandwerkTests PRIVATE Source)
    target_compile_definitions(BandwerkTests PRIVATE ${BANDWERK_DEFS} BANDWERK_TEST_BUILD=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(BandwerkTests PRIVATE juce::juce_audio_utils juce::juce_dsp
        juce::juce_recommended_config_flags)
endif()
