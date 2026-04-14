cmake_minimum_required(VERSION 3.16)
project(Local_TTS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOCAL_TTS_ENABLE_DASHBOARD "Enable diagnostics dashboard support" ON)

set(LOCAL_TTS_SOURCES
    src/main.cpp
    src/paths.cpp
    src/app_config.cpp
    src/whisper_runner.cpp
    src/live_mode.cpp
    src/audio_capture.cpp
    src/text_injection.cpp
    src/llm_correction.cpp
)

if(LOCAL_TTS_ENABLE_DASHBOARD)
    list(APPEND LOCAL_TTS_SOURCES
        src/diagnostics.cpp
        src/dashboard.cpp
    )
endif()

add_executable(local_tts ${LOCAL_TTS_SOURCES})

if(LOCAL_TTS_ENABLE_DASHBOARD)
    target_compile_definitions(local_tts PRIVATE LOCAL_TTS_ENABLE_DASHBOARD=1)
else()
    target_compile_definitions(local_tts PRIVATE LOCAL_TTS_ENABLE_DASHBOARD=0)
endif()

if(WIN32)
    target_link_libraries(local_tts PRIVATE user32 shell32 winmm)
endif()
