/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/IndexedDB/IndexedDBClient.h>

namespace Web::IndexedDB {

extern "C" {

// Defined in Bindings/Rust/src/indexed_db/client.rs.
void web_rust_indexed_db_set_client(IndexedDBClient*);
void web_rust_indexed_db_did_receive_events(JSVM*, u64 client_id, u8 const* bytes, size_t length);

// Declared on the Rust side in Bindings/Rust/src/indexed_db/client.rs.
bool web_indexed_db_client_connect(IndexedDBClient*, u64 page_id, HTML::EnvironmentSettingsObject*, u64* client_id);
void web_indexed_db_client_request(IndexedDBClient*, u64 client_id, u8 const* bytes, size_t length);
void web_indexed_db_client_disconnect(IndexedDBClient*, u64 client_id);
void web_indexed_db_client_inspect(IndexedDBClient*, u64 client_id, u8 const* bytes, size_t length, void* context, void (*on_result)(void*, u8 const*, size_t));
}

void set_indexed_db_client(IndexedDBClient* client)
{
    web_rust_indexed_db_set_client(client);
}

void indexed_db_did_receive_events(u64 client_id, ReadonlyBytes events)
{
    web_rust_indexed_db_did_receive_events(JS::HostABI::vm_to_abi(JS::VM::the()), client_id, events.data(), events.size());
}

bool web_indexed_db_client_connect(IndexedDBClient* client, u64 page_id, HTML::EnvironmentSettingsObject* environment, u64* client_id)
{
    auto id = client->connect_to_indexed_db(PageId { page_id }, environment->id);
    if (!id.has_value())
        return false;
    *client_id = *id;
    return true;
}

void web_indexed_db_client_request(IndexedDBClient* client, u64 client_id, u8 const* bytes, size_t length)
{
    client->indexed_db_request(client_id, { bytes, length });
}

void web_indexed_db_client_disconnect(IndexedDBClient* client, u64 client_id)
{
    client->disconnect_from_indexed_db(client_id);
}

void web_indexed_db_client_inspect(IndexedDBClient* client, u64 client_id, u8 const* bytes, size_t length, void* context, void (*on_result)(void*, u8 const*, size_t))
{
    auto result = client->inspect_indexed_db(client_id, { bytes, length });
    on_result(context, result.data(), result.size());
}

}
