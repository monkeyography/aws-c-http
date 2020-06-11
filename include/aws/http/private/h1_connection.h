#ifndef AWS_HTTP_H1_CONNECTION_H
#define AWS_HTTP_H1_CONNECTION_H

/*
 * Copyright 2010-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/http/private/connection_impl.h>
#include <aws/http/private/h1_encoder.h>
#include <aws/http/statistics.h>

struct h1_connection {
    struct aws_http_connection base;

    size_t initial_window_size;

    /* Single task used repeatedly for sending data from streams. */
    struct aws_channel_task outgoing_stream_task;

    /* Single task used for issuing window updates from off-thread */
    struct aws_channel_task window_update_task;

    /* Only the event-loop thread may touch this data */
    struct {
        /* List of streams being worked on. */
        struct aws_linked_list stream_list;

        /* Points to the stream whose data is currently being sent.
         * This stream is ALWAYS in the `stream_list`.
         * HTTP pipelining is supported, so once the stream is completely written
         * we'll start working on the next stream in the list */
        struct aws_h1_stream *outgoing_stream;

        /* Points to the stream being decoded.
         * This stream is ALWAYS in the `stream_list`. */
        struct aws_h1_stream *incoming_stream;
        struct aws_h1_decoder *incoming_stream_decoder;

        /* Used to encode requests and responses */
        struct aws_h1_encoder encoder;

        /* Amount to let read-window shrink after a channel message has been processed. */
        size_t incoming_message_window_shrink_size;

        /* Messages received after the connection has switched protocols.
         * These are passed downstream to the next handler. */
        struct aws_linked_list midchannel_read_messages;

        /* True when read and/or writing has stopped, whether due to errors or normal channel shutdown. */
        bool is_reading_stopped;
        bool is_writing_stopped;

        /* If true, the connection has upgraded to another protocol.
         * It will pass data to adjacent channel handlers without altering it.
         * The connection can no longer service request/response streams. */
        bool has_switched_protocols;

        /* Server-only. Request-handler streams can only be created while this is true. */
        bool can_create_request_handler_stream;

        struct aws_crt_statistics_http1_channel stats;

        uint64_t outgoing_stream_timestamp_ns;
        uint64_t incoming_stream_timestamp_ns;

    } thread_data;

    /* Any thread may touch this data, but the lock must be held */
    struct {
        struct aws_mutex lock;

        /* New client streams that have not been moved to `stream_list` yet.
         * This list is not used on servers. */
        struct aws_linked_list new_client_stream_list;

        bool is_outgoing_stream_task_active;

        /* For checking status from outside the event-loop thread. */
        bool is_open;

        /* If non-zero, reason to immediately reject new streams. (ex: closing, switched protocols) */
        int new_stream_error_code;

        /* If non-zero, then window_update_task is scheduled */
        size_t window_update_size;
    } synced_data;
};

AWS_EXTERN_C_BEGIN

AWS_HTTP_API
struct aws_http_connection *aws_http_connection_new_http1_1_server(
    struct aws_allocator *allocator,
    bool manual_window_management,
    size_t initial_window_size);

AWS_HTTP_API
struct aws_http_connection *aws_http_connection_new_http1_1_client(
    struct aws_allocator *allocator,
    bool manual_window_management,
    size_t initial_window_size);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_H1_CONNECTION_H */
