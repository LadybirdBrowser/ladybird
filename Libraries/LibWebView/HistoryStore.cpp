/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/QuickSort.h>
#include <AK/Utf8View.h>
#include <LibDatabase/Database.h>
#include <LibURL/URL.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/HistoryStore.h>

namespace WebView {

static constexpr auto DEFAULT_AUTOCOMPLETE_SUGGESTION_LIMIT = 8uz;
static constexpr size_t MINIMUM_TITLE_AUTOCOMPLETE_QUERY_LENGTH = 3;

static Optional<StringView> url_without_scheme(StringView url)
{
    auto scheme_separator = url.find("://"sv);
    if (!scheme_separator.has_value())
        return {};

    return url.substring_view(*scheme_separator + 3);
}

static StringView autocomplete_searchable_url(StringView url)
{
    auto stripped_url = url_without_scheme(url).value_or(url);
    if (stripped_url.starts_with("www."sv, CaseSensitivity::CaseInsensitive))
        stripped_url = stripped_url.substring_view(4);

    return stripped_url;
}

static StringView autocomplete_url_query(StringView query)
{
    auto stripped_query = url_without_scheme(query).value_or(query);
    if (stripped_query.starts_with("www."sv, CaseSensitivity::CaseInsensitive))
        stripped_query = stripped_query.substring_view(4);

    return stripped_query;
}

static StringView autocomplete_title_query(StringView query)
{
    if (Utf8View { query }.length() < MINIMUM_TITLE_AUTOCOMPLETE_QUERY_LENGTH)
        return {};

    return query;
}

static StringView autocomplete_url_contains_query(StringView query)
{
    // Non-prefix URL matches get noisy very quickly, so only enable them
    // once the user has typed enough to disambiguate path fragments.
    if (Utf8View { query }.length() < MINIMUM_TITLE_AUTOCOMPLETE_QUERY_LENGTH)
        return {};

    return query;
}

static bool matches_query(HistoryEntry const& entry, StringView title_query, StringView url_query)
{
    auto searchable_url = autocomplete_searchable_url(entry.url.bytes_as_string_view());
    if (!url_query.is_empty() && searchable_url.starts_with(url_query, CaseSensitivity::CaseInsensitive))
        return true;

    auto url_contains_query = autocomplete_url_contains_query(url_query);
    if (!url_contains_query.is_empty() && searchable_url.contains(url_contains_query, CaseSensitivity::CaseInsensitive))
        return true;

    return !title_query.is_empty()
        && entry.title.has_value()
        && entry.title->contains(title_query, CaseSensitivity::CaseInsensitive);
}

static u8 match_rank(HistoryEntry const& entry, StringView title_query, StringView url_query)
{
    auto searchable_url = autocomplete_searchable_url(entry.url.bytes_as_string_view());

    if (!url_query.is_empty()) {
        if (searchable_url.equals_ignoring_ascii_case(url_query))
            return 0;
        if (searchable_url.starts_with(url_query, CaseSensitivity::CaseInsensitive))
            return 1;
    }

    if (!title_query.is_empty() && entry.title.has_value() && entry.title->starts_with_bytes(title_query, CaseSensitivity::CaseInsensitive))
        return 2;

    return 3;
}

static void sort_matching_entries(Vector<HistoryEntry const*>& matches, StringView title_query, StringView url_query)
{
    quick_sort(matches, [&](auto const* left, auto const* right) {
        auto left_rank = match_rank(*left, title_query, url_query);
        auto right_rank = match_rank(*right, title_query, url_query);
        if (left_rank != right_rank)
            return left_rank < right_rank;

        if (left->visit_count != right->visit_count)
            return left->visit_count > right->visit_count;

        if (left->last_visited_time != right->last_visited_time)
            return left->last_visited_time > right->last_visited_time;

        return left->url < right->url;
    });
}

[[maybe_unused]] static ByteString log_history_entries(Vector<HistoryEntry> const& entries)
{
    Vector<String> suggestions;
    suggestions.ensure_capacity(entries.size());
    for (auto const& entry : entries)
        suggestions.unchecked_append(entry.url);
    return history_log_suggestions(suggestions);
}

ErrorOr<NonnullOwnPtr<HistoryStore>> HistoryStore::create(Database::Database& database)
{
    if (auto database_path = database.database_path(); database_path.has_value())
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Opening persisted history store at {}", database_path->string());
    else
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Opening memory-backed persisted history store");

    Statements statements {};

    auto create_history_table = TRY(database.prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS History (
            url TEXT PRIMARY KEY,
            title TEXT NOT NULL,
            favicon TEXT,
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
    statements.update_favicon = TRY(database.prepare_statement(R"#(
        UPDATE History
        SET favicon = ?
        WHERE url = ?;
    )#"sv));
    statements.get_entry = TRY(database.prepare_statement(R"#(
        SELECT title, visit_count, last_visited_time, COALESCE(favicon, '')
        FROM History
        WHERE url = ?;
    )#"sv));
    statements.search_entries = TRY(database.prepare_statement(R"#(
        SELECT url, title, visit_count, last_visited_time, COALESCE(favicon, '')
        FROM (
            SELECT
                url,
                title,
                visit_count,
                last_visited_time,
                COALESCE(favicon, '') AS favicon,
                CASE
                    WHEN LOWER(CASE
                        WHEN INSTR(url, '://') > 0 THEN SUBSTR(url, INSTR(url, '://') + 3)
                        ELSE url
                    END) LIKE 'www.%'
                    THEN SUBSTR(CASE
                        WHEN INSTR(url, '://') > 0 THEN SUBSTR(url, INSTR(url, '://') + 3)
                        ELSE url
                    END, 5)
                    ELSE CASE
                        WHEN INSTR(url, '://') > 0 THEN SUBSTR(url, INSTR(url, '://') + 3)
                        ELSE url
                    END
                END AS searchable_url
            FROM History
        )
        WHERE ((?1 != '' AND LOWER(searchable_url) LIKE LOWER(?1) || '%')
            OR (?2 != '' AND INSTR(LOWER(searchable_url), LOWER(?2)) > 0)
            OR (?3 != '' AND INSTR(LOWER(title), LOWER(?3)) > 0))
        ORDER BY
            CASE
                WHEN ?1 != '' AND LOWER(searchable_url) = LOWER(?1) THEN 0
                WHEN ?1 != '' AND LOWER(searchable_url) LIKE LOWER(?1) || '%' THEN 1
                WHEN ?3 != '' AND LOWER(title) LIKE LOWER(?3) || '%' THEN 2
                ELSE 3
            END,
            visit_count DESC,
            last_visited_time DESC,
            url ASC
        LIMIT ?4;
    )#"sv));
    statements.clear_entries = TRY(database.prepare_statement("DELETE FROM History;"sv));
    statements.delete_entries_accessed_since = TRY(database.prepare_statement("DELETE FROM History WHERE last_visited_time >= ?;"sv));

    return adopt_own(*new HistoryStore { PersistedStorage { database, statements } });
}

NonnullOwnPtr<HistoryStore> HistoryStore::create()
{
    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Opening transient history store");

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
    if (url.scheme().is_empty()) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping history entry without a scheme: {}", url);
        return {};
    }

    if (url.scheme().is_one_of("about"sv, "data"sv)) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping non-browsable history URL: {}", url);
        return {};
    }

    auto normalized_url = url.serialize(URL::ExcludeFragment::Yes);
    if (normalized_url.is_empty()) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping history entry with an empty normalized URL: {}", url);
        return {};
    }

    return normalized_url;
}

void HistoryStore::record_visit(URL::URL const& url, Optional<String> title, UnixDateTime visited_at)
{
    if (m_is_disabled)
        return;

    auto normalized_url = normalize_url(url);
    if (!normalized_url.has_value())
        return;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Recording visit in {} store: url='{}' title='{}' visited_at={}",
        m_persisted_storage.has_value() ? "SQL"sv : "transient"sv,
        *normalized_url,
        title.has_value() ? title->bytes_as_string_view() : "<none>"sv,
        visited_at.seconds_since_epoch());

    if (m_persisted_storage.has_value())
        m_persisted_storage->record_visit(*normalized_url, title, visited_at);
    else
        m_transient_storage.record_visit(normalized_url.release_value(), move(title), visited_at);
}

void HistoryStore::update_title(URL::URL const& url, String const& title)
{
    if (m_is_disabled)
        return;

    if (title.is_empty()) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Ignoring empty history title update for {}", url);
        return;
    }

    auto normalized_url = normalize_url(url);
    if (!normalized_url.has_value())
        return;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Updating history title in {} store: url='{}' title='{}'",
        m_persisted_storage.has_value() ? "SQL"sv : "transient"sv,
        *normalized_url,
        title);

