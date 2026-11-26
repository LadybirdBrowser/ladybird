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

}
