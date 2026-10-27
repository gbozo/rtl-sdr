# Shared by production and explicitly opted-in hardware-free GPU tests.
if(TARGET rtl_stream_vkfft)
    return()
endif()
if(CMAKE_VERSION VERSION_LESS 3.18)
    message(FATAL_ERROR "VkFFT builds require CMake 3.18 or newer")
endif()
enable_language(CXX)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_package(glslang CONFIG REQUIRED)
get_target_property(_glslang_includes glslang::glslang INTERFACE_INCLUDE_DIRECTORIES)
find_path(RTL_GLSLANG_C_INCLUDE glslang_c_interface.h
    HINTS ${_glslang_includes} PATH_SUFFIXES glslang/Include REQUIRED)
include(FetchContent)
FetchContent_Declare(VkFFT
    GIT_REPOSITORY https://github.com/DTolm/VkFFT.git
    GIT_TAG 066a17c17068c0f11c9298d848c2976c71fad1c1
    SOURCE_SUBDIR _fetch_only)
FetchContent_MakeAvailable(VkFFT)
add_library(rtl_stream_vkfft INTERFACE)
target_include_directories(rtl_stream_vkfft SYSTEM INTERFACE
    ${vkfft_SOURCE_DIR} ${vkfft_SOURCE_DIR}/vkFFT ${RTL_GLSLANG_C_INCLUDE})
target_compile_definitions(rtl_stream_vkfft INTERFACE
    HAVE_VKFFT VKFFT_BACKEND=0)
target_compile_features(rtl_stream_vkfft INTERFACE cxx_std_17)
target_link_libraries(rtl_stream_vkfft INTERFACE Vulkan::Vulkan
    glslang::SPIRV glslang::glslang glslang::glslang-default-resource-limits
    Threads::Threads)
