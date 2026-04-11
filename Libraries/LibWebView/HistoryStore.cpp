/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/QuickSort.h>
#include <LibDatabase/Database.h>
#include <LibURL/URL.h>
#include <LibWebView/HistoryStore.h>

namespace WebView {

static constexpr auto DEFAULT_AUTOCOMPLETE_SUGGESTION_LIMIT = 8uz;

static bool matches_query(HistoryEntry const& entry, StringView query)
{
    if (entry.url.contains(query, CaseSensitivity::CaseInsensitive))
        return true;

    return entry.title.has_value()
        && entry.title->contains(query, CaseSensitivity::CaseInsensitive);
}

static u8 match_rank(HistoryEntry const& entry, StringView query)
{
    auto url = entry.url.bytes_as_string_view();

    if (entry.url.equals_ignoring_ascii_case(query))
        return 0;
    if (url.starts_with(query, CaseSensitivity::CaseInsensitive))
        return 1;
    if (auto scheme_separator = url.find("://"sv); scheme_separator.has_value() && url.substring_view(*scheme_separator + 3).starts_with(query, CaseSensitivity::CaseInsensitive))
        return 2;
    if (entry.title.has_value() && entry.title->starts_with_bytes(query, CaseSensitivity::CaseInsensitive))
        return 3;
    return 4;
}

static void sort_matching_entries(Vector<HistoryEntry const*>& matches, StringView query)
{
    quick_sort(matches, [&](auto const* left, auto const* right) {
        auto left_rank = match_rank(*left, query);
        auto right_rank = match_rank(*right, query);
        if (left_rank != right_rank)
            return left_rank < right_rank;

        if (left->visit_count != right->visit_count)
            return left->visit_count > right->visit_count;

        if (left->last_visited_time != right->last_visited_time)
            return left->last_visited_time > right->last_visited_time;

        return left->url < right->url;
    });
}

ErrorOr<NonnullOwnPtr<HistoryStore>> HistoryStore::create(Database::Database& database)
{
    Statements statements {};

    auto create_history_table = TRY(database.prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS History (
            url TEXT PRIMARY KEY,
            title TEXT NOT NULL,
            visit_count INTEGER NOT NULL,
            last_visited_time INTEGER NOT NULL
        );
    )#"sv));
    database.execute_statement(create_history_table, {});

    auto create_last_visited_index = TRY(database.prepare_statement(R"#(
        CREATE INDEX IF NOT EXISTS HistoryLastVisitedTimeIndex
        ON History(last_visited_time DESC);
    )#"sv));
    database.execute_statement(create_last_visited_index, {});

    statements.upsert_entry = TRY(database.prepare_statement(R"#(
        INSERT INTO History (url, title, visit_count, last_visited_time)
        VALUES (?, ?, 1, ?)
        ON CONFLICT(url) DO UPDATE SET
            title = CASE
                WHEN excluded.title != '' THEN excluded.title
                ELSE History.title
            END,
            visit_count = History.visit_count + 1,
            last_visited_time = excluded.last_visited_time;
    )#"sv));
    statements.update_title = TRY(database.prepare_statement(R"#(
        UPDATE History
        SET title = ?
        WHERE url = ?;
    )#"sv));
    statements.get_entry = TRY(database.prepare_statement(R"#(
        SELECT title, visit_count, last_visited_time
        FROM History
        WHERE url = ?;
    )#"sv));
    statements.search_entries = TRY(database.prepare_statement(R"#(
        SELECT url
        FROM History
        WHERE INSTR(LOWER(url), LOWER(?)) > 0
           OR INSTR(LOWER(title), LOWER(?)) > 0
        ORDER BY
            CASE
                WHEN LOWER(url) = LOWER(?) THEN 0
                WHEN LOWER(url) LIKE LOWER(?) || '%' THEN 1
                WHEN INSTR(LOWER(url), '://' || LOWER(?)) > 0 THEN 2
                WHEN LOWER(title) LIKE LOWER(?) || '%' THEN 3
                ELSE 4
            END,
            visit_count DESC,
            last_visited_time DESC,
            url ASC
        LIMIT ?;
    )#"sv));
    statements.clear_entries = TRY(database.prepare_statement("DELETE FROM History;"sv));
    statements.delete_entries_accessed_since = TRY(database.prepare_statement("DELETE FROM History WHERE last_visited_time >= ?;"sv));

    return adopt_own(*new HistoryStore { PersistedStorage { database, statements } });
}

NonnullOwnPtr<HistoryStore> HistoryStore::create()
{
    return adopt_own(*new HistoryStore { OptionalNone {} });
}

NonnullOwnPtr<HistoryStore> HistoryStore::create_disabled()
{
    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Opening disabled history store");

    return adopt_own(*new HistoryStore { OptionalNone {}, true });
}

HistoryStore::HistoryStore(Optional<PersistedStorage> persisted_storage, bool is_disabled)
    : m_persisted_storage(move(persisted_storage))
    , m_is_disabled(is_disabled)
{
}

HistoryStore::~HistoryStore() = default;

Optional<String> HistoryStore::normalize_url(URL::URL const& url)
{
    if (url.scheme().is_empty())
        return {};

    if (url.scheme().is_one_of("about"sv, "data"sv))
        return {};

    auto normalized_url = url.serialize(URL::ExcludeFragment::Yes);
    if (normalized_url.is_empty())
        return {};

    return normalized_url;
}

void HistoryStore::record_visit(URL::URL const& url, Optional<String> title, UnixDateTime visited_at)
{
    if (m_is_disabled)
        return;

    auto normalized_url = normalize_url(url);
    if (!normalized_url.has_value())
        return;

    if (m_persisted_storage.has_value())
        m_persisted_storage->record_visit(*normalized_url, title, visited_at);
    else
        m_transient_storage.record_visit(normalized_url.release_value(), move(title), visited_at);
}

void HistoryStore::update_title(URL::URL const& url, String const& title)
{
    if (m_is_disabled)
        return;

    if (title.is_empty())
        return;

    auto normalized_url = normalize_url(url);
    if (!normalized_url.has_value())
        return;

    if (m_persisted_storage.has_value())
        m_persisted_storage->update_title(*normalized_url, title);
    else
        m_transient_storage.update_title(*normalized_url, title);
}

Optional<HistoryEntry> HistoryStore::entry_for_url(URL::URL const& url)
{
    if (m_is_disabled)
        return {};

    auto normalized_url = normalize_url(url);
    if (!normalized_url.has_value())
        return {};

    if (m_persisted_storage.has_value())
        return m_persisted_storage->entry_for_url(*normalized_url);
    return m_transient_storage.entry_for_url(*normalized_url);
}

Vector<String> HistoryStore::autocomplete_suggestions(StringView query, size_t limit)
{
    if (m_is_disabled)
        return {};

    auto trimmed_query = query.trim_whitespace();
    if (trimmed_query.is_empty())
        return {};

    if (m_persisted_storage.has_value())
        return m_persisted_storage->autocomplete_suggestions(trimmed_query, limit);
    return m_transient_storage.autocomplete_suggestions(trimmed_query, limit);
}

void HistoryStore::clear()
{
    if (m_is_disabled)
        return;
    if (m_persisted_storage.has_value())
        m_persisted_storage->clear();
    else
        m_transient_storage.clear();
}

void HistoryStore::remove_entries_accessed_since(UnixDateTime since)
{
    if (m_is_disabled)
        return;
    if (m_persisted_storage.has_value())
        m_persisted_storage->remove_entries_accessed_since(since);
    else
        m_transient_storage.remove_entries_accessed_since(since);
}

void HistoryStore::TransientStorage::record_visit(String url, Optional<String> title, UnixDateTime visited_at)
{
    auto entry = m_entries.find(url);
    if (entry == m_entries.end()) {
        auto new_entry = HistoryEntry {
            .url = url,
            .title = move(title),
            .visit_count = 1,
            .last_visited_time = visited_at,
        };
        m_entries.set(
            move(url),
            move(new_entry));
        return;
    }

    entry->value.visit_count++;
    entry->value.last_visited_time = visited_at;
    if (title.has_value() && !title->is_empty())
        entry->value.title = move(title);
}

void HistoryStore::TransientStorage::update_title(String const& url, String title)
{
    auto entry = m_entries.find(url);
    if (entry == m_entries.end())
        return;

    entry->value.title = move(title);
}

Optional<HistoryEntry> HistoryStore::TransientStorage::entry_for_url(String const& url)
{
    auto entry = m_entries.get(url);
    if (!entry.has_value())
        return {};

    return *entry;
}

Vector<String> HistoryStore::TransientStorage::autocomplete_suggestions(StringView query, size_t limit)
{
    Vector<HistoryEntry const*> matches;

    for (auto const& entry : m_entries) {
        if (matches_query(entry.value, query))
            matches.append(&entry.value);
    }

    sort_matching_entries(matches, query);

    Vector<String> suggestions;
    suggestions.ensure_capacity(min(limit, matches.size()));

    for (size_t i = 0; i < matches.size() && i < limit; ++i)
        suggestions.unchecked_append(matches[i]->url);

    return suggestions;
}

void HistoryStore::TransientStorage::clear()
{
    m_entries.clear();
}

void HistoryStore::TransientStorage::remove_entries_accessed_since(UnixDateTime since)
{
    m_entries.remove_all_matching([&](auto const&, auto const& entry) {
        return entry.last_visited_time >= since;
    });
}

void HistoryStore::PersistedStorage::record_visit(String const& url, Optional<String> const& title, UnixDateTime visited_at)
{
    database.execute_statement(
        statements.upsert_entry,
        {},
        url,
        title.value_or(String {}),
        visited_at);
}

void HistoryStore::PersistedStorage::update_title(String const& url, String const& title)
{
    database.execute_statement(
        statements.update_title,
        {},
        title,
        url);
}

Optional<HistoryEntry> HistoryStore::PersistedStorage::entry_for_url(String const& url)
{
    Optional<HistoryEntry> entry;

    database.execute_statement(
        statements.get_entry,
        [&](auto statement_id) {
            auto title = database.result_column<String>(statement_id, 0);

            entry = HistoryEntry {
                .url = url,
                .title = title.is_empty() ? Optional<String> {} : Optional<String> { move(title) },
                .visit_count = database.result_column<u64>(statement_id, 1),
                .last_visited_time = database.result_column<UnixDateTime>(statement_id, 2),
            };
        },
        url);

    return entry;
}

Vector<String> HistoryStore::PersistedStorage::autocomplete_suggestions(StringView query, size_t limit)
{
    Vector<String> suggestions;
    suggestions.ensure_capacity(min(limit, DEFAULT_AUTOCOMPLETE_SUGGESTION_LIMIT));

    database.execute_statement(
        statements.search_entries,
        [&](auto statement_id) {
            suggestions.append(database.result_column<String>(statement_id, 0));
        },
        MUST(String::from_utf8(query)),
        MUST(String::from_utf8(query)),
        MUST(String::from_utf8(query)),
        MUST(String::from_utf8(query)),
        MUST(String::from_utf8(query)),
        MUST(String::from_utf8(query)),
        static_cast<i64>(limit));

    return suggestions;
}

void HistoryStore::PersistedStorage::clear()
{
    database.execute_statement(statements.clear_entries, {});
}

void HistoryStore::PersistedStorage::remove_entries_accessed_since(UnixDateTime since)
{
    database.execute_statement(statements.delete_entries_accessed_since, {}, since);
}

}
