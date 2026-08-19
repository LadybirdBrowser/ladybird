/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibDatabase/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

class WEBVIEW_API FaviconStore {
    AK_MAKE_NONCOPYABLE(FaviconStore);
    AK_MAKE_NONMOVABLE(FaviconStore);

public:
    static constexpr size_t MAXIMUM_FAVICON_BYTE_COUNT = 1uz * MiB;

    static ErrorOr<Database::MigrationOutcome> migrate_schema(Database::Database&, Database::MigrationMode = Database::MigrationMode::Apply);

    static ErrorOr<NonnullOwnPtr<FaviconStore>> create(Database::Database&);
    static NonnullOwnPtr<FaviconStore> create();

    ~FaviconStore();

    Optional<String> add_favicon(ByteBuffer favicon_png);
    Optional<ByteBuffer> favicon_png(StringView favicon_hash);
    void remove_unreferenced_favicons(HashTable<String> const& referenced_hashes);

private:
    struct Statements {
        Database::StatementID insert_favicon { 0 };
        Database::StatementID select_favicon { 0 };
        Database::StatementID select_hashes { 0 };
        Database::StatementID delete_favicon { 0 };
    };

    class StorageImpl {
    public:
        virtual ~StorageImpl() = default;

        virtual bool add_favicon(String const& hash, ByteBuffer favicon_png) = 0;
        virtual Optional<ByteBuffer> favicon_png(StringView hash) = 0;
        virtual void remove_unreferenced_favicons(HashTable<String> const& referenced_hashes) = 0;
    };

    class TransientStorage final : public StorageImpl {
    public:
        virtual bool add_favicon(String const& hash, ByteBuffer favicon_png) override;
        virtual Optional<ByteBuffer> favicon_png(StringView hash) override;
        virtual void remove_unreferenced_favicons(HashTable<String> const& referenced_hashes) override;

    private:
        HashMap<String, ByteBuffer> m_favicons;
    };

    class PersistedStorage final : public StorageImpl {
    public:
        PersistedStorage(Database::Database&, Statements);
        virtual ~PersistedStorage() override;

        virtual bool add_favicon(String const& hash, ByteBuffer favicon_png) override;
        virtual Optional<ByteBuffer> favicon_png(StringView hash) override;
        virtual void remove_unreferenced_favicons(HashTable<String> const& referenced_hashes) override;

    private:
        Database::Database& m_database;
        Statements m_statements;
    };

    explicit FaviconStore(NonnullOwnPtr<StorageImpl>);

    NonnullOwnPtr<StorageImpl> m_storage;
};

}
