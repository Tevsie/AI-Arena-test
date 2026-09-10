cmake_minimum_required(VERSION 3.16)
project(atw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(ATW_CORE
    src/core/window_x11.cpp
    src/core/window_headless.cpp
)
set(ATW_RENDER
    src/render/gl.cpp
    src/render/renderer.cpp
)
set(ATW_GAME
    src/game/game.cpp
)
set(ATW_AUDIO
    src/audio/audio.cpp
    src/audio/audio_linux.cpp
)

add_executable(atw src/main.cpp ${ATW_CORE} ${ATW_RENDER} ${ATW_GAME} ${ATW_AUDIO})
target_compile_options(atw PRIVATE -O3 -ffast-math -Wall -Wextra)
target_link_libraries(atw PRIVATE dl)

enable_testing()
add_executable(atw_tests tests/tests.cpp ${ATW_CORE} ${ATW_RENDER} ${ATW_GAME} ${ATW_AUDIO})
target_include_directories(atw_tests PRIVATE .)
target_compile_options(atw_tests PRIVATE -O2 -Wall -Wextra)
target_link_libraries(atw_tests PRIVATE dl)
add_test(NAME core COMMAND atw_tests)
