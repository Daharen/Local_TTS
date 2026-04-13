cmake_minimum_required(VERSION 3.16)
project(Local_TTS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(local_tts
    src/main.cpp
    src/paths.cpp
    src/whisper_runner.cpp
    src/live_mode.cpp
    src/audio_capture.cpp
    src/text_injection.cpp
    src/llm_correction.cpp
)

if(WIN32)
    target_link_libraries(local_tts PRIVATE user32 shell32 winmm)
endif()
