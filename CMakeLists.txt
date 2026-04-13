cmake_minimum_required(VERSION 3.16)
project(Local_TTS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(local_tts
    src/main.cpp
    src/paths.cpp
)
