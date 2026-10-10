cmake_minimum_required(VERSION 3.22)
project(ggak_android LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

set(NNG_ENABLE_NNGCAT OFF CACHE BOOL "" FORCE)
set(NNG_ENABLE_TLS OFF CACHE BOOL "" FORCE)
set(NNG_ENABLE_HTTP OFF CACHE BOOL "" FORCE)
set(NNG_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nng
    GIT_REPOSITORY https://github.com/nanomsg/nng.git
    GIT_TAG v1.8.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nng)

set(APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(ggak_android SHARED
    android_main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../../../ggak_core.cpp
    ${APP_GLUE}/android_native_app_glue.c
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_android.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_compile_features(ggak_android PRIVATE cxx_std_17)
target_include_directories(ggak_android PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../../..
    ${CMAKE_CURRENT_LIST_DIR}
    ${APP_GLUE}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(ggak_android PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_compile_definitions(ggak_android PRIVATE IMGUI_USER_CONFIG=\"android_imconfig.h\")
target_link_options(ggak_android PRIVATE
    -u ANativeActivity_onCreate
    "LINKER:-z,max-page-size=16384"
    "LINKER:-z,common-page-size=16384")
target_include_directories(ggak_android PRIVATE ${nng_SOURCE_DIR}/include)
target_link_libraries(ggak_android PRIVATE android log EGL GLESv3 nng)
