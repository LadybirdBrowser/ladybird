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
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWebCommon/Export.h>

namespace Web::FFI {

struct IndexedDbBackend;

}

namespace Web::IndexedDB {

// Where a backend keeps its databases between runs, as rows of bytes that only the backend reads: a row for each
// database's shape, and one for each record. A backend loads a storage key's rows when its first client for the key
// connects, and writes each committed change between begin_write() and end_write(), which applies it all at once.
class WEBCOMMON_API BackendPersistence {
public:
    virtual ~BackendPersistence() = default;

    using OnDatabase = Function<void(ReadonlyBytes name, ReadonlyBytes metadata)>;
    using OnRecord = Function<void(ReadonlyBytes database, u64 store, ReadonlyBytes key, ReadonlyBytes value, ReadonlyBytes index_keys)>;
    // Returns whether every row could be read; none is handed over if not.
    virtual bool load(StringView storage_key, OnDatabase const&, OnRecord const&) = 0;

    virtual void begin_write(StringView storage_key, ReadonlyBytes database) = 0;
    virtual void put_metadata(ReadonlyBytes metadata) = 0;
    virtual void put_record(u64 store, ReadonlyBytes key, ReadonlyBytes value, ReadonlyBytes index_keys) = 0;
    virtual void delete_record(u64 store, ReadonlyBytes key) = 0;
    virtual void delete_store(u64 store) = 0;
    virtual void delete_database() = 0;
    // Returns whether the changes were made.
    virtual bool end_write() = 0;
};

// The databases of one browsing session, which Hub holds for every agent of the session: its documents in any process,
// and its workers. Requests and events go as the bytes that the agents and the backend encode.
class WEBCOMMON_API Backend {
    AK_MAKE_NONCOPYABLE(Backend);
    AK_MAKE_NONMOVABLE(Backend);

public:
    AK_ALLOC_WITH_KMALLOC;

    Backend();

    // A backend that keeps its databases between runs in the persistence, which must outlive it.
    explicit Backend(BackendPersistence&);

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

    // The storage keys whose databases changed in this run at or after the time.
    Vector<String> storage_keys_written_since(UnixDateTime) const;

    // Deletes every database of the storage key, as when the user clears the site's data: Each connection to one is
    // closed with the forced flag, which aborts its transactions. Returns whether every database's rows went; a database
    // whose rows couldn't be deleted stays.
    bool clear_storage_key(StringView storage_key);

private:
    FFI::IndexedDbBackend* m_backend { nullptr };
};

}
