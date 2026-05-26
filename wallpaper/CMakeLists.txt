cmake_minimum_required(VERSION 3.20)
project(fluid-wallpaper LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CUDAToolkit REQUIRED)

# Console-subsystem exe (no WIN32 flag) — Phase 1 keeps a console window
# for logs and Ctrl+C exit. Phase 5 will turn this into a proper tray app.
add_executable(fluid-wallpaper
    src/main.cpp
    src/wallpaper.cpp
    src/renderer.cpp
    src/cuda_renderer.cu
    src/fluid_sim.cu
    src/input.cpp
)

# Turing (75), Ampere (80, 86), Ada (89), Hopper (90). Drop the highest
# entries if nvcc complains about an unsupported arch on older toolkits.
set_target_properties(fluid-wallpaper PROPERTIES
    CUDA_ARCHITECTURES "75;80;86;89;90"
    CUDA_SEPARABLE_COMPILATION ON
)

target_include_directories(fluid-wallpaper PRIVATE src)

target_link_libraries(fluid-wallpaper PRIVATE
    d3d11
    dxgi
    d3dcompiler
    CUDA::cudart
)

target_compile_definitions(fluid-wallpaper PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# D3DCompile runs at startup against animated.hlsl, so the shader needs
# to sit next to the exe in the build output.
add_custom_command(TARGET fluid-wallpaper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:fluid-wallpaper>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/animated.hlsl
        $<TARGET_FILE_DIR:fluid-wallpaper>/shaders/animated.hlsl
    VERBATIM
)
