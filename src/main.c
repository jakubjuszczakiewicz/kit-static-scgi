/* Copyright (c) 2024 Krypto-IT Jakub Juszczakiewicz
 * All rights reserved.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>

#include "tcp.h"
#include "unix.h"
#include "mimedb.h"
#include "version.h"
#include "xattr.h"
#include "common.h"
#include "exec.h"

#define MAX_PARALLEL_REQUESTS 1024
#define MAX_URI 1024
#define MAX_FILE_PATH 2048
#define MAX_HEADER_LINE_SIZE 2048
#define MAX_RESPONS_BUFFER_SIZE (1024 * 128)
#define MAX_ETAG 48
#define MAX_DOMAIN 64

#ifndef QUEUE_SIZE
#define QUEUE_SIZE 64
#endif

#ifndef MAX_THREADS
#define MAX_THREADS 16
#endif

#ifndef MAX_PARALLEL_ON_DEMAND
#define MAX_PARALLEL_ON_DEMAND 8
#endif

#define HEADER_CONTENT_TYPE     "Content-Type: %s\r\n"
#define HEADER_CONTENT_TYPE_PT  "Content-Type: text/plain\r\n"
#define HEADER_CONTENT_LENGHT   "Content-Lenght: %llu\r\n"
#define HEADER_CONTENT_ENCODING "Content-Encoding: %s\r\n"
#define HEADER_ETAG             "ETag: \"%s\"\r\n"
#define HEADER_X_KIT_SERVICE    "X-KIT-Service: kit-static-scgi/%s\r\n"
#define HEADER_ACCEPT_RANGES    "Accept-Ranges: none\r\n"

#define XATTR_COUNTER_NAME          "kitcounter"
#define XATTR_PREV_COUNTER_NAME     "kitprevcounter"
#define XATTR_304_COUNTER_NAME      "kit304counter"

#define EXIT_CMD "#EXIT"

#ifndef ZSTD_CMD
#define ZSTD_CMD "zstd --no-progress --ultra -22"
#endif
#ifndef ZSTD_FAST_CMD
#define ZSTD_FAST_CMD "zstd --no-progress -3"
#endif

#ifndef BROTLI_CMD
#define BROTLI_CMD "brotli -Z"
#endif
#ifndef BROTLI_FAST_CMD
#define BROTLI_FAST_CMD "brotli -3"
#endif

#ifndef GZIP_CMD
#define GZIP_CMD "gzip -9"
#endif
#ifndef GZIP_FAST_CMD
#define GZIP_FAST_CMD "gzip -5"
#endif

#ifndef ENV_ZSTD_CMD
#define ENV_ZSTD_CMD "ZSTD_CMD"
#endif
#ifndef ENV_ZSTD_FAST_CMD
#define ENV_ZSTD_FAST_CMD "ZSTD_FAST_CMD"
#endif

#ifndef ENV_BROTLI_CMD
#define ENV_BROTLI_CMD "BROTLI_CMD"
#endif
#ifndef ENV_BROTLI_FAST_CMD
#define ENV_BROTLI_FAST_CMD "BROTLI_FAST_CMD"
#endif

#ifndef ENV_GZIP_CMD
#define ENV_GZIP_CMD "GZIP_CMD"
#endif
#ifndef ENV_GZIP_FAST_CMD
#define ENV_GZIP_FAST_CMD "GZIP_FAST_CMD"
#endif

#ifdef HAVE_LOGS
#ifndef PRINTF_LOG
#define PRINTF_LOG(format, ...) if (printf_log_ptr) { printf_log_ptr(format, __VA_ARGS__); }
#endif
#else
#define PRINTF_LOG(format, ...)
#endif

enum method
{
  METHOD_UNKNOWN,
  METHOD_GET,
  METHOD_HEAD,
  METHOD_OTHER,
};

enum output_encode
{
  ENCODE_PLAIN,
  ENCODE_GZIP,
  ENCODE_BROTLI,
  ENCODE_ZSTD,
};

struct request_desc {
  long headers_length;
  char uri[MAX_URI];
  char etag[MAX_ETAG];
  char domain[MAX_DOMAIN];
  enum method method;
  enum output_encode content_encode;
  char buffer[MAX_HEADER_LINE_SIZE];
  size_t buffer_fill;
  pid_t ondemand_pid;

  int response_sock;
  char response_buffer[MAX_RESPONS_BUFFER_SIZE];
  size_t response_buffer_size;
  ssize_t (*fread)(void *, size_t, struct request_desc *);
  int (*close)(struct request_desc *);
};

const char * domain_valid_char =
    "abcdefghijklmnopqrstuvwxyz0123456789.-ABCDEFGHIJKLMNOPQRSTUVWXYZ";

volatile int end_now;

const char * cache_dir = NULL;
size_t cache_dir_len = 0;
const char * sources_dir = NULL;
size_t sources_dir_len = 0;

const char * tcp_listen = NULL;
uint16_t tcp_port = 0;

const char * unix_listen;

const char * brotli_cmd = BROTLI_CMD;
const char * zstd_cmd = ZSTD_CMD;
const char * gzip_cmd = GZIP_CMD;

const char * brotli_fast_cmd = BROTLI_FAST_CMD;
const char * zstd_fast_cmd = ZSTD_FAST_CMD;
const char * gzip_fast_cmd = GZIP_FAST_CMD;

const char * pid_file;
const char * log_file;

#ifdef HAVE_LOGS
void (*printf_log_ptr)(const char * format, ...) = NULL;
int log_fd = 0;
#endif

struct pollfd fds[MAX_PARALLEL_REQUESTS * 2 + 2];
nfds_t fds_fill_listen;
nfds_t fds_fill;

struct request_desc request_descs[MAX_PARALLEL_REQUESTS];

struct queue_t {
  const char * cmd;
  const char * ext;
  int outfd;
  char in_path[MAX_FILE_PATH];
} queue[QUEUE_SIZE];
size_t queue_fill;
sem_t queue_sem;
sem_t queue_worker_sem;

size_t min_threads = 0, max_threads = 1, cur_threads = 0, wait_threads = 0;
pthread_t thread[MAX_THREADS];

struct subprocess_desc {
  atomic_int thread_wait;
  sem_t exec_sem;
  pid_t exec_pid;
} subprocess_data[MAX_THREADS];
sem_t missing_pid_sem;

size_t cur_on_demand = 0;
size_t cur_requests = 0;

void update_xattr_counter(int fd, const char * name)
{
  if (!has_xattr_support())
    return;

  char counter_str[16];
  ssize_t len = get_user_fxattr(fd, name, counter_str,
    sizeof(counter_str) - 1);
  if (len > 0) {
    counter_str[len] = 0;
    unsigned long long l = strtoull(counter_str, NULL, 10) + 1;
    len = snprintf(counter_str, sizeof(counter_str) - 1, "%llu", l);
    update_user_fxattr(fd, name, counter_str, len);
  } else {
    counter_str[0] = '1';
    counter_str[1] = 0;
    len = 1;
    create_user_fxattr(fd, name, counter_str, len);
  }
}

unsigned long long get_xattr_prev_counter(const char * path_get)
{
  if (!has_xattr_support())
    return 0;

  int fd = open(path_get, O_RDONLY, 0);
  if (fd < 0)
    return 0;

  char counter_str[16];
  ssize_t len = get_user_fxattr(fd, XATTR_COUNTER_NAME, counter_str,
        sizeof(counter_str) - 1);
  if (len <= 0) {
    close(fd);
    return 0;
  }

  counter_str[len] = 0;
  unsigned long long counter = strtoull(counter_str, NULL, 10);
  len = get_user_fxattr(fd, XATTR_PREV_COUNTER_NAME, counter_str,
        sizeof(counter_str) - 1);
  if (len <= 0) {
    close(fd);
    return counter;
  }
  close(fd);
  counter_str[len] = 0;

  return counter + strtoull(counter_str, NULL, 10);
}

void set_xattr_counter(int fd, const char * name, unsigned long long counter)
{
  char counter_str[16];
  size_t len = snprintf(counter_str, sizeof(counter_str) - 1, "%llu", counter);
  if (create_user_fxattr(fd, name, counter_str, len) < 0)
    update_user_fxattr(fd, name, counter_str, len);
}

#ifdef HAVE_LOGS
void printf_log(const char * format, ...)
{
  struct tm tm;
  time_t now;

  time(&now);
  gmtime_r(&now, &tm);

  char timestamp[26];
  asctime_r(&tm, timestamp);

  va_list ap;
  va_start(ap, format);
  int n = vsnprintf(NULL, 0, format, ap);
  va_end(ap);

  if (n < 0)
    return;

  size_t ofs = strlen(timestamp) - 1;
  char * log = alloca(n + 4 + ofs);
  memcpy(log, timestamp, ofs);
  memcpy(log + ofs, "  ", 2);
  va_start(ap, format);
  vsnprintf(log + ofs + 2, n + 1, format, ap);
  va_end(ap);
  strcat(log, "\n");

  write(log_fd, log, n + 4 + ofs);
}
#endif

void signal_exit(int unused)
{
  (void)unused;
  end_now = 1;
}

void signal_child(int unused)
{
  (void)unused;
  pid_t p = wait(NULL);

  sem_wait(&missing_pid_sem);
  for (size_t i = 0; i < MAX_THREADS; i++) {
    if (subprocess_data[i].exec_pid == p) {
      sem_post(&missing_pid_sem);
      sem_post(&subprocess_data[i].exec_sem);
      return;
    }
  }
  sem_post(&missing_pid_sem);
}

int fake_close(struct request_desc * f)
{
  return 0;
}

ssize_t fake_fread(void * buf, size_t size, struct request_desc * desc)
{
  return -1;
}

int p_close(struct request_desc * desc)
{
  cur_on_demand--;
  return close(desc->response_sock);
}

ssize_t call_read(void * buf, size_t size, struct request_desc * desc)
{
  return read(desc->response_sock, buf, size);
}

int call_close(struct request_desc * desc)
{
  return close(desc->response_sock);
}

int get_fsize_and_mod_time(struct timespec * ts, uint64_t * fsize,
    const char * path)
{
  struct stat st;
  int r = stat(path, &st);
  if (r < 0)
    return r;

  if ((st.st_mode & S_IFMT) != S_IFREG) {
    errno = ENOENT;
    return -1;
  }

  memcpy(ts, &st.st_mtim, sizeof(*ts));
  *fsize = st.st_size;

  return 0;
}

int response_invalid_request(struct request_desc * desc)
{
  desc->response_buffer_size = snprintf(desc->response_buffer,
      MAX_RESPONS_BUFFER_SIZE,
      "Status: 400 Bad Request\r\n"
      HEADER_CONTENT_TYPE_PT
      HEADER_X_KIT_SERVICE
      "\r\n", version_str);
  desc->close = fake_close;
  desc->fread = fake_fread;

  return 0;
}

int response_not_found(struct request_desc * desc)
{
  desc->response_buffer_size = snprintf(desc->response_buffer,
      MAX_RESPONS_BUFFER_SIZE,
      "Status: 404 Not Found\r\n"
      HEADER_CONTENT_TYPE_PT
      HEADER_X_KIT_SERVICE
      "\r\n", version_str);
  desc->close = fake_close;
  desc->fread = fake_fread;

  return 0;
}

int response_ise(struct request_desc * desc)
{
  desc->response_buffer_size = snprintf(desc->response_buffer,
      MAX_RESPONS_BUFFER_SIZE,
      "Status: 500 Internal Server Error\r\n"
      HEADER_CONTENT_TYPE_PT
      HEADER_X_KIT_SERVICE
      "\r\n", version_str);
  desc->close = fake_close;
  desc->fread = fake_fread;

  return 0;
}

int response_plain(struct request_desc * desc, const char * path,
    uint64_t fsize, const char * mime, const char * etag)
{
  desc->response_buffer_size = snprintf(desc->response_buffer,
      MAX_RESPONS_BUFFER_SIZE,
      "Status: 200 OK\r\n"
      HEADER_CONTENT_TYPE
      HEADER_CONTENT_LENGHT
      HEADER_ETAG
      HEADER_ACCEPT_RANGES
      HEADER_X_KIT_SERVICE
      "\r\n", mime, (unsigned long long)fsize, etag, version_str);
  if (desc->method == METHOD_GET) {
    desc->close = call_close;
    desc->fread = call_read;
    desc->response_sock = open(path, O_RDONLY);
    if (desc->response_sock < 0)
      return -1;
    update_xattr_counter(desc->response_sock, XATTR_COUNTER_NAME);
    PRINTF_LOG("200 Plain GET \"%s\" size: %lluB", path, fsize);
  } else {
    desc->close = fake_close;
    desc->fread = fake_fread;
  }

  return 0;
}

int response_not_modified(struct request_desc * desc, const char * path,
    const char * mime, const char * etag)
{
  desc->response_buffer_size = snprintf(desc->response_buffer,
      MAX_RESPONS_BUFFER_SIZE,
      "Status: 304 Not Modified\r\n"
      HEADER_CONTENT_TYPE
      HEADER_ETAG
      HEADER_ACCEPT_RANGES
      HEADER_X_KIT_SERVICE
      "\r\n", mime, etag, version_str);
  desc->close = fake_close;
  desc->fread = fake_fread;

  PRINTF_LOG("304 Plain GET \"%s\" etag: %s", path, etag);
  if (has_xattr_support()) {
    int f = open(path, O_RDONLY);
    update_xattr_counter(f, XATTR_304_COUNTER_NAME);
    close(f);
  }

  return 0;
}

void * worker_thread(void * void_data)
{
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  sigaddset(&set, SIGPIPE);
  sigaddset(&set, SIGCHLD);
  pthread_sigmask(SIG_BLOCK, &set, NULL);

  struct subprocess_desc * th_desc = (struct subprocess_desc *)void_data;

  struct queue_t entry;

  char * path_tmp = alloca(MAX_FILE_PATH);
  char * path_tmp_2 = alloca(MAX_FILE_PATH);

  while (!end_now) {
    if (sem_wait(&queue_worker_sem) < 0)
      continue;

    for(;;) {
      if (sem_wait(&queue_sem) < 0)
        continue;
      if (!queue_fill) {
        sem_post(&queue_sem);
        break;
      }

      memcpy(&entry, queue, sizeof(entry));
      queue_fill--;
      if (queue_fill) {
        memmove(queue, &queue[1], queue_fill * sizeof(queue[0]));
      }
      sem_post(&queue_sem);
      if (strcmp(entry.in_path, EXIT_CMD) == 0) {
        atomic_store(&th_desc->thread_wait, 1);
        return NULL;
      }

      const char * uri = entry.in_path + sources_dir_len;
      int in = open(entry.in_path, O_RDONLY);

      size_t len = snprintf(path_tmp, MAX_FILE_PATH, "%s%s.%s.tmp", cache_dir,
          uri, entry.ext);

      sem_init(&th_desc->exec_sem, 0, 0);
      sem_wait(&missing_pid_sem);
      th_desc->exec_pid = int_system(in, entry.outfd, strlen(entry.cmd),
          entry.cmd);
      sem_post(&missing_pid_sem);
      if (th_desc->exec_pid > 0) {
        sem_wait(&th_desc->exec_sem);
        th_desc->exec_pid = 0;
        sem_destroy(&th_desc->exec_sem);

        memcpy(path_tmp_2, path_tmp, len - 4);
        path_tmp_2[len - 4] = 0;
        rename(path_tmp, path_tmp_2);
        PRINTF_LOG("Compression success: \"%s\" \"%s\"", entry.cmd, uri);
      } else {
        th_desc->exec_pid = 0;
        sem_destroy(&th_desc->exec_sem);
        unlink(path_tmp);
        PRINTF_LOG("Compression error: \"%s\" \"%s\"", entry.cmd, uri);
      }
    }
  }
  atomic_store(&th_desc->thread_wait, 1);
  return NULL;
}

void check_dir(const char * dirname)
{
  size_t str_len = strlen(dirname);
  char * str = alloca(str_len + 1);
  memcpy(str, dirname, str_len);
  str[str_len] = 0;

  char * pos = strrchr(str, '/');
  size_t counter = 0;
  struct stat st;

  while (pos > str) {
    *pos = 0;

    int r = stat(str, &st);
    if (r == 0)
      break;
    counter++;
    pos = strrchr(str, '/');
  }

  mode_t m = S_IRWXU | S_IXGRP | S_IRGRP;
  for (; counter > 0; counter--) {
    str[strlen(str)] = '/';
    if (mkdir(str, m) < 0)
      break;
  }
}

int is_dir(const char * path)
{
  struct stat st;
  int r = stat(path, &st);
  if (r != 0)
    return -1;
  if ((st.st_mode & S_IFMT) == S_IFDIR)
    return 0;
  return 1;
}

void enqueue_uri(const char * path, enum output_encode encode,
    unsigned long long counter)
{
  struct stat st;

  const char * sh_cmd, * ext;
  switch (encode) {
    case ENCODE_GZIP:
      sh_cmd = gzip_cmd;
      ext = "gz";
      break;
    case ENCODE_ZSTD:
      sh_cmd = zstd_cmd;
      ext = "zst";
      break;
    case ENCODE_BROTLI:
      sh_cmd = brotli_cmd;
      ext = "br";
      break;
    default:
      return;
  }

  const char * uri = path + sources_dir_len;
  char * path_tmp;
  size_t path_tmp_len = SSPRINTF(path_tmp, "%s%s.%s.tmp", cache_dir, uri, ext);

  int r = stat(path_tmp, &st);
  if (r == 0)
    return;
  path_tmp[path_tmp_len - 4] = 0;
  r = stat(path_tmp, &st);
  if (r == 0)
    return;
  path_tmp[path_tmp_len - 4] = '.';

  int f = creat(path_tmp, S_IRUSR | S_IWUSR | S_IRGRP);
  if (f < 0) {
    check_dir(path_tmp);
    f = creat(path_tmp, S_IRUSR | S_IWUSR | S_IRGRP);
    if (f < 0)
      return;
  }
  set_xattr_counter(f, XATTR_PREV_COUNTER_NAME, counter);
  set_xattr_counter(f, XATTR_COUNTER_NAME, 0);

  if (sem_wait(&queue_sem) < 0) {
    close(f);
    return;
  }

  if (queue_fill < QUEUE_SIZE) {
    queue[queue_fill].cmd = sh_cmd;
    queue[queue_fill].ext = ext;
    queue[queue_fill].outfd = f;
    strncpy(queue[queue_fill].in_path, path, MAX_FILE_PATH);
    queue_fill++;

    if (queue_fill > cur_threads) {
      size_t j = -1;
      for (size_t i = cur_threads; (i < max_threads) && (i < queue_fill); i++) {
        for (j++; j < MAX_THREADS; j++) {
          if (!thread[j]) {
            atomic_store(&subprocess_data[j].thread_wait, 0);
            pthread_create(&thread[j], NULL, worker_thread,
                &subprocess_data[j].thread_wait);
            cur_threads++;
            break;
          }
        }
      }
    }
  }

  sem_post(&queue_sem);
  sem_post(&queue_worker_sem);
}

int response_fast(struct request_desc * desc, const char * plain_path,
    const char * fast_cmd, const char * encode_str, const char * mime,
    enum output_encode encode, const char * etag, unsigned long long counter)
{
  if (cur_on_demand >=  MAX_PARALLEL_ON_DEMAND) {
    struct timespec ts;
    uint64_t plain_fsize;
    get_fsize_and_mod_time(&ts, &plain_fsize, plain_path);
    enqueue_uri(plain_path, encode, counter);
    return response_plain(desc, plain_path, plain_fsize, mime, etag);
  }

  desc->response_buffer_size = snprintf(desc->response_buffer,
      MAX_RESPONS_BUFFER_SIZE,
      "Status: 200 OK\r\n"
      HEADER_CONTENT_TYPE
      HEADER_CONTENT_ENCODING
      HEADER_ETAG
      HEADER_ACCEPT_RANGES
      HEADER_X_KIT_SERVICE
      "\r\n", mime, encode_str, etag, version_str);
  if (desc->method == METHOD_GET) {
    desc->close = p_close;
    desc->fread = call_read;
    struct int_exec_t stat;
    int input = open(plain_path, O_RDONLY);
    if ((input <= 0) ||
        (int_exec(&stat, input, strlen(fast_cmd), fast_cmd) < 0)) {
      struct timespec ts;
      uint64_t plain_fsize;
      get_fsize_and_mod_time(&ts, &plain_fsize, plain_path);
      enqueue_uri(plain_path, encode, counter);
      if (input > 0)
        close(input);
      return response_plain(desc, plain_path, plain_fsize, mime, etag);
    }
    if (input > 0) {
      update_xattr_counter(input, XATTR_COUNTER_NAME);
      close(input);
    }
    desc->response_sock = stat.stdout_fd;
    PRINTF_LOG("200 fast %s GET \"%s\"", encode_str, plain_path);
    cur_on_demand++;
  } else {
    desc->close = fake_close;
    desc->fread = fake_fread;
  }

  enqueue_uri(plain_path, encode, counter);
  return 0;
}

int response_compressed(struct request_desc * desc, const char * path,
    uint64_t fsize, const char * encode_str, const char * mime,
    const char * etag)
{
  desc->response_buffer_size = snprintf(desc->response_buffer,
      MAX_RESPONS_BUFFER_SIZE,
      "Status: 200 OK\r\n"
      HEADER_CONTENT_TYPE
      HEADER_CONTENT_LENGHT
      HEADER_CONTENT_ENCODING
      HEADER_ETAG
      HEADER_ACCEPT_RANGES
      HEADER_X_KIT_SERVICE
      "\r\n", mime, (unsigned long long)fsize, encode_str, etag, version_str);
  if (desc->method == METHOD_GET) {
    desc->close = call_close;
    desc->fread = call_read;
    desc->response_sock = open(path, O_RDONLY);
    if (desc->response_sock < 0)
      return -1;
    update_xattr_counter(desc->response_sock, XATTR_COUNTER_NAME);
    PRINTF_LOG("200 %s GET \"%s\" size: %lluB", encode_str, path, fsize);
  } else {
    desc->close = fake_close;
    desc->fread = fake_fread;
  }

  return 0;
}

void canonize_path(char * output, const char * input)
{
  char * ptr = output;
  if (*input != '/')
    *ptr++ = '/';

  while (*input) {
    if (*input == '/') {
      if (strcmp(input, "/./") == 0) {
        input += 2;
      } else if (strcmp(input, "/../") == 0) {
        input += 3;
        while ((ptr > output) && (*(ptr - 1) != '/'))
          ptr--;
        continue;
      } else if (*ptr == '/') {
        input++;
        continue;
      }
    }
    *ptr++ = *input++;
  }

  *ptr = 0;
}

int choose_file(struct request_desc * desc)
{
  const char * uri = desc->uri;
  const size_t uri_len = strlen(uri);
  const char * host = desc->domain;
  size_t host_len = host[0];
  host++;

  char * plain_path = alloca(sources_dir_len + host_len + uri_len + 2);
  memcpy(plain_path, sources_dir, sources_dir_len);
  if (host_len) {
    plain_path[sources_dir_len] = '/';
    memcpy(plain_path + sources_dir_len + 1, host, host_len);
    plain_path[sources_dir_len + host_len + 1] = 0;
    if (is_dir(plain_path) != 0) {
      host_len = 0;
    } else {
      host_len++;
    }
  }

  memcpy(plain_path + sources_dir_len + host_len, uri, uri_len);
  plain_path[sources_dir_len + host_len + uri_len] = 0;

  struct timespec plain_ts;
  uint64_t plain_fsize;
  if (get_fsize_and_mod_time(&plain_ts, &plain_fsize, plain_path) < 0) {
    if (errno == ENOENT) {
      return response_not_found(desc);
    }
    return response_invalid_request(desc);
  }

  char * etag;
  SSPRINTF(etag, "%08llX%08lX", (unsigned long long)plain_ts.tv_sec,
      plain_ts.tv_nsec);

  if (strcmp(etag, desc->etag) == 0) {
    return response_not_modified(desc, plain_path,
        get_mime_from_ext(plain_path), etag);
  }

  if (desc->content_encode == ENCODE_PLAIN) {
    return response_plain(desc, plain_path, plain_fsize,
        get_mime_from_ext(plain_path), etag);
  }

  char * encoded_path;
  const char * fast_cmd, * encode_str;

  encoded_path = alloca(cache_dir_len + uri_len + host_len + 5);
  memcpy(encoded_path, cache_dir, cache_dir_len);
  if (host_len) {
    encoded_path[cache_dir_len] = '/';
    memcpy(encoded_path + cache_dir_len + 1, host, host_len - 1);
  }
  memcpy(encoded_path + cache_dir_len + host_len, uri, uri_len);

  if (desc->content_encode == ENCODE_GZIP) {
    memcpy(encoded_path + cache_dir_len + host_len + uri_len, ".gz", 4);
    fast_cmd = gzip_fast_cmd;
    encode_str = "gzip";
  } else if (desc->content_encode == ENCODE_BROTLI) {
    memcpy(encoded_path + cache_dir_len + host_len + uri_len, ".br", 4);
    fast_cmd = brotli_fast_cmd;
    encode_str = "br";
  } else if (desc->content_encode == ENCODE_ZSTD) {
    memcpy(encoded_path + cache_dir_len + host_len + uri_len, ".zst", 5);
    fast_cmd = zstd_fast_cmd;
    encode_str = "zstd";
  }

  struct timespec encoded_ts;
  uint64_t encoded_fsize;
  if (get_fsize_and_mod_time(&encoded_ts, &encoded_fsize, encoded_path) < 0) {
    if (errno == ENOENT) {
      return response_fast(desc, plain_path, fast_cmd, encode_str,
        get_mime_from_ext(plain_path), desc->content_encode, etag, 0);
    }
    return response_invalid_request(desc);
  }

  if ((encoded_ts.tv_sec < plain_ts.tv_sec) ||
      (encoded_ts.tv_sec == plain_ts.tv_sec) &&
      (encoded_ts.tv_nsec < plain_ts.tv_nsec)) {
    unsigned long long counter = get_xattr_prev_counter(encoded_path);
    unlink(encoded_path);
    return response_fast(desc, plain_path, fast_cmd, encode_str,
        get_mime_from_ext(plain_path), desc->content_encode, etag, counter);
  }

  return response_compressed(desc, encoded_path, encoded_fsize, encode_str,
        get_mime_from_ext(plain_path), etag);
}

int response_file(struct request_desc * desc)
{
  if ((desc->method == METHOD_UNKNOWN) || (desc->uri[0] == 0)) {
    return response_invalid_request(desc);
  }

  return choose_file(desc);
}

int ascii_str_cmp(const char * a, const char * b)
{
  while (*a && *b) {
    char ach = *a;
    char bch = *b;

    if (ach == bch) {
      a++;
      b++;
      continue;
    }

    if ((ach >= 'a') && (ach <= 'z'))
      a -= 'a' - 'Z';
    if ((bch >= 'a') && (bch <= 'z'))
      b -= 'a' - 'Z';

    if (ach == bch) {
      a++;
      b++;
      continue;
    }

    break;
  }

  return *a - *b;
}

int process_header_content_length(struct request_desc * desc,
    const char * value)
{
  char * ch;
  long l = strtol(value, &ch, 10);
  if ((l != 0) || (*ch != 0))
    return -1;
  return 0;
}

int process_header_scgi(struct request_desc * desc, const char * value,
    size_t value_len)
{
  if ((value_len != 1) || (value[0] != '1'))
    return -1;
  return 0;
}

int process_header_request_method(struct request_desc * desc,
    const char * value)
{
  if (ascii_str_cmp(value, "GET") == 0)
    desc->method = METHOD_GET;
  else if (ascii_str_cmp(value, "HEAD") == 0)
    desc->method = METHOD_HEAD;
  else {
    desc->method = METHOD_OTHER;
    return -1;
  }

  return 0;
}

int process_header_request_uri(struct request_desc * desc, const char * value,
    size_t value_len)
{
  if (value[0] != '/')
    return -1;
  size_t len = value_len;
  char * ptr = strchr(value, '?');

  if (ptr && (ptr - value < len))
    len = ptr - value;

  if (len + 1 > MAX_URI)
    return -1;

  char * str = alloca(len + 1);
  size_t o = 0;
  for (size_t i = 0; i < len; i++, o++) {
    if (value[i] == '%') {
      if (i + 2 >= len)
        return -1;
      int x = hex2byte(&value[i + 1]);
      if (x < 0)
        return -1;
      i += 2;
      str[o] = x;
    } else {
      str[o] = value[i];
    }
  }
  str[o] = 0;

  canonize_path(desc->uri, str);
  return 0;
}

int process_header_accept_encoding(struct request_desc * desc,
    const char * value)
{
  char * ptr = strstr(value, "zstd");
  if ((ptr) && ((ptr[4] == 0) || (strchr(", \t\r\n", ptr[4])))) {
    desc->content_encode = ENCODE_ZSTD;
    return 0;
  }

  ptr = strstr(value, "br");
  if ((ptr) && ((ptr[2] == 0) || (strchr(", \t\r\n", ptr[2])))) {
    desc->content_encode = ENCODE_BROTLI;
    return 0;
  }

  ptr = strstr(value, "gzip");
  if ((ptr) && ((ptr[4] == 0) || (strchr(", \t\r\n", ptr[4])))) {
    desc->content_encode = ENCODE_GZIP;
    return 0;
  }

  desc->content_encode = ENCODE_PLAIN;
  return 0;
}

int process_header_if_none_match(struct request_desc * desc,
    const char * value, size_t value_len)
{
  if (value[0] != '\"')
    return 0;

  if (value[value_len - 1] != '\"')
    return 0;

  if (value_len > MAX_ETAG)
    return 0;

  memcpy(desc->etag, &value[1], value_len - 2);
  desc->etag[value_len - 1] = 0;

  return 0;
}

int process_header_host(struct request_desc * desc, const char * value)
{
  char * domain = desc->domain;
  *domain++ = 0;

  for (char ch = *value; (ch = *value); value++) {
    if (strchr(domain_valid_char, ch) == NULL)
      return -1;
    *domain++ = ch;
    if (domain - desc->domain == sizeof(desc->domain))
      return -1;
  }
  if (domain - desc->domain == 1)
    return -1;

  *domain = 0;
  desc->domain[0] = domain - desc->domain - 1;

  return 0;
}

int process_header(struct request_desc * desc, const char * name,
    const char * value, size_t value_len)
{
  if (ascii_str_cmp(name, "CONTENT_LENGTH") == 0)
    return process_header_content_length(desc, value);
  if (ascii_str_cmp(name, "SCGI") == 0)
    return process_header_scgi(desc, value, value_len);
  if (ascii_str_cmp(name, "REQUEST_METHOD") == 0)
    return process_header_request_method(desc, value);
  if (ascii_str_cmp(name, "REQUEST_URI") == 0)
    return process_header_request_uri(desc, value, value_len);
  if (ascii_str_cmp(name, "HTTP_ACCEPT_ENCODING") == 0)
    return process_header_accept_encoding(desc, value);
  if (ascii_str_cmp(name, "HTTP_IF_NONE_MATCH") == 0)
    return process_header_if_none_match(desc, value, value_len);
  if (ascii_str_cmp(name, "SERVER_NAME") == 0)
    return process_header_host(desc, value);

  return 0;
}

int process_headers(struct request_desc * desc, int infd)
{
  size_t r = read(infd, desc->buffer + desc->buffer_fill,
      sizeof(desc->buffer) - desc->buffer_fill);
  if ((r < 0) || ((r == 0) && (desc->headers_length == 0)))
    return -1;

  if (r == 0) {
    return response_invalid_request(desc);
  }

  desc->buffer_fill += r;

  char * ptr = desc->buffer;
  if (desc->headers_length == 0) {
    char * ch;
    unsigned long len = strtoul(ptr, &ch, 10);
    if (*ch != ':')
      return response_invalid_request(desc);

    desc->headers_length = len;
    ptr = ch + 1;
  }

  char * buf_end = desc->buffer + desc->buffer_fill;

  for(;desc->headers_length >= 0;) {
    if (*ptr == ',')
      return response_file(desc);

    size_t max = buf_end - ptr;
    size_t len1 = strnlen(ptr, max);
    if (len1 == max)
      break;

    max -= len1 + 1;
    size_t len2 = strnlen(ptr + len1 + 1, max);
    if (len2 == max)
      break;

    if (process_header(desc, ptr, ptr + len1 + 1, len2) < 0)
      return response_invalid_request(desc);

    ptr += len1 + len2 + 2;
    desc->headers_length -= len1 + len2 + 2;
  }

  if (desc->headers_length < 0)
    return -1;

  size_t len = buf_end - ptr;
  memmove(desc->buffer, ptr, len);
  desc->buffer_fill = len;

  return r;
}

ssize_t write_output(struct request_desc * desc, int outfd)
{
  if (desc->response_buffer_size == 0) {
    ssize_t r = desc->fread(desc->response_buffer, MAX_RESPONS_BUFFER_SIZE,
        desc);
    if (r <= 0) {
      return -1;
    }
    desc->response_buffer_size = r;
  }

  ssize_t w = write(outfd, desc->response_buffer, desc->response_buffer_size);
  if (w > 0) {
    if (w < desc->response_buffer_size) {
      memmove(desc->response_buffer, &desc->response_buffer[w],
          desc->response_buffer_size - w);
    }
    desc->response_buffer_size -= w;
  }
  return w;
}

void usage(const char * argv0)
{
  fprintf(stderr, "Krypto-IT HTTP/SCGI static content service.\n");
  fprintf(stderr, "Usage:\n");
  fprintf(stderr, "%s [args...] <source_dir> <cache_dir> <unix_socket_path>\n",
      argv0);
  fprintf(stderr, "%s [args...] <source_dir> <cache_dir> <listen_ip>"
      " <tcp_port>\n", argv0);
  fprintf(stderr, "%s [args...] <source_dir> <cache_dir> <unix_socket_path>"
      " <listen_ip> <tcp_port>\n\n", argv0);
  fprintf(stderr, "[args...]:\n");
  fprintf(stderr, " [-pidfile file.pid] - pid file path\n");
  fprintf(stderr, " [-logfile file.log] - log file path\n");
  fprintf(stderr, " [-minthreads <uint>] - min compression threads count\n");
  fprintf(stderr, " [-maxthreads <uint>] - max compression threads count\n\n");
  fprintf(stderr, "\nVersion: %s\n", version_str);
  exit(1);
}

void check_env(const char * name, const char ** out)
{
  const char * env = getenv(name);
  if (!env)
    return;

  *out = env;
}

int main(int argc, char * argv[])
{
  if (argc < 4) {
    usage(argv[0]);
  }

  int arg_start = 1;
  for (; arg_start + 1 < argc; arg_start++) {
    if (argv[arg_start][0] != '-')
      break;
    if (strcmp(argv[arg_start], "-pidfile") == 0) {
      arg_start++;
      pid_file = argv[arg_start];
    } else if (strcmp(argv[arg_start], "-logfile") == 0) {
      arg_start++;
      log_file = argv[arg_start];
    } else if (strcmp(argv[arg_start], "-minthreads") == 0) {
      arg_start++;
      int i = atoi(argv[arg_start]);
      if ((i >= 0) && (i < MAX_THREADS))
        min_threads = i;
    } else if (strcmp(argv[arg_start], "-maxthreads") == 0) {
      arg_start++;
      int i = atoi(argv[arg_start]);
      if ((i >= 0) && (i < MAX_THREADS))
        max_threads = i;
    }
  }

  if (max_threads < min_threads) {
    min_threads = max_threads;
  }
  if (max_threads == 0) {
    max_threads = 1;
  }

  if (arg_start + 2 >= argc) {
    usage(argv[0]);
  }

  sources_dir = argv[arg_start];
  sources_dir_len = strlen(sources_dir);
  cache_dir = argv[arg_start + 1];
  cache_dir_len = strlen(cache_dir);
  if (argc - arg_start == 3) {
    unix_listen = argv[arg_start + 2];
  } else if (argc - arg_start == 4) {
    tcp_listen = argv[arg_start + 2];
    tcp_port = atoi(argv[arg_start + 3]);
  } else if (argc - arg_start == 5) {
    unix_listen = argv[arg_start + 2];
    tcp_listen = argv[arg_start + 3];
    tcp_port = atoi(argv[arg_start + 4]);
  } else
    usage(argv[0]);

  daemon(0, 0);

  check_env(ENV_ZSTD_CMD, &zstd_cmd);
  check_env(ENV_ZSTD_FAST_CMD, &zstd_fast_cmd);
  check_env(ENV_BROTLI_CMD, &brotli_cmd);
  check_env(ENV_BROTLI_FAST_CMD, &brotli_fast_cmd);
  check_env(ENV_GZIP_CMD, &gzip_cmd);
  check_env(ENV_GZIP_FAST_CMD, &gzip_fast_cmd);

  if (pid_file) {
    FILE * f = fopen(pid_file, "w");
    if (f) {
      size_t p = getpid();
      fprintf(f, "%zu", p);
      fclose(f);
    }
  }

#ifdef HAVE_LOGS
  if (log_file) {
    log_fd = open(log_file, O_CREAT | O_APPEND | O_NDELAY | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP);
    if (log_fd > 0) {
      printf_log_ptr = printf_log;
    }
  }
#endif

  if (unix_listen) {
    fds[fds_fill].fd = open_unix_listen(unix_listen);
    fds[fds_fill].events = POLLIN;
    fds[fds_fill].revents = 0;
    fds_fill++;
  }

  if (tcp_listen) {
    fds[fds_fill].fd = open_tcp_listen(tcp_listen, tcp_port);
    fds[fds_fill].events = POLLIN;
    fds[fds_fill].revents = 0;
    fds_fill++;
  }

  signal(SIGTERM, signal_exit);
  signal(SIGINT, signal_exit);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, signal_child);

  sem_init(&queue_sem, 0, 1);
  sem_init(&queue_worker_sem, 0, 0);
  sem_init(&missing_pid_sem, 0, 1);

  for (size_t i = 0; i < min_threads; i++) {
    atomic_store(&subprocess_data[i].thread_wait, 0);
    pthread_create(&thread[i], NULL, worker_thread,
        &subprocess_data[i].thread_wait);
  }
  for (size_t i = min_threads; i < MAX_THREADS; i++) {
    atomic_store(&subprocess_data[i].thread_wait, 0);
    thread[i] = 0;
  }
  cur_threads = min_threads;

  fds_fill_listen = fds_fill;
  int cleanup;

  time_t last = 0;
  for (end_now = 0;!end_now;) {
    if (cur_requests < MAX_PARALLEL_REQUESTS) {
      for (nfds_t i = 0; i < fds_fill_listen; i++)
        fds[i].events = POLLIN;
    } else {
      for (nfds_t i = 0; i < fds_fill_listen; i++)
        fds[i].events = 0;
    }

    poll(fds, fds_fill, 100);
    cleanup = 0;

    for (nfds_t i = 0; i + i + fds_fill_listen < fds_fill; i++) {
      struct pollfd * current_fd = &fds[fds_fill_listen + i + i];
      struct request_desc * current_desc = &request_descs[i];

      if (current_fd->revents & POLLIN) {
        int r = process_headers(current_desc, current_fd->fd);
        if (r < 0) {
          close(current_fd->fd);
          current_fd->fd = 0;
          current_fd->events = 0;
          memset(current_desc, 0, sizeof(*current_desc));
          cur_requests--;
          cleanup++;
          continue;
        }
        if (r == 0) {
          current_fd->events = POLLOUT;
          if (current_desc->response_sock) {
            current_fd[1].fd = current_desc->response_sock;
            current_fd[1].events = POLLIN;
          }
        }
      } else if (current_fd->revents & (POLLERR | POLLHUP)) {
        close(current_fd->fd);
        current_fd->fd = 0;
        current_fd->events = 0;
        if (current_desc->close) {
          current_desc->close(current_desc);
        }
        current_fd[1].fd = 0;
        current_fd[1].events = 0;
        memset(current_desc, 0, sizeof(*current_desc));
        cur_requests--;
        cleanup++;
      } else if (current_fd->revents & POLLOUT) {
        if (((current_fd[1].revents & POLLIN) != 0) ||
            (current_desc->response_buffer_size)) {
          ssize_t r = write_output(current_desc, current_fd->fd);
          if (r < 0) {
            close(current_fd->fd);
            current_fd->fd = 0;
            current_fd->events = 0;
            current_desc->close(current_desc);
            current_fd[1].fd = 0;
            current_fd[1].events = 0;
            memset(current_desc, 0, sizeof(*current_desc));
            cur_requests--;
            cleanup++;
          }
        } else if (current_fd[1].events & POLLIN) {
          current_fd->events = 0;
        } else if ((current_fd[1].fd == 0) &&
            (current_desc->response_buffer_size == 0)) {
          close(current_fd->fd);
            current_fd->fd = 0;
            current_fd->events = 0;
            current_desc->close(current_desc);
            current_fd[1].events = 0;
            memset(current_desc, 0, sizeof(*current_desc));
            cur_requests--;
            cleanup++;
        }
      } else if (current_fd[1].revents & POLLIN) {
        current_fd->events = POLLOUT;
      } else if (current_fd[1].revents & (POLLERR | POLLHUP)) {
        write_output(current_desc, current_fd->fd);
        close(current_fd->fd);
        current_fd->fd = 0;
        current_fd->events = 0;
        current_desc->close(current_desc);
        current_fd[1].fd = 0;
        current_fd[1].events = 0;
        memset(current_desc, 0, sizeof(*current_desc));
        cur_requests--;
        cleanup++;
      }
    }
    if (cleanup) {
      for (; (fds_fill_listen + 2 < fds_fill) && (fds[fds_fill - 2].fd == 0)
        && (fds[fds_fill - 1].fd == 0); fds_fill -= 2);
    }

    for (nfds_t i = 0; i < fds_fill_listen; i++) {
      struct pollfd * current_fd = &fds[i];

      if ((current_fd->revents & POLLIN) == 0)
        continue;
      if (cur_requests == MAX_PARALLEL_REQUESTS)
        break;

      int sock = accept(current_fd->fd, NULL, NULL);
      if (sock > 0) {
        size_t idx;
        for (idx = fds_fill_listen; fds[idx].fd != 0; idx += 2);

        fds[idx].fd = sock;
        fds[idx + 1].fd = 0;
        fds[idx].events = POLLIN;
        fds[idx + 1].events = 0;
        if (idx >= fds_fill)
          fds_fill = idx + 2;
        cur_requests++;
      }
    }

    for (size_t i = 0; (wait_threads) && (i < MAX_THREADS); i++) {
      if (atomic_load(&subprocess_data[i].thread_wait)) {
        pthread_join(thread[i], NULL);
        atomic_store(&subprocess_data[i].thread_wait, 0);
        thread[i] = 0;
        wait_threads--;
        cur_threads--;
      }
    }

    if (sem_wait(&queue_sem) == 0) {
      if (queue_fill < cur_threads) {
        for(; (queue_fill < QUEUE_SIZE) &&
            (cur_threads - wait_threads > min_threads); wait_threads++) {
          memcpy(queue[queue_fill].in_path, EXIT_CMD, strlen(EXIT_CMD) + 1);
          queue[queue_fill].cmd = NULL;
          queue[queue_fill].ext = NULL;
          queue[queue_fill].outfd = 0;
          queue_fill++;
        }
      }
      sem_post(&queue_worker_sem);
      sem_post(&queue_sem);
    }
  }

  sem_post(&queue_sem);
  sem_post(&queue_worker_sem);
  sem_destroy(&queue_sem);
  sem_destroy(&queue_worker_sem);
  sem_destroy(&missing_pid_sem);

  for (size_t i = 0; i < MAX_THREADS; i++) {
    if (thread[i]) {
      pthread_join(thread[i], NULL);
    }
  }

  for (nfds_t i = 0; (i < fds_fill_listen); i++)
    close(fds[i].fd);

  if (unix_listen) {
    unlink(unix_listen);
  }
#ifdef HAVE_LOGS
  if (log_fd > 0) {
    close(log_fd);
  }
#endif
  if (pid_file) {
    unlink(pid_file);
  }

  return 0;
}
