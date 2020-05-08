/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/connection_impl.h>
#include <aws/http/private/connection_monitor.h>

#include <aws/http/private/h1_connection.h>
#include <aws/http/private/h2_connection.h>

#include <aws/http/private/proxy_impl.h>

#include <aws/common/hash_table.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/http/request_response.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/logging.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>

#if _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

static struct aws_http_connection_system_vtable s_default_system_vtable = {
    .new_socket_channel = aws_client_bootstrap_new_socket_channel,
};

static const struct aws_http_connection_system_vtable *s_system_vtable_ptr = &s_default_system_vtable;

void aws_http_connection_set_system_vtable(const struct aws_http_connection_system_vtable *system_vtable) {
    s_system_vtable_ptr = system_vtable;
}

AWS_STATIC_STRING_FROM_LITERAL(s_alpn_protocol_http_1_1, "http/1.1");
AWS_STATIC_STRING_FROM_LITERAL(s_alpn_protocol_http_2, "h2");

struct aws_http_server {
    struct aws_allocator *alloc;
    struct aws_server_bootstrap *bootstrap;
    bool is_using_tls;
    bool manual_window_management;
    size_t initial_window_size;
    void *user_data;
    aws_http_server_on_incoming_connection_fn *on_incoming_connection;
    aws_http_server_on_destroy_fn *on_destroy_complete;
    struct aws_socket *socket;

    /* Any thread may touch this data, but the lock must be held */
    struct {
        struct aws_mutex lock;
        bool is_shutting_down;
        struct aws_hash_table channel_to_connection_map;
    } synced_data;
};

static void s_server_lock_synced_data(struct aws_http_server *server) {
    int err = aws_mutex_lock(&server->synced_data.lock);
    AWS_ASSERT(!err);
    (void)err;
}

static void s_server_unlock_synced_data(struct aws_http_server *server) {
    int err = aws_mutex_unlock(&server->synced_data.lock);
    AWS_ASSERT(!err);
    (void)err;
}

/* Determine the http-version, create appropriate type of connection, and insert it into the channel. */
static struct aws_http_connection *s_connection_new(
    struct aws_allocator *alloc,
    struct aws_channel *channel,
    bool is_server,
    bool is_using_tls,
    bool manual_window_management,
    size_t initial_window_size,
    const struct aws_http2_connection_options *http2_options) {

    struct aws_channel_slot *connection_slot = NULL;
    struct aws_http_connection *connection = NULL;

    /* Create slot for connection. */
    connection_slot = aws_channel_slot_new(channel);
    if (!connection_slot) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to create slot in channel %p, error %d (%s).",
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    int err = aws_channel_slot_insert_end(channel, connection_slot);
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to insert slot into channel %p, error %d (%s).",
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto error;
    }

    /* Determine HTTP version */
    enum aws_http_version version = AWS_HTTP_VERSION_1_1;

    if (is_using_tls) {
        /* Query TLS channel handler (immediately to left in the channel) for negotiated ALPN protocol */
        if (!connection_slot->adj_left || !connection_slot->adj_left->handler) {
            aws_raise_error(AWS_ERROR_INVALID_STATE);
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION, "static: Failed to find TLS handler in channel %p.", (void *)channel);
            goto error;
        }

        struct aws_channel_slot *tls_slot = connection_slot->adj_left;
        struct aws_channel_handler *tls_handler = tls_slot->handler;
        struct aws_byte_buf protocol = aws_tls_handler_protocol(tls_handler);
        if (protocol.len) {
            if (aws_string_eq_byte_buf(s_alpn_protocol_http_1_1, &protocol)) {
                version = AWS_HTTP_VERSION_1_1;
            } else if (aws_string_eq_byte_buf(s_alpn_protocol_http_2, &protocol)) {
                version = AWS_HTTP_VERSION_2;
            } else {
                AWS_LOGF_WARN(AWS_LS_HTTP_CONNECTION, "static: Unrecognized ALPN protocol. Assuming HTTP/1.1");
                AWS_LOGF_DEBUG(
                    AWS_LS_HTTP_CONNECTION, "static: Unrecognized ALPN protocol " PRInSTR, AWS_BYTE_BUF_PRI(protocol));

                version = AWS_HTTP_VERSION_1_1;
            }
        }
    }

    /* Create connection/handler */
    switch (version) {
        case AWS_HTTP_VERSION_1_1:
            if (is_server) {
                connection =
                    aws_http_connection_new_http1_1_server(alloc, manual_window_management, initial_window_size);
            } else {
                connection =
                    aws_http_connection_new_http1_1_client(alloc, manual_window_management, initial_window_size);
            }
            break;
        case AWS_HTTP_VERSION_2:
            if (is_server) {
                connection = aws_http_connection_new_http2_server(alloc, manual_window_management, http2_options);
            } else {
                connection = aws_http_connection_new_http2_client(alloc, manual_window_management, http2_options);
            }
            break;
        default:
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION,
                "static: Unsupported version " PRInSTR,
                AWS_BYTE_CURSOR_PRI(aws_http_version_to_str(version)));

            aws_raise_error(AWS_ERROR_HTTP_UNSUPPORTED_PROTOCOL);
            goto error;
    }

    if (!connection) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to create " PRInSTR " %s connection object, error %d (%s).",
            AWS_BYTE_CURSOR_PRI(aws_http_version_to_str(version)),
            is_server ? "server" : "client",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    /* Connect handler and slot */
    err = aws_channel_slot_set_handler(connection_slot, &connection->channel_handler);
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to setting HTTP handler into slot on channel %p, error %d (%s).",
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    /* Success! Inform connection that installation is complete */
    connection->vtable->on_channel_handler_installed(&connection->channel_handler, connection_slot);

    return connection;

