cmake_minimum_required(VERSION 3.22)
project(GlibcWorkerAndroidJni VERSION 0.1 LANGUAGES C)
find_package(Threads REQUIRED)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Werror)
if(ANDROID)
  add_compile_options(-fno-emulated-tls)
endif()
find_package(GlibcWorkerDlopen CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_path(GW_JNI_INCLUDE_DIR jni.h REQUIRED)
find_path(GW_FFI_INCLUDE_DIR ffi.h REQUIRED)
find_library(GW_FFI_LIBRARY NAMES ffi REQUIRED)
set(GW_FFI_LICENSE_FILE "" CACHE FILEPATH "License accompanying the provided libffi archive")
add_library(glibc_worker::android_ffi STATIC IMPORTED)
set_target_properties(glibc_worker::android_ffi PROPERTIES IMPORTED_LOCATION "${GW_FFI_LIBRARY}")
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/host-jni.inc
                          ${CMAKE_CURRENT_BINARY_DIR}/generated/guest-jni.inc
                          ${CMAKE_CURRENT_BINARY_DIR}/generated/guest-policy.inc
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-jni.py
          --jni-header ${GW_JNI_INCLUDE_DIR}/jni.h
          --output ${CMAKE_CURRENT_BINARY_DIR}/generated
  DEPENDS scripts/gen-jni.py ${GW_JNI_INCLUDE_DIR}/jni.h VERBATIM)
add_custom_target(jni_tables DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/host-jni.inc
                                    ${CMAKE_CURRENT_BINARY_DIR}/generated/guest-jni.inc
                                    ${CMAKE_CURRENT_BINARY_DIR}/generated/guest-policy.inc)
set(GW_BINDING_BACKEND "zig" CACHE STRING "Host resource stores (zig only)")
if(NOT GW_BINDING_BACKEND STREQUAL "zig")
  message(FATAL_ERROR "The C resource backend was removed; use zig or omit GW_BINDING_BACKEND")
endif()
find_program(GW_ZIG_EXECUTABLE NAMES zig REQUIRED)
set(zig_target native)
if(ANDROID)
  if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(zig_target aarch64-linux-android.29)
  elseif(ANDROID_ABI STREQUAL "x86_64")
    set(zig_target x86_64-linux-android.29)
  else()
    message(FATAL_ERROR "The Host stores support Android ARM64 and x86_64")
  endif()
endif()
set(binding_objects ${CMAKE_CURRENT_BINARY_DIR}/host_store.o)
add_custom_command(OUTPUT ${binding_objects}
  COMMAND ${GW_ZIG_EXECUTABLE} build --build-file ${CMAKE_CURRENT_SOURCE_DIR}/build.zig
          --prefix ${CMAKE_CURRENT_BINARY_DIR}
          --cache-dir ${CMAKE_CURRENT_BINARY_DIR}/zig-cache
          --global-cache-dir ${CMAKE_CURRENT_SOURCE_DIR}/build/zig-global-cache
          host-store -Dtarget=${zig_target} -Dcpu=baseline
  DEPENDS build.zig src/host/host_store.zig src/host/binding_store.zig src/host/binding_store.h
          src/host/slot_store.zig src/host/slot_index.zig src/host/slot_store.h VERBATIM)
set_source_files_properties(${binding_objects} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_custom_target(android_jni_binding_store DEPENDS ${binding_objects})
install(FILES third_party/zig/LICENSE
        DESTINATION share/licenses/glibc-worker/android-jni RENAME zig-LICENSE.txt)
add_library(android_jni_objects OBJECT src/host/domain_runtime.c
  src/host/runtime_bridge.c src/host/full_host.c src/host/references.c
  src/host/bindings.c src/host/failure.c src/host/slot_allocator.c src/host/loader_transport.c src/host/command.c src/host/binding_table.c src/host/array_copy.c src/host/loader_backend.c src/host/image_policy.c)
add_dependencies(android_jni_objects jni_tables)
target_compile_definitions(android_jni_objects PRIVATE GW_DOMAIN_JNI)
option(GW_NATIVE_UNWIND_INTEGRATION "Enable incomplete native nonlocal recovery validation" OFF)
if(GW_NATIVE_UNWIND_INTEGRATION)
  target_compile_definitions(android_jni_objects PRIVATE GW_DOMAIN_NONLOCAL)
endif()
option(GW_NATIVE_UNWIND_TEST_HOOKS "Enable deterministic native unwind injection" OFF)
if(GW_NATIVE_UNWIND_TEST_HOOKS)
  if(NOT GW_NATIVE_UNWIND_INTEGRATION)
    message(FATAL_ERROR "Unwind test hooks require native unwind integration")
  endif()
  target_compile_definitions(android_jni_objects PRIVATE GW_DOMAIN_NONLOCAL_TEST)
endif()
target_include_directories(android_jni_objects PRIVATE include src/common src/host src/guest
  ${CMAKE_CURRENT_BINARY_DIR} ${GW_JNI_INCLUDE_DIR} ${GW_FFI_INCLUDE_DIR})
target_link_libraries(android_jni_objects PUBLIC glibc_worker::dlopen glibc_worker::domains)
add_library(android_jni SHARED $<TARGET_OBJECTS:android_jni_objects> ${binding_objects})
add_library(android_jni_static STATIC $<TARGET_OBJECTS:android_jni_objects> ${binding_objects})
foreach(target android_jni android_jni_static)
  add_dependencies(${target} android_jni_binding_store)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/glibc-worker/android-jni>)
  target_link_libraries(${target} PUBLIC glibc_worker::dlopen glibc_worker::domains PRIVATE glibc_worker::android_ffi ${CMAKE_DL_LIBS})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME gw_android_jni)
endforeach()
find_package(Java COMPONENTS Development REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/android-jni-helpers.jar
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/classes
  COMMAND ${Java_JAVAC_EXECUTABLE} --release 8 -d ${CMAKE_CURRENT_BINARY_DIR}/classes
          ${CMAKE_CURRENT_SOURCE_DIR}/src/java/dev/glibc/worker/BridgeTypes.java
          ${CMAKE_CURRENT_SOURCE_DIR}/src/java/dev/glibc/worker/GuestImages.java
  COMMAND ${Java_JAR_EXECUTABLE} cf ${CMAKE_CURRENT_BINARY_DIR}/android-jni-helpers.jar
          -C ${CMAKE_CURRENT_BINARY_DIR}/classes .
  DEPENDS src/java/dev/glibc/worker/BridgeTypes.java src/java/dev/glibc/worker/GuestImages.java VERBATIM)
add_custom_target(android_jni_helpers ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/android-jni-helpers.jar)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/android-jni-helpers.jar DESTINATION share/glibc-worker/android-jni)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/android-jni)
install(DIRECTORY src/common/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/android-jni/internal FILES_MATCHING PATTERN "*.h")
install(TARGETS android_jni android_jni_static EXPORT GlibcWorkerAndroidJniTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT GlibcWorkerAndroidJniTargets NAMESPACE glibc_worker::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerAndroidJni)
configure_package_config_file(cmake/GlibcWorkerAndroidJniConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/GlibcWorkerAndroidJniConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerAndroidJni)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/GlibcWorkerAndroidJniConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerAndroidJni)

install(FILES ${GW_FFI_LIBRARY} DESTINATION ${CMAKE_INSTALL_LIBDIR} RENAME libgw_android_jni_ffi.a)
if(GW_FFI_LICENSE_FILE)
  install(FILES ${GW_FFI_LICENSE_FILE} DESTINATION share/licenses/glibc-worker/android-jni
          RENAME libffi-LICENSE.txt)
endif()
