/* SPDX-License-Identifier: MIT */

#include "config.h"

#include "connection.h"
#include "plaintext.h"
#include "utils.h"

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <glib.h>
#include <ngtcp2/ngtcp2.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SIZE 12800
#define MAX_STREAMS 10

typedef struct _Client
{
  uv_loop_t* loop;
  uv_udp_t udp_recv_socket;
  Connection *connection;
  Stream *streams[MAX_STREAMS]; /* owned by connection */
  size_t n_streams;             /* how many streams we open */
  size_t stream_index;          /* the current stream index */
  size_t n_coalescing;          /* how many lines are coalesced into a
                                   single packet */
  size_t coalesce_count;        /* the number of lines currently coalesced */
} Client;

static void
client_deinit (Client *client)
{
  connection_free (client->connection);
}

int rand_bytes(uint8_t *data, size_t len)
{
    static int for_srand = 0;
    if (!for_srand) {
        srand(timestamp());
        for_srand = 1;
    }

    for (size_t i = 0; i < len; ++i)
        data[i] = (uint8_t)rand();

    return 1;
}

static void
rand_cb (uint8_t *dest, size_t destlen,
	 const ngtcp2_rand_ctx *rand_ctx __attribute__((unused)))
{
    size_t i;
    for (i = 0; i < destlen; ++i) {
        *dest = (uint8_t) rand();
    }
}

static int
get_new_connection_id_cb (ngtcp2_conn *conn __attribute__((unused)),
			  ngtcp2_cid *cid, uint8_t *token,
                          size_t cidlen,
			  void *user_data __attribute__((unused)))
{
    if (rand_bytes(cid->data, (int) cidlen) != 1)
        return NGTCP2_ERR_CALLBACK_FAILURE;

    cid->datalen = cidlen;

    if (rand_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1)
        return NGTCP2_ERR_CALLBACK_FAILURE;

    return 0;
}

static int
acked_stream_data_offset_cb (ngtcp2_conn *conn __attribute__((unused)),
			     int64_t stream_id,
                             uint64_t offset, uint64_t datalen,
                             void *user_data,
			     void *stream_user_data __attribute__((unused)))
{
  Connection *connection = user_data;
  Stream *stream = connection_find_stream (connection, stream_id);
  if (stream)
    stream_mark_acked (stream, offset + datalen);
  return 0;
}

static int
recv_stream_data_cb (ngtcp2_conn *conn __attribute__((unused)),
		     uint32_t flags __attribute__((unused)),
		     int64_t stream_id,
                     uint64_t offset __attribute__((unused)),
		     const uint8_t *data, size_t datalen,
                     void *user_data __attribute__((unused)),
		     void *stream_user_data __attribute__((unused)))
{
  g_debug ("receiving %zu bytes from stream #%zd", datalen, stream_id);
  write (STDOUT_FILENO, data, datalen);
  return 0;
}

static const ngtcp2_callbacks callbacks =
  {
    /* Use the default implementation from ngtcp2_crypto */
    .client_initial = client_initial,
    .recv_crypto_data = recv_crypto_data_client,
    .encrypt = null_encrypt,
    .decrypt = null_decrypt,
    .hp_mask = null_hp_mask,
    .recv_retry = recv_retry,
    .update_key = update_key,
    .delete_crypto_aead_ctx = delete_crypto_aead_ctx,
    .delete_crypto_cipher_ctx = delete_crypto_cipher_ctx,
    .get_path_challenge_data = get_path_challenge_data,

    .acked_stream_data_offset = acked_stream_data_offset_cb,
    .recv_stream_data = recv_stream_data_cb,
    .rand = rand_cb,
    .get_new_connection_id = get_new_connection_id_cb,
  };

static gint n_streams = 1;
static gint n_coalescing = 1;

static GOptionEntry entries[] =
  {
    { "streams", 's', 0, G_OPTION_ARG_INT, &n_streams,
      "Open M streams", "M" },
    { "coalescing", 'c', 0, G_OPTION_ARG_INT, &n_coalescing,
      "Coalesce input lines per N", "N" },
    { NULL }
  };

void udp_recv_cb(uv_udp_t *req,
                 ssize_t nread,
                 const uv_buf_t *buf,
                 const struct sockaddr *addr,
                 unsigned flags) {
  if (nread < 0) {
    fprintf(stderr, "Read error: %s\n", uv_strerror(nread));
    uv_close((uv_handle_t *)req, NULL);
    free(buf->base);
    return;
  }
  if (nread > 0) {
    Client *client = req->data;
    Connection *connection = client->connection;
    size_t remote_addrlen = (addr->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

    ngtcp2_path path;
    ngtcp2_conn *conn = connection_get_ngtcp2_conn(connection);
    memcpy(&path, ngtcp2_conn_get_path(conn), sizeof(path));
    path.remote.addrlen = remote_addrlen;
    path.remote.addr = (struct sockaddr *)addr;

    ngtcp2_pkt_info pi;
    memset(&pi, 0, sizeof(pi));

    int ret = ngtcp2_conn_read_pkt(conn, &path, &pi, (const uint8_t *)buf->base, nread,
                               timestamp());
    if (ret < 0) {
      g_message("ngtcp2_conn_read_pkt: %s", ngtcp2_strerror(ret));
      goto out;
    }
    connection_start(connection);
  }
out:
  free(buf->base);
}

void on_tty_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread > 0) {
        Client *client = stream->data;
        int ret = 0;

        if (!client->streams[client->stream_index]) {
          ngtcp2_conn *conn = connection_get_ngtcp2_conn(client->connection);
          if (!ngtcp2_conn_get_streams_bidi_left(conn)) {
            g_info("no available bidi streams; skipping");
            goto out;
          }

          int64_t stream_id;

          ret = ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
          if (ret < 0) {
            g_message("ngtcp2_conn_open_bidi_stream: %s", ngtcp2_strerror(ret));
            goto out;
          }

          __attribute__((cleanup(stream_freep))) Stream *stream = NULL;

          stream = stream_new(stream_id);
          if (!stream)
            goto out;

          client->streams[client->stream_index] = g_steal_pointer(&stream);
          connection_add_stream(client->connection,
                                client->streams[client->stream_index]);

          g_debug("opened stream #%zd", stream_id);
        }

        if (client->streams[client->stream_index]) {
          ret = stream_push_data(client->streams[client->stream_index], (const uint8_t *)buf->base,
                                 nread);
          if (ret < 0)
            goto out;

          g_debug("buffered %zd bytes", nread);

          if (++client->coalesce_count < client->n_coalescing)
            goto out;
        }

        ret = connection_write(client->connection);
        if (ret < 0)
          goto out;

        client->stream_index++;
        client->stream_index %= client->n_streams;
        client->coalesce_count = 0;
    } else {
        uv_read_stop(stream);
    }
