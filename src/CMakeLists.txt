# Copyright 2012-2020 Osmocom Project
#
# This file is part of rtl-sdr
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

########################################################################
# Setup shared library variant
########################################################################
add_library(rtlsdr SHARED librtlsdr.c
  tuner_e4k.c tuner_fc0012.c tuner_fc0013.c tuner_fc2580.c tuner_r82xx.c)
target_link_libraries(rtlsdr ${LIBUSB_LIBRARIES} ${THREADS_PTHREADS_LIBRARY})
target_include_directories(rtlsdr PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>  # <prefix>/include
  ${LIBUSB_INCLUDE_DIRS}
  ${THREADS_PTHREADS_INCLUDE_DIR}
  )
set_target_properties(rtlsdr PROPERTIES DEFINE_SYMBOL "rtlsdr_EXPORTS")
set_target_properties(rtlsdr PROPERTIES OUTPUT_NAME rtlsdr)
set_target_properties(rtlsdr PROPERTIES SOVERSION 0)
set_target_properties(rtlsdr PROPERTIES VERSION ${LIBVER})
generate_export_header(rtlsdr)

########################################################################
# Setup static library variant
########################################################################
add_library(rtlsdr_static STATIC librtlsdr.c
  tuner_e4k.c tuner_fc0012.c tuner_fc0013.c tuner_fc2580.c tuner_r82xx.c)
target_link_libraries(rtlsdr_static ${LIBUSB_LIBRARIES} ${THREADS_PTHREADS_LIBRARY})
target_include_directories(rtlsdr_static PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>  # <prefix>/include
  ${LIBUSB_INCLUDE_DIRS}
  ${THREADS_PTHREADS_INCLUDE_DIR}
  )
set_property(TARGET rtlsdr_static APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
if(NOT WIN32)
# Force same library filename for static and shared variants of the library
set_target_properties(rtlsdr_static PROPERTIES OUTPUT_NAME rtlsdr)
endif()
generate_export_header(rtlsdr_static)

########################################################################
# Set up Windows DLL resource files
########################################################################
IF(MSVC)
    include(${CMAKE_SOURCE_DIR}/cmake/Modules/Version.cmake)

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/rtlsdr.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/rtlsdr.rc
    @ONLY)
  target_sources(rtlsdr PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/rtlsdr.rc)
  target_sources(rtlsdr_static PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/rtlsdr.rc)
ENDIF(MSVC)

