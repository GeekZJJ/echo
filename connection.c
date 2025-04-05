/* SPDX-License-Identifier: MIT */

#include "config.h"

#include "connection.h"

#include <unistd.h>
#include "utils.h"

#define BUF_SIZE 12800

struct _Connection
{
  ngtcp2_conn *conn;
  uv_loop_t *loop;
  uv_timer_t timer;
  uv_udp_t *udp_socket;
  struct sockaddr_storage local_addr;
  size_t local_addrlen;
  struct sockaddr_storage remote_addr;
  size_t remote_addrlen;
  GList *streams;
  bool is_closed;
};

void timer_cb(uv_timer_t *handle) {
  Connection *connection = handle->data;
  ngtcp2_conn *conn = connection_get_ngtcp2_conn (connection);
  int ret = ngtcp2_conn_handle_expiry (conn, timestamp ());
  if (ret < 0)
    {
      g_debug ("ngtcp2_conn_handle_expiry: %s", ngtcp2_strerror (ret));
    }
  (void)connection_write (connection);
}

Connection *
connection_new (uv_loop_t *loop, void* session, uv_udp_t *udp_socket)
{
  __attribute__((cleanup(connection_freep)))
    Connection *connection = NULL;

  connection = g_new0 (Connection, 1);
  if (!connection)
    return NULL;

  connection->udp_socket = udp_socket;

  uv_timer_init(loop, &connection->timer);
  connection->timer.data = connection;
  printf("connection new\n");

  return g_steal_pointer (&connection);
}

void
connection_free (Connection *connection)
{
  if (!connection)
    return;

  if (connection->conn)
    ngtcp2_conn_del (connection->conn);
  uv_timer_stop(&connection->timer);
  g_list_free_full (connection->streams, (GDestroyNotify)stream_free);
  g_free (connection);
}

void
connection_add_stream (Connection *connection, Stream *stream)
{
  connection->streams = g_list_append (connection->streams, stream);
}

Stream *
connection_find_stream (Connection *connection, int64_t stream_id)
{
  for (GList *l = connection->streams; l; l = l->next)
    {
      Stream *stream = l->data;
      if (stream_get_id (stream) == stream_id)
        return stream;
    }
  return NULL;
}

ngtcp2_conn *
connection_get_ngtcp2_conn (Connection *connection)
{
  return connection->conn;
}

void
connection_set_ngtcp2_conn (Connection *connection, ngtcp2_conn *conn)
{
  connection->conn = conn;
}

struct sockaddr *
connection_get_local_addr (Connection *connection, size_t *local_addrlen)
{
  *local_addrlen = connection->local_addrlen;
  return (struct sockaddr *)&connection->local_addr;
}

void
connection_set_local_addr (Connection *connection,
                           struct sockaddr *local_addr,
                           size_t local_addrlen)
{
  memcpy (&connection->local_addr, local_addr, local_addrlen);
  connection->local_addrlen = local_addrlen;
}

void
connection_set_remote_addr (Connection *connection,
                           struct sockaddr *remote_addr,
                           size_t remote_addrlen)
{
  memcpy (&connection->remote_addr, remote_addr, remote_addrlen);
  connection->remote_addrlen = remote_addrlen;
}

int
connection_start (Connection *connection)
{
  g_return_val_if_fail (connection->conn, -1);

  uv_timer_start(&connection->timer, timer_cb, 0, 0);
  return 0;
}

void udp_send_cb(uv_udp_send_t *req, int status) {
  if (status) {
      fprintf(stderr, "Send error: %s\n", uv_strerror(status));
  }
  free(req->data);
  free(req);
}

void send_udp_message(uv_udp_t *send_socket, const uint8_t *data, size_t data_size,
  const struct sockaddr *remote_addr) {
  g_message("send_udp_message %lu", data_size);
  uv_udp_send_t *send_req = malloc(sizeof(uv_udp_send_t));
  void *buf = malloc(data_size);
  memcpy(buf, data, data_size);
  uv_buf_t buffer = uv_buf_init((char *)buf, data_size);
  send_req->data = buf;
  int r = uv_udp_send(send_req, send_socket, &buffer, 1, remote_addr, udp_send_cb);
  if (r) {
      fprintf(stderr, "uv_udp_send error: %s\n", uv_strerror(r));
      free(buf);
      free(send_req);
  }
}

static int
write_to_stream (Connection *connection, Stream *stream)
{
  uint8_t buf[BUF_SIZE];

  ngtcp2_path_storage ps;
  ngtcp2_path_storage_zero(&ps);

  ngtcp2_pkt_info pi;
  uint64_t ts = timestamp ();

  uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;

  for (;;)
    {
      ngtcp2_vec datav;
      int64_t stream_id;

      if (stream)
        {
          datav.base = (void *)stream_peek_data (stream, &datav.len);
          if (datav.len == 0)
            {
              /* No stream data to be sent */
              stream_id = -1;
              flags &= ~NGTCP2_WRITE_STREAM_FLAG_MORE;
            }
          else
            stream_id = stream_get_id (stream);
        }
      else
        {
          datav.base = NULL;
          datav.len = 0;
          stream_id = -1;
        }

      ngtcp2_ssize n_read, n_written;

      n_written = ngtcp2_conn_writev_stream (connection->conn, &ps.path, &pi,
					     buf, sizeof(buf),
					     &n_read,
					     flags,
					     stream_id,
					     &datav, 1,
					     ts);
      if (n_written < 0)
        {
          if (n_written == NGTCP2_ERR_WRITE_MORE)
            {
              stream_mark_sent (stream, n_read);
              continue;
            }
          g_message ("ngtcp2_conn_writev_stream: %s",
                     ngtcp2_strerror ((int)n_written));
          return -1;
        }

      if (n_written == 0)
        return 0;

      if (stream && n_read > 0)
        stream_mark_sent (stream, n_read);

      send_udp_message(connection->udp_socket, buf, n_written, (const struct sockaddr *)&connection->remote_addr);

      /* No stream data to be sent */
      if (stream && datav.len == 0)
        break;
    }

  return 0;
}

int
connection_write (Connection *connection)
{
  int ret;

  if (!connection->streams)
    {
      ret = write_to_stream (connection, NULL);
      if (ret < 0)
        return -1;
    }
  else
    for (GList *l = connection->streams; l; l = l->next)
      {
	ret = write_to_stream (connection, l->data);
	if (ret < 0)
	  return -1;
      }

  ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry (connection->conn);
  ngtcp2_tstamp now = timestamp ();
  uint64_t t = (expiry <= now) ? 0 : (expiry - now) / NGTCP2_MILLISECONDS;
  uv_timer_start(&connection->timer, timer_cb, t, 0);

  return 0;
}

void
connection_close (Connection *connection)
{
  ngtcp2_pkt_info pi;
  uint8_t buf[BUF_SIZE];

  ngtcp2_path_storage ps;
  ngtcp2_path_storage_zero(&ps);

  ngtcp2_ssize n_written;
  ngtcp2_ccerr ccerr;

  n_written = ngtcp2_conn_write_connection_close (connection->conn,
						  &ps.path, &pi,
						  buf, sizeof(buf),
						  &ccerr,
						  timestamp());
  if (n_written < 0)
    g_message ("ngtcp2_conn_write_connection_close: %s",
               ngtcp2_strerror ((int)n_written));
  else
    {
      send_udp_message(connection->udp_socket, buf, n_written, (const struct sockaddr *)&connection->remote_addr);
    }

  connection->is_closed = true;
}

bool
connection_is_closed (Connection *connection)
{
  return connection->is_closed;
}