error:
    if (connection_slot) {
        if (!connection_slot->handler && connection) {
            aws_channel_handler_destroy(&connection->channel_handler);
        }

        aws_channel_slot_remove(connection_slot);
    }

    return NULL;
}

void aws_http_connection_close(struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    connection->vtable->close(connection);
}

bool aws_http_connection_is_open(const struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    return connection->vtable->is_open(connection);
}

bool aws_http_connection_new_requests_allowed(const struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    return connection->vtable->new_requests_allowed(connection);
}

bool aws_http_connection_is_client(const struct aws_http_connection *connection) {
    return connection->client_data;
}

bool aws_http_connection_is_server(const struct aws_http_connection *connection) {
    return connection->server_data;
}

void aws_http_connection_update_window(struct aws_http_connection *connection, size_t increment_size) {
    AWS_ASSERT(connection);
    connection->vtable->update_window(connection, increment_size);
}

static int s_check_http2_connection(const struct aws_http_connection *http2_connection) {
    if (http2_connection->http_version == AWS_HTTP_VERSION_2) {
        return AWS_OP_SUCCESS;
    } else {
        AWS_LOGF_WARN(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: HTTP/2 connection only function invoked on connection with other protocol, ignoring call.",
            (void *)http2_connection);
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
}

int aws_http2_connection_change_settings(
    struct aws_http_connection *http2_connection,
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    aws_http2_on_change_settings_complete_fn *on_completed,
    void *user_data) {
    AWS_ASSERT(http2_connection);
    AWS_PRECONDITION(http2_connection->vtable);
    if (s_check_http2_connection(http2_connection)) {
        return AWS_OP_ERR;
    }
    return http2_connection->vtable->change_settings(
        http2_connection, settings_array, num_settings, on_completed, user_data);
}

int aws_http2_connection_ping(
    struct aws_http_connection *http2_connection,
    const struct aws_byte_cursor *optional_opaque_data,
    aws_http2_on_ping_complete_fn *on_ack,
    void *user_data) {
    AWS_ASSERT(http2_connection);
    AWS_PRECONDITION(http2_connection->vtable);
    if (s_check_http2_connection(http2_connection)) {
        return AWS_OP_ERR;
    }
    return http2_connection->vtable->send_ping(http2_connection, optional_opaque_data, on_ack, user_data);
}

int aws_http2_connection_send_goaway(
    struct aws_http_connection *http2_connection,
    uint32_t http2_error,
    bool allow_more_streams,
    const struct aws_byte_cursor *optional_debug_data) {
    AWS_ASSERT(http2_connection);
    AWS_PRECONDITION(http2_connection->vtable);
    if (s_check_http2_connection(http2_connection)) {
        return AWS_OP_ERR;
    }
    return http2_connection->vtable->send_goaway(
        http2_connection, http2_error, allow_more_streams, optional_debug_data);
}

int aws_http2_connection_get_sent_goaway(
    struct aws_http_connection *http2_connection,
    uint32_t *out_http2_error,
    uint32_t *out_last_stream_id) {
    AWS_ASSERT(http2_connection);
    AWS_PRECONDITION(out_http2_error);
    AWS_PRECONDITION(out_last_stream_id);
    AWS_PRECONDITION(http2_connection->vtable);
    if (s_check_http2_connection(http2_connection)) {
        return AWS_OP_ERR;
    }
    return http2_connection->vtable->get_sent_goaway(http2_connection, out_http2_error, out_last_stream_id);
}

int aws_http2_connection_get_received_goaway(
    struct aws_http_connection *http2_connection,
    uint32_t *out_http2_error,
    uint32_t *out_last_stream_id) {
    AWS_ASSERT(http2_connection);
    AWS_PRECONDITION(out_http2_error);
    AWS_PRECONDITION(out_last_stream_id);
    AWS_PRECONDITION(http2_connection->vtable);
    if (s_check_http2_connection(http2_connection)) {
        return AWS_OP_ERR;
    }
    return http2_connection->vtable->get_received_goaway(http2_connection, out_http2_error, out_last_stream_id);
}

int aws_http2_connection_get_local_settings(
    const struct aws_http_connection *http2_connection,
    struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT]) {
    AWS_ASSERT(http2_connection);
    AWS_PRECONDITION(http2_connection->vtable);
    if (s_check_http2_connection(http2_connection)) {
        return AWS_OP_ERR;
    }
    http2_connection->vtable->get_local_settings(http2_connection, out_settings);
    return AWS_OP_SUCCESS;
}

