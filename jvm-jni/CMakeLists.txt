cmake_minimum_required(VERSION 3.22)
project(GlibcWorkerJvmJni VERSION 0.1 LANGUAGES C)
find_package(Threads REQUIRED)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Werror)
if(ANDROID)
  add_compile_options(-fno-emulated-tls)
endif()
find_package(GlibcWorkerAndroidJni CONFIG REQUIRED)
find_path(GW_JNI_INCLUDE_DIR jni.h REQUIRED)
if(GW_JRE_ADAPTER_HEADER)
  configure_file(${GW_JRE_ADAPTER_HEADER} ${CMAKE_CURRENT_BINARY_DIR}/jre-adapter-profile.h COPYONLY)
else()
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/jre-adapter-profile.h [=[
#define GW_JVM_PROFILE_NAME "unconfigured"
#define GW_JVM_LIBJAVA_SHA256 ""
#define GW_JVM_LIBJVM_SHA256 ""
#define GW_JVM_PROFILE_FAMILY GW_JVM_NATIVE_LIBRARIES_15
]=])
endif()
add_library(jvm_jni_objects OBJECT src/native/adapter.c src/native/profiles.c src/native/calls.c src/native/families.c)
target_include_directories(jvm_jni_objects PRIVATE include src/native ${CMAKE_CURRENT_BINARY_DIR} ${GW_JNI_INCLUDE_DIR})
target_link_libraries(jvm_jni_objects PUBLIC glibc_worker::android_jni)
add_library(jvm_jni SHARED $<TARGET_OBJECTS:jvm_jni_objects>)
add_library(jvm_jni_static STATIC $<TARGET_OBJECTS:jvm_jni_objects>)
foreach(target jvm_jni jvm_jni_static)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/glibc-worker/jvm-jni>)
  target_link_libraries(${target} PUBLIC glibc_worker::android_jni PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME gw_jvm_jni)
endforeach()
find_package(Java COMPONENTS Development REQUIRED)
find_file(GW_ANDROID_JNI_HELPERS android-jni-helpers.jar
  HINTS ${CMAKE_PREFIX_PATH}
  PATH_SUFFIXES share/glibc-worker/android-jni NO_CMAKE_FIND_ROOT_PATH REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/jvm-jni-helpers.jar
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/classes
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/classes
  COMMAND ${Java_JAVAC_EXECUTABLE} --release 8 -cp ${GW_ANDROID_JNI_HELPERS} -d ${CMAKE_CURRENT_BINARY_DIR}/classes
          ${CMAKE_CURRENT_SOURCE_DIR}/src/java/dev/glibc/worker/JvmAdapter.java
  COMMAND ${Java_JAR_EXECUTABLE} cf ${CMAKE_CURRENT_BINARY_DIR}/jvm-jni-helpers.jar
          -C ${CMAKE_CURRENT_BINARY_DIR}/classes .
  DEPENDS src/java/dev/glibc/worker/JvmAdapter.java ${GW_ANDROID_JNI_HELPERS} VERBATIM)
add_custom_target(jvm_jni_helpers ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/jvm-jni-helpers.jar)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/jvm-jni-helpers.jar DESTINATION share/glibc-worker/jvm-jni)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(FILES include/jvm_adapter.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/jvm-jni)
install(TARGETS jvm_jni jvm_jni_static EXPORT GlibcWorkerJvmJniTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT GlibcWorkerJvmJniTargets NAMESPACE glibc_worker::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerJvmJni)
configure_package_config_file(cmake/GlibcWorkerJvmJniConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/GlibcWorkerJvmJniConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerJvmJni)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/GlibcWorkerJvmJniConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerJvmJni)
