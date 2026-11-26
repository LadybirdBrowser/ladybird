/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/BrowsingSession.h>
#include <LibWebView/IndexedDBRegistry.h>

namespace WebView {

IndexedDBRegistry& IndexedDBRegistry::the()
{
    static auto* registry = new IndexedDBRegistry;
    return *registry;
}

Web::IndexedDB::Backend& IndexedDBRegistry::backend_for_session(u64 session_id)
{
    return *m_backends.ensure(session_id, [] { return make<Web::IndexedDB::Backend>(); });
}

Optional<u64> IndexedDBRegistry::connect(BrowsingSession& session, String const& storage_key, IndexedDBClientOwner& owner)
{
    auto client_id = ++m_next_client_id;
    m_clients.set(client_id, { session.id(), &owner });
    backend_for_session(session.id()).connect_client(client_id, storage_key);
    return client_id;
}

bool IndexedDBRegistry::handle_request(IndexedDBClientOwner& owner, u64 client_id, ReadonlyBytes request)
{
    auto client = m_clients.get(client_id);
    // NB: A request can trail a client that its process just disconnected; only another process's client is a lie.
    if (!client.has_value())
        return true;
    if (client->owner != &owner)
        return false;
    auto& backend = backend_for_session(client->session_id);
    if (!backend.handle_request(client_id, request))
        return false;
    deliver_events(backend);
    return true;
}

Optional<ByteBuffer> IndexedDBRegistry::inspect(IndexedDBClientOwner& owner, u64 client_id, ReadonlyBytes inspection)
{
    auto client = m_clients.get(client_id);
    // NB: A client that its process just disconnected has no databases left to show.
    if (!client.has_value())
        return ByteBuffer {};
    if (client->owner != &owner)
        return {};
    auto& backend = backend_for_session(client->session_id);
    auto result = backend.inspect(client_id, inspection);
    deliver_events(backend);
    return result;
}

void IndexedDBRegistry::disconnect(IndexedDBClientOwner& owner, u64 client_id)
{
    auto client = m_clients.get(client_id);
    if (!client.has_value() || client->owner != &owner)
        return;
    // NB: The client is a reference into the table, so its backend is found before the entry goes.
    auto& backend = backend_for_session(client->session_id);
    m_clients.remove(client_id);
    backend.disconnect_client(client_id);
    deliver_events(backend);
}

void IndexedDBRegistry::remove_owner(IndexedDBClientOwner& owner)
{
    Vector<u64> client_ids;
    for (auto const& [client_id, client] : m_clients) {
        if (client.owner == &owner)
            client_ids.append(client_id);
    }
    for (auto client_id : client_ids)
        disconnect(owner, client_id);
}

void IndexedDBRegistry::remove_session(u64 session_id)
{
    m_clients.remove_all_matching([&](auto, auto const& client) { return client.session_id == session_id; });
    m_backends.remove(session_id);
}

void IndexedDBRegistry::deliver_events(Web::IndexedDB::Backend& backend)
{
    backend.take_events([&](u64 client_id, ReadonlyBytes bytes) {
        auto client = m_clients.get(client_id);
        if (!client.has_value())
            return;
        // NB: The backend handed these events over for good, so they can't be dropped: The client's requests would
        //     wait for them forever.
        client->owner->indexed_db_events(client_id, MUST(ByteBuffer::copy(bytes)));
    });
}

}
