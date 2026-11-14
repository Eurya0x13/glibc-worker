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
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-jni.py
          --jni-header ${GW_JNI_INCLUDE_DIR}/jni.h
          --output ${CMAKE_CURRENT_BINARY_DIR}/generated
  DEPENDS scripts/gen-jni.py ${GW_JNI_INCLUDE_DIR}/jni.h VERBATIM)
add_custom_target(jni_tables DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/host-jni.inc)
add_library(android_jni_objects OBJECT host/full_host.c host/references.c
  host/bindings.c host/array_copy.c host/loader_backend.c host/image_policy.c)
add_dependencies(android_jni_objects jni_tables)
target_include_directories(android_jni_objects PRIVATE include common host guest
  ${CMAKE_CURRENT_BINARY_DIR} ${GW_JNI_INCLUDE_DIR} ${GW_FFI_INCLUDE_DIR})
target_link_libraries(android_jni_objects PUBLIC glibc_worker::dlopen)
add_library(android_jni SHARED $<TARGET_OBJECTS:android_jni_objects>)
add_library(android_jni_static STATIC $<TARGET_OBJECTS:android_jni_objects>)
foreach(target android_jni android_jni_static)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/glibc-worker/android-jni>)
  target_link_libraries(${target} PUBLIC glibc_worker::dlopen PRIVATE glibc_worker::android_ffi ${CMAKE_DL_LIBS})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME gw_android_jni)
endforeach()
find_package(Java COMPONENTS Development REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/android-jni-helpers.jar
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/classes
  COMMAND ${Java_JAVAC_EXECUTABLE} --release 8 -d ${CMAKE_CURRENT_BINARY_DIR}/classes
          ${CMAKE_CURRENT_SOURCE_DIR}/java/dev/glibc/worker/BridgeTypes.java
          ${CMAKE_CURRENT_SOURCE_DIR}/java/dev/glibc/worker/GuestImages.java
  COMMAND ${Java_JAR_EXECUTABLE} cf ${CMAKE_CURRENT_BINARY_DIR}/android-jni-helpers.jar
          -C ${CMAKE_CURRENT_BINARY_DIR}/classes .
  DEPENDS java/dev/glibc/worker/BridgeTypes.java java/dev/glibc/worker/GuestImages.java VERBATIM)
add_custom_target(android_jni_helpers ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/android-jni-helpers.jar)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/android-jni-helpers.jar DESTINATION share/glibc-worker/android-jni)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/android-jni)
install(DIRECTORY common/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/android-jni/internal FILES_MATCHING PATTERN "*.h")
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
