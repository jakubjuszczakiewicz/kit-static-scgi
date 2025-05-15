# Copyright (c) 2025 Krypto-IT Jakub Juszczakiewicz
# All rights reserved.

cmake_minimum_required(VERSION 3.5)
project(kit-static-scgi C)

find_package(Threads)

set(SOURCE_FILES
  src/main.c
  src/tcp.c
  src/unix.c
  src/mimedb.c
  src/version.c
  src/common.c
  src/xattr.c
  src/exec.c
)

# Try to use "nice /usr/bin/...."
set(ZSTD_CMD "nice zstd --no-progress -19" CACHE STRING "zstd max compression command")
set(BROTLI_CMD "nice brotli -Z" CACHE STRING "brotli max compression command")
set(GZIP_CMD "nice gzip -9" CACHE STRING "gzip max compression command")

set(ENV_ZSTD_CMD "ZSTD_CMD" CACHE STRING "environment variable name for zstd max compression command")
set(ENV_BROTLI_CMD "BROTLI_CMD" CACHE STRING "environment variable name for brotli max compression command")
set(ENV_GZIP_CMD "GZIP_CMD" CACHE STRING "environment variable name for gzip max compression command")

add_definitions(-DZSTD_CMD=\"${ZSTD_CMD}\")
add_definitions(-DBROTLI_CMD=\"${BROTLI_CMD}\")
add_definitions(-DGZIP_CMD=\"${GZIP_CMD}\")
add_definitions(-DENV_ZSTD_CMD=\"${ENV_ZSTD_CMD}\")
add_definitions(-DENV_BROTLI_CMD=\"${ENV_BROTLI_CMD}\")
add_definitions(-DENV_GZIP_CMD=\"${ENV_GZIP_CMD}\")

set(XATTR_OS "NONE" CACHE STRING "OS (Linux/FreeBSD) with xattr support")

if (XATTR_OS STREQUAL "Linux")
add_definitions(-DXATTR_LINUX)
elseif(XATTR_OS STREQUAL "FreeBSD")
add_definitions(-DXATTR_FREEBSD)
endif()

set(QUEUE_SIZE "" CACHE STRING "Strong compression cache queue size")
set(MAX_THREADS "" CACHE STRING "Max strong compression cache treads suppoort")
set(LOGS_SUPPORT False CACHE BOOL "Add (debug) logging support")
set(FORCE_MALLOC False CACHE BOOL "Force malloc instead of alloca")
set(NEED_ALLOCA_INCLUDE False CACHE BOOL "Force include alloca.h")

if (${QUEUE_SIZE})
add_definitions(-DQUEUE_SIZE=${QUEUE_SIZE})
endif()

if (${MAX_THREADS})
add_definitions(-DMAX_THREADS=${MAX_THREADS})
endif()

if (${LOGS_SUPPORT})
add_definitions(-DHAVE_LOGS)
endif()

if (${FORCE_MALLOC})
add_definitions(-DFORCE_MALLOC)
endif()

if (${NEED_ALLOCA_INCLUDE})
add_definitions(-DNEED_ALLOCA_INCLUDE)
endif()

add_executable(${PROJECT_NAME} ${SOURCE_FILES})
target_link_libraries(${PROJECT_NAME} Threads::Threads)
