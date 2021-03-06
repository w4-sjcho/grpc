/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <assert.h>
#include <string.h>

#include <grpc/compression.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/slice_buffer.h>

#include "src/core/channel/compress_filter.h"
#include "src/core/channel/channel_args.h"
#include "src/core/profiling/timers.h"
#include "src/core/compression/message_compress.h"
#include "src/core/support/string.h"

typedef struct call_data {
  gpr_slice_buffer slices; /**< Buffers up input slices to be compressed */
  grpc_linked_mdelem compression_algorithm_storage;
  grpc_linked_mdelem accept_encoding_storage;
  gpr_uint32 remaining_slice_bytes;
  /** Compression algorithm we'll try to use. It may be given by incoming
   * metadata, or by the channel's default compression settings. */
  grpc_compression_algorithm compression_algorithm;
  /** If true, contents of \a compression_algorithm are authoritative */
  int has_compression_algorithm;

  grpc_transport_stream_op send_op;
  gpr_uint32 send_length;
  gpr_uint32 send_flags;
  gpr_slice incoming_slice;
  grpc_slice_buffer_stream replacement_stream;
  grpc_closure *post_send;
  grpc_closure send_done;
  grpc_closure got_slice;
} call_data;

typedef struct channel_data {
  /** Metadata key for the incoming (requested) compression algorithm */
  grpc_mdstr *mdstr_request_compression_algorithm_key;
  /** Metadata key for the outgoing (used) compression algorithm */
  grpc_mdstr *mdstr_outgoing_compression_algorithm_key;
  /** Metadata key for the accepted encodings */
  grpc_mdstr *mdstr_compression_capabilities_key;
  /** Precomputed metadata elements for all available compression algorithms */
  grpc_mdelem *mdelem_compression_algorithms[GRPC_COMPRESS_ALGORITHMS_COUNT];
  /** Precomputed metadata elements for the accepted encodings */
  grpc_mdelem *mdelem_accept_encoding;
  /** The default, channel-level, compression algorithm */
  grpc_compression_algorithm default_compression_algorithm;
  /** Compression options for the channel */
  grpc_compression_options compression_options;
} channel_data;

/** For each \a md element from the incoming metadata, filter out the entry for
 * "grpc-encoding", using its value to populate the call data's
 * compression_algorithm field. */
static grpc_mdelem *compression_md_filter(void *user_data, grpc_mdelem *md) {
  grpc_call_element *elem = user_data;
  call_data *calld = elem->call_data;
  channel_data *channeld = elem->channel_data;

  if (md->key == channeld->mdstr_request_compression_algorithm_key) {
    const char *md_c_str = grpc_mdstr_as_c_string(md->value);
    if (!grpc_compression_algorithm_parse(md_c_str, strlen(md_c_str),
                                          &calld->compression_algorithm)) {
      gpr_log(GPR_ERROR,
              "Invalid compression algorithm: '%s' (unknown). Ignoring.",
              md_c_str);
      calld->compression_algorithm = GRPC_COMPRESS_NONE;
    }
    if (grpc_compression_options_is_algorithm_enabled(
            &channeld->compression_options, calld->compression_algorithm) ==
        0) {
      gpr_log(GPR_ERROR,
              "Invalid compression algorithm: '%s' (previously disabled). "
              "Ignoring.",
              md_c_str);
      calld->compression_algorithm = GRPC_COMPRESS_NONE;
    }
    calld->has_compression_algorithm = 1;
    return NULL;
  }

  return md;
}

static int skip_compression(grpc_call_element *elem) {
  call_data *calld = elem->call_data;
  channel_data *channeld = elem->channel_data;
  if (calld->has_compression_algorithm) {
    if (calld->compression_algorithm == GRPC_COMPRESS_NONE) {
      return 1;
    }
    return 0; /* we have an actual call-specific algorithm */
  }
  /* no per-call compression override */
  return channeld->default_compression_algorithm == GRPC_COMPRESS_NONE;
}

/** Filter initial metadata */
static void process_send_initial_metadata(
    grpc_call_element *elem, grpc_metadata_batch *initial_metadata) {
  call_data *calld = elem->call_data;
  channel_data *channeld = elem->channel_data;
  /* Parse incoming request for compression. If any, it'll be available
   * at calld->compression_algorithm */
  grpc_metadata_batch_filter(initial_metadata, compression_md_filter, elem);
  if (!calld->has_compression_algorithm) {
    /* If no algorithm was found in the metadata and we aren't
     * exceptionally skipping compression, fall back to the channel
     * default */
    calld->compression_algorithm = channeld->default_compression_algorithm;
    calld->has_compression_algorithm = 1; /* GPR_TRUE */
  }
  /* hint compression algorithm */
  grpc_metadata_batch_add_tail(
      initial_metadata, &calld->compression_algorithm_storage,
      GRPC_MDELEM_REF(
          channeld
              ->mdelem_compression_algorithms[calld->compression_algorithm]));

  /* convey supported compression algorithms */
  grpc_metadata_batch_add_tail(
      initial_metadata, &calld->accept_encoding_storage,
      GRPC_MDELEM_REF(channeld->mdelem_accept_encoding));
}