    if (m_persisted_storage.has_value())
        m_persisted_storage->update_title(*normalized_url, title);
    else
        m_transient_storage.update_title(*normalized_url, title);
}

void HistoryStore::update_favicon(URL::URL const& url, String const& favicon_base64_png)
{
    if (favicon_base64_png.is_empty()) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Ignoring empty history favicon update for {}", url);
        return;
    }

    auto normalized_url = normalize_url(url);
    if (!normalized_url.has_value())
        return;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Updating history favicon in {} store: url='{}' bytes={}",
        m_persisted_storage.has_value() ? "SQL"sv : "transient"sv,
        *normalized_url,
        favicon_base64_png.bytes().size());

    if (m_persisted_storage.has_value())
        m_persisted_storage->update_favicon(*normalized_url, favicon_base64_png);
    else
        m_transient_storage.update_favicon(*normalized_url, favicon_base64_png);
}

Optional<HistoryEntry> HistoryStore::entry_for_url(URL::URL const& url)
{
    if (m_is_disabled)
        return {};

    auto normalized_url = normalize_url(url);
    if (!normalized_url.has_value())
        return {};

    auto entry = m_persisted_storage.has_value()
        ? m_persisted_storage->entry_for_url(*normalized_url)
        : m_transient_storage.entry_for_url(*normalized_url);

    if (entry.has_value()) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Found history entry for '{}': title='{}' visits={} last_visited={} has_favicon={}",
            entry->url,
            entry->title.has_value() ? entry->title->bytes_as_string_view() : "<none>"sv,
            entry->visit_count,
            entry->last_visited_time.seconds_since_epoch(),
            entry->favicon_base64_png.has_value());
    } else {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] No history entry found for '{}'", *normalized_url);
    }

    return entry;
}

