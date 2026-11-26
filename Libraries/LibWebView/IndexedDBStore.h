/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibDatabase/Database.h>
#include <LibRequests/CacheSizes.h>
#include <LibWebCommon/IndexedDB/Backend.h>
#include <LibWebView/Export.h>

namespace WebView {

// The rows of a browsing session's IndexedDB databases, in the browsing database of its profile, which the session's
// IndexedDB backend loads and writes. The rows are bytes that only the backend reads.
class WEBVIEW_API IndexedDBStore final : public Web::IndexedDB::BackendPersistence {
    AK_MAKE_NONCOPYABLE(IndexedDBStore);
    AK_MAKE_NONMOVABLE(IndexedDBStore);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<Database::MigrationOutcome> migrate_schema(Database::Database&, Database::MigrationMode = Database::MigrationMode::Apply);
    static ErrorOr<NonnullOwnPtr<IndexedDBStore>> create(Database::Database&);

    virtual ~IndexedDBStore() override = default;

    virtual bool load(StringView storage_key, OnDatabase const&, OnRecord const&) override;

    virtual void begin_write(StringView storage_key, ReadonlyBytes database) override;
    virtual void put_metadata(ReadonlyBytes metadata) override;
    virtual void put_record(u64 store, ReadonlyBytes key, ReadonlyBytes value, ReadonlyBytes index_keys) override;
    virtual void delete_record(u64 store, ReadonlyBytes key) override;
    virtual void delete_store(u64 store) override;
    virtual void delete_database() override;
    virtual bool end_write() override;

    // The storage keys whose databases changed at or after the time, in any run.
    Vector<String> storage_keys_written_since(UnixDateTime) const;

    // Deletes the rows of every database of the storage key, and returns whether it could.
    bool delete_storage_key(StringView storage_key);

    // The bytes that the rows take, of every storage key, and of those whose databases changed at or after the time.
    Requests::CacheSizes estimate_size_written_since(UnixDateTime) const;

private:
    struct Statements {
        Database::StatementID select_databases { 0 };
        Database::StatementID select_records { 0 };
        Database::StatementID put_database { 0 };
        Database::StatementID put_record { 0 };
        Database::StatementID delete_record { 0 };
        Database::StatementID delete_store { 0 };
        Database::StatementID delete_database { 0 };
        Database::StatementID delete_database_records { 0 };
        Database::StatementID set_last_write { 0 };
        Database::StatementID select_storage_keys_written_since { 0 };
        Database::StatementID delete_storage_key_databases { 0 };
        Database::StatementID delete_storage_key_records { 0 };
        Database::StatementID estimate_size_written_since { 0 };
    };

    // A change to the rows of the database that the write in progress is for, which end_write() applies.
    struct PendingWrite {
        enum class Type : u8 {
            Metadata,
            Record,
            DeleteRecord,
            DeleteStore,
            DeleteDatabase,
        };

        Type type;
        i64 store { 0 };
        ByteBuffer key {};
        ByteBuffer value {};
        ByteBuffer index_keys {};
    };

    IndexedDBStore(NonnullRefPtr<Database::Database>, Statements);

    NonnullRefPtr<Database::Database> m_database;
    Statements m_statements;

    String m_storage_key;
    ByteBuffer m_database_name;
    Vector<PendingWrite> m_pending_writes;
};

}
