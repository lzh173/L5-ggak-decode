cmake_minimum_required(VERSION 3.16)
project(ggak_decode LANGUAGES CXX)

include(FetchContent)

set(NNG_ENABLE_NNGCAT OFF CACHE BOOL "" FORCE)
set(NNG_ENABLE_TLS OFF CACHE BOOL "" FORCE)
set(NNG_ENABLE_HTTP OFF CACHE BOOL "" FORCE)
set(NNG_TESTS OFF CACHE BOOL "" FORCE)

set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)

add_executable(ggak_decode ggak_decode.cpp)
target_compile_features(ggak_decode PRIVATE cxx_std_17)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    nng
    GIT_REPOSITORY https://github.com/nanomsg/nng.git
    GIT_TAG v1.8.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nng)

file(TO_NATIVE_PATH "${imgui_SOURCE_DIR}" imgui_native_dir)
file(TO_NATIVE_PATH "${imgui_SOURCE_DIR}/backends" imgui_native_backends_dir)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PRIVATE "${imgui_native_dir}" "${imgui_native_backends_dir}")
target_include_directories(imgui_lib INTERFACE "${imgui_native_dir}" "${imgui_native_backends_dir}")
target_link_libraries(imgui_lib PUBLIC glfw)

add_executable(ggak_gui ggak_gui.cpp)
target_compile_features(ggak_gui PRIVATE cxx_std_17)
if (MSVC)
    target_compile_options(ggak_gui PRIVATE /utf-8)
elseif (MINGW OR CMAKE_CXX_COMPILER MATCHES "mingw|ucrt64")
    target_compile_options(ggak_gui PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()
if (WIN32 OR MINGW OR CMAKE_CXX_COMPILER MATCHES "mingw|ucrt64")
    target_link_libraries(ggak_gui PRIVATE imgui_lib nng opengl32)
    if (WIN32 OR MINGW)
        target_link_libraries(ggak_gui PRIVATE comdlg32)
    endif()
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(ggak_gui PRIVATE imgui_lib OpenGL::GL)
endif()
target_include_directories(ggak_gui PRIVATE "${imgui_native_dir}" "${imgui_native_backends_dir}")

if (MSVC)
    target_compile_options(ggak_decode PRIVATE  /W4 /permissive-)
    target_compile_options(ggak_gui PRIVATE /W4 /permissive-)
else()
    target_compile_options(ggak_decode PRIVATE -O3 -Wall -Wextra -Wpedantic)
    target_compile_options(ggak_gui PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
