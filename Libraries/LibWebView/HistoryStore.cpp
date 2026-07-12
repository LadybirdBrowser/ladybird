/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Debug.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <AK/Utf8View.h>
#include <LibDatabase/Database.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/HistoryStore.h>

namespace WebView {

static constexpr auto DEFAULT_AUTOCOMPLETE_SUGGESTION_LIMIT = 8uz;
static constexpr size_t MINIMUM_TITLE_AUTOCOMPLETE_QUERY_LENGTH = 3;
static constexpr i32 HISTORY_DATABASE_BUSY_TIMEOUT_MS = 250;

static constexpr u32 HISTORY_SCHEMA_BASELINE_VERSION = 1u;
static constexpr u32 HISTORY_SCHEMA_RANKING_SIGNALS_VERSION = 2u;
static constexpr u32 HISTORY_SCHEMA_OMNIBOX_ENGAGEMENTS_VERSION = 3u;

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

        if (left->direct_visit_count != right->direct_visit_count)
            return left->direct_visit_count > right->direct_visit_count;

        if (left->decayed_direct_score != right->decayed_direct_score)
            return left->decayed_direct_score > right->decayed_direct_score;

        if (left->decayed_visit_score != right->decayed_visit_score)
            return left->decayed_visit_score > right->decayed_visit_score;

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

ErrorOr<Database::MigrationOutcome> HistoryStore::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    Array<Database::Migration, 3> migrations { {
        { .version = HISTORY_SCHEMA_BASELINE_VERSION, .sql = R"#(
            CREATE TABLE IF NOT EXISTS History (
                url TEXT PRIMARY KEY,
                title TEXT NOT NULL,
                favicon TEXT,
                visit_count INTEGER NOT NULL,
                last_visited_time INTEGER NOT NULL
            );

            CREATE INDEX IF NOT EXISTS HistoryLastVisitedTimeIndex
            ON History(last_visited_time DESC);
        )#"sv },
        { .version = HISTORY_SCHEMA_RANKING_SIGNALS_VERSION, .sql = R"#(
            ALTER TABLE History ADD COLUMN direct_visit_count INTEGER NOT NULL DEFAULT 0;
            ALTER TABLE History ADD COLUMN last_qualifying_visit_time INTEGER NOT NULL DEFAULT 0;
            ALTER TABLE History ADD COLUMN last_direct_visit_time INTEGER NOT NULL DEFAULT 0;
            ALTER TABLE History ADD COLUMN decayed_visit_score REAL NOT NULL DEFAULT 0;
            ALTER TABLE History ADD COLUMN decayed_direct_score REAL NOT NULL DEFAULT 0;
            ALTER TABLE History ADD COLUMN score_updated_at INTEGER NOT NULL DEFAULT 0;

            UPDATE History
            SET last_qualifying_visit_time = last_visited_time,
                decayed_visit_score = MIN(CAST(visit_count AS REAL), 8.0),
                score_updated_at = last_visited_time;
        )#"sv },
        { .version = HISTORY_SCHEMA_OMNIBOX_ENGAGEMENTS_VERSION, .sql = R"#(
            CREATE TABLE OmniboxEngagements (
                normalized_input TEXT NOT NULL,
                destination_kind INTEGER NOT NULL,
                destination_key TEXT NOT NULL,
                destination TEXT NOT NULL,
                explicit_use_count INTEGER NOT NULL,
                default_use_count INTEGER NOT NULL,
                last_used_time INTEGER NOT NULL,
                PRIMARY KEY (normalized_input, destination_kind, destination_key)
            );

            CREATE INDEX OmniboxEngagementsByInput
            ON OmniboxEngagements(destination_kind, normalized_input);
        )#"sv },
    } };

    return database.migrate("History"sv, migrations, mode);
}

