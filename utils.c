/* SPDX-License-Identifier: MIT */

#include "config.h"

#include "utils.h"

#include <errno.h>
#include <glib.h>
#include <ngtcp2/ngtcp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <uv.h>

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

bool
resolve_and_connect (uv_loop_t *loop, const char *host, const char *port,
                     uv_udp_t *udp_recv_socket, uv_udp_recv_cb udp_recv_cb,
                     struct sockaddr *local_addr, size_t *local_addrlen,
                     struct sockaddr *remote_addr, size_t *remote_addrlen)
{
  uv_getaddrinfo_t resolver;
  int r = uv_getaddrinfo(loop, &resolver, NULL, host, port, NULL);
  if (r) {
      fprintf(stderr, "getaddrinfo call error: %s\n", uv_strerror(r));
      return false;
  }

  struct addrinfo* ai;
  for (ai = resolver.addrinfo; ai != NULL; ai = ai->ai_next) {
    if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
      continue;
    uv_udp_init(loop, udp_recv_socket);
    struct sockaddr_storage local;
    memset(&local, 0, sizeof(local));
    if (ai->ai_family == AF_INET) {
      struct sockaddr_in *l = (struct sockaddr_in *)&local;
      l->sin_family = AF_INET;
      l->sin_addr.s_addr = INADDR_ANY;
      l->sin_port = htons(0);
    } else {
      struct sockaddr_in6 *l = (struct sockaddr_in6 *)&local;
      l->sin6_family = AF_INET6;
      l->sin6_addr = in6addr_any;
      l->sin6_port = htons(0);
    }
    if (0==uv_udp_bind(udp_recv_socket, (const struct sockaddr *) &local, 0)) {
      *remote_addrlen = ai->ai_addrlen;
      memcpy(remote_addr, ai->ai_addr, ai->ai_addrlen);
      int len = ai->ai_addrlen;
      if (uv_udp_getsockname (udp_recv_socket, local_addr, &len) != 0)
        return false;
      *local_addrlen = len;
      uv_udp_recv_start(udp_recv_socket, alloc_buffer, udp_recv_cb);
      break;
    }
  }

  uv_freeaddrinfo(resolver.addrinfo);
  if (ai == NULL)
    return false;

  return true;
}

bool
resolve_and_bind (uv_loop_t *loop, const char *host, const char *port,
                  uv_udp_t *udp_recv_socket, uv_udp_recv_cb udp_recv_cb,
                  struct sockaddr *local_addr, size_t *local_addrlen)
{
  uv_getaddrinfo_t resolver;
  int r = uv_getaddrinfo(loop, &resolver, NULL, host, port, NULL);
  if (r) {
      fprintf(stderr, "getaddrinfo call error: %s\n", uv_strerror(r));
      return false;
  }

  struct addrinfo* ai;
  for (ai = resolver.addrinfo; ai != NULL; ai = ai->ai_next) {
    uv_udp_init(loop, udp_recv_socket);
    if (0==uv_udp_bind(udp_recv_socket, (const struct sockaddr *) ai->ai_addr, UV_UDP_REUSEADDR)) {
      uv_udp_recv_start(udp_recv_socket, alloc_buffer, udp_recv_cb);
      *local_addrlen = ai->ai_addrlen;
      memcpy(local_addr, ai->ai_addr, ai->ai_addrlen);
      break;
    }
  }

  uv_freeaddrinfo(resolver.addrinfo);
  if (ai == NULL)
    return false;

  return true;
}

uint64_t
timestamp (void)
{
  return uv_hrtime();
}

void
log_printf (void *user_data, const char *fmt, ...)
{
  va_list ap;
  (void)user_data;

  va_start (ap, fmt);
  g_logv ("ngtcp2", G_LOG_LEVEL_DEBUG, fmt, ap);
  va_end (ap);
}

int rand_bytes(uint8_t *data, size_t len);

int
get_random_cid (ngtcp2_cid *cid)
{
    uint8_t buf[NGTCP2_MAX_CIDLEN];

    if (rand_bytes(buf, sizeof(buf)) < 0) {
        return -1;
    }
    ngtcp2_cid_init(cid, buf, sizeof(buf));
    return 0;
}