int aws_http2_connection_get_remote_settings(
    const struct aws_http_connection *http2_connection,
    struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT]) {
    AWS_ASSERT(http2_connection);
    AWS_PRECONDITION(http2_connection->vtable);
    if (s_check_http2_connection(http2_connection)) {
        return AWS_OP_ERR;
    }
    http2_connection->vtable->get_remote_settings(http2_connection, out_settings);
    return AWS_OP_SUCCESS;
}

struct aws_channel *aws_http_connection_get_channel(struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    return connection->channel_slot->channel;
}

struct aws_host_address *aws_http_connection_get_host_address(struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    struct aws_channel *channel = aws_http_connection_get_channel(connection);

    AWS_ASSERT(channel);
    return aws_channel_get_host_address(channel);
}

void aws_http_connection_acquire(struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    aws_atomic_fetch_add(&connection->refcount, 1);
}

void aws_http_connection_release(struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    size_t prev_refcount = aws_atomic_fetch_sub(&connection->refcount, 1);
    if (prev_refcount == 1) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Final connection refcount released, shut down if necessary.",
            (void *)connection);

        /* Channel might already be shut down, but make sure */
        aws_channel_shutdown(connection->channel_slot->channel, AWS_ERROR_SUCCESS);

        /* When the channel's refcount reaches 0, it destroys its slots/handlers, which will destroy the connection */
        aws_channel_release_hold(connection->channel_slot->channel);
    } else {
        AWS_ASSERT(prev_refcount != 0);
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Connection refcount released, %zu remaining.",
            (void *)connection,
            prev_refcount - 1);
    }
}

/* At this point, the server bootstrapper has accepted an incoming connection from a client and set up a channel.
 * Now we need to create an aws_http_connection and insert it into the channel as a channel-handler.
 * Note: Be careful not to access server->socket until lock is acquired to avoid race conditions */
