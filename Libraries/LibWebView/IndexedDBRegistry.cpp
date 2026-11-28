/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/BrowsingSession.h>
#include <LibWebView/IndexedDBRegistry.h>
#include <LibWebView/IndexedDBStore.h>

namespace WebView {

IndexedDBRegistry& IndexedDBRegistry::the()
{
    static auto* registry = new IndexedDBRegistry;
    return *registry;
}

Web::IndexedDB::Backend& IndexedDBRegistry::backend_for_session(BrowsingSession& session)
{
    return *m_backends.ensure(session.id(), [&] {
        if (session.indexed_db_store)
            return make<Web::IndexedDB::Backend>(*session.indexed_db_store);
        return make<Web::IndexedDB::Backend>();
    });
}

// NB: A client's session made its backend when the client connected, and both go away together.
Web::IndexedDB::Backend& IndexedDBRegistry::backend_of_client(Client const& client)
{
    return *m_backends.get(client.session_id).value();
}

Optional<u64> IndexedDBRegistry::connect(BrowsingSession& session, String const& storage_key, IndexedDBClientOwner& owner)
{
    auto client_id = ++m_next_client_id;
    m_clients.set(client_id, { session.id(), &owner });
    backend_for_session(session).connect_client(client_id, storage_key);
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
    auto& backend = backend_of_client(*client);
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
    auto& backend = backend_of_client(*client);
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
    auto& backend = backend_of_client(*client);
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

void IndexedDBRegistry::clear_site_data_since(BrowsingSession& session, UnixDateTime since)
{
    Vector<String> storage_keys;
    if (session.indexed_db_store)
        storage_keys = session.indexed_db_store->storage_keys_written_since(since);

    // NB: A session that no agent used IndexedDB in yet has no backend, and only rows from earlier runs.
    Vector<String> kept;
    if (auto backend = m_backends.get(session.id()); backend.has_value()) {
        for (auto& storage_key : (*backend)->storage_keys_written_since(since)) {
            if (!storage_keys.contains_slow(storage_key))
                storage_keys.append(move(storage_key));
        }
        for (auto const& storage_key : storage_keys) {
            if (!(*backend)->clear_storage_key(storage_key)) {
                warnln("Unable to delete the IndexedDB databases of {} from the profile; they stay", storage_key);
                kept.append(storage_key);
            }
        }
        deliver_events(**backend);
    }

    // NB: The backend deleted the rows of the databases it loaded; this deletes those of the storage keys it didn't.
    //     A storage key whose databases the backend kept, as their rows couldn't be deleted, keeps its rows too, so
    //     the profile still matches what the backend holds.
    if (session.indexed_db_store) {
        for (auto const& storage_key : storage_keys) {
            if (!kept.contains_slow(storage_key))
                session.indexed_db_store->delete_storage_key(storage_key);
        }
    }
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