Vector<HistoryEntry> HistoryStore::autocomplete_entries(StringView query, size_t limit)
{
    if (m_is_disabled)
        return {};

    auto trimmed_query = query.trim_whitespace();
    if (trimmed_query.is_empty()) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] History autocomplete query is empty after trimming");
        return {};
    }

    auto title_query = autocomplete_title_query(trimmed_query);
    auto url_query = autocomplete_url_query(trimmed_query);

    auto entries = m_persisted_storage.has_value()
        ? m_persisted_storage->autocomplete_entries(title_query, url_query, limit)
        : m_transient_storage.autocomplete_entries(title_query, url_query, limit);

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] {} history autocomplete suggestions for '{}' (title_query='{}', url_query='{}', limit={}): {}",
        m_persisted_storage.has_value() ? "SQL"sv : "Transient"sv,
        trimmed_query,
        title_query,
        url_query,
        limit,
        log_history_entries(entries));

    return entries;
}

void HistoryStore::clear()
{
    if (m_is_disabled)
        return;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Clearing {} history store", m_persisted_storage.has_value() ? "SQL"sv : "transient"sv);
    if (m_persisted_storage.has_value())
        m_persisted_storage->clear();
    else
        m_transient_storage.clear();
}

void HistoryStore::remove_entries_accessed_since(UnixDateTime since)
{
    if (m_is_disabled)
        return;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Removing {} history entries accessed since {}",
        m_persisted_storage.has_value() ? "SQL"sv : "transient"sv,
        since.seconds_since_epoch());
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
            .favicon_base64_png = {},
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

