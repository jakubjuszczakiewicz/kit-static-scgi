/* Copyright (c) 2024 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#pragma once
#include <stdint.h>
#include "common.h"

socket_t open_tcp_listen(const char * addr, uint16_t port);
