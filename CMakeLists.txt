cmake_minimum_required(VERSION 3.22)
project(lightnfs CXX)

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

# ---- liburing: system first, then vendored (scripts/fetch_liburing.sh) ----
find_path(LIBURING_INCLUDE_DIR liburing.h
  PATHS ${CMAKE_SOURCE_DIR}/third_party/liburing/src/include /usr/include /usr/local/include)
find_library(LIBURING_LIBRARY NAMES uring
  PATHS ${CMAKE_SOURCE_DIR}/third_party/liburing/src /usr/lib /usr/local/lib)
if(NOT LIBURING_INCLUDE_DIR OR NOT LIBURING_LIBRARY)
  message(FATAL_ERROR "liburing not found; install liburing-dev or run scripts/fetch_liburing.sh")
endif()
message(STATUS "liburing: ${LIBURING_LIBRARY} (${LIBURING_INCLUDE_DIR})")

# ---- core library ----
add_library(lnfs_core STATIC
  src/backend/api.cpp
  src/backend/local.cpp
  src/backend/memory.cpp
  src/core/config.cpp
  src/core/file_handle.cpp
  src/core/obj_lock.cpp
  src/core/readdir.cpp
  src/core/errmap.cpp
  src/nfsv3/nfs3_types.cpp
  src/nfsv3/engine.cpp
  src/mountd/mount3.cpp
  src/server/rpcbind.cpp
  src/runtime/reactor.cpp
  src/runtime/uring_ring.cpp
  src/runtime/epoll_ring.cpp
  src/runtime/offload_pool.cpp
  src/runtime/runtime.cpp
  src/runtime/buffer.cpp
  src/util/log.cpp
  src/transport/record_stream.cpp
  src/transport/listener.cpp
  src/transport/connection.cpp
  src/rpc/rpc_msg.cpp
  src/rpc/auth.cpp
  src/rpc/dispatch.cpp
)
target_include_directories(lnfs_core PUBLIC src ${LIBURING_INCLUDE_DIR})
target_link_libraries(lnfs_core PUBLIC ${LIBURING_LIBRARY} Threads::Threads)
if(LNFS_RING STREQUAL "epoll")
  target_compile_definitions(lnfs_core PUBLIC LNFS_DEFAULT_RING_EPOLL=1)
endif()

add_executable(lightnfsd src/main.cpp)
target_link_libraries(lightnfsd PRIVATE lnfs_core)

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
  tests/test_nfs3.cpp
)
target_link_libraries(lnfs_tests PRIVATE lnfs_core)
add_test(NAME lnfs_tests COMMAND lnfs_tests)

# ---- benchmarks ----
add_executable(bench_echo bench/bench_echo.cpp)
target_link_libraries(bench_echo PRIVATE lnfs_core)
target_include_directories(bench_echo PRIVATE ${CMAKE_SOURCE_DIR})
add_executable(bench_nullrpc bench/bench_nullrpc.cpp)
target_link_libraries(bench_nullrpc PRIVATE lnfs_core)
target_include_directories(bench_nullrpc PRIVATE ${CMAKE_SOURCE_DIR})

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
