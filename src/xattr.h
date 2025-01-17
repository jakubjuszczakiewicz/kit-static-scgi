/* Copyright (c) 2024 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#pragma once

#include <sys/types.h>

int has_xattr_support(void);
ssize_t get_user_fxattr(int fd, const char * name, char * out, size_t outsize);
ssize_t create_user_fxattr(int fd, const char * name, const char * value,
    size_t value_len);
ssize_t update_user_fxattr(int fd, const char * name, const char * value,
    size_t value_len);
