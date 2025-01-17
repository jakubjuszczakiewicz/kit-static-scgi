/* Copyright (c) 2024 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#include "unix.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define BACKLOG 16

socket_t open_unix_listen(const char * path)
{
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0)
    return sock;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));

  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  int r = bind(sock, (const struct sockaddr *)&addr, sizeof(addr));
  if (r < 0) {
    close(sock);
    return r;
  }

  r = listen(sock, BACKLOG);
  if (r < 0) {
    close(sock);
    return r;
  }

  return sock;
}