static void s_server_bootstrap_on_accept_channel_setup(
    struct aws_server_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    AWS_ASSERT(user_data);
    struct aws_http_server *server = user_data;
    bool user_cb_invoked = false;
    struct aws_http_connection *connection = NULL;
    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER,
            "%p: Incoming connection failed with error code %d (%s)",
            (void *)server,
            error_code,
            aws_error_name(error_code));

        goto error;
    }
    /* Create connection */
    connection = s_connection_new(
        server->alloc,
        channel,
        true,
        server->is_using_tls,
        server->manual_window_management,
        server->initial_window_size,
        NULL /*http2_connection_options*/);
    if (!connection) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER,
            "%p: Failed to create connection object, error %d (%s).",
            (void *)server,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    int put_err = 0;
    /* BEGIN CRITICAL SECTION */
    s_server_lock_synced_data(server);
    if (server->synced_data.is_shutting_down) {
        error_code = AWS_ERROR_HTTP_CONNECTION_CLOSED;
    }
    if (!error_code) {
        put_err = aws_hash_table_put(&server->synced_data.channel_to_connection_map, channel, connection, NULL);
    }
    s_server_unlock_synced_data(server);
    /* END CRITICAL SECTION */
    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_ERROR_HTTP_SERVER_CLOSED,
            "id=%p: Incoming connection failed. The server is shutting down.",
            (void *)server);
        goto error;
    }

    if (put_err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER,
            "%p: %s:%d: Failed to store connection object, error %d (%s).",
            (void *)server,
            server->socket->local_endpoint.address,
            server->socket->local_endpoint.port,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    /* Tell user of successful connection. */
    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: " PRInSTR " server connection established at %p %s:%d.",
        (void *)connection,
        AWS_BYTE_CURSOR_PRI(aws_http_version_to_str(connection->http_version)),
        (void *)server,
        server->socket->local_endpoint.address,
        server->socket->local_endpoint.port);

    server->on_incoming_connection(server, connection, AWS_ERROR_SUCCESS, server->user_data);
    user_cb_invoked = true;

    /* If user failed to configure the server during callback, shut down the channel. */
    if (!connection->server_data->on_incoming_request) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Caller failed to invoke aws_http_connection_configure_server() during on_incoming_connection "
            "callback, closing connection.",
            (void *)connection);

        aws_raise_error(AWS_ERROR_HTTP_REACTION_REQUIRED);
        goto error;
    }
    return;

error:

    if (!error_code) {
        error_code = aws_last_error();
    }

    if (!user_cb_invoked) {
        server->on_incoming_connection(server, NULL, error_code, server->user_data);
    }

    if (channel) {
        aws_channel_shutdown(channel, error_code);
    }

    if (connection) {
        /* release the ref count for the user side */
        aws_http_connection_release(connection);
    }
}

/* clean the server memory up */
static void s_http_server_clean_up(struct aws_http_server *server) {
    if (!server) {
        return;
    }
    /* invoke the user callback */
    if (server->on_destroy_complete) {
        server->on_destroy_complete(server->user_data);
    }
    aws_hash_table_clean_up(&server->synced_data.channel_to_connection_map);
    aws_mutex_clean_up(&server->synced_data.lock);
    aws_mem_release(server->alloc, server);
}

/* At this point, the channel for a server connection has completed shutdown, but hasn't been destroyed yet. */
static void s_server_bootstrap_on_accept_channel_shutdown(
    struct aws_server_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    AWS_ASSERT(user_data);
    struct aws_http_server *server = user_data;

    /* Figure out which connection this was, and remove that entry from the map.
     * It won't be in the map if something went wrong while setting up the connection. */
    struct aws_hash_element map_elem;
    int was_present;

    /* BEGIN CRITICAL SECTION */
    s_server_lock_synced_data(server);
    int remove_err =
        aws_hash_table_remove(&server->synced_data.channel_to_connection_map, channel, &map_elem, &was_present);
    s_server_unlock_synced_data(server);
    /* END CRITICAL SECTION */

    if (!remove_err && was_present) {
        struct aws_http_connection *connection = map_elem.value;
        AWS_LOGF_INFO(AWS_LS_HTTP_CONNECTION, "id=%p: Server connection shut down.", (void *)connection);
        /* Tell user about shutdown */
        if (connection->server_data->on_shutdown) {
            connection->server_data->on_shutdown(connection, error_code, connection->user_data);
        }
    }
}