########################################################################
# Setup libraries used in executables
########################################################################
add_library(convenience_static STATIC
    convenience/convenience.c
)
target_include_directories(convenience_static
  PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
add_library(libgetopt_static STATIC
    getopt/getopt.c
)
target_link_libraries(convenience_static
    rtlsdr
)
endif()

########################################################################
# Build utility
########################################################################
add_executable(rtl_sdr rtl_sdr.c)
add_executable(rtl_tcp rtl_tcp.c)
add_executable(rtl_test rtl_test.c)
add_executable(rtl_fm rtl_fm.c)
add_executable(rtl_eeprom rtl_eeprom.c)
add_executable(rtl_adsb rtl_adsb.c)
add_executable(rtl_power rtl_power.c)
add_executable(rtl_biast rtl_biast.c)
add_executable(rtl_list rtl_list.c)
option(ENABLE_VKFFT "Enable VkFFT GPU-accelerated FFT backend" OFF)

set(RTL_STREAM_SOURCES
    rtl_stream.c
    rtl_stream_server.c
    rtl_stream_proto.c
    rtl_dsp.c
    rtl_ring.c
)

if(ENABLE_VKFFT)
    list(APPEND RTL_STREAM_SOURCES fft/fft_vkfft.c)

    include(FetchContent)
    FetchContent_Declare(
        VkFFT
        GIT_REPOSITORY https://github.com/DTolm/VkFFT.git
        GIT_TAG v1.3.4
    )
    FetchContent_GetProperties(VkFFT)
    if(NOT vkfft_POPULATED)
        FetchContent_Populate(VkFFT)
    endif()

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(VULKAN vulkan IMPORTED_TARGET)
    endif()
    if(VULKAN_FOUND)
        set(VULKAN_TARGET PkgConfig::VULKAN)
    else()
        find_library(VULKAN_LIB vulkan)
        find_path(VULKAN_INCLUDE vulkan/vulkan.h)
        if(VULKAN_LIB AND VULKAN_INCLUDE)
            add_library(Vulkan_rtl UNKNOWN IMPORTED)
            set_target_properties(Vulkan_rtl PROPERTIES
                IMPORTED_LOCATION "${VULKAN_LIB}"
                INTERFACE_INCLUDE_DIRECTORIES "${VULKAN_INCLUDE}"
            )
            set(VULKAN_TARGET Vulkan_rtl)
        endif()
    endif()

    if(NOT VULKAN_TARGET)
        message(FATAL_ERROR "Vulkan library not found. Install libvulkan-dev or disable ENABLE_VKFFT")
    endif()

    add_executable(rtl_stream ${RTL_STREAM_SOURCES})
    target_link_libraries(rtl_stream rtlsdr_static
        ${LIBUSB_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ${VULKAN_TARGET}
    )
    target_include_directories(rtl_stream PRIVATE
        ${vkfft_SOURCE_DIR}
        ${vkfft_SOURCE_DIR}/vkFFT
        /usr/include/glslang/Include
    )
    target_compile_definitions(rtl_stream PRIVATE HAVE_VKFFT VKFFT_BACKEND=0)

    find_library(GLSLANG_LIB glslang)
    find_library(GLSLANG_OSDEP OSDependent)
    find_library(GLSLANG_MI MachineIndependent)
    find_library(GLSLANG_OGLC OGLCompiler)
    find_library(GLSLANG_GENCODE GenericCodeGen)
    find_library(GLSLANG_HLSL HLSL)
    find_library(GLSLANG_SPIRV SPIRV)
    find_library(GLSLANG_RLIMITS glslang-default-resource-limits)
    find_library(SPIRV_TOOLS SPIRV-Tools)
    find_library(SPIRV_TOOLS_OPT SPIRV-Tools-opt)
    find_library(SPIRV_TOOLS_LINK SPIRV-Tools-link)
    target_link_libraries(rtl_stream
        "-Wl,--start-group"
        ${GLSLANG_GENCODE}
        ${GLSLANG_OSDEP}
        ${GLSLANG_OGLC}
        ${GLSLANG_MI}
        ${GLSLANG_LIB}
        ${SPIRV_TOOLS}
        ${SPIRV_TOOLS_LINK}
        ${SPIRV_TOOLS_OPT}
        ${GLSLANG_SPIRV}
        ${GLSLANG_HLSL}
        ${GLSLANG_RLIMITS}
        "-Wl,--end-group"
        stdc++
    )
else()
    list(APPEND RTL_STREAM_SOURCES fft/fft_cpu.c)
    add_executable(rtl_stream ${RTL_STREAM_SOURCES})
    target_link_libraries(rtl_stream rtlsdr_static
        ${LIBUSB_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
    )
endif()

set(INSTALL_TARGETS rtlsdr rtlsdr_static rtl_sdr rtl_tcp rtl_test rtl_fm rtl_eeprom rtl_adsb rtl_power rtl_biast rtl_list rtl_stream)

target_link_libraries(rtl_sdr rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_tcp rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_test rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_fm rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_eeprom rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_adsb rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_power rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_biast rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_list rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
# Optional VkFFT backend - enable with find_package(VkFFT) and add fft/fft_vkfft.c to sources
if(UNIX)
target_link_libraries(rtl_fm m)
target_link_libraries(rtl_adsb m)
target_link_libraries(rtl_power m)
target_link_libraries(rtl_stream m)
if(APPLE OR CMAKE_SYSTEM MATCHES "OpenBSD")
    target_link_libraries(rtl_test m)
else()
    target_link_libraries(rtl_test m rt)
endif()
endif()

if(WIN32)
target_link_libraries(rtl_sdr libgetopt_static)
target_link_libraries(rtl_tcp ws2_32 libgetopt_static)
target_link_libraries(rtl_test libgetopt_static)
target_link_libraries(rtl_fm libgetopt_static)
target_link_libraries(rtl_eeprom libgetopt_static)
target_link_libraries(rtl_adsb libgetopt_static)
target_link_libraries(rtl_power libgetopt_static)
target_link_libraries(rtl_biast libgetopt_static)
target_link_libraries(rtl_list libgetopt_static)
target_link_libraries(rtl_stream libgetopt_static)
set_property(TARGET rtl_sdr APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_tcp APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_test APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_fm APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_eeprom APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_adsb APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_power APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_biast APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_list APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_stream APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
endif()
########################################################################
# Install built library files & utilities
########################################################################
install(TARGETS rtlsdr EXPORT RTLSDR-export
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # .so/.dylib file
  )
install(TARGETS rtlsdr_static EXPORT RTLSDR-export
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} # .so/.dylib file
  )
install(TARGETS rtl_sdr rtl_tcp rtl_test rtl_fm rtl_eeprom rtl_adsb rtl_power rtl_biast rtl_list rtl_stream
  DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
