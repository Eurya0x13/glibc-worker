cmake_minimum_required(VERSION 3.22)
project(GlibcWorkerCore VERSION 0.1 LANGUAGES C)
find_package(Threads REQUIRED)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Werror)
if(ANDROID)
  add_compile_options(-fno-emulated-tls)
endif()
enable_language(ASM)
add_library(core STATIC src/bootstrap/loader_image.c src/bootstrap/clone.S
  src/domain/signal-contract.c)
add_library(domains STATIC src/domain/switch.S src/domain/start.S
  src/domain/signals.c src/domain/signal-entry.S src/domain/signal-router.c
  src/domain/nonlocal.c src/domain/signal-stack.S src/domain/signal-pending.c
  src/domain/signal-deferred.c src/domain/signal-consume.c)
add_library(glibc_worker::domains ALIAS domains)
target_compile_definitions(domains PRIVATE GW_DOMAIN_CHAIN)
target_include_directories(domains PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/domain>
  $<INSTALL_INTERFACE:include/glibc-worker/core>
  $<INSTALL_INTERFACE:include/glibc-worker/core/internal>
  PRIVATE src/bootstrap)
target_link_libraries(domains PUBLIC core ${CMAKE_DL_LIBS})
set_target_properties(domains PROPERTIES OUTPUT_NAME glibc_worker_domains)
# Explicit bootstrap interposition is separate from the regular SDK target.
add_library(signal_interpose STATIC src/domain/signal-interpose.c
  src/domain/signal-mask-interpose.c src/domain/signal-wait-interpose.c
  src/domain/signal-suspend-interpose.c src/domain/signal-fd-interpose.c
  src/domain/signal-fd-kernel.c src/domain/signal-fd-entry.S)
add_library(glibc_worker::signal_interpose ALIAS signal_interpose)
target_link_libraries(signal_interpose PUBLIC domains)
target_link_options(signal_interpose INTERFACE -Wl,-u,sigaction -Wl,-u,signal
  -Wl,-u,pthread_sigmask -Wl,-u,sigpending -Wl,-u,sigtimedwait
  -Wl,-u,sigsuspend -Wl,-u,ppoll -Wl,-u,signalfd -Wl,-u,fcntl)
set_target_properties(signal_interpose PROPERTIES OUTPUT_NAME glibc_worker_signal_interpose)
# Native capture/restore interposition stays opt-in during recovery integration.
add_library(nonlocal_interpose STATIC src/domain/nonlocal-interpose.c
  src/domain/nonlocal-entry.S)
