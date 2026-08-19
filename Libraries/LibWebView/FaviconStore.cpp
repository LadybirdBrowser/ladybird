/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Hex.h>
#include <AK/Vector.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibDatabase/Database.h>
#include <LibDatabase/ResultRow.h>
#include <LibWebView/FaviconStore.h>

namespace WebView {

static constexpr auto FAVICONS_SCHEMA_BASELINE_VERSION = 1u;
static constexpr auto FAVICON_DATABASE_BUSY_TIMEOUT_MS = 250;
static constexpr auto SHA256_HEX_DIGEST_LENGTH = 64uz;

ErrorOr<Database::MigrationOutcome> FaviconStore::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    auto migrations = to_array<Database::Migration>({
        {
            .version = FAVICONS_SCHEMA_BASELINE_VERSION,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS Favicons (
                    hash TEXT PRIMARY KEY,
                    png_data BLOB NOT NULL
                );
            )#"sv,
        },
    });

    return database.migrate("Favicons"sv, migrations, mode);
}

ErrorOr<NonnullOwnPtr<FaviconStore>> FaviconStore::create(Database::Database& database)
{
    TRY(database.set_busy_timeout(FAVICON_DATABASE_BUSY_TIMEOUT_MS));

    Statements statements {};
    statements.insert_favicon = TRY(database.prepare_statement(R"#(
        INSERT INTO Favicons (hash, png_data)
        VALUES (:hash, :png_data)
        ON CONFLICT(hash) DO NOTHING;
    )#"sv));
    statements.select_favicon = TRY(database.prepare_statement(R"#(
        SELECT png_data
        FROM Favicons
        WHERE hash = :hash;
    )#"sv));
    statements.select_hashes = TRY(database.prepare_statement("SELECT hash FROM Favicons;"sv));
    statements.delete_favicon = TRY(database.prepare_statement(R"#(
        DELETE FROM Favicons
        WHERE hash = :hash;
    )#"sv));

    return adopt_own(*new FaviconStore { adopt_own<StorageImpl>(*new PersistedStorage { database, statements }) });
}

NonnullOwnPtr<FaviconStore> FaviconStore::create()
{
    return adopt_own(*new FaviconStore { adopt_own<StorageImpl>(*new TransientStorage {}) });
}

FaviconStore::FaviconStore(NonnullOwnPtr<StorageImpl> storage)
    : m_storage(move(storage))
{
}

FaviconStore::~FaviconStore() = default;

Optional<String> FaviconStore::add_favicon(ByteBuffer favicon_png)
{
    if (favicon_png.is_empty() || favicon_png.size() > MAXIMUM_FAVICON_BYTE_COUNT)
        return {};

    auto hash_bytes = encode_hex(Crypto::Hash::SHA256::hash(favicon_png).bytes());
    auto hash = MUST(String::from_byte_string(hash_bytes));
    if (!m_storage->add_favicon(hash, move(favicon_png)))
        return {};

    return hash;
}

Optional<ByteBuffer> FaviconStore::favicon_png(StringView favicon_hash)
{
    return m_storage->favicon_png(favicon_hash);
}

bool FaviconStore::TransientStorage::add_favicon(String const& hash, ByteBuffer favicon_png)
{
    m_favicons.set(hash, move(favicon_png), AK::HashSetExistingEntryBehavior::Keep);
    return true;
}

void FaviconStore::remove_unreferenced_favicons(HashTable<String> const& referenced_hashes)
{
    m_storage->remove_unreferenced_favicons(referenced_hashes);
}

Optional<ByteBuffer> FaviconStore::TransientStorage::favicon_png(StringView hash)
{
    if (auto favicon = m_favicons.get(hash); favicon.has_value())
        return favicon.release_value();
    return {};
}

void FaviconStore::TransientStorage::remove_unreferenced_favicons(HashTable<String> const& referenced_hashes)
{
    m_favicons.remove_all_matching([&](auto const& hash, auto const&) {
        return !referenced_hashes.contains(hash);
    });
}

FaviconStore::PersistedStorage::PersistedStorage(Database::Database& database, Statements statements)
    : m_database(database)
    , m_statements(statements)
{
}

FaviconStore::PersistedStorage::~PersistedStorage() = default;

bool FaviconStore::PersistedStorage::add_favicon(String const& hash, ByteBuffer favicon_png)
{
    auto result = m_database.try_execute_bound_statement(
        m_statements.insert_favicon,
        [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("hash"sv, hash));
            TRY(bind("png_data"sv, favicon_png));
            return {};
        });
    return !result.is_error();
}

Optional<ByteBuffer> FaviconStore::PersistedStorage::favicon_png(StringView hash)
{
    auto hash_string = String::from_utf8(hash);
    if (hash_string.is_error())
        return {};

    Optional<ByteBuffer> favicon_png;

    auto result = m_database.try_execute_bound_statement(
        m_statements.select_favicon,
        [&](auto& bind) -> ErrorOr<void> {
            return bind("hash"sv, hash_string.value());
        },
        [&](Database::ResultRow& row) -> ErrorOr<void> {
            favicon_png = TRY(row.read_blob("png_data"sv, MAXIMUM_FAVICON_BYTE_COUNT));
            return {};
        });
    if (result.is_error())
        return {};

    return favicon_png;
}

void FaviconStore::PersistedStorage::remove_unreferenced_favicons(HashTable<String> const& referenced_hashes)
{
    auto result = m_database.transaction([&]() -> ErrorOr<void> {
        Vector<String> hashes;
        TRY(m_database.try_execute_bound_statement(
            m_statements.select_hashes,
            [](auto&) -> ErrorOr<void> { return {}; },
            [&](Database::ResultRow& row) -> ErrorOr<void> {
                hashes.append(TRY(row.read_text("hash"sv, SHA256_HEX_DIGEST_LENGTH)));
                return {};
            }));

        for (auto const& hash : hashes) {
            if (referenced_hashes.contains(hash))
                continue;
            TRY(m_database.try_execute_bound_statement(m_statements.delete_favicon, [&](auto& bind) -> ErrorOr<void> {
                return bind("hash"sv, hash);
            }));
        }
        return {};
    });
    if (result.is_error())
        warnln("Unable to remove unreferenced favicons: {}", result.error());
}

}