/* the server listener has finished the destroy process, no existing connections
 * finally safe to clean the server up */
static void s_server_bootstrap_on_server_listener_destroy(struct aws_server_bootstrap *bootstrap, void *user_data) {
    (void)bootstrap;
    AWS_ASSERT(user_data);
    struct aws_http_server *server = user_data;
    s_http_server_clean_up(server);
}

struct aws_http_server *aws_http_server_new(const struct aws_http_server_options *options) {
    aws_http_fatal_assert_library_initialized();

    struct aws_http_server *server = NULL;

    if (!options || options->self_size == 0 || !options->allocator || !options->bootstrap || !options->socket_options ||
        !options->on_incoming_connection || !options->endpoint) {

        AWS_LOGF_ERROR(AWS_LS_HTTP_SERVER, "static: Invalid options, cannot create server.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        /* nothing to clean up */
        return NULL;
    }

    server = aws_mem_calloc(options->allocator, 1, sizeof(struct aws_http_server));
    if (!server) {
        /* nothing to clean up */
        return NULL;
    }

    server->alloc = options->allocator;
    server->bootstrap = options->bootstrap;
    server->is_using_tls = options->tls_options != NULL;
    server->initial_window_size = options->initial_window_size;
    server->user_data = options->server_user_data;
    server->on_incoming_connection = options->on_incoming_connection;
    server->on_destroy_complete = options->on_destroy_complete;
    server->manual_window_management = options->manual_window_management;

    int err = aws_mutex_init(&server->synced_data.lock);
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER, "static: Failed to initialize mutex, error %d (%s).", err, aws_error_name(err));
        goto mutex_error;
    }
    err = aws_hash_table_init(
        &server->synced_data.channel_to_connection_map, server->alloc, 16, aws_hash_ptr, aws_ptr_eq, NULL, NULL);
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER,
            "static: Cannot create server, error %d (%s).",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto hash_table_error;
    }
    /* Protect against callbacks firing before server->socket is set */
    s_server_lock_synced_data(server);
    if (options->tls_options) {
        server->is_using_tls = true;
    }

    struct aws_server_socket_channel_bootstrap_options bootstrap_options = {
        .enable_read_back_pressure = options->manual_window_management,
        .tls_options = options->tls_options,
        .bootstrap = options->bootstrap,
        .socket_options = options->socket_options,
        .incoming_callback = s_server_bootstrap_on_accept_channel_setup,
        .shutdown_callback = s_server_bootstrap_on_accept_channel_shutdown,
        .destroy_callback = s_server_bootstrap_on_server_listener_destroy,
        .host_name = options->endpoint->address,
        .port = options->endpoint->port,
        .user_data = server,
    };

    server->socket = aws_server_bootstrap_new_socket_listener(&bootstrap_options);

    s_server_unlock_synced_data(server);

    if (!server->socket) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER,
            "static: Failed creating new socket listener, error %d (%s). Cannot create server.",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto socket_error;
    }

    AWS_LOGF_INFO(
        AWS_LS_HTTP_SERVER,
        "%p %s:%d: Server setup complete, listening for incoming connections.",
        (void *)server,
        server->socket->local_endpoint.address,
        server->socket->local_endpoint.port);

    return server;

socket_error:
    aws_hash_table_clean_up(&server->synced_data.channel_to_connection_map);
hash_table_error:
    aws_mutex_clean_up(&server->synced_data.lock);
mutex_error:
    aws_mem_release(server->alloc, server);
    return NULL;
}

