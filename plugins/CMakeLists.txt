# DATTRACK plugins for Ableton Live (VST3, plus AU on macOS):
#   DATLAB    - the DATTRACK Sample Lab as an audio effect, 51 offline processes, drag results into Ableton
#   DATTRACK  - the DATTRACK tracker as an instrument, synced to Ableton (slave) or driving it through Ableton Link (master)
cmake_minimum_required(VERSION 3.22)
project(DATTRACK_PLUGINS VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "macOS deployment target")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(DAT_BUILD_DATTRACK "Build the DATTRACK host plugin (needs a web view)" ON)
option(DAT_BUILD_TESTS "Build the command-line DSP / bridge tests" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# JUCE 8, with a one-line fix that lets a Windows WebView2 move between windows
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.10
    GIT_SHALLOW TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/juce-webview2-reparent.patch -DDIR=<SOURCE_DIR> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake
    UPDATE_DISCONNECTED TRUE)
FetchContent_MakeAvailable(juce)

set(DAT_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND DAT_FORMATS AU)
endif()

# ---------------- DATLAB ----------------
add_library(datlab_dsp STATIC
    datlab/Source/dsp/LabProcs.cpp
    datlab/Source/dsp/LabProcsNew.cpp
    datlab/Source/dsp/LabDefs.cpp
    datlab/Source/dsp/LabEngine.cpp)
target_include_directories(datlab_dsp PUBLIC datlab/Source/dsp)
set_target_properties(datlab_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(datlab_dsp PRIVATE /fp:precise /bigobj)
endif()

juce_add_plugin(DATLAB
    COMPANY_NAME "DATTRACK"
    BUNDLE_ID "com.dattrack.datlab"
    PLUGIN_MANUFACTURER_CODE Dttk
    PLUGIN_CODE Dlab
    FORMATS ${DAT_FORMATS}
    PRODUCT_NAME "DATLAB"
    DESCRIPTION "DATTRACK Sample Lab: offline sample mangling with drag-and-drop to the DAW"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Tools
    AU_MAIN_TYPE kAudioUnitType_Effect
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.audio-input)

target_sources(DATLAB PRIVATE datlab/Source/PluginProcessor.cpp datlab/Source/PluginEditor.cpp)
target_compile_definitions(DATLAB PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_OGGVORBIS=1 JUCE_USE_FLAC=1)
target_link_libraries(DATLAB
    PRIVATE datlab_dsp juce::juce_audio_utils juce::juce_audio_formats
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

if(DAT_BUILD_DATTRACK)
    add_subdirectory(dattrack)
endif()

if(DAT_BUILD_TESTS)
    add_executable(lab_cli tests/lab_cli.cpp)
    target_link_libraries(lab_cli PRIVATE datlab_dsp)
endif()
