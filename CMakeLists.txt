cmake_minimum_required(VERSION 3.20)
project(shader_jit_engine CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── GLAD (pre-generated: python -m glad --profile core --api gl=3.3 --generator c --out-path vendor/glad) ──
add_library(glad STATIC vendor/glad/src/glad.c)
target_include_directories(glad PUBLIC vendor/glad/include)

# ── GLFW (fetched from source) ────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ── System libraries ──────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(shader_jit_engine src/main.cpp)

target_include_directories(shader_jit_engine PRIVATE
    vendor/glad/include
    vendor
)

target_link_libraries(shader_jit_engine PRIVATE
    glad
    glfw
    OpenGL::GL
    Threads::Threads
)

target_compile_options(shader_jit_engine PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>
)

# Copy shaders next to the binary so relative paths work at runtime
add_custom_command(TARGET shader_jit_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:shader_jit_engine>/shaders
)
