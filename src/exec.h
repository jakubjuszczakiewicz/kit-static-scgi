/* Copyright (c) 2024 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#pragma once

#include <unistd.h>

struct int_exec_t
{
  pid_t pid;
  int stdout_fd;
};

int int_exec(struct int_exec_t * stat, int stdin, size_t command_len,
    const char * command);
pid_t int_system(int stdin, int stdout, size_t command_len, const char * command);