void aws_http_server_release(struct aws_http_server *server) {
    if (!server) {
        return;
    }
    bool already_shutting_down = false;
    /* BEGIN CRITICAL SECTION */
    s_server_lock_synced_data(server);
    if (server->synced_data.is_shutting_down) {
        already_shutting_down = true;
    } else {
        server->synced_data.is_shutting_down = true;
    }
    if (!already_shutting_down) {
        /* shutdown all existing channels */
        for (struct aws_hash_iter iter = aws_hash_iter_begin(&server->synced_data.channel_to_connection_map);
             !aws_hash_iter_done(&iter);
             aws_hash_iter_next(&iter)) {
            struct aws_channel *channel = (struct aws_channel *)iter.element.key;
            aws_channel_shutdown(channel, AWS_ERROR_HTTP_CONNECTION_CLOSED);
        }
    }
    s_server_unlock_synced_data(server);
    /* END CRITICAL SECTION */

    if (already_shutting_down) {
        /* The service is already shutting down, not shutting it down again */
        AWS_LOGF_TRACE(AWS_LS_HTTP_SERVER, "id=%p: The server is already shutting down", (void *)server);
        return;
    }

    /* stop listening, clean up the socket, after all existing connections finish shutting down, the
     * s_server_bootstrap_on_server_listener_destroy will be invoked, clean up of the server will be there */
    AWS_LOGF_INFO(
        AWS_LS_HTTP_SERVER,
        "%p %s:%d: Shutting down the server.",
        (void *)server,
        server->socket->local_endpoint.address,
        server->socket->local_endpoint.port);

    aws_server_bootstrap_destroy_socket_listener(server->bootstrap, server->socket);

    /* wait for connections to finish shutting down
     * clean up will be called from eventloop */
}

/* At this point, the channel bootstrapper has established a connection to the server and set up a channel.
 * Now we need to create the aws_http_connection and insert it into the channel as a channel-handler. */
static void s_client_bootstrap_on_channel_setup(
    struct aws_client_bootstrap *channel_bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)channel_bootstrap;
    AWS_ASSERT(user_data);
    struct aws_http_client_bootstrap *http_bootstrap = user_data;

    /* Contract for setup callbacks is: channel is NULL if error_code is non-zero. */
    AWS_FATAL_ASSERT((error_code != 0) == (channel == NULL));

    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Client connection failed with error %d (%s).",
            error_code,
            aws_error_name(error_code));

        /* Immediately tell user of failed connection.
         * No channel exists, so there will be no channel_shutdown callback. */
        http_bootstrap->on_setup(NULL, error_code, http_bootstrap->user_data);

        /* Clean up the http_bootstrap, it has no more work to do. */
        aws_mem_release(http_bootstrap->alloc, http_bootstrap);
        return;
    }

    AWS_LOGF_TRACE(AWS_LS_HTTP_CONNECTION, "static: Socket connected, creating client connection object.");

    http_bootstrap->connection = s_connection_new(
        http_bootstrap->alloc,
        channel,
        false,
        http_bootstrap->is_using_tls,
        http_bootstrap->manual_window_management,
        http_bootstrap->initial_window_size,
        &http_bootstrap->http2_options);
    if (!http_bootstrap->connection) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to create the client connection object, error %d (%s).",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    if (aws_http_connection_monitoring_options_is_valid(&http_bootstrap->monitoring_options)) {
        /*
         * On creation we validate monitoring options, if they exist, and fail if they're not
         * valid.  So at this point, is_valid() functions as an is-monitoring-on? check.  A false
         * value here is not an error, it's just not enabled.
         */
        struct aws_crt_statistics_handler *http_connection_monitor =
            aws_crt_statistics_handler_new_http_connection_monitor(
                http_bootstrap->alloc, &http_bootstrap->monitoring_options);
        if (http_connection_monitor == NULL) {
            goto error;
        }

        aws_channel_set_statistics_handler(channel, http_connection_monitor);
    }

    http_bootstrap->connection->proxy_request_transform = http_bootstrap->proxy_request_transform;
    http_bootstrap->connection->user_data = http_bootstrap->user_data;

    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: " PRInSTR " client connection established.",
        (void *)http_bootstrap->connection,
        AWS_BYTE_CURSOR_PRI(aws_http_version_to_str(http_bootstrap->connection->http_version)));

    /* Tell user of successful connection.
     * Then clear the on_setup callback so that we know it's been called */
    http_bootstrap->on_setup(http_bootstrap->connection, AWS_ERROR_SUCCESS, http_bootstrap->user_data);
    http_bootstrap->on_setup = NULL;

    return;

