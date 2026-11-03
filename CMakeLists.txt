cmake_minimum_required(VERSION 3.22)
project(WannengyaAudio VERSION 1.0.7 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures" FORCE)

# JUCE is pinned to 8.0.15. A checkout at third_party/JUCE is used when present
# (offline builds, and what developers here have on disk); otherwise CMake
# fetches the same tagged tarball and verifies its SHA-256, so a fresh clone
# builds without a manual submodule step.
set(WNG_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE")
if(EXISTS "${WNG_JUCE_DIR}/CMakeLists.txt")
    add_subdirectory(${WNG_JUCE_DIR} ${CMAKE_BINARY_DIR}/third_party/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        URL      https://codeload.github.com/juce-framework/JUCE/tar.gz/refs/tags/8.0.15
        URL_HASH SHA256=0eafd736286c6e7bb6958d79fce18d42cb654b8e231890d327b7cece379bd156
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

option(WNG_SKIP_PLUGIN_COPY "Do not install VST3 bundles into ~/Library" OFF)

set(WNG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# -----------------------------------------------------------------------------
# Shared audio primitives. Compiled into every target: the Agent, both plug-ins
# and the driver's loopback does not need them, but the HAL build links the ring
# and resampler so behaviour matches the plug-ins exactly.
# -----------------------------------------------------------------------------
# Logo, reused by the plug-in editors and the Agent icon.
juce_add_binary_data(WngAssets
    HEADER_NAME "BinaryData.h"
    NAMESPACE wng_assets
    SOURCES ${WNG_ROOT}/ui/logo.png
)

add_library(wng_audio STATIC
    shared/RingBuffer.cpp
    shared/Resampler.cpp
    shared/DriftBridge.cpp
)
target_include_directories(wng_audio PUBLIC ${WNG_ROOT}/shared)
target_compile_features(wng_audio PUBLIC cxx_std_20)

# -----------------------------------------------------------------------------
# Background Agent. Owns TCC and the tap; feeds the plug-ins over shared memory.
# -----------------------------------------------------------------------------
add_executable(WannengyaAgent MACOSX_BUNDLE
    agent/AgentMain.mm
    agent/Core.mm
    agent/PermissionGuide.mm
    hub/AudioDevices.mm
    hub/ProcessList.mm
    hub/ProcessTap.mm
    hub/RingPlayer.mm
    hub/RingRecorder.mm
    hub/Permissions.mm
)

set(WNG_OBJC_SOURCES
    agent/AgentMain.mm
    agent/Core.mm
    agent/PermissionGuide.mm
    hub/AudioDevices.mm
    hub/ProcessList.mm
    hub/ProcessTap.mm
    hub/RingPlayer.mm
    hub/RingRecorder.mm
    hub/Permissions.mm
)
set_source_files_properties(${WNG_OBJC_SOURCES} PROPERTIES COMPILE_FLAGS "-fobjc-arc")

target_include_directories(WannengyaAgent PRIVATE ${WNG_ROOT}/hub)
target_link_libraries(WannengyaAgent PRIVATE
    wng_audio
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework AVFoundation"
    "-framework AppKit"
    "-framework Foundation"
)

set_target_properties(WannengyaAgent PROPERTIES
    OUTPUT_NAME "wannengya Agent"
    MACOSX_BUNDLE_BUNDLE_NAME "万能鸭录音机架"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.wannengya.agent"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.7"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.7"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/agent"
)

# -----------------------------------------------------------------------------
# Plug-ins. VST3 serves Cubase / Nuendo / Studio One / Reaper / Bitwig; AU (v2)
# serves Logic Pro / GarageBand / MainStage, which have no VST support at all.
# Both wrappers are built from the same sources and share the manufacturer and
# product codes, so one plug-in keeps one identity across formats.
# -----------------------------------------------------------------------------
set(WNG_PLUGIN_COMMON
    plugins/AgentLauncher.mm
    hub/ProcessList.mm
    hub/AudioDevices.mm
)

set(WNG_PLUGIN_LIBS
    wng_audio
    juce::juce_audio_processors
    juce::juce_gui_extra
    "-framework CoreAudio"
    "-framework AppKit"
    "-framework Foundation"
)

juce_add_plugin(WannengyaCapture
    PRODUCT_NAME "wannengya Capture"
    COMPANY_NAME "wannengya"
    BUNDLE_ID "com.wannengya.capture"
    PLUGIN_MANUFACTURER_CODE Wngy
    PLUGIN_CODE Cap1
    FORMATS VST3 AU
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES Fx
)

juce_generate_juce_header(WannengyaCapture)

target_sources(WannengyaCapture PRIVATE
    plugins/Capture/CaptureProcessor.cpp
    plugins/Capture/CaptureEditor.cpp
    ${WNG_PLUGIN_COMMON}
)

target_include_directories(WannengyaCapture PRIVATE
    ${WNG_ROOT}/ui
    ${WNG_ROOT}/plugins
    ${WNG_ROOT}/plugins/Capture
    ${WNG_ROOT}/hub
)

target_link_libraries(WannengyaCapture PRIVATE ${WNG_PLUGIN_LIBS} WngAssets)
target_compile_definitions(WannengyaCapture PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)

juce_add_plugin(WannengyaReturn
    PRODUCT_NAME "wannengya Return"
    COMPANY_NAME "wannengya"
    BUNDLE_ID "com.wannengya.return"
    PLUGIN_MANUFACTURER_CODE Wngy
    PLUGIN_CODE Ret1
    FORMATS VST3 AU
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES Fx
)

juce_generate_juce_header(WannengyaReturn)

target_sources(WannengyaReturn PRIVATE
    plugins/Return/ReturnProcessor.cpp
    plugins/Return/ReturnEditor.cpp
    ${WNG_PLUGIN_COMMON}
)

target_include_directories(WannengyaReturn PRIVATE
    ${WNG_ROOT}/ui
    ${WNG_ROOT}/plugins
    ${WNG_ROOT}/plugins/Return
    ${WNG_ROOT}/hub
)

target_link_libraries(WannengyaReturn PRIVATE ${WNG_PLUGIN_LIBS} WngAssets)
target_compile_definitions(WannengyaReturn PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)

if(WNG_SKIP_PLUGIN_COPY)
    set_target_properties(WannengyaCapture WannengyaReturn
        PROPERTIES JUCE_COPY_PLUGIN_AFTER_BUILD FALSE)
endif()

# -----------------------------------------------------------------------------
# Diagnostics.
# -----------------------------------------------------------------------------
add_executable(wngmon tools/wngmon.cpp)
target_link_libraries(wngmon PRIVATE wng_audio)

add_executable(wngdoctor tools/doctor.cpp)
target_link_libraries(wngdoctor PRIVATE "-framework CoreAudio" "-framework CoreFoundation")

# Development-only: pretends to be the Return plug-in so the Agent's heartbeat
# handling can be checked without a DAW.
add_executable(wngreturnsim tools/return_sim.cpp)
target_include_directories(wngreturnsim PRIVATE ${WNG_ROOT}/shared)
target_compile_features(wngreturnsim PRIVATE cxx_std_20)

# -----------------------------------------------------------------------------
# Development-only offscreen renderer for the two editors.
# -----------------------------------------------------------------------------
juce_add_gui_app(WannengyaPreview
    PRODUCT_NAME "wannengya Preview"
    COMPANY_NAME "wannengya"
    BUNDLE_ID "com.wannengya.preview"
    VERSION "1.0.7"
)

juce_generate_juce_header(WannengyaPreview)

target_sources(WannengyaPreview PRIVATE
    tools/preview/Preview.cpp
    plugins/Capture/CaptureProcessor.cpp
    plugins/Capture/CaptureEditor.cpp
    plugins/Return/ReturnProcessor.cpp
    plugins/Return/ReturnEditor.cpp
    ${WNG_PLUGIN_COMMON}
)

target_include_directories(WannengyaPreview PRIVATE
    ${WNG_ROOT}/ui
    ${WNG_ROOT}/plugins
    ${WNG_ROOT}/plugins/Capture
    ${WNG_ROOT}/plugins/Return
    ${WNG_ROOT}/hub
)

target_link_libraries(WannengyaPreview PRIVATE
    wng_audio
    WngAssets
    juce::juce_audio_processors
    juce::juce_gui_extra
    "-framework CoreAudio"
    "-framework AppKit"
    "-framework Foundation"
)

target_compile_definitions(WannengyaPreview PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 WNG_PREVIEW=1)
