cmake_minimum_required(VERSION 3.16)
project(Local_TTS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOCAL_TTS_ENABLE_DASHBOARD "Enable diagnostics dashboard support" ON)
set(LOCAL_TTS_WHISPER_CPP_ROOT "F:/Local_TTS_Large_Data/external/whisper.cpp_cuda" CACHE PATH "Path to external whisper.cpp source tree")

if(NOT EXISTS "${LOCAL_TTS_WHISPER_CPP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "LOCAL_TTS_WHISPER_CPP_ROOT is invalid: '${LOCAL_TTS_WHISPER_CPP_ROOT}'. Expected whisper.cpp source tree with CMakeLists.txt.")
endif()

add_subdirectory("${LOCAL_TTS_WHISPER_CPP_ROOT}" "${CMAKE_BINARY_DIR}/external/whisper_cpp")

get_property(LOCAL_TTS_WHISPER_CPP_TARGETS DIRECTORY "${CMAKE_BINARY_DIR}/external/whisper_cpp" PROPERTY BUILDSYSTEM_TARGETS)
if(NOT LOCAL_TTS_WHISPER_CPP_TARGETS)
    get_property(LOCAL_TTS_WHISPER_CPP_TARGETS DIRECTORY "${LOCAL_TTS_WHISPER_CPP_ROOT}" PROPERTY BUILDSYSTEM_TARGETS)
endif()
if(NOT LOCAL_TTS_WHISPER_CPP_TARGETS)
    get_property(LOCAL_TTS_WHISPER_CPP_TARGETS GLOBAL PROPERTY TARGETS)
endif()

set(LOCAL_TTS_WHISPER_CANDIDATE_NAMES whisper whisper-lib whisper_cpp whisper.cpp ggml-whisper)
set(LOCAL_TTS_WHISPER_LIBRARY_TARGET "")

foreach(candidate IN LISTS LOCAL_TTS_WHISPER_CANDIDATE_NAMES)
    if(TARGET "${candidate}")
        get_target_property(candidate_type "${candidate}" TYPE)
        if(candidate_type STREQUAL "STATIC_LIBRARY" OR candidate_type STREQUAL "SHARED_LIBRARY")
            set(LOCAL_TTS_WHISPER_LIBRARY_TARGET "${candidate}")
            break()
        endif()
    endif()
endforeach()

foreach(candidate IN LISTS LOCAL_TTS_WHISPER_CPP_TARGETS)
    if(LOCAL_TTS_WHISPER_LIBRARY_TARGET)
        break()
    endif()
    get_target_property(candidate_type "${candidate}" TYPE)
    if((candidate_type STREQUAL "STATIC_LIBRARY" OR candidate_type STREQUAL "SHARED_LIBRARY") AND
       candidate MATCHES "(^|[-_.:])whisper($|[-_.:])")
        set(LOCAL_TTS_WHISPER_LIBRARY_TARGET "${candidate}")
        break()
    endif()
endforeach()

if(LOCAL_TTS_WHISPER_LIBRARY_TARGET STREQUAL "")
    message(FATAL_ERROR "Unable to find whisper.cpp library target in '${LOCAL_TTS_WHISPER_CPP_ROOT}'. Found targets: ${LOCAL_TTS_WHISPER_CPP_TARGETS}")
endif()

set(LOCAL_TTS_SOURCES
    src/main.cpp
    src/paths.cpp
    src/app_config.cpp
    src/whisper_runner.cpp
    src/live_mode.cpp
    src/audio_capture.cpp
    src/text_injection.cpp
    src/llm_correction.cpp
    src/pipeline_debug.cpp
    src/streaming_dictation.cpp
)

if(LOCAL_TTS_ENABLE_DASHBOARD)
    list(APPEND LOCAL_TTS_SOURCES
        src/diagnostics.cpp
        src/dashboard.cpp
    )
endif()

add_executable(local_tts ${LOCAL_TTS_SOURCES})
target_link_libraries(local_tts PRIVATE ${LOCAL_TTS_WHISPER_LIBRARY_TARGET})

if(EXISTS "${LOCAL_TTS_WHISPER_CPP_ROOT}/include")
    target_include_directories(local_tts PRIVATE "${LOCAL_TTS_WHISPER_CPP_ROOT}/include")
endif()

if(LOCAL_TTS_ENABLE_DASHBOARD)
    target_compile_definitions(local_tts PRIVATE LOCAL_TTS_ENABLE_DASHBOARD=1)
else()
    target_compile_definitions(local_tts PRIVATE LOCAL_TTS_ENABLE_DASHBOARD=0)
endif()

if(WIN32)
    target_link_libraries(local_tts PRIVATE user32 shell32 winmm winhttp)
endif()
