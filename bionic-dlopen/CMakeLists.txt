cmake_minimum_required(VERSION 3.22)
project(GlibcWorkerDlopen VERSION 0.1 LANGUAGES C)
find_package(Threads REQUIRED)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Werror)
if(ANDROID)
  add_compile_options(-fno-emulated-tls)
endif()
find_package(GlibcWorkerCore CONFIG REQUIRED)
add_library(dlopen_objects OBJECT loader.c elf_route.c)
target_include_directories(dlopen_objects PUBLIC include)
target_link_libraries(dlopen_objects PUBLIC glibc_worker::core)
add_library(dlopen SHARED $<TARGET_OBJECTS:dlopen_objects>)
add_library(dlopen_static STATIC $<TARGET_OBJECTS:dlopen_objects>)
foreach(target dlopen dlopen_static)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/glibc-worker/bionic-dlopen>)
  target_link_libraries(${target} PUBLIC glibc_worker::core)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME gw_dlopen)
endforeach()
include(CTest)
if(BUILD_TESTING)
  add_executable(loader_contract tests/loader_contract.c)
  target_link_libraries(loader_contract PRIVATE dlopen)
  add_test(NAME loader_contract COMMAND loader_contract)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/bionic-dlopen)
install(TARGETS dlopen dlopen_static EXPORT GlibcWorkerDlopenTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT GlibcWorkerDlopenTargets NAMESPACE glibc_worker::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerDlopen)
configure_package_config_file(cmake/GlibcWorkerDlopenConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/GlibcWorkerDlopenConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerDlopen)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/GlibcWorkerDlopenConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerDlopen)
