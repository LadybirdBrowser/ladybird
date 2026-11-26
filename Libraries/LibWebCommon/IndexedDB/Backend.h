/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/kmalloc.h>
#include <LibWebCommon/Export.h>

namespace Web::FFI {

struct IndexedDbBackend;

}

namespace Web::IndexedDB {

// The databases of one browsing session, which Hub holds for every agent of the session: its documents in any process,
// and its workers. Requests and events go as the bytes that the agents and the backend encode.
class WEBCOMMON_API Backend {
    AK_MAKE_NONCOPYABLE(Backend);
    AK_MAKE_NONMOVABLE(Backend);

public:
    AK_ALLOC_WITH_KMALLOC;

    Backend();
    ~Backend();

    void connect_client(u64 client_id, StringView storage_key);
    void disconnect_client(u64 client_id);

    // Returns false for bytes that don't decode as a request.
    bool handle_request(u64 client_id, ReadonlyBytes request);

    // Answers what the developer tools ask of a client's databases, encoded; none for bytes that don't decode as a
    // question.
    Optional<ByteBuffer> inspect(u64 client_id, ReadonlyBytes inspection);

    // Hands over each client's events since the last call, as one encoded list per client.
    void take_events(Function<void(u64 client_id, ReadonlyBytes events)> const&);

private:
    FFI::IndexedDbBackend* m_backend { nullptr };
};

}