error:
    /* Something went wrong. Invoke channel shutdown. Then wait for channel shutdown to complete
     * before informing the user that setup failed and cleaning up the http_bootstrap.*/
    aws_channel_shutdown(channel, aws_last_error());
}

/* At this point, the channel for a client connection has completed its shutdown */
static void s_client_bootstrap_on_channel_shutdown(
    struct aws_client_bootstrap *channel_bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)channel_bootstrap;
    (void)channel;

    AWS_ASSERT(user_data);
    struct aws_http_client_bootstrap *http_bootstrap = user_data;

    /* If on_setup hasn't been called yet, inform user of failed setup.
     * If on_setup was already called, inform user that it's shut down now. */
    if (http_bootstrap->on_setup) {
        /* make super duper sure that failed setup receives a non-zero error_code */
        if (error_code == 0) {
            error_code = AWS_ERROR_UNKNOWN;
        }

        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Client setup failed with error %d (%s).",
            error_code,
            aws_error_name(error_code));

        http_bootstrap->on_setup(NULL, error_code, http_bootstrap->user_data);

    } else if (http_bootstrap->on_shutdown) {
        AWS_LOGF_INFO(
            AWS_LS_HTTP_CONNECTION,
            "%p: Client shutdown completed with error %d (%s).",
            (void *)http_bootstrap->connection,
            error_code,
            aws_error_name(error_code));

        http_bootstrap->on_shutdown(http_bootstrap->connection, error_code, http_bootstrap->user_data);
    }

    /* Clean up bootstrapper */
    aws_mem_release(http_bootstrap->alloc, http_bootstrap);
}

static int s_validate_http_client_connection_options(const struct aws_http_client_connection_options *options) {
    if (!options) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "static: http connection options are null.");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_http2_connection_options http2_options = AWS_HTTP2_CONNECTION_OPTIONS_INIT;
    if (options->http2_options) {
        http2_options = *options->http2_options;
    }

    if (options->self_size == 0) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "static: Invalid connection options, self size not initialized");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (!options->allocator) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "static: Invalid connection options, no allocator supplied");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (options->host_name.len == 0) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "static: Invalid connection options, empty host name.");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (!options->socket_options) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "static: Invalid connection options, socket options are null.");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (!options->on_setup) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "static: Invalid connection options, setup callback is null");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (http2_options.num_initial_settings && !http2_options.initial_settings_array) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Invalid connection options, h2 settings count is non-zero but settings array is null");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (options->monitoring_options && !aws_http_connection_monitoring_options_is_valid(options->monitoring_options)) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "static: Invalid connection options, invalid monitoring options");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return AWS_OP_SUCCESS;
}

