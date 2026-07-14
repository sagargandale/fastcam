cmake_minimum_required(VERSION 3.22.1)
project("fastcam-engine" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

add_library(fastcam-engine SHARED
    native_bridge.cpp
    CameraEngine.cpp
    GlRenderer.cpp
    MediaEncoder.cpp
    PidController.cpp
)

target_link_libraries(fastcam-engine
    android
    log
    camera2ndk
    mediandk
    EGL
    GLESv3
    OpenSLES
    glm::glm
)
