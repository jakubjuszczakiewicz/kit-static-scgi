/* Copyright (c) 2025 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#include "version.h"

#define VERSION_MAJOR 1
#define VERSION_MINOR 1
#define VERSION_PATCH 0
#define VERSION_SUBSTR ""

#define STR(s) #s
#define MAKE_VER(ma, mi, pa, st) (STR(ma) "." STR(mi) "." STR(pa) st)

#define VERSION_STR MAKE_VER(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_SUBSTR)

uint16_t version[3] = { VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH };
const char * version_str = VERSION_STR;
