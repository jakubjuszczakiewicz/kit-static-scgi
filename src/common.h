/* Copyright (c) 2024 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>

#ifndef FORCE_MALLOC
#ifdef NEED_ALLOCA_INCLUDE
#include <alloca.h>
#endif
#else
#define alloca(x) malloc(x)
#endif

#define SSPRINTF(out, args, ...) \
  ({  \
    out = NULL; \
    ssize_t len = snprintf(out, 0, args, __VA_ARGS__ ); \
    if (len > 0) { \
      out = alloca(len + 1); \
      snprintf(out, len + 1, args, __VA_ARGS__ ); \
    } \
    len; })

typedef int socket_t;

int hex2byte(const char * str);
