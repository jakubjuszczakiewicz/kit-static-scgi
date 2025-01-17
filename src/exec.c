/* Copyright (c) 2024 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#include "exec.h"
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include "common.h"

int int_exec(struct int_exec_t * stat, int input, size_t command_len,
    const char * command)
{
  int stderr = open("/dev/null", O_WRONLY);
  int stdout[2];
  pipe(stdout);

  char * args = alloca(command_len + 1);
  memcpy(args, command, command_len + 1);

  char ch = 0;
  size_t argsc = 2;
  for (size_t i = 0; i < command_len; i++) {
    switch (args[i]) {
      case ' ':
        if (ch == 0) {
          args[i] = 0;
          argsc++;
        }
        break;
      case '\\':
        i++;
        break;
      case '"':
      case '\'':
        if (ch == args[i])
          ch = 0;
        else if (ch == 0)
          ch = args[i];
        break;
    }
  }
  char ** argv = alloca(sizeof(char *) * argsc);
  argv[0] = args;
  size_t j = 1;
  for (size_t i = 1; i < command_len; i++) {
    if (args[i] == 0) {
      argv[j++] = &args[++i];
    }
  }
  argv[j] = NULL;

  pid_t child_pid = fork();
  if (child_pid < 0) {
    close(stdout[0]);
    close(stdout[1]);
    close(input);
    close(stderr);
    return -1;
  }

  if (child_pid == 0) {
    errno = 0;
    close(stdout[0]);
    dup2(input, STDIN_FILENO);
    dup2(stderr, STDERR_FILENO);
    dup2(stdout[1], STDOUT_FILENO);
    close(stdout[1]);
    close(input);
    close(stderr);

    execvp(args, argv);
    exit(1);
  }

  close(stderr);
  close(stdout[1]);
  stat->pid = child_pid;
  stat->stdout_fd = stdout[0];

  return 0;
}

pid_t int_system(int input, int stdout, size_t command_len, const char * command)
{
  int stderr = open("/dev/null", O_WRONLY);

  char * args = alloca(command_len + 1);
  memcpy(args, command, command_len + 1);

  char ch = 0;
  size_t argsc = 2;
  for (size_t i = 0; i < command_len; i++) {
    switch (args[i]) {
      case ' ':
        if (ch == 0) {
          args[i] = 0;
          argsc++;
        }
        break;
      case '\\':
        i++;
        break;
      case '"':
      case '\'':
        if (ch == args[i])
          ch = 0;
        else if (ch == 0)
          ch = args[i];
        break;
    }
  }
  char ** argv = alloca(sizeof(char *) * argsc);
  argv[0] = args;
  size_t j = 1;
  for (size_t i = 1; i < command_len; i++) {
    if (args[i] == 0) {
      argv[j++] = &args[++i];
    }
  }
  argv[j] = NULL;

  pid_t child_pid = fork();
  if (child_pid < 0) {
    close(input);
    close(stdout);
    close(stderr);
    return -1;
  }

  if (child_pid == 0) {
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    dup2(input, STDIN_FILENO);
    dup2(stderr, STDERR_FILENO);
    dup2(stdout, STDOUT_FILENO);

    close(input);
    close(stdout);
    close(stderr);

    execvp(args, argv);
    return -1;
  }

  close(input);
  close(stdout);
  close(stderr);

  return child_pid;
}
