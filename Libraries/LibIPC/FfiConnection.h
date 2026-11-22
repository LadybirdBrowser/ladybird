/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibIPC/RawAttachment.h>
#include <LibIPC/RawMessage.h>

namespace IPC {

struct FfiConnection {
    void* connection_with_one_reference;
    bool (*send_sync_request)(void* connection, ReadonlyBytes, ReadonlySpan<RawAttachment>, u32 response_endpoint_magic, i32 response_message_id, void* response_sink, void (*take_response)(void* response_sink, FfiRawMessage));
    void (*ref)(void* connection);
    void (*unref)(void* connection);
};
static_assert(sizeof(FfiConnection) == 4 * sizeof(void*), "FfiConnection of libipc_rust is written to match this by hand");

}
