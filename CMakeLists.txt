cmake_minimum_required(VERSION 3.16)
project(merge-folders LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
include(FetchContent)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_library(merge-core
    src/merge_plan.cpp
    src/rsync_process.cpp)
target_include_directories(merge-core PUBLIC src)
target_compile_options(merge-core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(merge-folders
    src/main.cpp
    src/app.cpp
    src/dir_browser.cpp)
target_link_libraries(merge-folders PRIVATE merge-core imgui Threads::Threads)
target_compile_options(merge-folders PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(merge-core-tests tests/merge_plan_test.cpp)
target_link_libraries(merge-core-tests PRIVATE merge-core)
target_compile_options(merge-core-tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME merge-core-tests COMMAND merge-core-tests)

install(TARGETS merge-folders RUNTIME DESTINATION bin)