int aws_http_client_connect_internal(
    const struct aws_http_client_connection_options *options,
    aws_http_proxy_request_transform_fn *proxy_request_transform) {

    struct aws_http_client_bootstrap *http_bootstrap = NULL;
    struct aws_string *host_name = NULL;
    int err = 0;

    if (s_validate_http_client_connection_options(options)) {
        goto error;
    }

    AWS_FATAL_ASSERT(options->proxy_options == NULL);

    struct aws_http2_connection_options http2_options = AWS_HTTP2_CONNECTION_OPTIONS_INIT;
    if (options->http2_options) {
        http2_options = *options->http2_options;
    }

    /* bootstrap_new() functions requires a null-terminated c-str */
    host_name = aws_string_new_from_array(options->allocator, options->host_name.ptr, options->host_name.len);
    if (!host_name) {
        goto error;
    }

    struct aws_http2_setting *setting_array = NULL;
    if (!aws_mem_acquire_many(
            options->allocator,
            2,
            &http_bootstrap,
            sizeof(struct aws_http_client_bootstrap),
            &setting_array,
            http2_options.num_initial_settings * sizeof(struct aws_http2_setting))) {
        goto error;
    }

    AWS_ZERO_STRUCT(*http_bootstrap);

    http_bootstrap->alloc = options->allocator;
    http_bootstrap->is_using_tls = options->tls_options != NULL;
    http_bootstrap->manual_window_management = options->manual_window_management;
    http_bootstrap->initial_window_size = options->initial_window_size;
    http_bootstrap->user_data = options->user_data;
    http_bootstrap->on_setup = options->on_setup;
    http_bootstrap->on_shutdown = options->on_shutdown;
    http_bootstrap->proxy_request_transform = proxy_request_transform;
    http_bootstrap->http2_options = http2_options;

    /* keep a copy of the settings array if it's not NULL */
    if (http2_options.initial_settings_array) {
        memcpy(
            setting_array,
            http2_options.initial_settings_array,
            http2_options.num_initial_settings * sizeof(struct aws_http2_setting));
        http_bootstrap->http2_options.initial_settings_array = setting_array;
    }

    if (options->monitoring_options) {
        http_bootstrap->monitoring_options = *options->monitoring_options;
    }

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION,
        "static: attempting to initialize a new client channel to %s:%d",
        aws_string_c_str(host_name),
        (int)options->port);

    struct aws_socket_channel_bootstrap_options channel_options = {
        .bootstrap = options->bootstrap,
        .host_name = aws_string_c_str(host_name),
        .port = options->port,
        .socket_options = options->socket_options,
        .tls_options = options->tls_options,
        .setup_callback = s_client_bootstrap_on_channel_setup,
        .shutdown_callback = s_client_bootstrap_on_channel_shutdown,
        .enable_read_back_pressure = options->manual_window_management,
        .user_data = http_bootstrap,
    };

    err = s_system_vtable_ptr->new_socket_channel(&channel_options);

    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to initiate socket channel for new client connection, error %d (%s).",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    aws_string_destroy(host_name);
    return AWS_OP_SUCCESS;

error:
    if (http_bootstrap) {
        aws_mem_release(http_bootstrap->alloc, http_bootstrap);
    }

    if (host_name) {
        aws_string_destroy(host_name);
    }

    return AWS_OP_ERR;
}

int aws_http_client_connect(const struct aws_http_client_connection_options *options) {
    aws_http_fatal_assert_library_initialized();

    if (options->proxy_options != NULL) {
        return aws_http_client_connect_via_proxy(options);
    } else {
        return aws_http_client_connect_internal(options, NULL);
    }
}

enum aws_http_version aws_http_connection_get_version(const struct aws_http_connection *connection) {
    return connection->http_version;
}

int aws_http_connection_configure_server(
    struct aws_http_connection *connection,
    const struct aws_http_server_connection_options *options) {

    if (!connection || !options || !options->on_incoming_request) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "id=%p: Invalid server configuration options.", (void *)connection);
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (!connection->server_data) {
        AWS_LOGF_WARN(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Server-only function invoked on client, ignoring call.",
            (void *)connection);
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
    if (connection->server_data->on_incoming_request) {
        AWS_LOGF_WARN(
            AWS_LS_HTTP_CONNECTION, "id=%p: Connection is already configured, ignoring call.", (void *)connection);
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    connection->user_data = options->connection_user_data;
    connection->server_data->on_incoming_request = options->on_incoming_request;
    connection->server_data->on_shutdown = options->on_shutdown;

    return AWS_OP_SUCCESS;
}

/* Stream IDs are only 31 bits [5.1.1] */
static const uint32_t MAX_STREAM_ID = UINT32_MAX >> 1;

uint32_t aws_http_connection_get_next_stream_id(struct aws_http_connection *connection) {

    uint32_t next_id = connection->next_stream_id;

    if (AWS_UNLIKELY(next_id > MAX_STREAM_ID)) {
        AWS_LOGF_INFO(AWS_LS_HTTP_CONNECTION, "id=%p: All available stream ids are gone", (void *)connection);

        next_id = 0;
        aws_raise_error(AWS_ERROR_HTTP_STREAM_IDS_EXHAUSTED);
    } else {
        connection->next_stream_id += 2;
    }

    return next_id;
}
