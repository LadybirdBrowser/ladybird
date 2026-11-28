/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/Page/PageId.h>

namespace Web::IndexedDB {

// A thread's link to the process that holds the IndexedDB databases, for the agent that runs on the thread, which is
// implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/indexed_db/client.rs). WebContent connects its main thread to
// Hub, and each worker thread connects through its worker's own connection.
class WEB_API IndexedDBClient {
public:
    virtual ~IndexedDBClient() = default;

    // A client of the backend for an environment, bound to the environment's storage key. A worker's link has no
    // page, and ignores the page id.
    virtual Optional<u64> connect_to_indexed_db(PageId, HTML::EnvironmentId const&) = 0;
    virtual void indexed_db_request(u64 client_id, ReadonlyBytes request) = 0;
    virtual void disconnect_from_indexed_db(u64 client_id) = 0;

    // Asks what the developer tools show of a client's databases, and waits for the encoded answer, which is empty if
    // there's none. Only a document's link is ever asked.
    virtual ByteBuffer inspect_indexed_db(u64 client_id, ReadonlyBytes inspection) = 0;
};

// Sets the link of the calling thread.
WEB_API void set_indexed_db_client(IndexedDBClient*);

// The events the backend sent a client of the calling thread, encoded.
WEB_API void indexed_db_did_receive_events(u64 client_id, ReadonlyBytes events);

}