static void continue_send_message(grpc_exec_ctx *exec_ctx,
                                  grpc_call_element *elem);

static void send_done(grpc_exec_ctx *exec_ctx, void *elemp, int success) {
  grpc_call_element *elem = elemp;
  call_data *calld = elem->call_data;
  gpr_slice_buffer_reset_and_unref(&calld->slices);
  calld->post_send->cb(exec_ctx, calld->post_send->cb_arg, success);
}

static void finish_send_message(grpc_exec_ctx *exec_ctx,
                                grpc_call_element *elem) {
  call_data *calld = elem->call_data;
  int did_compress;
  gpr_slice_buffer tmp;
  gpr_slice_buffer_init(&tmp);
  did_compress =
      grpc_msg_compress(calld->compression_algorithm, &calld->slices, &tmp);
  if (did_compress) {
    gpr_slice_buffer_swap(&calld->slices, &tmp);
    calld->send_flags |= GRPC_WRITE_INTERNAL_COMPRESS;
  }
  gpr_slice_buffer_destroy(&tmp);

  grpc_slice_buffer_stream_init(&calld->replacement_stream, &calld->slices,
                                calld->send_flags);
  calld->send_op.send_message = &calld->replacement_stream.base;
  calld->post_send = calld->send_op.on_complete;
  calld->send_op.on_complete = &calld->send_done;

  grpc_call_next_op(exec_ctx, elem, &calld->send_op);
}

static void got_slice(grpc_exec_ctx *exec_ctx, void *elemp, int success) {
  grpc_call_element *elem = elemp;
  call_data *calld = elem->call_data;
  gpr_slice_buffer_add(&calld->slices, calld->incoming_slice);
  if (calld->send_length == calld->slices.length) {
    finish_send_message(exec_ctx, elem);
  } else {
    continue_send_message(exec_ctx, elem);
  }
}

static void continue_send_message(grpc_exec_ctx *exec_ctx,
                                  grpc_call_element *elem) {
  call_data *calld = elem->call_data;
  while (grpc_byte_stream_next(exec_ctx, calld->send_op.send_message,
                               &calld->incoming_slice, ~(size_t)0,
                               &calld->got_slice)) {
    gpr_slice_buffer_add(&calld->slices, calld->incoming_slice);
    if (calld->send_length == calld->slices.length) {
      finish_send_message(exec_ctx, elem);
      break;
    }
  }
}

static void compress_start_transport_stream_op(grpc_exec_ctx *exec_ctx,
                                               grpc_call_element *elem,
                                               grpc_transport_stream_op *op) {
  call_data *calld = elem->call_data;

  GPR_TIMER_BEGIN("compress_start_transport_stream_op", 0);

  if (op->send_initial_metadata) {
    process_send_initial_metadata(elem, op->send_initial_metadata);
  }
  if (op->send_message != NULL && !skip_compression(elem) &&
      0 == (op->send_message->flags & GRPC_WRITE_NO_COMPRESS)) {
    calld->send_op = *op;
    calld->send_length = op->send_message->length;
    calld->send_flags = op->send_message->flags;
    continue_send_message(exec_ctx, elem);
  } else {
    /* pass control down the stack */
    grpc_call_next_op(exec_ctx, elem, op);
  }

  GPR_TIMER_END("compress_start_transport_stream_op", 0);
}

/* Constructor for call_data */
static void init_call_elem(grpc_exec_ctx *exec_ctx, grpc_call_element *elem,
                           grpc_call_element_args *args) {
  /* grab pointers to our data from the call element */
  call_data *calld = elem->call_data;

  /* initialize members */
  gpr_slice_buffer_init(&calld->slices);
  calld->has_compression_algorithm = 0;
  grpc_closure_init(&calld->got_slice, got_slice, elem);
  grpc_closure_init(&calld->send_done, send_done, elem);
}