target_link_libraries(nonlocal_interpose PUBLIC domains)
target_link_options(nonlocal_interpose INTERFACE -Wl,-u,longjmp -Wl,-u,setjmp)
set_target_properties(nonlocal_interpose PROPERTIES OUTPUT_NAME glibc_worker_nonlocal_interpose)
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
  add_executable(signal_contract_test tests/signal_contract.c)
  target_link_libraries(signal_contract_test PRIVATE core)
  # These tests use assertions as executable validation, including Release.
  target_compile_options(signal_contract_test PRIVATE -UNDEBUG)
  add_test(NAME signal_contract COMMAND signal_contract_test)
  add_executable(carrier_lifecycle_test tests/carrier_lifecycle.c)
  target_link_libraries(carrier_lifecycle_test PRIVATE signal_interpose)
  target_compile_options(carrier_lifecycle_test PRIVATE -UNDEBUG)
  add_test(NAME carrier_lifecycle COMMAND carrier_lifecycle_test)
  add_executable(signal_pending_test tests/signal_pending.c)
  target_link_libraries(signal_pending_test PRIVATE domains)
  target_compile_options(signal_pending_test PRIVATE -UNDEBUG)
  add_test(NAME signal_pending COMMAND signal_pending_test)
  add_executable(signal_deferred_test tests/signal_deferred.c)
  target_link_libraries(signal_deferred_test PRIVATE domains)
  target_compile_options(signal_deferred_test PRIVATE -UNDEBUG)
  add_test(NAME signal_deferred COMMAND signal_deferred_test)
  set_tests_properties(signal_deferred PROPERTIES TIMEOUT 10)
  add_executable(signal_waiting_test tests/signal_waiting.c)
  target_link_libraries(signal_waiting_test PRIVATE signal_interpose)
  target_link_options(signal_waiting_test PRIVATE -Wl,--export-dynamic)
  target_compile_options(signal_waiting_test PRIVATE -UNDEBUG)
  add_test(NAME signal_waiting COMMAND signal_waiting_test)
  add_test(NAME signal_waiting_native COMMAND signal_waiting_test --native)
  set_tests_properties(signal_waiting signal_waiting_native PROPERTIES TIMEOUT 10)
  add_executable(signal_fd_test tests/signal_fd.c)
  add_library(signal_close_backend SHARED tests/signal_close_backend.c)
  target_link_libraries(signal_close_backend PRIVATE ${CMAKE_DL_LIBS})
  add_dependencies(signal_fd_test signal_close_backend)
  target_link_libraries(signal_fd_test PRIVATE signal_interpose nonlocal_interpose)
  target_link_options(signal_fd_test PRIVATE -Wl,--export-dynamic)
  target_compile_options(signal_fd_test PRIVATE -UNDEBUG)
  add_test(NAME signal_fd COMMAND signal_fd_test)
  add_test(NAME signal_fd_native_close COMMAND ${CMAKE_COMMAND} -E env
    "LD_PRELOAD=$<TARGET_FILE:signal_close_backend>"
    $<TARGET_FILE:signal_fd_test> --native-close-window)
  set_tests_properties(signal_fd signal_fd_native_close PROPERTIES TIMEOUT 15)
  add_executable(signal_fd_lifecycle_test tests/signal_fd_lifecycle.c)
  target_link_libraries(signal_fd_lifecycle_test PRIVATE signal_interpose)
  target_link_options(signal_fd_lifecycle_test PRIVATE -Wl,--export-dynamic)
  target_compile_options(signal_fd_lifecycle_test PRIVATE -UNDEBUG)
  add_test(NAME signal_fd_lifecycle COMMAND signal_fd_lifecycle_test)
  set_tests_properties(signal_fd_lifecycle PROPERTIES TIMEOUT 15)
  add_executable(signal_delivery_test tests/signal_delivery.c)
  target_link_libraries(signal_delivery_test PRIVATE signal_interpose)
  target_compile_options(signal_delivery_test PRIVATE -UNDEBUG)
  add_test(NAME signal_delivery COMMAND signal_delivery_test)
  set_tests_properties(signal_delivery PROPERTIES TIMEOUT 15)
  add_executable(signal_restart_test tests/signal_restart.c)
  target_link_libraries(signal_restart_test PRIVATE signal_interpose)
  target_compile_options(signal_restart_test PRIVATE -UNDEBUG)
  add_test(NAME signal_restart COMMAND signal_restart_test)
  set_tests_properties(signal_restart PROPERTIES TIMEOUT 20)
  add_executable(signal_consume_test tests/signal_consume.c)
  target_link_libraries(signal_consume_test PRIVATE signal_interpose nonlocal_interpose)
  target_link_options(signal_consume_test PRIVATE -Wl,--export-dynamic)
  target_compile_options(signal_consume_test PRIVATE -UNDEBUG)
  add_test(NAME signal_consume COMMAND signal_consume_test)
  set_tests_properties(signal_consume PROPERTIES TIMEOUT 20)
  add_executable(signal_aliases_test tests/signal_aliases.c)
  target_link_libraries(signal_aliases_test PRIVATE signal_interpose)
  target_link_options(signal_aliases_test PRIVATE -Wl,--export-dynamic)
  target_compile_options(signal_aliases_test PRIVATE -UNDEBUG)
  add_test(NAME signal_aliases COMMAND signal_aliases_test)
  foreach(probe wait_events rpc_handoff ready_poll runtime_patches audit_boundaries mutex_contract nonlocal)
    add_test(NAME ${probe} COMMAND ${Python3_EXECUTABLE}
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/${probe}.py)
  endforeach()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/core)
install(FILES src/bootstrap/loader_image.h src/bootstrap/raw_syscall.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/core)
install(DIRECTORY src/domain/ DESTINATION
        ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/core/internal
        FILES_MATCHING PATTERN "*.h")
install(FILES src/domain/signal-stack.S src/domain/signal-pending.c DESTINATION
        ${CMAKE_INSTALL_INCLUDEDIR}/glibc-worker/core/internal)
install(TARGETS core domains signal_interpose nonlocal_interpose EXPORT GlibcWorkerCoreTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT GlibcWorkerCoreTargets NAMESPACE glibc_worker::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerCore)
configure_package_config_file(cmake/GlibcWorkerCoreConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/GlibcWorkerCoreConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerCore)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/GlibcWorkerCoreConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GlibcWorkerCore)
