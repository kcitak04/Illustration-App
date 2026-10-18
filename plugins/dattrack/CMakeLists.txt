# DATTRACK host plugin: the DATTRACK tracker (vendored web app) in a web view, its audio routed into the DAW,
# synced as SLAVE (follows the host transport) or MASTER (drives Ableton through Ableton Link).

FetchContent_Declare(abletonlink
    GIT_REPOSITORY https://github.com/Ableton/link.git
    GIT_TAG Link-3.1.5
    GIT_SHALLOW TRUE
    GIT_SUBMODULES modules/asio-standalone
    UPDATE_DISCONNECTED TRUE)
FetchContent_GetProperties(abletonlink)
if(NOT abletonlink_POPULATED)
    FetchContent_Populate(abletonlink)
endif()
include(${abletonlink_SOURCE_DIR}/AbletonLinkConfig.cmake)

juce_add_binary_data(dattrack_web
    HEADER_NAME BinaryData.h
    NAMESPACE BinaryData
    SOURCES web/dattrack.html web/dt-bridge.js web/dt-tap.js)
set_target_properties(dattrack_web PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(DATTRACK_CORE_SOURCES Source/LocalServer.cpp Source/DatTrackCore.cpp)

juce_add_plugin(DATTRACK
    COMPANY_NAME "DATTRACK"
    BUNDLE_ID "com.dattrack.dattrack"
    PLUGIN_MANUFACTURER_CODE Dttk
    PLUGIN_CODE Dtrk
    FORMATS ${DAT_FORMATS}
    PRODUCT_NAME "DATTRACK"
    DESCRIPTION "DATTRACK tracker synced to the DAW (slave) or driving it through Ableton Link (master)"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    NEEDS_WEBVIEW2 TRUE
    NEEDS_WEB_BROWSER TRUE
    VST3_CATEGORIES Instrument Sampler
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.network.server com.apple.security.network.client)

target_sources(DATTRACK PRIVATE ${DATTRACK_CORE_SOURCES} Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(DATTRACK PRIVATE Source)
target_compile_definitions(DATTRACK PUBLIC
    JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1 DAT_WITH_LINK=1
    # WebView2 (patched JUCE): Web Audio starts without a click, and a hidden page is never throttled
    "JUCE_WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=\"--autoplay-policy=no-user-gesture-required --disable-background-timer-throttling --disable-renderer-backgrounding --disable-backgrounding-occluded-windows\"")
target_link_libraries(DATTRACK
    PRIVATE dattrack_web Ableton::Link juce::juce_audio_utils juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

if(DAT_BUILD_TESTS)
    juce_add_console_app(dattrack_host PRODUCT_NAME "dattrack_host")
    target_sources(dattrack_host PRIVATE ../tests/dattrack_host.cpp ${DATTRACK_CORE_SOURCES})
    target_include_directories(dattrack_host PRIVATE Source)
    target_compile_definitions(dattrack_host PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 DAT_WITH_LINK=1)
    target_link_libraries(dattrack_host PRIVATE dattrack_web Ableton::Link juce::juce_core juce::juce_events juce::juce_audio_basics
        juce::juce_recommended_config_flags)
endif()
