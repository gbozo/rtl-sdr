cmake_minimum_required(VERSION 3.7.2...4.2.0)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(rtl_stream_tests C)
    enable_testing()
endif()

find_package(Threads REQUIRED)
enable_language(CXX)
set(STREAM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(CLIENT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../stream_client)
option(TEST_VKFFT "Build and run Vulkan GPU tests (requires a working GPU)" OFF)
if(TEST_VKFFT)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Modules/RTLStreamVkFFT.cmake)
endif()
set(STREAM_TEST_SOURCES
    ${STREAM_SRC}/rtl_stream_server.c
    ${STREAM_SRC}/rtl_stream_proto.c
    ${STREAM_SRC}/rtl_ring.c
    ${STREAM_SRC}/rtl_dsp.c
    ${STREAM_SRC}/fft/fft_cpu.c)

add_executable(test_dsp test_dsp.c ${STREAM_SRC}/rtl_dsp.c)
# This white-box test includes rtl_dsp.c with allocation hooks.
add_executable(test_dsp_spectral test_dsp_spectral.c)
add_executable(test_stream test_stream.c ${STREAM_TEST_SOURCES})
add_executable(test_ring test_ring.c ${STREAM_SRC}/rtl_ring.c)
add_executable(test_server test_server.c ${STREAM_TEST_SOURCES})
add_executable(test_proto test_proto.c ${STREAM_SRC}/rtl_stream_proto.c)
add_executable(test_client_c test_client_c.c ${CLIENT_SRC}/c/client.c)
add_executable(test_client_cpp test_client_cpp.cpp ${CLIENT_SRC}/cpp/client.cpp)

foreach(target test_dsp test_dsp_spectral test_stream test_server)
    target_link_libraries(${target} PRIVATE Threads::Threads m)
endforeach()

# Test-only reference; never linked into the production Vulkan executable.
add_library(fft_reference OBJECT ${STREAM_SRC}/fft/fft_cpu.c)
target_compile_definitions(fft_reference PRIVATE
    fft_plan_create=reference_create fft_plan_destroy=reference_destroy
    fft_execute=reference_execute fft_get_window=reference_window
    fft_backend_name=reference_name)
set_target_properties(fft_reference PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
add_executable(test_fft_backend test_fft_backend.c ${STREAM_SRC}/fft/fft_cpu.c
    $<TARGET_OBJECTS:fft_reference>)
set(FFT_TEST_TARGETS test_fft_backend)
if(TEST_VKFFT)
    add_executable(test_fft_vkfft test_fft_backend.c ${STREAM_SRC}/fft/fft_vkfft.c
        $<TARGET_OBJECTS:fft_reference>)
    set(GPU_STREAM_SOURCES ${STREAM_TEST_SOURCES})
    list(REMOVE_ITEM GPU_STREAM_SOURCES ${STREAM_SRC}/fft/fft_cpu.c)
    list(APPEND GPU_STREAM_SOURCES ${STREAM_SRC}/fft/fft_vkfft.c)
    add_executable(test_stream_vkfft test_stream.c ${GPU_STREAM_SOURCES})
    add_executable(test_server_vkfft test_server.c ${GPU_STREAM_SOURCES})
    add_executable(test_fft_vkfft_faults test_fft_vkfft_faults.c)
    foreach(target test_fft_vkfft test_stream_vkfft test_server_vkfft test_fft_vkfft_faults)
        target_link_libraries(${target} PRIVATE rtl_stream_vkfft)
        set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
        list(APPEND FFT_TEST_TARGETS ${target})
    endforeach()
endif()
foreach(target ${FFT_TEST_TARGETS})
    target_include_directories(${target} PRIVATE ${STREAM_SRC})
    target_link_libraries(${target} PRIVATE Threads::Threads m)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 60)
    if(target MATCHES "vkfft")
        set_tests_properties(${target} PROPERTIES FAIL_REGULAR_EXPRESSION
            "Validation Error;SYNC-HAZARD")
    endif()
endforeach()
target_link_libraries(test_ring PRIVATE Threads::Threads)

foreach(target test_dsp test_dsp_spectral test_stream test_ring test_server
        test_proto test_client_c test_client_cpp)
    target_include_directories(${target} PRIVATE ${STREAM_SRC})
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES
        CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 60)
endforeach()
