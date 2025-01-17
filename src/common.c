/* Copyright (c) 2024 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#include "common.h"
#include <stdint.h>

const uint8_t ascii_lookup_table[128] = {
  0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  0U,    1U,    2U,    3U,    4U,    5U,    6U,    7U,    8U,    9U,    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  0xFFU, 10U,   11U,   12U,   13U,   14U,   15U,   16U,   17U,   18U,   19U,   20U,   21U,   22U,   23U,   24U,
  25U,   26U,   27U,   28U,   29U,   30U,   31U,   32U,   33U,   34U,   35U,   0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  0xFFU, 10U,   11U,   12U,   13U,   14U,   15U,   16U,   17U,   18U,   19U,   20U,   21U,   22U,   23U,   24U,
  25U,   26U,   27U,   28U,   29U,   30U,   31U,   32U,   33U,   34U,   35U,   0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
};

int hex2byte(const char * str)
{
  unsigned char x = (*str) & 0x7F;
  unsigned char r1 = ascii_lookup_table[x] | (*str - x);
  if (r1 > 15)
    return -1;

  str++;
  x = (*str) & 0x7F;
  unsigned char r2 = ascii_lookup_table[x] | (*str - x);
  if (r2 > 15)
    return -1;

  return (r1 << 4) | r2;
}
