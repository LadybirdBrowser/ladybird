/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibDatabase/Database.h>
#include <LibWebView/DownloadStore.h>

namespace WebView {

static constexpr u32 DOWNLOAD_SCHEMA_BASELINE_VERSION = 1u;
static constexpr u32 DOWNLOAD_SCHEMA_RESTARTABILITY_VERSION = 2u;

static String serialize_segments(Vector<DownloadSegmentRecord> const& segments)
{
    JsonArray serialized;

    for (auto const& segment : segments) {
        JsonObject serialized_segment;
        serialized_segment.set("start"sv, segment.start_offset);
        serialized_segment.set("end"sv, segment.end_offset);
        serialized_segment.set("next"sv, segment.next_offset);

        serialized.must_append(move(serialized_segment));
    }

    return serialized.serialized();
}

static Optional<Vector<DownloadSegmentRecord>> deserialize_segments(StringView serialized)
{
    auto json = JsonValue::from_string(serialized);
    if (json.is_error() || !json.value().is_array())
        return {};

    Vector<DownloadSegmentRecord> segments;

    u64 expected_start = 0;

    for (auto const& value : json.value().as_array().values()) {
        if (!value.is_object())
            return {};

        auto const& object = value.as_object();
        auto start_offset = object.get_integer<u64>("start"sv);
        auto end_offset = object.get_integer<u64>("end"sv);
        auto next_offset = object.get_integer<u64>("next"sv);

        if (!start_offset.has_value() || !end_offset.has_value() || !next_offset.has_value())
            return {};

        if (*start_offset != expected_start || *end_offset < *start_offset)
            return {};
        if (*next_offset < *start_offset || *next_offset > *end_offset + 1)
            return {};

        expected_start = *end_offset + 1;
        segments.append({ *start_offset, *end_offset, *next_offset });
    }

    if (segments.is_empty())
        return {};

    return segments;
}

ErrorOr<Database::MigrationOutcome> DownloadStore::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    Array<Database::Migration, 2> migrations { {
        { .version = DOWNLOAD_SCHEMA_BASELINE_VERSION, .sql = R"#(
            CREATE TABLE IF NOT EXISTS Downloads (
                id INTEGER PRIMARY KEY,
                url TEXT NOT NULL,
                display_url TEXT NOT NULL,
                destination TEXT NOT NULL,
                temporary_destination TEXT NOT NULL,
                total_size INTEGER NOT NULL,
                etag TEXT NOT NULL,
                last_modified TEXT NOT NULL,
                segments TEXT NOT NULL,
                created_time INTEGER NOT NULL,
                updated_time INTEGER NOT NULL
            );
        )#"sv },
        { .version = DOWNLOAD_SCHEMA_RESTARTABILITY_VERSION, .sql = R"#(
            ALTER TABLE Downloads ADD COLUMN can_restart_from_zero INTEGER NOT NULL DEFAULT 0;
        )#"sv },
    } };

    return database.migrate("Downloads"sv, migrations, mode);
}

ErrorOr<NonnullOwnPtr<DownloadStore>> DownloadStore::create(Database::Database& database)
{
    Statements statements {};

    statements.upsert_download = TRY(database.prepare_statement(R"#(
        INSERT INTO Downloads (
            id,
            url,
            display_url,
            destination,
            temporary_destination,
            total_size,
            etag,
            last_modified,
            segments,
            can_restart_from_zero,
            created_time,
            updated_time
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
            url = excluded.url,
            display_url = excluded.display_url,
            destination = excluded.destination,
            temporary_destination = excluded.temporary_destination,
            total_size = excluded.total_size,
            etag = excluded.etag,
            last_modified = excluded.last_modified,
            segments = excluded.segments,
            can_restart_from_zero = excluded.can_restart_from_zero,
            updated_time = excluded.updated_time;
    )#"sv));

    statements.delete_download = TRY(database.prepare_statement(R"#(
        DELETE FROM Downloads
        WHERE id = ?;
    )#"sv));

    statements.list_downloads = TRY(database.prepare_statement(R"#(
        SELECT id, url, display_url, destination, temporary_destination, total_size, etag, last_modified, segments, can_restart_from_zero, created_time
        FROM Downloads
        ORDER BY id ASC;
    )#"sv));

    statements.maximum_download_id = TRY(database.prepare_statement(R"#(
        SELECT IFNULL(MAX(id), 0)
        FROM Downloads;
    )#"sv));

    return adopt_own(*new DownloadStore { database, move(statements) });
}

NonnullOwnPtr<DownloadStore> DownloadStore::create_disabled()
{
    return adopt_own(*new DownloadStore {});
}

DownloadStore::DownloadStore(Database::Database& database, Statements&& statements)
    : m_database(&database)
    , m_statements(move(statements))
{
}

DownloadStore::~DownloadStore() = default;

void DownloadStore::save_download(DownloadRecord const& download, UnixDateTime updated_at)
{
    if (!m_database)
        return;

    m_database->execute_statement(
        m_statements.upsert_download,
        {},
        download.id,
        download.url,
        download.display_url,
        download.destination,
        download.temporary_destination,
        download.total_size,
        download.etag.value_or(String {}),
        download.last_modified.value_or(String {}),
        serialize_segments(download.segments),
        static_cast<u64>(download.can_restart_from_zero ? 1 : 0),
        download.created_time,
        updated_at);
}

void DownloadStore::remove_download(u64 id)
{
    if (!m_database)
        return;

    m_database->execute_statement(m_statements.delete_download, {}, id);
}

Vector<DownloadRecord> DownloadStore::resumable_downloads()
{
    if (!m_database)
        return {};

    Vector<DownloadRecord> downloads;

    m_database->execute_statement(
        m_statements.list_downloads,
        [&](auto statement_id) {
            auto segments = deserialize_segments(m_database->result_column<String>(statement_id, 8));
            if (!segments.has_value())
                return;

            auto etag = m_database->result_column<String>(statement_id, 6);
            auto last_modified = m_database->result_column<String>(statement_id, 7);

            downloads.append(DownloadRecord {
                .id = m_database->result_column<u64>(statement_id, 0),
                .url = m_database->result_column<String>(statement_id, 1),
                .display_url = m_database->result_column<String>(statement_id, 2),
                .destination = m_database->result_column<String>(statement_id, 3),
                .temporary_destination = m_database->result_column<String>(statement_id, 4),
                .total_size = m_database->result_column<u64>(statement_id, 5),
                .etag = etag.is_empty() ? Optional<String> {} : Optional<String> { move(etag) },
                .last_modified = last_modified.is_empty() ? Optional<String> {} : Optional<String> { move(last_modified) },
                .segments = segments.release_value(),
                .created_time = m_database->result_column<UnixDateTime>(statement_id, 10),
                .can_restart_from_zero = m_database->result_column<u64>(statement_id, 9) != 0,
            });
        });

    return downloads;
}

u64 DownloadStore::maximum_download_id()
{
    if (!m_database)
        return 0;

    u64 maximum_id = 0;

    m_database->execute_statement(
        m_statements.maximum_download_id,
        [&](auto statement_id) {
            maximum_id = m_database->result_column<u64>(statement_id, 0);
        });

    return maximum_id;
}

}