void HistoryStore::TransientStorage::update_favicon(String const& url, String favicon_base64_png)
{
    auto entry = m_entries.find(url);
    if (entry == m_entries.end())
        return;

    entry->value.favicon_base64_png = move(favicon_base64_png);
}

Optional<HistoryEntry> HistoryStore::TransientStorage::entry_for_url(String const& url)
{
    auto entry = m_entries.get(url);
    if (!entry.has_value())
        return {};

    return *entry;
}

Vector<HistoryEntry> HistoryStore::TransientStorage::autocomplete_entries(StringView title_query, StringView url_query, size_t limit)
{
    Vector<HistoryEntry const*> matches;

    for (auto const& entry : m_entries) {
        if (matches_query(entry.value, title_query, url_query))
            matches.append(&entry.value);
    }

    sort_matching_entries(matches, title_query, url_query);

    Vector<HistoryEntry> entries;
    entries.ensure_capacity(min(limit, matches.size()));

    for (size_t i = 0; i < matches.size() && i < limit; ++i)
        entries.unchecked_append(*matches[i]);

    return entries;
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

void HistoryStore::PersistedStorage::update_favicon(String const& url, String const& favicon_base64_png)
{
    database.execute_statement(
        statements.update_favicon,
        {},
        favicon_base64_png,
        url);
}

Optional<HistoryEntry> HistoryStore::PersistedStorage::entry_for_url(String const& url)
{
    Optional<HistoryEntry> entry;

    database.execute_statement(
        statements.get_entry,
        [&](auto statement_id) {
            auto title = database.result_column<String>(statement_id, 0);
            auto favicon = database.result_column<String>(statement_id, 3);

            entry = HistoryEntry {
                .url = url,
                .title = title.is_empty() ? Optional<String> {} : Optional<String> { move(title) },
                .favicon_base64_png = favicon.is_empty() ? Optional<String> {} : Optional<String> { move(favicon) },
                .visit_count = database.result_column<u64>(statement_id, 1),
                .last_visited_time = database.result_column<UnixDateTime>(statement_id, 2),
            };
        },
        url);

    return entry;
}

Vector<HistoryEntry> HistoryStore::PersistedStorage::autocomplete_entries(StringView title_query, StringView url_query, size_t limit)
{
    Vector<HistoryEntry> entries;
    entries.ensure_capacity(min(limit, DEFAULT_AUTOCOMPLETE_SUGGESTION_LIMIT));
    auto url_query_string = MUST(String::from_utf8(url_query));
    auto title_query_string = MUST(String::from_utf8(title_query));
    auto url_contains_query_string = MUST(String::from_utf8(autocomplete_url_contains_query(url_query)));

    database.execute_statement(
        statements.search_entries,
        [&](auto statement_id) {
            auto title = database.result_column<String>(statement_id, 1);
            auto favicon = database.result_column<String>(statement_id, 4);

            entries.append(HistoryEntry {
                .url = database.result_column<String>(statement_id, 0),
                .title = title.is_empty() ? Optional<String> {} : Optional<String> { move(title) },
                .favicon_base64_png = favicon.is_empty() ? Optional<String> {} : Optional<String> { move(favicon) },
                .visit_count = database.result_column<u64>(statement_id, 2),
                .last_visited_time = database.result_column<UnixDateTime>(statement_id, 3),
            });
        },
        url_query_string,
        url_contains_query_string,
        title_query_string,
        static_cast<i64>(limit));

    return entries;
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
