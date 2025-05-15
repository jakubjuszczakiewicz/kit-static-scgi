/* Copyright (c) 2025 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#include "xattr.h"
#include "common.h"

#ifdef XATTR_LINUX

#include <sys/xattr.h>

int has_xattr_support(void)
{
  return 1;
}

ssize_t get_user_fxattr(int fd, const char * name, char * out, size_t outsize)
{
  char * fullname;
  SSPRINTF(fullname, "user.%s", name);
  return fgetxattr(fd, fullname, out, outsize);
}

ssize_t create_user_fxattr(int fd, const char * name, const char * value,
    size_t value_len)
{
  char * fullname;
  SSPRINTF(fullname, "user.%s", name);
  return fsetxattr(fd, fullname, value, value_len, XATTR_CREATE);
}

ssize_t update_user_fxattr(int fd, const char * name, const char * value,
    size_t value_len)
{
  char * fullname;
  SSPRINTF(fullname, "user.%s", name);
  return fsetxattr(fd, fullname, value, value_len, XATTR_REPLACE);
}

#else
#ifdef XATTR_FREEBSD

#include <sys/types.h>
#include <sys/extattr.h>

int has_xattr_support(void)
{
  return 1;
}

ssize_t get_user_fxattr(int fd, const char * name, char * out, size_t outsize)
{
  return extattr_get_fd(fd, EXTATTR_NAMESPACE_USER, name, out, outsize);
}

ssize_t create_user_fxattr(int fd, const char * name, const char * value,
    size_t value_len)
{
  return extattr_set_fd(fd, EXTATTR_NAMESPACE_USER, name, value, value_len);
}

ssize_t update_user_fxattr(int fd, const char * name, const char * value,
    size_t value_len)
{
  return extattr_set_fd(fd, EXTATTR_NAMESPACE_USER, name, value, value_len);
}

#else

int has_xattr_support(void)
{
  return 0;
}

ssize_t get_user_fxattr(int fd, const char * name, char * out, size_t outsize)
{
  return -1;
}

ssize_t create_user_fxattr(int fd, const char * name, const char * value,
    size_t value_len)
{
  return -1;
}

ssize_t update_user_fxattr(int fd, const char * name, const char * value,
    size_t value_len)
{
  return -1;
}

#endif
#endif
