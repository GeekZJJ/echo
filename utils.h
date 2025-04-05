/* SPDX-License-Identifier: MIT */

#ifndef UTILS_H_
#define UTILS_H_

#include <ngtcp2/ngtcp2.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <uv.h>
#include <stdbool.h>

void sockaddr_to_string(struct sockaddr_in *addr, char *str, size_t len);

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);

bool 
resolve_and_connect (uv_loop_t *loop, const char *host, const char *port,
                     uv_udp_t *udp_recv_socket, uv_udp_recv_cb udp_recv_cb,
                     struct sockaddr *local_addr, size_t *local_addrlen,
                     struct sockaddr *remote_addr, size_t *remote_addrlen);

bool
resolve_and_bind (uv_loop_t *loop, const char *host, const char *port,
                  uv_udp_t *udp_recv_socket, uv_udp_recv_cb udp_recv_cb,
                  struct sockaddr *local_addr, size_t *local_addrlen);

uint64_t timestamp (void);
void log_printf (void *user_data, const char *fmt, ...);
int get_random_cid (ngtcp2_cid *cid);

ssize_t recv_packet (int fd, uint8_t *data, size_t data_size,
                     struct sockaddr *remote_addr, size_t *remote_addrlen);
ssize_t send_packet (int fd, const uint8_t *data, size_t data_size,
                     struct sockaddr *remote_addr, size_t remote_addrlen);

/* For __attribute__((cleanup)) */
static inline void
closep (int *p)
{
  int fd = *p;
  if (fd >= 0)
    close (fd);
}

static inline int
steal_fd (int *p)
{
  int fd = *p;
  *p = -1;
  return fd;
}

#endif  /* UTILS_H_ */
