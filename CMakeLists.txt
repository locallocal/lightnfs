cmake_minimum_required(VERSION 3.22)
project(lightnfs VERSION 1.3.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# LNFS_SANITIZE: "" | address | thread
set(LNFS_SANITIZE "" CACHE STRING "sanitizer: empty, address or thread")
# LNFS_RING: auto | uring | epoll — which RingOps backends to *default* to at runtime.
# Both backends are always compiled; this only sets the default probe order.
set(LNFS_RING "auto" CACHE STRING "default ring backend: auto, uring or epoll")
option(LNFS_BUILD_FUZZ "build libFuzzer targets (requires clang)" OFF)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
add_compile_definitions(_GNU_SOURCE)
if(LNFS_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(LNFS_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

# ---- liburing: vendored submodule when built, system otherwise ----
# third_party/liburing is a git submodule (pinned upstream tag); its headers alone are
# not usable before `scripts/fetch_liburing.sh` runs configure+make (compat.h is
# generated), so only a built submodule takes precedence over the system package.
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/liburing/src/liburing.a)
  set(LIBURING_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/liburing/src/include)
  set(LIBURING_LIBRARY ${CMAKE_SOURCE_DIR}/third_party/liburing/src/liburing.a)
else()
  find_path(LIBURING_INCLUDE_DIR liburing.h PATHS /usr/include /usr/local/include)
  find_library(LIBURING_LIBRARY NAMES uring PATHS /usr/lib /usr/local/lib)
endif()
if(NOT LIBURING_INCLUDE_DIR OR NOT LIBURING_LIBRARY)
  message(FATAL_ERROR "liburing not found; install liburing-dev or run scripts/fetch_liburing.sh")
endif()
message(STATUS "liburing: ${LIBURING_LIBRARY} (${LIBURING_INCLUDE_DIR})")

# spdlog: logging implementation (git submodule, static lib; util/log.hpp is the facade)
add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)

# ---- core library ----
add_library(lnfs_core STATIC
  src/backend/api.cpp
  src/backend/local.cpp
  src/backend/memory.cpp
  src/core/boot_epoch.cpp
  src/core/config.cpp
  src/core/file_handle.cpp
  src/core/obj_lock.cpp
  src/core/readdir.cpp
  src/core/errmap.cpp
  src/nfsv3/nfs3_types.cpp
  src/nfsv4/nfs4_types.cpp
  src/nfsv4/attrs.cpp
  src/nfsv4/engine.cpp
  src/core/pseudofs.cpp
  src/state/lock_mgr.cpp
  src/state/state_mgr.cpp
  src/nfsv3/engine.cpp
  src/mountd/mount3.cpp
  src/server/rpcbind.cpp
  src/server/ctl.cpp
  src/runtime/reactor.cpp
  src/runtime/uring_ring.cpp
  src/runtime/epoll_ring.cpp
  src/runtime/offload_pool.cpp
  src/runtime/runtime.cpp
  src/runtime/buffer.cpp
  src/util/log.cpp
  src/obs/metrics.cpp
  src/obs/errlog.cpp
  src/transport/record_stream.cpp
  src/transport/listener.cpp
  src/transport/connection.cpp
  src/transport/backchannel.cpp
  src/nfsv4/callback.cpp
  src/rpc/rpc_msg.cpp
  src/rpc/auth.cpp
  src/rpc/drc.cpp
  src/rpc/dispatch.cpp
)
target_include_directories(lnfs_core PUBLIC src ${LIBURING_INCLUDE_DIR})
target_link_libraries(lnfs_core PUBLIC ${LIBURING_LIBRARY} Threads::Threads
  spdlog::spdlog)
target_compile_definitions(lnfs_core PUBLIC LIGHTNFS_VERSION="${PROJECT_VERSION}")
if(LNFS_RING STREQUAL "epoll")
  target_compile_definitions(lnfs_core PUBLIC LNFS_DEFAULT_RING_EPOLL=1)
endif()

# ccmd: command-line framework for the lightnfsd entry point (git submodule,
# header-only + static cflag; needs `git submodule update --init --recursive`)
add_subdirectory(third_party/ccmd EXCLUDE_FROM_ALL)
# vendored code compiles under this project's -Wall -Wextra; silence cflag's own
# warnings without touching the submodule
target_compile_options(ccmd_cflag PRIVATE -Wno-sign-compare
  $<$<CXX_COMPILER_ID:Clang>:-Wno-delete-non-abstract-non-virtual-dtor>)

add_executable(lightnfsd src/main.cpp)
target_link_libraries(lightnfsd PRIVATE lnfs_core ccmd::ccmd)

# ---- admin tools (design 08 §8.6) ----
# lightnfs-ctl also hosts the three-layer benchmarks (`lightnfs-ctl bench …`,
# design 02 §2.8), so it carries the bench sources and links the core.
add_executable(lightnfs-ctl tools/lightnfs_ctl.cpp
  tools/bench/bench_echo.cpp tools/bench/bench_nullrpc.cpp tools/bench/bench_fullpath.cpp)
target_link_libraries(lightnfs-ctl PRIVATE lnfs_core ccmd::ccmd)
target_include_directories(lightnfs-ctl PRIVATE ${CMAKE_SOURCE_DIR})
add_executable(lightnfs-fh tools/lightnfs_fh.cpp)
target_link_libraries(lightnfs-fh PRIVATE lnfs_core ccmd::ccmd)

# ---- tests ----
enable_testing()
add_executable(lnfs_tests
  tests/test_main.cpp
  tests/test_util.cpp
  tests/test_task.cpp
  tests/test_sync.cpp
  tests/test_timer.cpp
  tests/test_buffer.cpp
  tests/test_fake_ring.cpp
  tests/test_xdr.cpp
  tests/test_record_stream.cpp
  tests/test_rpc.cpp
  tests/test_uring_smoke.cpp
  tests/test_offload.cpp
  tests/test_transport_loopback.cpp
  tests/test_backend.cpp
  tests/test_backend_write.cpp
  tests/test_nfs3.cpp
  tests/test_write_path.cpp
  tests/test_nfs4.cpp
  tests/test_state.cpp
  tests/test_lock_mgr.cpp
  tests/test_ctl.cpp
)
target_link_libraries(lnfs_tests PRIVATE lnfs_core)
add_test(NAME lnfs_tests COMMAND lnfs_tests)

# Acceptance client: userspace NFSv3/v4.1 client that drives a running lightnfsd over
# TCP. Invoked by scripts/accept_m2_local.sh and accept_m6_local.sh, not by ctest.
add_executable(lnfs_accept_client tests/accept_client.cpp)
target_link_libraries(lnfs_accept_client PRIVATE lnfs_core)

# ---- fuzz ----
if(LNFS_BUILD_FUZZ)
  add_executable(fuzz_handle_request fuzz/fuzz_handle_request.cpp)
  target_compile_options(fuzz_handle_request PRIVATE -fsanitize=fuzzer,address)
  target_link_options(fuzz_handle_request PRIVATE -fsanitize=fuzzer,address)
  target_link_libraries(fuzz_handle_request PRIVATE lnfs_core)
else()
  # regression driver: runs corpus files through the same entry without libFuzzer
  add_executable(fuzz_regress fuzz/fuzz_handle_request.cpp fuzz/regress_main.cpp)
  target_compile_definitions(fuzz_regress PRIVATE LNFS_FUZZ_REGRESS=1)
  target_link_libraries(fuzz_regress PRIVATE lnfs_core)
  add_test(NAME fuzz_regress COMMAND fuzz_regress)
endif()

# ---- install (packaging scripts stage via `cmake --install` + DESTDIR, plan doc 10 §4.5) ----
include(GNUInstallDirs)
install(TARGETS lightnfsd lightnfs-ctl lightnfs-fh
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/lightnfs.toml.example
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/lightnfs)
install(FILES packaging/systemd/lightnfs.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
install(FILES README.md docs/deployment.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
