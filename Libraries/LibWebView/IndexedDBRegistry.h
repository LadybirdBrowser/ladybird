/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibWebCommon/IndexedDB/Backend.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// A process that runs agents which use IndexedDB: a WebContent process for its documents, or a worker's connection for
// the worker. The events of each of its clients reach it here.
class WEBVIEW_API IndexedDBClientOwner {
public:
    virtual ~IndexedDBClientOwner() = default;

    virtual void indexed_db_events(u64 client_id, ByteBuffer events) = 0;
};

// The IndexedDB backends of the browsing sessions, one each, and the clients of every process that uses them. Every
// agent of a session reaches the same databases through its client, whichever process or thread it runs in. A client
// is bound to the storage key that this process derived for the agent's environment, so an agent never names one.
class WEBVIEW_API IndexedDBRegistry {
public:
    AK_ALLOC_WITH_KMALLOC;

    static IndexedDBRegistry& the();

    Optional<u64> connect(BrowsingSession&, String const& storage_key, IndexedDBClientOwner&);

    // Returns false for a request that doesn't decode, which marks the process that sent it as misbehaving.
    bool handle_request(IndexedDBClientOwner&, u64 client_id, ReadonlyBytes request);

    // Answers what the developer tools ask of a client's databases. Returns none for a question that is for another
    // process's client, or doesn't decode.
    Optional<ByteBuffer> inspect(IndexedDBClientOwner&, u64 client_id, ReadonlyBytes inspection);

    void disconnect(IndexedDBClientOwner&, u64 client_id);
    void remove_owner(IndexedDBClientOwner&);
    void remove_session(u64 session_id);

private:
    IndexedDBRegistry() = default;

    struct Client {
        u64 session_id { 0 };
        IndexedDBClientOwner* owner { nullptr };
    };

    Web::IndexedDB::Backend& backend_for_session(u64 session_id);
    void deliver_events(Web::IndexedDB::Backend&);

    HashMap<u64, NonnullOwnPtr<Web::IndexedDB::Backend>> m_backends;
    HashMap<u64, Client> m_clients;
    u64 m_next_client_id { 0 };
};

}
