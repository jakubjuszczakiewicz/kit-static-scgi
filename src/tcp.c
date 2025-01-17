/* Copyright (c) 2024 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#include "tcp.h"
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BACKLOG 16

union sockaddr_in64
{
  struct sockaddr_in ip4;
  struct sockaddr_in6 ip6;
};

socket_t open_tcp_listen(const char * addr, uint16_t port)
{
  union sockaddr_in64 ipaddr;
  size_t ipaddr_size;
  int af_inet;

  if (inet_pton(AF_INET6, addr, &ipaddr.ip6.sin6_addr)) {
    af_inet = AF_INET6;
    ipaddr.ip6.sin6_family = AF_INET6;
    ipaddr_size = sizeof(ipaddr.ip6);
    ipaddr.ip6.sin6_port = htons(port);
  } else if (inet_pton(AF_INET, addr, &ipaddr.ip4.sin_addr)) {
    af_inet = AF_INET;
    ipaddr.ip4.sin_family = AF_INET;
    ipaddr_size = sizeof(ipaddr.ip4);
    ipaddr.ip4.sin_port = htons(port);
  } else {
    return -1;
  }

  int sock = socket(af_inet, SOCK_STREAM, 0);
  if (sock < 0)
    return sock;

  int optval = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

  int r = bind(sock, (const struct sockaddr *)&ipaddr, ipaddr_size);
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
