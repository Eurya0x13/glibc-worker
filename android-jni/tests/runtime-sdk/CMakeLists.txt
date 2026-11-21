cmake_minimum_required(VERSION 3.22)
project(RuntimeSdkConsumer LANGUAGES C)
find_package(GlibcWorkerAndroidJni CONFIG REQUIRED)
find_path(GW_JNI_INCLUDE_DIR jni.h REQUIRED)
add_executable(runtime_sdk host.c host-guest-signalfd.c host-jni-recovery.c)
target_compile_options(runtime_sdk PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_include_directories(runtime_sdk PRIVATE ${GW_JNI_INCLUDE_DIR})
option(GW_TEST_NATIVE_UNWIND "Test native unwind integration" OFF)
if(GW_TEST_NATIVE_UNWIND)
  target_link_libraries(runtime_sdk PRIVATE glibc_worker::nonlocal_interpose)
  target_compile_definitions(runtime_sdk PRIVATE GW_TEST_NATIVE_UNWIND)
endif()
target_link_libraries(runtime_sdk PRIVATE
  glibc_worker::signal_bootstrap glibc_worker::android_jni_static ${CMAKE_DL_LIBS} m)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sdk-inputs.txt"
  CONTENT "$<TARGET_FILE:glibc_worker::android_jni_static>\n$<TARGET_FILE:glibc_worker::dlopen_static>\n$<TARGET_FILE:glibc_worker::domains>\n$<TARGET_FILE:glibc_worker::signal_bootstrap>\n$<TARGET_FILE:glibc_worker::signal_interpose>\n$<TARGET_FILE:glibc_worker::nonlocal_interpose>\n")

if(ANDROID)
  if(NOT GW_VM_STARTUP_ROOT OR NOT GW_VM_STARTUP_PROFILE)
    message(FATAL_ERROR "Android test requires explicit reviewed VM startup fixtures/profile")
  endif()
  configure_file(${GW_VM_STARTUP_PROFILE} ${CMAKE_CURRENT_BINARY_DIR}/jre-layout.h COPYONLY)
  target_sources(runtime_sdk PRIVATE
    ${GW_VM_STARTUP_ROOT}/tests/same-thread/vm-start.c
    ${GW_VM_STARTUP_ROOT}/tests/native/harness/jre_layout_spike.c
    ${GW_VM_STARTUP_ROOT}/tests/native/harness/jre_exec_compat.c)
  target_include_directories(runtime_sdk PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
    ${GW_VM_STARTUP_ROOT}/tests/same-thread
    ${GW_VM_STARTUP_ROOT}/tests/native/harness)
  target_compile_options(runtime_sdk PRIVATE -fno-emulated-tls)
endif()