/* Destructor for call_data */
static void destroy_call_elem(grpc_exec_ctx *exec_ctx,
                              grpc_call_element *elem) {
  /* grab pointers to our data from the call element */
  call_data *calld = elem->call_data;
  gpr_slice_buffer_destroy(&calld->slices);
}

/* Constructor for channel_data */
static void init_channel_elem(grpc_exec_ctx *exec_ctx,
                              grpc_channel_element *elem,
                              grpc_channel_element_args *args) {
  channel_data *channeld = elem->channel_data;
  grpc_compression_algorithm algo_idx;
  const char *supported_algorithms_names[GRPC_COMPRESS_ALGORITHMS_COUNT - 1];
  size_t supported_algorithms_idx = 0;
  char *accept_encoding_str;
  size_t accept_encoding_str_len;

  grpc_compression_options_init(&channeld->compression_options);
  channeld->compression_options.enabled_algorithms_bitset =
      (gpr_uint32)grpc_channel_args_compression_algorithm_get_states(
          args->channel_args);

  channeld->default_compression_algorithm =
      grpc_channel_args_get_compression_algorithm(args->channel_args);
  /* Make sure the default isn't disabled. */
  GPR_ASSERT(grpc_compression_options_is_algorithm_enabled(
      &channeld->compression_options, channeld->default_compression_algorithm));
  channeld->compression_options.default_compression_algorithm =
      channeld->default_compression_algorithm;

  channeld->mdstr_request_compression_algorithm_key = grpc_mdstr_from_string(
      args->metadata_context, GRPC_COMPRESS_REQUEST_ALGORITHM_KEY);

  channeld->mdstr_outgoing_compression_algorithm_key =
      grpc_mdstr_from_string(args->metadata_context, "grpc-encoding");

  channeld->mdstr_compression_capabilities_key =
      grpc_mdstr_from_string(args->metadata_context, "grpc-accept-encoding");

  for (algo_idx = 0; algo_idx < GRPC_COMPRESS_ALGORITHMS_COUNT; ++algo_idx) {
    char *algorithm_name;
    /* skip disabled algorithms */
    if (grpc_compression_options_is_algorithm_enabled(
            &channeld->compression_options, algo_idx) == 0) {
      continue;
    }
    GPR_ASSERT(grpc_compression_algorithm_name(algo_idx, &algorithm_name) != 0);
    channeld->mdelem_compression_algorithms[algo_idx] =
        grpc_mdelem_from_metadata_strings(
            args->metadata_context,
            GRPC_MDSTR_REF(channeld->mdstr_outgoing_compression_algorithm_key),
            grpc_mdstr_from_string(args->metadata_context, algorithm_name));
    if (algo_idx > 0) {
      supported_algorithms_names[supported_algorithms_idx++] = algorithm_name;
    }
  }

  /* TODO(dgq): gpr_strjoin_sep could be made to work with statically allocated
   * arrays, as to avoid the heap allocs */
  accept_encoding_str =
      gpr_strjoin_sep(supported_algorithms_names, supported_algorithms_idx, ",",
                      &accept_encoding_str_len);

  channeld->mdelem_accept_encoding = grpc_mdelem_from_metadata_strings(
      args->metadata_context,
      GRPC_MDSTR_REF(channeld->mdstr_compression_capabilities_key),
      grpc_mdstr_from_string(args->metadata_context, accept_encoding_str));
  gpr_free(accept_encoding_str);

  GPR_ASSERT(!args->is_last);
}

/* Destructor for channel data */
static void destroy_channel_elem(grpc_exec_ctx *exec_ctx,
                                 grpc_channel_element *elem) {
  channel_data *channeld = elem->channel_data;
  grpc_compression_algorithm algo_idx;

  GRPC_MDSTR_UNREF(channeld->mdstr_request_compression_algorithm_key);
  GRPC_MDSTR_UNREF(channeld->mdstr_outgoing_compression_algorithm_key);
  GRPC_MDSTR_UNREF(channeld->mdstr_compression_capabilities_key);
  for (algo_idx = 0; algo_idx < GRPC_COMPRESS_ALGORITHMS_COUNT; ++algo_idx) {
    GRPC_MDELEM_UNREF(channeld->mdelem_compression_algorithms[algo_idx]);
  }
  GRPC_MDELEM_UNREF(channeld->mdelem_accept_encoding);
}

const grpc_channel_filter grpc_compress_filter = {
    compress_start_transport_stream_op, grpc_channel_next_op, sizeof(call_data),
    init_call_elem, grpc_call_stack_ignore_set_pollset, destroy_call_elem,
    sizeof(channel_data), init_channel_elem, destroy_channel_elem,
    grpc_call_next_get_peer, "compress"};
