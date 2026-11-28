/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibWebCommon/IndexedDB/Backend.h>
#include <LibWebCommon/RustFFI.h>

namespace Web::IndexedDB {

// NB: Everything the backend holds stays in memory, so all of it together gets no more than this share of it.
static void limit_to_memory(FFI::IndexedDbBackend* backend)
{
    FFI::rust_indexed_db_backend_set_session_limit(backend, Core::System::physical_memory_bytes() / 4);
}

Backend::Backend()
    : m_backend(FFI::rust_indexed_db_backend_create())
{
    limit_to_memory(m_backend);
}

static FFI::IndexedDbPersistenceCallbacks callbacks_for(BackendPersistence& persistence)
{
    return {
        .context = &persistence,
        .load = [](void* context, u8 const* storage_key, size_t storage_key_length, void* sink, auto on_database, auto on_record) { return static_cast<BackendPersistence*>(context)->load(
                                                                                                                                        StringView { storage_key, storage_key_length },
                                                                                                                                        [&](ReadonlyBytes name, ReadonlyBytes metadata) { on_database(sink, name.data(), name.size(), metadata.data(), metadata.size()); },
                                                                                                                                        [&](ReadonlyBytes database, u64 store, ReadonlyBytes key, ReadonlyBytes value, ReadonlyBytes index_keys) {
                                                                                                                                            on_record(sink, database.data(), database.size(), store, key.data(), key.size(), value.data(), value.size(), index_keys.data(), index_keys.size());
                                                                                                                                        }); },
        .begin_write = [](void* context, u8 const* storage_key, size_t storage_key_length, u8 const* database, size_t database_length) { static_cast<BackendPersistence*>(context)->begin_write(StringView { storage_key, storage_key_length }, { database, database_length }); },
        .put_metadata = [](void* context, u8 const* metadata, size_t length) { static_cast<BackendPersistence*>(context)->put_metadata({ metadata, length }); },
        .put_record = [](void* context, u64 store, u8 const* key, size_t key_length, u8 const* value, size_t value_length, u8 const* index_keys, size_t index_keys_length) { static_cast<BackendPersistence*>(context)->put_record(store, { key, key_length }, { value, value_length }, { index_keys, index_keys_length }); },
        .delete_record = [](void* context, u64 store, u8 const* key, size_t key_length) { static_cast<BackendPersistence*>(context)->delete_record(store, { key, key_length }); },
        .delete_store = [](void* context, u64 store) { static_cast<BackendPersistence*>(context)->delete_store(store); },
        .delete_database = [](void* context) { static_cast<BackendPersistence*>(context)->delete_database(); },
        .end_write = [](void* context) { return static_cast<BackendPersistence*>(context)->end_write(); },
    };
}

Backend::Backend(BackendPersistence& persistence)
    : m_backend(FFI::rust_indexed_db_backend_create_persistent(callbacks_for(persistence)))
{
    limit_to_memory(m_backend);
}

Backend::~Backend()
{
    FFI::rust_indexed_db_backend_destroy(m_backend);
}

void Backend::connect_client(u64 client_id, StringView storage_key)
{
    FFI::rust_indexed_db_backend_connect_client(m_backend, client_id, reinterpret_cast<u8 const*>(storage_key.characters_without_null_termination()), storage_key.length());
}

void Backend::disconnect_client(u64 client_id)
{
    FFI::rust_indexed_db_backend_disconnect_client(m_backend, client_id);
}

bool Backend::handle_request(u64 client_id, ReadonlyBytes request)
{
    return FFI::rust_indexed_db_backend_handle(m_backend, client_id, request.data(), request.size());
}

Optional<ByteBuffer> Backend::inspect(u64 client_id, ReadonlyBytes inspection)
{
    Optional<ByteBuffer> result;
    auto decoded = FFI::rust_indexed_db_backend_inspect(m_backend, client_id, inspection.data(), inspection.size(), &result, [](void* context, u8 const* bytes, size_t length) {
        auto& result = *static_cast<Optional<ByteBuffer>*>(context);
        result = MUST(ByteBuffer::copy(bytes, length));
    });
    if (!decoded)
        return {};
    return result;
}

void Backend::take_events(Function<void(u64 client_id, ReadonlyBytes events)> const& callback)
{
    FFI::rust_indexed_db_backend_take_events(m_backend, const_cast<void*>(static_cast<void const*>(&callback)), [](void* context, u64 client_id, u8 const* bytes, size_t length) {
        auto const& callback = *static_cast<Function<void(u64, ReadonlyBytes)> const*>(context);
        callback(client_id, { bytes, length });
    });
}

Vector<String> Backend::storage_keys_written_since(UnixDateTime since) const
{
    Vector<String> storage_keys;
    auto since_milliseconds = static_cast<u64>(max<i64>(since.milliseconds_since_epoch(), 0));
    FFI::rust_indexed_db_backend_storage_keys_written_since(m_backend, since_milliseconds, &storage_keys, [](void* context, u8 const* bytes, size_t length) {
        static_cast<Vector<String>*>(context)->append(MUST(String::from_utf8(StringView { bytes, length })));
    });
    return storage_keys;
}

bool Backend::clear_storage_key(StringView storage_key)
{
    return FFI::rust_indexed_db_backend_clear_storage_key(m_backend, reinterpret_cast<u8 const*>(storage_key.characters_without_null_termination()), storage_key.length());
}

}
