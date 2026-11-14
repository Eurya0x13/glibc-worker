cmake_minimum_required(VERSION 3.22)
project(GlibcWorkerCore VERSION 0.1 LANGUAGES C)
find_package(Threads REQUIRED)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Werror)
if(ANDROID)
  add_compile_options(-fno-emulated-tls)
endif()
enable_language(ASM)
add_library(core STATIC src/bootstrap/loader_image.c src/bootstrap/clone.S)
add_library(glibc_worker::core ALIAS core)
target_include_directories(core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap>
  $<INSTALL_INTERFACE:include/glibc-worker/core>)
target_link_libraries(core PUBLIC Threads::Threads)
set_target_properties(core PROPERTIES OUTPUT_NAME glibc_worker_core)
include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  foreach(probe wait_events rpc_handoff ready_poll runtime_patches audit_boundaries mutex_contract)
    add_test(NAME ${probe} COMMAND ${Python3_EXECUTABLE}
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/${probe}.py)
  endforeach()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/core)
install(FILES src/bootstrap/loader_image.h src/bootstrap/raw_syscall.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/core)
install(TARGETS core EXPORT GlibcWorkerCoreTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT GlibcWorkerCoreTargets NAMESPACE glibc_worker::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerCore)
configure_package_config_file(cmake/GlibcWorkerCoreConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/GlibcWorkerCoreConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerCore)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/GlibcWorkerCoreConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerCore)
