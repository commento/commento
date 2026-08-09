cmake_minimum_required(VERSION 3.24)

project(Commento VERSION 0.1.0 LANGUAGES C CXX)

set(COMMENTO_JUCE_SOURCE_DIR "" CACHE PATH "Optional path to an existing JUCE checkout")
option(COMMENTO_ALSA_DIAGNOSTICS "Log detailed ALSA setup on Linux" ON)
option(COMMENTO_ALSA_BYPASS_PCM_LINK
    "Keep ALSA capture/playback streams independent to avoid an unprimed linked start"
    ON)

if (COMMENTO_JUCE_SOURCE_DIR)
    add_subdirectory("${COMMENTO_JUCE_SOURCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.13
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(Commento
    PRODUCT_NAME "Commento"
    COMPANY_NAME "Commento"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Commento usa l'ingresso audio per creare la memoria del sax."
)

target_sources(Commento PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/Engine/AmbientSynth.cpp
    Source/Engine/EcosystemEngine.cpp
    Source/Engine/PerformanceLevels.cpp
    Source/Engine/SaxProcessor.cpp
    Source/Engine/Scenarios.cpp
    Source/Hardware/Model12AudioRouter.cpp
)

target_include_directories(Commento PRIVATE Source)
target_compile_features(Commento PRIVATE cxx_std_17)

target_compile_definitions(Commento PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND COMMENTO_ALSA_DIAGNOSTICS)
    target_compile_definitions(Commento PRIVATE JUCE_ALSA_LOGGING=1)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND COMMENTO_ALSA_BYPASS_PCM_LINK)
    target_sources(Commento PRIVATE
        Source/Hardware/AlsaPcmLinkBypass.cpp
    )
    target_link_options(Commento PRIVATE "LINKER:--wrap=snd_pcm_link")
endif()

target_link_libraries(Commento PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

juce_generate_juce_header(Commento)

include(CTest)
if (BUILD_TESTING)
    juce_add_console_app(CommentoEngineTests PRODUCT_NAME "Commento Engine Tests")
    target_sources(CommentoEngineTests PRIVATE
        Tests/EcosystemEngineTests.cpp
        Source/Engine/AmbientSynth.cpp
        Source/Engine/EcosystemEngine.cpp
        Source/Engine/PerformanceLevels.cpp
        Source/Engine/SaxProcessor.cpp
        Source/Engine/Scenarios.cpp
        Source/Hardware/Model12AudioRouter.cpp
    )
    target_include_directories(CommentoEngineTests PRIVATE Source)
    target_compile_features(CommentoEngineTests PRIVATE cxx_std_17)
    target_compile_definitions(CommentoEngineTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
    )
    target_link_libraries(CommentoEngineTests PRIVATE
        juce::juce_audio_devices
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
    juce_generate_juce_header(CommentoEngineTests)
    add_test(NAME CommentoEngineTests COMMAND CommentoEngineTests)
endif()