ErrorOr<NonnullOwnPtr<HistoryStore>> HistoryStore::create(Database::Database& database)
{
    TRY(database.set_busy_timeout(HISTORY_DATABASE_BUSY_TIMEOUT_MS));

    if (auto database_path = database.database_path(); database_path.has_value())
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Opening persisted history store at {}", database_path->string());
    else
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Opening memory-backed persisted history store");

    Statements statements {};

    statements.upsert_entry = TRY(database.prepare_statement(R"#(
        INSERT INTO History (
            url,
            title,
            visit_count,
            last_visited_time,
            direct_visit_count,
            last_qualifying_visit_time,
            last_direct_visit_time,
            decayed_visit_score,
            decayed_direct_score,
            score_updated_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(url) DO UPDATE SET
            title = CASE
                WHEN excluded.title != '' THEN excluded.title
                ELSE History.title
            END,
            visit_count = excluded.visit_count,
            last_visited_time = excluded.last_visited_time,
            direct_visit_count = excluded.direct_visit_count,
            last_qualifying_visit_time = excluded.last_qualifying_visit_time,
            last_direct_visit_time = excluded.last_direct_visit_time,
            decayed_visit_score = excluded.decayed_visit_score,
            decayed_direct_score = excluded.decayed_direct_score,
            score_updated_at = excluded.score_updated_at;
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
        SELECT
            title,
            visit_count,
            last_visited_time,
            COALESCE(favicon, ''),
            direct_visit_count,
            last_qualifying_visit_time,
            last_direct_visit_time,
            decayed_visit_score,
            decayed_direct_score,
            score_updated_at
        FROM History
        WHERE url = ?;
    )#"sv));
    statements.search_entries = TRY(database.prepare_statement(R"#(
        SELECT
            url,
            title,
            visit_count,
            last_visited_time,
            COALESCE(favicon, ''),
            direct_visit_count,
            last_qualifying_visit_time,
            last_direct_visit_time,
            decayed_visit_score,
            decayed_direct_score,
            score_updated_at
        FROM (
            SELECT
                url,
                title,
                visit_count,
                last_visited_time,
                COALESCE(favicon, '') AS favicon,
                direct_visit_count,
                last_qualifying_visit_time,
                last_direct_visit_time,
                decayed_visit_score,
                decayed_direct_score,
                score_updated_at,
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
            direct_visit_count DESC,
            decayed_direct_score DESC,
            decayed_visit_score DESC,
            visit_count DESC,
            last_visited_time DESC,
            url ASC
        LIMIT ?4;
    )#"sv));
    statements.list_entries = TRY(database.prepare_statement(R"#(
        SELECT
            url,
            title,
            visit_count,
            last_visited_time,
            favicon,
            direct_visit_count,
            last_qualifying_visit_time,
            last_direct_visit_time,
            decayed_visit_score,
            decayed_direct_score,
            score_updated_at
        FROM (
            SELECT
                url,
                title,
                visit_count,
                last_visited_time,
                COALESCE(favicon, '') AS favicon,
                direct_visit_count,
                last_qualifying_visit_time,
                last_direct_visit_time,
                decayed_visit_score,
                decayed_direct_score,
                score_updated_at,
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
        WHERE ((?1 = '' AND ?2 = '')
            OR (?1 != '' AND INSTR(LOWER(title), LOWER(?1)) > 0)
            OR (?2 != '' AND INSTR(LOWER(searchable_url), LOWER(?2)) > 0))
        ORDER BY last_visited_time DESC, url ASC
        LIMIT ?3 OFFSET ?4;
    )#"sv));
    statements.delete_entry = TRY(database.prepare_statement("DELETE FROM History WHERE url = ?;"sv));
    statements.delete_entries_accessed_since = TRY(database.prepare_statement("DELETE FROM History WHERE last_visited_time >= ?;"sv));
    statements.all_urls = TRY(database.prepare_statement("SELECT url FROM History;"sv));
    statements.upsert_omnibox_engagement = TRY(database.prepare_statement(R"#(
        INSERT INTO OmniboxEngagements (
            normalized_input,
            destination_kind,
            destination_key,
            destination,
            explicit_use_count,
            default_use_count,
            last_used_time
        )
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(normalized_input, destination_kind, destination_key) DO UPDATE SET
            destination = excluded.destination,
            explicit_use_count = OmniboxEngagements.explicit_use_count + excluded.explicit_use_count,
            default_use_count = OmniboxEngagements.default_use_count + excluded.default_use_count,
            last_used_time = MAX(OmniboxEngagements.last_used_time, excluded.last_used_time);
    )#"sv));
    statements.search_omnibox_engagements = TRY(database.prepare_statement(R"#(
        SELECT
            normalized_input,
            destination_kind,
            destination,
            explicit_use_count,
            default_use_count,
            last_used_time
        FROM OmniboxEngagements
        WHERE (?1 != ''
                AND destination_kind = 0
                AND SUBSTR(normalized_input, 1, LENGTH(?1)) = ?1)
            OR (?2 != ''
                AND destination_kind = 1
                AND SUBSTR(normalized_input, 1, LENGTH(?2)) = ?2)
        ORDER BY
            (2 * explicit_use_count + default_use_count) DESC,
            last_used_time DESC,
            normalized_input ASC,
            destination_key ASC
        LIMIT ?3;
    )#"sv));
    statements.delete_omnibox_engagements_for_url = TRY(database.prepare_statement(R"#(
        DELETE FROM OmniboxEngagements
        WHERE destination_kind = 0 AND destination_key = ?;
    )#"sv));
    statements.delete_omnibox_engagements_used_since = TRY(database.prepare_statement(R"#(
        DELETE FROM OmniboxEngagements
        WHERE last_used_time >= ?;
    )#"sv));

    return adopt_own(*new HistoryStore { adopt_own<StorageImpl>(*new PersistedStorage { database, move(statements) }) });
}

NonnullOwnPtr<HistoryStore> HistoryStore::create()
{
    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Opening transient history store");

    return adopt_own(*new HistoryStore { adopt_own<StorageImpl>(*new TransientStorage {}) });
}

NonnullOwnPtr<HistoryStore> HistoryStore::create_disabled()
{
    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Opening disabled history store");

    return adopt_own(*new HistoryStore { adopt_own<StorageImpl>(*new TransientStorage {}), true });
}

HistoryStore::HistoryStore(NonnullOwnPtr<StorageImpl>&& storage, bool is_disabled)
    : m_storage(move(storage))
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

static bool transition_updates_general_score(HistoryVisitTransition transition)
{
    return transition == HistoryVisitTransition::Omnibox
        || transition == HistoryVisitTransition::Link
        || transition == HistoryVisitTransition::Other;
}

static double score_after_event(double score, UnixDateTime score_updated_at, UnixDateTime event_time, double half_life_in_days)
{
    auto elapsed_seconds = static_cast<double>(event_time.seconds_since_epoch()) - static_cast<double>(score_updated_at.seconds_since_epoch());
    if (elapsed_seconds >= 0) {
        auto decay = AK::pow(0.5, elapsed_seconds / (half_life_in_days * 86'400.0));
        return score * decay + 1.0;
    }

    auto event_age = -elapsed_seconds / (half_life_in_days * 86'400.0);
    return score + AK::pow(0.5, event_age);
}

static HistoryEntry entry_after_visit(Optional<HistoryEntry> existing_entry, String const& url, Optional<String> const& title, UnixDateTime visited_at, HistoryVisitTransition transition)
{
    auto entry = existing_entry.value_or(HistoryEntry {
        .url = url,
        .title = {},
        .favicon_base64_png = {},
        .last_visited_time = visited_at,
        .last_qualifying_visit_time = {},
        .last_direct_visit_time = {},
        .score_updated_at = {},
    });

    if (title.has_value() && !title->is_empty())
        entry.title = title;
    VERIFY(entry.visit_count != NumericLimits<u64>::max());
    ++entry.visit_count;
    entry.last_visited_time = max(entry.last_visited_time, visited_at);

    if (transition_updates_general_score(transition)) {
        entry.decayed_visit_score = score_after_event(entry.decayed_visit_score, entry.score_updated_at, visited_at, 30.0);
        entry.last_qualifying_visit_time = max(entry.last_qualifying_visit_time, visited_at);

        if (transition == HistoryVisitTransition::Omnibox) {
            VERIFY(entry.direct_visit_count != NumericLimits<u64>::max());
            ++entry.direct_visit_count;
            entry.decayed_direct_score = score_after_event(entry.decayed_direct_score, entry.score_updated_at, visited_at, 60.0);
            entry.last_direct_visit_time = max(entry.last_direct_visit_time, visited_at);
        }

        entry.score_updated_at = max(entry.score_updated_at, visited_at);
    }

    return entry;
}

void HistoryStore::record_visit(URL::URL const& url, Optional<String> title, UnixDateTime visited_at, HistoryVisitTransition transition)
{
    if (m_is_disabled)
        return;

    auto normalized_url = normalize_url(url);
    if (!normalized_url.has_value())
        return;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Recording visit in {} store: url='{}' title='{}' visited_at={}",
        m_storage->name(),
        *normalized_url,
        title.has_value() ? title->bytes_as_string_view() : "<none>"sv,
        visited_at.seconds_since_epoch());

    m_storage->record_visit(*normalized_url, title, visited_at, transition);
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
        m_storage->name(),
        *normalized_url,
        title);

    m_storage->update_title(*normalized_url, title);
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
        m_storage->name(),
        *normalized_url,
        favicon_base64_png.bytes().size());

    m_storage->update_favicon(*normalized_url, favicon_base64_png);
}

void HistoryStore::record_closed_tab(URL::URL const& url, UnixDateTime closed_at)
{
    m_recently_closed_entries.empend(RecentlyClosedEntry {
        .urls = { url },
        .was_window = false,
        .active_tab_index = 0,
        .closed_time = closed_at,
    });
}

void HistoryStore::record_closed_window(Vector<URL::URL> urls, size_t active_tab_index, UnixDateTime closed_at)
{
    if (urls.is_empty())
        return;

    m_recently_closed_entries.empend(RecentlyClosedEntry {
        .urls = move(urls),
        .was_window = true,
        .active_tab_index = 0,
        .closed_time = closed_at,
    });

    auto& entry = m_recently_closed_entries.last();
    entry.active_tab_index = active_tab_index < entry.urls.size() ? active_tab_index : entry.urls.size() - 1;
}

bool HistoryStore::has_recently_closed_entries() const
{
    return !m_recently_closed_entries.is_empty();
}

Optional<RecentlyClosedEntry const&> HistoryStore::most_recently_closed_entry() const
{
    if (m_recently_closed_entries.is_empty())
        return {};

    return m_recently_closed_entries.last();
}

Optional<RecentlyClosedEntry> HistoryStore::pop_most_recently_closed_entry()
{
    if (m_recently_closed_entries.is_empty())
        return {};

    return m_recently_closed_entries.take_last();
}

Optional<HistoryEntry> HistoryStore::entry_for_url(URL::URL const& url)
{
    if (m_is_disabled)
        return {};

    auto normalized_url = normalize_url(url);
    if (!normalized_url.has_value())
        return {};

    auto entry = m_storage->entry_for_url(*normalized_url);

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

    auto entries = m_storage->autocomplete_entries(title_query, url_query, limit);

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] {} history autocomplete suggestions for '{}' (title_query='{}', url_query='{}', limit={}): {}",
        m_storage->name(),
        trimmed_query,
        title_query,
        url_query,
        limit,
        log_history_entries(entries));

    return entries;
}

Vector<HistoryEntry> HistoryStore::list_entries(StringView query, size_t offset, size_t limit)
{
    if (m_is_disabled || limit == 0)
        return {};

    auto title_query = query.trim_whitespace();
    auto url_query = autocomplete_url_query(title_query);

    auto entries = m_storage->list_entries(title_query, url_query, offset, limit);

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] {} history page entries for '{}' (title_query='{}', url_query='{}', offset={}, limit={}): {}",
        m_storage->name(),
        title_query,
        title_query,
        url_query,
        offset,
        limit,
        log_history_entries(entries));

    return entries;
}

void HistoryStore::record_omnibox_engagement(OmniboxEngagement const& engagement, UnixDateTime used_at)
{
    if (m_is_disabled || engagement.input.is_empty() || engagement.destination.is_empty())
        return;
    m_storage->record_omnibox_engagement(engagement, used_at);
}

Vector<StoredOmniboxEngagement> HistoryStore::omnibox_engagements(StringView input, size_t limit)
{
    if (m_is_disabled || input.is_empty() || limit == 0)
        return {};
    return m_storage->omnibox_engagements(
        normalize_omnibox_input(input, OmniboxDestinationKind::URL),
        normalize_omnibox_input(input, OmniboxDestinationKind::Search),
        limit);
}

void HistoryStore::remove_entry_for_url(URL::URL const& url, RemoveHistoryEntryEngagements remove_engagements)
{
    if (m_is_disabled)
        return;

    auto normalized_url = normalize_url(url);
    if (!normalized_url.has_value())
        return;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Removing history entry for '{}'", *normalized_url);
    m_storage->remove_entry_for_url(*normalized_url, remove_engagements);
}

static Optional<String> site_key_for_history_entry(URL::URL const& url)
{
    if (!url.host().has_value() || url.host()->is_empty_host())
        return {};

    if (auto registrable_domain = url.host()->registrable_domain(); registrable_domain.has_value())
        return registrable_domain.release_value();

    return url.serialized_host();
}

static bool history_entry_matches_site_key(StringView entry_url, StringView site_key)
{
    auto parsed_url = URL::Parser::basic_parse(entry_url);
    if (!parsed_url.has_value())
        return false;

    auto const& host = parsed_url->host();
    if (!host.has_value() || host->is_empty_host())
        return false;

    auto serialized_host = parsed_url->serialized_host();
    auto serialized_host_view = serialized_host.bytes_as_string_view();
    if (serialized_host_view.equals_ignoring_ascii_case(site_key))
        return true;

    return serialized_host_view.length() > site_key.length()
        && serialized_host_view.ends_with(site_key, CaseSensitivity::CaseInsensitive)
        && serialized_host_view[serialized_host_view.length() - site_key.length() - 1] == '.';
}

void HistoryStore::remove_entries_for_same_site(URL::URL const& url)
{
    if (m_is_disabled)
        return;

    auto site_key = site_key_for_history_entry(url);
    if (!site_key.has_value()) {
        remove_entry_for_url(url, RemoveHistoryEntryEngagements::Yes);
        return;
    }

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Removing history entries for site '{}'", *site_key);
    m_storage->remove_entries_for_same_site(*site_key);
}

void HistoryStore::remove_entries_accessed_since(UnixDateTime since)
{
    if (m_is_disabled)
        return;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Removing {} history entries accessed since {}",
        m_storage->name(),
        since.seconds_since_epoch());
    m_storage->remove_entries_accessed_since(since);
    m_recently_closed_entries.remove_all_matching([&](auto const& entry) {
        return entry.closed_time >= since;
    });
}

void HistoryStore::TransientStorage::record_visit(String const& url, Optional<String> const& title, UnixDateTime visited_at, HistoryVisitTransition transition)
{
    Optional<HistoryEntry> existing_entry;
    if (auto entry = m_entries.get(url); entry.has_value())
        existing_entry = *entry;
    m_entries.set(url, entry_after_visit(move(existing_entry), url, title, visited_at, transition));
}

void HistoryStore::TransientStorage::update_title(String const& url, String const& title)
{
    auto entry = m_entries.find(url);
    if (entry == m_entries.end())
        return;

    entry->value.title = move(title);
}

void HistoryStore::TransientStorage::update_favicon(String const& url, String const& favicon_base64_png)
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

static bool matches_history_page_query(HistoryEntry const& entry, StringView title_query, StringView url_query)
{
    if (title_query.is_empty() && url_query.is_empty())
        return true;

    auto searchable_url = autocomplete_searchable_url(entry.url.bytes_as_string_view());
    if (!url_query.is_empty() && searchable_url.contains(url_query, CaseSensitivity::CaseInsensitive))
        return true;

    return !title_query.is_empty()
        && entry.title.has_value()
        && entry.title->contains(title_query, CaseSensitivity::CaseInsensitive);
}

static void sort_entries_for_history_page(Vector<HistoryEntry const*>& matches)
{
    quick_sort(matches, [](auto const* left, auto const* right) {
        if (left->last_visited_time != right->last_visited_time)
            return left->last_visited_time > right->last_visited_time;
        return left->url < right->url;
    });
}

Vector<HistoryEntry> HistoryStore::TransientStorage::list_entries(StringView title_query, StringView url_query, size_t offset, size_t limit)
{
    Vector<HistoryEntry const*> matches;

    for (auto const& entry : m_entries) {
        if (matches_history_page_query(entry.value, title_query, url_query))
            matches.append(&entry.value);
    }

    sort_entries_for_history_page(matches);

    Vector<HistoryEntry> entries;
    if (offset >= matches.size())
        return entries;

    auto end = min(matches.size(), offset + limit);
    entries.ensure_capacity(end - offset);

    for (size_t i = offset; i < end; ++i)
        entries.unchecked_append(*matches[i]);

    return entries;
}

void HistoryStore::TransientStorage::record_omnibox_engagement(OmniboxEngagement const& engagement, UnixDateTime used_at)
{
    auto normalized_input = normalize_omnibox_input(engagement.input, engagement.destination_kind);
    auto destination = normalize_omnibox_destination(engagement.destination, engagement.destination_kind);
    if (normalized_input.is_empty() || destination.is_empty())
        return;

    auto existing = m_omnibox_engagements.find_if([&](auto const& candidate) {
        return candidate.normalized_input == normalized_input
            && candidate.destination_kind == engagement.destination_kind
            && normalize_omnibox_destination(candidate.destination, candidate.destination_kind) == destination;
    });
    if (existing != m_omnibox_engagements.end()) {
        if (engagement.was_explicit) {
            VERIFY(existing->explicit_use_count != NumericLimits<u64>::max());
            ++existing->explicit_use_count;
        } else {
            VERIFY(existing->default_use_count != NumericLimits<u64>::max());
            ++existing->default_use_count;
        }
        existing->destination = move(destination);
        existing->last_used_time = max(existing->last_used_time, used_at);
        return;
    }

    m_omnibox_engagements.append({
        .normalized_input = move(normalized_input),
        .destination_kind = engagement.destination_kind,
        .destination = move(destination),
        .explicit_use_count = engagement.was_explicit ? 1u : 0u,
        .default_use_count = engagement.was_explicit ? 0u : 1u,
        .last_used_time = used_at,
    });
}

Vector<StoredOmniboxEngagement> HistoryStore::TransientStorage::omnibox_engagements(StringView normalized_url_input, StringView normalized_search_input, size_t limit)
{
    Vector<StoredOmniboxEngagement> results;
    for (auto const& engagement : m_omnibox_engagements) {
        auto normalized_input = engagement.destination_kind == OmniboxDestinationKind::URL
            ? normalized_url_input
            : normalized_search_input;
        if (!normalized_input.is_empty() && engagement.normalized_input.starts_with_bytes(normalized_input))
            results.append(engagement);
    }

    quick_sort(results, [](auto const& left, auto const& right) {
        auto left_weight = 2.0 * static_cast<double>(left.explicit_use_count) + static_cast<double>(left.default_use_count);
        auto right_weight = 2.0 * static_cast<double>(right.explicit_use_count) + static_cast<double>(right.default_use_count);
        if (left_weight != right_weight)
            return left_weight > right_weight;
        if (left.last_used_time != right.last_used_time)
            return left.last_used_time > right.last_used_time;
        if (left.normalized_input != right.normalized_input)
            return left.normalized_input < right.normalized_input;
        return left.destination < right.destination;
    });
    if (results.size() > limit)
        results.resize(limit);
    return results;
}

void HistoryStore::TransientStorage::remove_entry_for_url(String const& url, RemoveHistoryEntryEngagements remove_engagements)
{
    m_entries.remove(url);
    if (remove_engagements == RemoveHistoryEntryEngagements::No)
        return;
    m_omnibox_engagements.remove_all_matching([&](auto const& engagement) {
        return engagement.destination_kind == OmniboxDestinationKind::URL
            && normalize_omnibox_destination(engagement.destination, engagement.destination_kind) == url;
    });
}

void HistoryStore::TransientStorage::remove_entries_for_same_site(StringView site_key)
{
    m_entries.remove_all_matching([&](auto const&, auto const& entry) {
        return history_entry_matches_site_key(entry.url, site_key);
    });
    m_omnibox_engagements.remove_all_matching([&](auto const& engagement) {
        return engagement.destination_kind == OmniboxDestinationKind::URL
            && history_entry_matches_site_key(engagement.destination, site_key);
    });
}

void HistoryStore::TransientStorage::remove_entries_accessed_since(UnixDateTime since)
{
    m_entries.remove_all_matching([&](auto const&, auto const& entry) {
        return entry.last_visited_time >= since;
    });
    m_omnibox_engagements.remove_all_matching([&](auto const& engagement) {
        return engagement.last_used_time >= since;
    });
}

HistoryStore::PersistedStorage::PersistedStorage(Database::Database& database, Statements&& statements)
    : m_database(database)
    , m_statements(move(statements))
{
}

HistoryStore::PersistedStorage::~PersistedStorage() = default;

void HistoryStore::PersistedStorage::record_visit(String const& url, Optional<String> const& title, UnixDateTime visited_at, HistoryVisitTransition transition)
{
    auto entry = entry_after_visit(entry_for_url(url), url, title, visited_at, transition);
    m_database.execute_statement(
        m_statements.upsert_entry,
        {},
        url,
        title.value_or(String {}),
        entry.visit_count,
        entry.last_visited_time,
        entry.direct_visit_count,
        entry.last_qualifying_visit_time,
        entry.last_direct_visit_time,
        entry.decayed_visit_score,
        entry.decayed_direct_score,
        entry.score_updated_at);
}

void HistoryStore::PersistedStorage::update_title(String const& url, String const& title)
{
    m_database.execute_statement(
        m_statements.update_title,
        {},
        title,
        url);
}

void HistoryStore::PersistedStorage::update_favicon(String const& url, String const& favicon_base64_png)
{
    m_database.execute_statement(
        m_statements.update_favicon,
        {},
        favicon_base64_png,
        url);
}

Optional<HistoryEntry> HistoryStore::PersistedStorage::entry_for_url(String const& url)
{
    Optional<HistoryEntry> entry;

    m_database.execute_statement(
        m_statements.get_entry,
        [&](auto statement_id) {
            auto title = m_database.result_column<String>(statement_id, 0);
            auto favicon = m_database.result_column<String>(statement_id, 3);

            entry = HistoryEntry {
                .url = url,
                .title = title.is_empty() ? Optional<String> {} : Optional<String> { move(title) },
                .favicon_base64_png = favicon.is_empty() ? Optional<String> {} : Optional<String> { move(favicon) },
                .visit_count = m_database.result_column<u64>(statement_id, 1),
                .direct_visit_count = m_database.result_column<u64>(statement_id, 4),
                .last_visited_time = m_database.result_column<UnixDateTime>(statement_id, 2),
                .last_qualifying_visit_time = m_database.result_column<UnixDateTime>(statement_id, 5),
                .last_direct_visit_time = m_database.result_column<UnixDateTime>(statement_id, 6),
                .decayed_visit_score = m_database.result_column<double>(statement_id, 7),
                .decayed_direct_score = m_database.result_column<double>(statement_id, 8),
                .score_updated_at = m_database.result_column<UnixDateTime>(statement_id, 9),
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

    auto outcome = m_database.execute_interruptible_statement(
        m_statements.search_entries,
        [&](auto statement_id) {
            auto title = m_database.result_column<String>(statement_id, 1);
            auto favicon = m_database.result_column<String>(statement_id, 4);

            entries.append(HistoryEntry {
                .url = m_database.result_column<String>(statement_id, 0),
                .title = title.is_empty() ? Optional<String> {} : Optional<String> { move(title) },
                .favicon_base64_png = favicon.is_empty() ? Optional<String> {} : Optional<String> { move(favicon) },
                .visit_count = m_database.result_column<u64>(statement_id, 2),
                .direct_visit_count = m_database.result_column<u64>(statement_id, 5),
                .last_visited_time = m_database.result_column<UnixDateTime>(statement_id, 3),
                .last_qualifying_visit_time = m_database.result_column<UnixDateTime>(statement_id, 6),
                .last_direct_visit_time = m_database.result_column<UnixDateTime>(statement_id, 7),
                .decayed_visit_score = m_database.result_column<double>(statement_id, 8),
                .decayed_direct_score = m_database.result_column<double>(statement_id, 9),
                .score_updated_at = m_database.result_column<UnixDateTime>(statement_id, 10),
            });
        },
        url_query_string,
        url_contains_query_string,
        title_query_string,
        static_cast<i64>(limit));

    if (outcome == Database::Database::StatementExecutionOutcome::Interrupted)
        entries.clear();

    return entries;
}

Vector<HistoryEntry> HistoryStore::PersistedStorage::list_entries(StringView title_query, StringView url_query, size_t offset, size_t limit)
{
    Vector<HistoryEntry> entries;
    entries.ensure_capacity(limit);
    auto title_query_string = MUST(String::from_utf8(title_query));
    auto url_query_string = MUST(String::from_utf8(url_query));

    m_database.execute_statement(
        m_statements.list_entries,
        [&](auto statement_id) {
            auto title = m_database.result_column<String>(statement_id, 1);
            auto favicon = m_database.result_column<String>(statement_id, 4);

            entries.append(HistoryEntry {
                .url = m_database.result_column<String>(statement_id, 0),
                .title = title.is_empty() ? Optional<String> {} : Optional<String> { move(title) },
                .favicon_base64_png = favicon.is_empty() ? Optional<String> {} : Optional<String> { move(favicon) },
                .visit_count = m_database.result_column<u64>(statement_id, 2),
                .direct_visit_count = m_database.result_column<u64>(statement_id, 5),
                .last_visited_time = m_database.result_column<UnixDateTime>(statement_id, 3),
                .last_qualifying_visit_time = m_database.result_column<UnixDateTime>(statement_id, 6),
                .last_direct_visit_time = m_database.result_column<UnixDateTime>(statement_id, 7),
                .decayed_visit_score = m_database.result_column<double>(statement_id, 8),
                .decayed_direct_score = m_database.result_column<double>(statement_id, 9),
                .score_updated_at = m_database.result_column<UnixDateTime>(statement_id, 10),
            });
        },
        title_query_string,
        url_query_string,
        static_cast<i64>(limit),
        static_cast<i64>(offset));

    return entries;
}

void HistoryStore::PersistedStorage::record_omnibox_engagement(OmniboxEngagement const& engagement, UnixDateTime used_at)
{
    auto normalized_input = normalize_omnibox_input(engagement.input, engagement.destination_kind);
    auto destination = normalize_omnibox_destination(engagement.destination, engagement.destination_kind);
    if (normalized_input.is_empty() || destination.is_empty())
        return;

    m_database.execute_statement(
        m_statements.upsert_omnibox_engagement,
        {},
        normalized_input,
        static_cast<u8>(engagement.destination_kind),
        destination,
        destination,
        engagement.was_explicit ? 1u : 0u,
        engagement.was_explicit ? 0u : 1u,
        used_at);
}

Vector<StoredOmniboxEngagement> HistoryStore::PersistedStorage::omnibox_engagements(StringView normalized_url_input, StringView normalized_search_input, size_t limit)
{
    Vector<StoredOmniboxEngagement> results;
    auto outcome = m_database.execute_interruptible_statement(
        m_statements.search_omnibox_engagements,
        [&](auto statement_id) {
            results.append({
                .normalized_input = m_database.result_column<String>(statement_id, 0),
                .destination_kind = static_cast<OmniboxDestinationKind>(m_database.result_column<u8>(statement_id, 1)),
                .destination = m_database.result_column<String>(statement_id, 2),
                .explicit_use_count = m_database.result_column<u64>(statement_id, 3),
                .default_use_count = m_database.result_column<u64>(statement_id, 4),
                .last_used_time = m_database.result_column<UnixDateTime>(statement_id, 5),
            });
        },
        MUST(String::from_utf8(normalized_url_input)),
        MUST(String::from_utf8(normalized_search_input)),
        static_cast<i64>(limit));
    if (outcome == Database::Database::StatementExecutionOutcome::Interrupted)
        results.clear();
    return results;
}

void HistoryStore::PersistedStorage::remove_entry_for_url(String const& url, RemoveHistoryEntryEngagements remove_engagements)
{
    m_database.execute_statement(m_statements.delete_entry, {}, url);
    if (remove_engagements == RemoveHistoryEntryEngagements::Yes)
        m_database.execute_statement(m_statements.delete_omnibox_engagements_for_url, {}, url);
}

void HistoryStore::PersistedStorage::remove_entries_for_same_site(StringView site_key)
{
    Vector<String> urls_to_remove;

    m_database.execute_statement(
        m_statements.all_urls,
        [&](auto statement_id) {
            auto url = m_database.result_column<String>(statement_id, 0);
            if (history_entry_matches_site_key(url.bytes_as_string_view(), site_key))
                urls_to_remove.append(move(url));
        });

    for (auto const& url : urls_to_remove)
        remove_entry_for_url(url, RemoveHistoryEntryEngagements::Yes);
}

void HistoryStore::PersistedStorage::remove_entries_accessed_since(UnixDateTime since)
{
    m_database.execute_statement(m_statements.delete_entries_accessed_since, {}, since);
    m_database.execute_statement(m_statements.delete_omnibox_engagements_used_since, {}, since);
}

}