out:
    free(buf->base);
}


int
main (int argc, char **argv)
{
  __attribute__((cleanup(client_deinit))) Client client =
    {
      .connection = NULL,
      .streams = { NULL, },
      .n_streams = 0,
      .stream_index = 0,
      .n_coalescing = 0,
      .coalesce_count = 0,
      .loop = uv_default_loop(),
    };
  int ret;

  g_set_prgname ("cli");

  g_autoptr(GOptionContext) context = NULL;
  context = g_option_context_new ("HOST PORT CA-CERTS - QUIC echo client");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);

  g_autoptr(GError) err = NULL;
  if (!g_option_context_parse (context, &argc, &argv, &err))
    {
      g_printerr ("option parsing failed: %s\n", err->message);
      return EXIT_FAILURE;
    }

  if (argc != 4)
    {
      g_autofree gchar *help =
        g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);
      return EXIT_FAILURE;
    }

  /* Create a client socket */
  struct sockaddr_storage local_addr, remote_addr;
  size_t local_addrlen = sizeof(local_addr), remote_addrlen;

  if (!resolve_and_connect (client.loop, argv[1], argv[2],
    &client.udp_recv_socket, udp_recv_cb,
    (struct sockaddr *)&local_addr,
    &local_addrlen,
    (struct sockaddr *)&remote_addr,
    &remote_addrlen)) {
    g_error ("resolve_and_connect failed\n");
    return -errno;
  }
  
  client.udp_recv_socket.data = &client;

  /* Create an ngtcp2 client connection */
  ngtcp2_path path =
    {
      .local = {
        .addrlen = local_addrlen,
        .addr = (struct sockaddr *)&local_addr
      },
      .remote = {
        .addrlen = remote_addrlen,
        .addr = (struct sockaddr *)&remote_addr
      }
    };

  ngtcp2_settings settings;
  ngtcp2_settings_default (&settings);
  settings.initial_ts = timestamp ();
  settings.log_printf = log_printf;
  uint16_t pmtud_probes[] = {1300, 1400, 1500};
  settings.pmtud_probes = pmtud_probes;
  settings.pmtud_probeslen = 3;
  settings.max_tx_udp_payload_size = 1500;

  ngtcp2_transport_params params;
  ngtcp2_transport_params_default (&params);
  params.initial_max_streams_uni = 3;
  params.initial_max_stream_data_bidi_local = 128 * 1024;
  params.initial_max_data = 1024 * 1024;

  ngtcp2_cid scid, dcid;
  if (get_random_cid (&scid) < 0 || get_random_cid (&dcid) < 0) {
    g_error ("get_random_cid failed\n");
    return -EINVAL;
  }

  __attribute__((cleanup(connection_freep))) Connection *connection = NULL;

  connection = connection_new (client.loop, NULL, &client.udp_recv_socket);
  if (!connection) {
    g_error ("connection_new failed\n");
    return -EINVAL;
  }

  __attribute__((cleanup(ngtcp2_conn_delp))) ngtcp2_conn *conn = NULL;

  ret = ngtcp2_conn_client_new (&conn, &dcid, &scid, &path,
                                NGTCP2_PROTO_VER_V1,
                                &callbacks, &settings, &params, NULL,
                                connection);
  if (ret < 0){
    g_error ("ngtcp2_conn_client_new: %s\n",
           ngtcp2_strerror (ret));
    return -EINVAL;
  }

  ngtcp2_conn_set_keep_alive_timeout(conn, NGTCP2_SECONDS * 30);

  connection_set_ngtcp2_conn (connection, g_steal_pointer (&conn));
  connection_set_local_addr (connection,
                             (struct sockaddr *)&local_addr, local_addrlen);
  connection_set_remote_addr (connection,
                             (struct sockaddr *)&remote_addr, remote_addrlen);

  ret = connection_start (connection);
  if (ret < 0) {
    g_error("connection_start failed\n");
    return -EINVAL;
  }

  client.connection = g_steal_pointer (&connection);
  client.n_streams = n_streams;
  client.n_coalescing = n_coalescing;
  client.coalesce_count = 0;

  uv_tty_t tty;
  uv_tty_init(client.loop, &tty, STDIN_FILENO, 1);
  tty.data = &client;
  uv_read_start((uv_stream_t*)&tty, alloc_buffer, on_tty_read);

  uv_run(client.loop, UV_RUN_DEFAULT);
  return uv_loop_close(client.loop);
}
