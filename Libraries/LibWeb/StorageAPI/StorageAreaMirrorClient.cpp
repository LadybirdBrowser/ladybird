/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/StorageAPI/StorageAreaMirrorClient.h>

namespace Web::StorageAPI {

extern "C" {

// Defined in Bindings/Rust/src/storage_api/exports.rs.
void web_rust_storage_api_set_storage_area_mirror_client(StorageAreaMirrorClient*);
void web_rust_storage_api_storage_area_did_change(JSVM*, u64 id, bool has_key, FlatPtr key, bool has_old_value, FlatPtr old_value, bool has_new_value, FlatPtr new_value, ReadonlyBytes url);
void web_rust_storage_api_storage_area_did_acknowledge_write(u64 id);
void web_rust_storage_api_storage_area_did_refuse_write(JSVM*, u64 id, FlatPtr const* keys, FlatPtr const* values, size_t count);
void web_rust_storage_api_storage_area_did_reset(JSVM*, u64 id, FlatPtr const* keys, FlatPtr const* values, size_t count);

// Declared on the Rust side in Bindings/Rust/src/storage_api/exports.rs and storage_endpoint.rs.
void web_storage_api_storage_area_mirror_client_connect(StorageAreaMirrorClient*, u64 page_id, u8 endpoint_type, HTML::EnvironmentSettingsObject*, void* ctx, void (*on_connection)(void*, bool has_id, u64 id, FlatPtr const* keys, FlatPtr const* values, size_t count, u64 usage));
void web_storage_api_storage_area_mirror_client_disconnect(StorageAreaMirrorClient*, u64 id);
void web_storage_api_storage_area_mirror_client_set_item(StorageAreaMirrorClient*, u64 id, FlatPtr key, FlatPtr value, ReadonlyBytes document_url);
void web_storage_api_storage_area_mirror_client_remove_item(StorageAreaMirrorClient*, u64 id, FlatPtr key, ReadonlyBytes document_url);
void web_storage_api_storage_area_mirror_client_clear(StorageAreaMirrorClient*, u64 id, ReadonlyBytes document_url);
void web_storage_api_for_each_registered_endpoint(void* ctx, void (*on_endpoint)(void*, u8 identifier, bool is_session, bool has_quota, u64 quota));
}

void set_storage_area_mirror_client(StorageAreaMirrorClient* client)
{
    web_rust_storage_api_set_storage_area_mirror_client(client);
}

static FlatPtr raw_or_zero(Optional<Utf16String> const& string)
{
    if (!string.has_value())
        return 0;
    return Utf16String { *string }.into_raw();
}

static Vector<FlatPtr> into_raw(Vector<Utf16String>&& strings)
{
    Vector<FlatPtr> raws;
    raws.ensure_capacity(strings.size());
    for (auto& string : strings)
        raws.unchecked_append(move(string).into_raw());
    return raws;
}

static JSVM* vm()
{
    return JS::HostABI::vm_to_abi(Bindings::main_thread_vm());
}

void storage_area_did_change(StorageAreaId id, Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url)
{
    web_rust_storage_api_storage_area_did_change(vm(), id.value(), key.has_value(), raw_or_zero(key), old_value.has_value(), raw_or_zero(old_value), new_value.has_value(), raw_or_zero(new_value), url.bytes());
}

void storage_area_did_acknowledge_write(StorageAreaId id)
{
    web_rust_storage_api_storage_area_did_acknowledge_write(id.value());
}

void storage_area_did_refuse_write(StorageAreaId id, Vector<Utf16String> keys, Vector<Utf16String> values)
{
    VERIFY(keys.size() == values.size());
    auto raw_keys = into_raw(move(keys));
    auto raw_values = into_raw(move(values));
    web_rust_storage_api_storage_area_did_refuse_write(vm(), id.value(), raw_keys.data(), raw_values.data(), raw_keys.size());
}

void storage_area_did_reset(StorageAreaId id, Vector<Utf16String> keys, Vector<Utf16String> values)
{
    VERIFY(keys.size() == values.size());
    auto raw_keys = into_raw(move(keys));
    auto raw_values = into_raw(move(values));
    web_rust_storage_api_storage_area_did_reset(vm(), id.value(), raw_keys.data(), raw_values.data(), raw_keys.size());
}

void web_storage_api_storage_area_mirror_client_connect(StorageAreaMirrorClient* client, u64 page_id, u8 endpoint_type, HTML::EnvironmentSettingsObject* environment, void* ctx, void (*on_connection)(void*, bool has_id, u64 id, FlatPtr const* keys, FlatPtr const* values, size_t count, u64 usage))
{
    auto connection = client->connect_to_storage_area(PageId { page_id }, static_cast<StorageEndpointType>(endpoint_type), environment->id);
    // The keys and the values cross with one count.
    VERIFY(connection.keys.size() == connection.values.size());
    auto keys = into_raw(move(connection.keys));
    auto values = into_raw(move(connection.values));
    on_connection(ctx, connection.id.has_value(), connection.id.has_value() ? connection.id->value() : 0, keys.data(), values.data(), keys.size(), connection.usage);
}

void web_storage_api_storage_area_mirror_client_disconnect(StorageAreaMirrorClient* client, u64 id)
{
    client->disconnect_from_storage_area(StorageAreaId { id });
}

void web_storage_api_storage_area_mirror_client_set_item(StorageAreaMirrorClient* client, u64 id, FlatPtr key, FlatPtr value, ReadonlyBytes document_url)
{
    client->set_storage_item(StorageAreaId { id }, Utf16String::from_raw(key), Utf16String::from_raw(value), MUST(String::from_utf8(document_url)));
}

void web_storage_api_storage_area_mirror_client_remove_item(StorageAreaMirrorClient* client, u64 id, FlatPtr key, ReadonlyBytes document_url)
{
    client->remove_storage_item(StorageAreaId { id }, Utf16String::from_raw(key), MUST(String::from_utf8(document_url)));
}

void web_storage_api_storage_area_mirror_client_clear(StorageAreaMirrorClient* client, u64 id, ReadonlyBytes document_url)
{
    client->clear_storage_area(StorageAreaId { id }, MUST(String::from_utf8(document_url)));
}

void web_storage_api_for_each_registered_endpoint(void* ctx, void (*on_endpoint)(void*, u8 identifier, bool is_session, bool has_quota, u64 quota))
{
    for (auto const& endpoint : StorageEndpoint::registered_endpoints())
        on_endpoint(ctx, to_underlying(endpoint.identifier), endpoint.type == StorageType::Session, endpoint.quota.has_value(), endpoint.quota.value_or(0));
}

}
