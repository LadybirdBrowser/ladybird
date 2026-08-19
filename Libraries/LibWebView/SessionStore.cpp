/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Checked.h>
#include <AK/QuickSort.h>
#include <LibCore/Timer.h>
#include <LibDatabase/ResultRow.h>
#include <LibURL/InternalURLs.h>
#include <LibURL/Parser.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/SessionStore.h>

namespace WebView {

static constexpr u32 SESSIONS_SCHEMA_BASELINE_VERSION = 1u;
static constexpr u32 MAX_CLOSED_TABS = 25;
static constexpr u32 MAX_CLOSED_WINDOWS = 5;
static constexpr size_t MAX_ACTIVE_URL_BYTES = 8uz * 1024 * 1024;
static constexpr int FLUSH_TIMER_INTERVAL_MILLISECONDS = 30'000;

static i64 index_as_i64(size_t index)
{
    VERIFY(AK::is_within_range<i64>(index));
    return static_cast<i64>(index);
}

static Optional<size_t> index_as_size(i64 index)
{
    if (index < 0 || !AK::is_within_range<size_t>(index))
        return {};
    return static_cast<size_t>(index);
}

ErrorOr<Database::MigrationOutcome> SessionStore::PersistedStorage::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    auto migrations = to_array<Database::Migration>({
        {
            .version = SESSIONS_SCHEMA_BASELINE_VERSION,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS Sessions (
                    id INTEGER PRIMARY KEY,
                    kind INTEGER NOT NULL,
                    closed_time INTEGER NOT NULL,
                    close_sequence INTEGER NOT NULL DEFAULT 0,
                    source_window_id INTEGER,
                    source_tab_index INTEGER,
                    origin INTEGER NOT NULL DEFAULT 0,
                    active_tab_index INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS SessionTabs (
                    id INTEGER PRIMARY KEY,
                    session_id INTEGER NOT NULL REFERENCES Sessions(id) ON DELETE CASCADE,
                    tab_ordinal INTEGER NOT NULL,
                    active_url TEXT NOT NULL,
                    current_used_step_index INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS SessionUsedSteps (
                    tab_id INTEGER NOT NULL REFERENCES SessionTabs(id) ON DELETE CASCADE,
                    step_ordinal INTEGER NOT NULL,
                    step INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS SessionHistories (
                    id INTEGER PRIMARY KEY,
                    tab_id INTEGER NOT NULL REFERENCES SessionTabs(id) ON DELETE CASCADE
                );

                CREATE TABLE IF NOT EXISTS SessionNestedHistories (
                    parent_history_id INTEGER NOT NULL,
                    parent_entry_ordinal INTEGER NOT NULL,
                    nested_ordinal INTEGER NOT NULL,
                    nested_history_namespace_id INTEGER NOT NULL,
                    nested_history_local_id INTEGER NOT NULL,
                    child_history_id INTEGER NOT NULL UNIQUE REFERENCES SessionHistories(id) ON DELETE CASCADE,
                    FOREIGN KEY (parent_history_id, parent_entry_ordinal) REFERENCES SessionEntries(history_id, entry_ordinal) ON DELETE CASCADE
                );

                CREATE TABLE IF NOT EXISTS SessionEntries (
                    id INTEGER PRIMARY KEY,
                    history_id INTEGER NOT NULL REFERENCES SessionHistories(id) ON DELETE CASCADE,
                    entry_ordinal INTEGER NOT NULL,
                    step INTEGER NOT NULL,
                    url TEXT NOT NULL,
                    document_state_namespace_id INTEGER NOT NULL,
                    document_state_local_id INTEGER NOT NULL,
                    classic_state BLOB NOT NULL,
                    navigation_state BLOB NOT NULL,
                    navigation_api_key TEXT NOT NULL,
                    navigation_api_id TEXT NOT NULL,
                    scroll_restoration_mode INTEGER NOT NULL,
                    scroll_x_raw INTEGER,
                    scroll_y_raw INTEGER,
                    origin_kind INTEGER NOT NULL,
                    origin_nonce BLOB,
                    origin_scheme TEXT,
                    origin_host TEXT,
                    origin_port INTEGER,
                    origin_domain TEXT,
                    initiator_origin_kind INTEGER NOT NULL,
                    initiator_origin_nonce BLOB,
                    initiator_origin_scheme TEXT,
                    initiator_origin_host TEXT,
                    initiator_origin_port INTEGER,
                    initiator_origin_domain TEXT,
                    referrer_kind INTEGER NOT NULL,
                    referrer_url TEXT,
                    referrer_policy INTEGER NOT NULL,
                    resource_kind INTEGER NOT NULL,
                    resource_string TEXT,
                    about_base_url TEXT,
                    navigable_target_name TEXT NOT NULL,
                    ever_populated INTEGER NOT NULL,
                    reload_pending INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS SessionPolicyContainers (
                    id INTEGER PRIMARY KEY,
                    history_id INTEGER NOT NULL,
                    entry_ordinal INTEGER NOT NULL,
                    referrer_policy INTEGER NOT NULL,
                    embedder_policy_value INTEGER NOT NULL,
                    embedder_policy_report_only_value INTEGER NOT NULL,
                    embedder_policy_reporting_endpoint TEXT NOT NULL,
                    embedder_policy_report_only_reporting_endpoint TEXT NOT NULL,
                    FOREIGN KEY (history_id, entry_ordinal) REFERENCES SessionEntries(history_id, entry_ordinal) ON DELETE CASCADE
                );

                CREATE TABLE IF NOT EXISTS SessionCspPolicies (
                    id INTEGER PRIMARY KEY,
                    container_id INTEGER NOT NULL REFERENCES SessionPolicyContainers(id) ON DELETE CASCADE,
                    policy_ordinal INTEGER NOT NULL,
                    disposition INTEGER NOT NULL,
                    source INTEGER NOT NULL,
                    self_origin_kind INTEGER NOT NULL,
                    self_origin_nonce BLOB,
                    self_origin_scheme TEXT,
                    self_origin_host TEXT,
                    self_origin_port INTEGER,
                    self_origin_domain TEXT,
                    pre_parsed_policy_string TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS SessionCspDirectives (
                    id INTEGER PRIMARY KEY,
                    policy_id INTEGER NOT NULL REFERENCES SessionCspPolicies(id) ON DELETE CASCADE,
                    directive_ordinal INTEGER NOT NULL,
                    name TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS SessionCspDirectiveValues (
                    directive_id INTEGER NOT NULL REFERENCES SessionCspDirectives(id) ON DELETE CASCADE,
                    value_ordinal INTEGER NOT NULL,
                    value TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS SessionsRetentionIndex
                ON Sessions(kind, close_sequence DESC, closed_time DESC, id DESC);

                CREATE INDEX IF NOT EXISTS SessionTabsBySessionIndex
                ON SessionTabs(session_id);

                CREATE UNIQUE INDEX IF NOT EXISTS SessionUsedStepsByTabIndex
                ON SessionUsedSteps(tab_id, step_ordinal);

                CREATE INDEX IF NOT EXISTS SessionHistoriesByTabIndex
                ON SessionHistories(tab_id);

                CREATE UNIQUE INDEX IF NOT EXISTS SessionNestedHistoriesByParentIndex
                ON SessionNestedHistories(parent_history_id, parent_entry_ordinal, nested_ordinal);

                CREATE UNIQUE INDEX IF NOT EXISTS SessionEntriesByHistoryIndex
                ON SessionEntries(history_id, entry_ordinal);

                CREATE UNIQUE INDEX IF NOT EXISTS SessionPolicyContainersByHistoryIndex
                ON SessionPolicyContainers(history_id, entry_ordinal);

                CREATE UNIQUE INDEX IF NOT EXISTS SessionCspPoliciesByContainerIndex
                ON SessionCspPolicies(container_id, policy_ordinal);

                CREATE UNIQUE INDEX IF NOT EXISTS SessionCspDirectivesByPolicyIndex
                ON SessionCspDirectives(policy_id, directive_ordinal);

                CREATE UNIQUE INDEX IF NOT EXISTS SessionCspDirectiveValuesByDirectiveIndex
                ON SessionCspDirectiveValues(directive_id, value_ordinal);
            )#"sv,
        },
    });

    return database.migrate("Sessions"sv, migrations, mode);
}

ErrorOr<Database::MigrationOutcome> SessionStore::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    return PersistedStorage::migrate_schema(database, mode);
}

ErrorOr<SessionStore::PersistedStorage> SessionStore::PersistedStorage::create(Database::Database& database)
{
    if (database.options().foreign_keys != Database::Database::ForeignKeys::Yes)
        return Error::from_string_literal("Session storage relies on cascading deletes; open the connection with foreign keys enabled");

    Statements statements {};
    statements.insert_session = TRY(database.prepare_statement("INSERT INTO Sessions (id, kind, closed_time, close_sequence, source_window_id, source_tab_index, origin, active_tab_index) VALUES (:id, :kind, :closed_time, :close_sequence, :source_window_id, :source_tab_index, :origin, :active_tab_index);"sv));
    statements.insert_tab = TRY(database.prepare_statement("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (:id, :session_id, :tab_ordinal, :active_url, :current_used_step_index);"sv));
    statements.insert_tab_if_missing = TRY(database.prepare_statement("INSERT OR IGNORE INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (:id, :session_id, :tab_ordinal, :active_url, :current_used_step_index);"sv));
    statements.update_tab_row = TRY(database.prepare_statement("UPDATE SessionTabs SET active_url = :active_url, current_used_step_index = :current_used_step_index WHERE id = :id;"sv));
    statements.update_tab_parent = TRY(database.prepare_statement("UPDATE SessionTabs SET session_id = :session_id, tab_ordinal = :tab_ordinal WHERE id = :id;"sv));
    statements.update_session_closed = TRY(database.prepare_statement("UPDATE Sessions SET kind = :kind, closed_time = :closed_time, close_sequence = :close_sequence, source_window_id = :source_window_id, source_tab_index = :source_tab_index, origin = :origin, active_tab_index = :active_tab_index WHERE id = :id;"sv));
    statements.update_session_active_tab_index = TRY(database.prepare_statement("UPDATE Sessions SET active_tab_index = :active_tab_index WHERE id = :id;"sv));
    statements.delete_session = TRY(database.prepare_statement("DELETE FROM Sessions WHERE id = :id;"sv));
    statements.delete_tab = TRY(database.prepare_statement("DELETE FROM SessionTabs WHERE id = :id;"sv));
    statements.prune_closed = TRY(database.prepare_statement(R"#(
        DELETE FROM Sessions WHERE kind = :kind AND id NOT IN (
            SELECT id FROM Sessions WHERE kind = :kind ORDER BY close_sequence DESC, closed_time DESC, id DESC LIMIT :max_units);
    )#"sv));
    statements.select_max_session_id = TRY(database.prepare_statement("SELECT MAX(id) AS maximum_id FROM Sessions;"sv));
    statements.select_max_tab_id = TRY(database.prepare_statement("SELECT MAX(id) AS maximum_id FROM SessionTabs;"sv));
    statements.select_max_close_sequence = TRY(database.prepare_statement("SELECT MAX(close_sequence) AS maximum_id FROM Sessions;"sv));
    statements.select_closed_sessions = TRY(database.prepare_statement("SELECT id, closed_time, close_sequence, source_window_id, source_tab_index, origin, active_tab_index FROM Sessions WHERE kind = :kind ORDER BY close_sequence DESC, closed_time DESC, id DESC LIMIT :max_units;"sv));
    statements.select_session_tabs = TRY(database.prepare_statement("SELECT id, tab_ordinal, active_url, current_used_step_index FROM SessionTabs WHERE session_id = :session_id ORDER BY tab_ordinal;"sv));
    statements.select_open_sessions = TRY(database.prepare_statement("SELECT id FROM Sessions WHERE kind = :open_window_kind ORDER BY id;"sv));
    statements.recover_open_session = TRY(database.prepare_statement("UPDATE Sessions SET kind = :closed_window_kind, closed_time = :closed_time, close_sequence = :close_sequence, source_window_id = NULL, source_tab_index = NULL, origin = :origin WHERE id = :id;"sv));
    statements.delete_empty_open_sessions = TRY(database.prepare_statement("DELETE FROM Sessions WHERE kind = :open_window_kind AND NOT EXISTS (SELECT 1 FROM SessionTabs WHERE session_id = Sessions.id);"sv));
    statements.snapshot_statements = TRY(prepare_session_history_snapshot_statements(database));

    return PersistedStorage { database, move(statements) };
}

ErrorOr<NonnullOwnPtr<SessionStore>> SessionStore::create(Database::Database& database)
{
    auto persisted_storage = TRY(PersistedStorage::create(database));
    auto store = adopt_own(*new SessionStore(move(persisted_storage)));
    TRY(store->run_startup_recovery(UnixDateTime::now()));

    store->m_flush_timer = Core::Timer::create_repeating(FLUSH_TIMER_INTERVAL_MILLISECONDS, [store = store.ptr()]() {
        store->flush_dirty_state();
    });
    store->m_flush_timer->start();

    return store;
}

NonnullOwnPtr<SessionStore> SessionStore::create()
{
    return adopt_own(*new SessionStore({}));
}

SessionStore::SessionStore(Optional<PersistedStorage> persisted_storage)
    : m_persisted_storage(move(persisted_storage))
{
}

SessionStore::~SessionStore()
{
    if (m_flush_timer)
        m_flush_timer->stop();
    on_closed_units_changed = nullptr;
    flush_dirty_state();
}

void SessionStore::TransientStorage::open_window(SessionWindowId window_id)
{
    m_windows.set(window_id, OpenWindow {});
}

Optional<size_t> SessionStore::TransientStorage::open_tab(SessionWindowId window_id, SessionTabId tab_id, OpenTab tab, Optional<size_t> insertion_index)
{
    auto window = m_windows.find(window_id);
    if (window == m_windows.end())
        return {};

    auto ordinal = min(insertion_index.value_or(window->value.tabs.size()), window->value.tabs.size());
    window->value.tabs.insert(ordinal, tab_id);
    m_tabs.set(tab_id, move(tab));
    return ordinal;
}

SessionStore::OpenTab const* SessionStore::TransientStorage::find_tab(SessionTabId tab_id) const
{
    auto tab = m_tabs.find(tab_id);
    return tab == m_tabs.end() ? nullptr : &tab->value;
}

SessionStore::OpenWindow const* SessionStore::TransientStorage::find_window(SessionWindowId window_id) const
{
    auto window = m_windows.find(window_id);
    return window == m_windows.end() ? nullptr : &window->value;
}

Optional<size_t> SessionStore::TransientStorage::tab_index(SessionTabId tab_id) const
{
    auto const* tab = find_tab(tab_id);
    if (!tab)
        return {};
    auto const* window = find_window(tab->window_id);
    if (!window)
        return {};
    return window->tabs.find_first_index(tab_id);
}

void SessionStore::TransientStorage::set_tab_state(SessionTabId tab_id, Optional<SessionHistorySnapshot> history, URL::URL url, bool dirty)
{
    auto tab = m_tabs.find(tab_id);
    if (tab == m_tabs.end())
        return;

    tab->value.url = move(url);
    tab->value.history = move(history);
    tab->value.current_used_step_index = tab->value.history.has_value() ? tab->value.history->current_used_step_index : 0;
    tab->value.dirty = dirty;
}

void SessionStore::TransientStorage::mark_tab_flushed(SessionTabId tab_id, size_t current_used_step_index)
{
    auto tab = m_tabs.find(tab_id);
    if (tab == m_tabs.end())
        return;

    tab->value.dirty = false;
    tab->value.current_used_step_index = current_used_step_index;
    tab->value.history.clear();
}

Optional<SessionHistorySnapshot> SessionStore::TransientStorage::take_tab_history(SessionTabId tab_id)
{
    auto tab = m_tabs.find(tab_id);
    if (tab == m_tabs.end())
        return {};

    auto history = move(tab->value.history);
    tab->value.history.clear();
    return history;
}

void SessionStore::TransientStorage::set_active_tab(SessionTabId tab_id)
{
    auto tab = m_tabs.find(tab_id);
    if (tab == m_tabs.end())
        return;

    auto window = m_windows.find(tab->value.window_id);
    if (window == m_windows.end())
        return;
    if (window->value.active_tab == tab_id)
        return;

    window->value.active_tab = tab_id;
    window->value.metadata_dirty = true;
}

void SessionStore::TransientStorage::set_tab_order(SessionWindowId window_id, Vector<SessionTabId> const& ordered_tabs)
{
    auto window = m_windows.find(window_id);
    if (window == m_windows.end())
        return;

    Vector<SessionTabId> tabs;
    tabs.ensure_capacity(ordered_tabs.size());
    for (auto tab_id : ordered_tabs) {
        auto tab = m_tabs.find(tab_id);
        if (tab != m_tabs.end() && tab->value.window_id == window_id)
            tabs.append(tab_id);
    }
    if (window->value.tabs == tabs)
        return;
    window->value.tabs = move(tabs);
    window->value.metadata_dirty = true;
}

Optional<SessionWindowId> SessionStore::TransientStorage::move_tab(SessionTabId tab_id, SessionWindowId new_window_id, size_t ordinal)
{
    auto tab = m_tabs.find(tab_id);
    if (tab == m_tabs.end())
        return {};

    auto new_window = m_windows.find(new_window_id);
    if (new_window == m_windows.end())
        return {};

    auto old_window_id = tab->value.window_id;
    bool was_active = false;
    if (auto old_window = m_windows.find(old_window_id); old_window != m_windows.end()) {
        old_window->value.tabs.remove_all_matching([&](auto candidate) { return candidate == tab_id; });
        if (old_window->value.active_tab == tab_id) {
            was_active = true;
            old_window->value.active_tab = 0;
        }
        old_window->value.metadata_dirty = true;
    }

    auto insert_at = min(ordinal, new_window->value.tabs.size());
    new_window->value.tabs.insert(insert_at, tab_id);
    tab->value.window_id = new_window_id;
    if (was_active && old_window_id == new_window_id)
        new_window->value.active_tab = tab_id;
    new_window->value.metadata_dirty = true;
    return old_window_id;
}

void SessionStore::TransientStorage::remove_tab(SessionTabId tab_id)
{
    auto tab = m_tabs.find(tab_id);
    if (tab == m_tabs.end())
        return;

    if (auto window = m_windows.find(tab->value.window_id); window != m_windows.end()) {
        window->value.tabs.remove_all_matching([&](auto candidate) { return candidate == tab_id; });
        if (window->value.active_tab == tab_id)
            window->value.active_tab = 0;
        window->value.metadata_dirty = true;
    }
    m_tabs.remove(tab_id);
}

void SessionStore::TransientStorage::remove_window(SessionWindowId window_id)
{
    auto window = m_windows.find(window_id);
    if (window == m_windows.end())
        return;

    for (auto tab_id : window->value.tabs)
        m_tabs.remove(tab_id);
    m_windows.remove(window_id);
    if (default_window == window_id)
        default_window.clear();
}

void SessionStore::TransientStorage::mark_window_metadata_clean(SessionWindowId window_id)
{
    auto window = m_windows.find(window_id);
    if (window == m_windows.end())
        return;
    window->value.metadata_dirty = false;
}

void SessionStore::TransientStorage::set_closed_units(Vector<ClosedUnit> closed_units)
{
    m_closed_units = move(closed_units);
}

void SessionStore::TransientStorage::append_closed_unit(ClosedUnit unit)
{
    // A retried close can complete after newer closes.
    size_t insertion_index = m_closed_units.size();
    while (insertion_index > 0) {
        auto const& previous = m_closed_units[insertion_index - 1];
        if (previous.close_sequence < unit.close_sequence)
            break;
        if (previous.close_sequence == unit.close_sequence && previous.closed_time < unit.closed_time)
            break;
        if (previous.close_sequence == unit.close_sequence && previous.closed_time == unit.closed_time && previous.session_id < unit.session_id)
            break;
        --insertion_index;
    }
    m_closed_units.insert(insertion_index, move(unit));
}

SessionStore::ClosedUnit const* SessionStore::TransientStorage::last_closed_unit() const
{
    return m_closed_units.is_empty() ? nullptr : &m_closed_units.last();
}

SessionStore::ClosedUnit SessionStore::TransientStorage::take_last_closed_unit()
{
    return m_closed_units.take_last();
}

void SessionStore::TransientStorage::prune_closed_units(UnitKind kind, u32 max_units)
{
    size_t count = 0;
    for (auto const& unit : m_closed_units) {
        if (unit.kind == kind)
            ++count;
    }
    for (size_t i = 0; count > max_units && i < m_closed_units.size();) {
        if (m_closed_units[i].kind == kind) {
            m_closed_units.remove(i);
            --count;
        } else {
            ++i;
        }
    }
}

Vector<SessionWindowId> SessionStore::TransientStorage::remove_closed_units_since(UnixDateTime since)
{
    Vector<SessionWindowId> removed_units;
    m_closed_units.remove_all_matching([&](auto const& unit) {
        if (unit.closed_time < since)
            return false;
        removed_units.append(unit.session_id);
        return true;
    });
    return removed_units;
}

// The bounded active_url reads reject an over-cap cell, so writes past the cap durably degrade to
// about:blank; the transient plane keeps the real URL for in-session reopens.
static String persistable_active_url(URL::URL const& url)
{
    auto serialized = url.serialize();
    if (serialized.byte_count() <= MAX_ACTIVE_URL_BYTES)
        return serialized;
    if (history_debug_enabled())
        dbgln("[History] Active URL exceeds the persisted cap; storing about:blank");
    return URL::about_blank().serialize();
}

ErrorOr<void> SessionStore::PersistedStorage::apply_tab_row_write(TabWrite const& write)
{
    switch (write.snapshot_disposition) {
    case SnapshotDisposition::Preserve:
        break;
    case SnapshotDisposition::Replace:
        TRY(delete_session_history_snapshot(database, statements.snapshot_statements, write.id));
        TRY(store_session_history_snapshot(database, statements.snapshot_statements, write.id, *write.snapshot));
        break;
    case SnapshotDisposition::Clear:
        TRY(delete_session_history_snapshot(database, statements.snapshot_statements, write.id));
        break;
    }

    TRY(database.try_execute_bound_statement(statements.update_tab_row, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("active_url"sv, persistable_active_url(write.url)));
        TRY(bind("current_used_step_index"sv, write.current_used_step_index));
        TRY(bind("id"sv, write.id));
        return {};
    }));
    return {};
}

ErrorOr<void> SessionStore::PersistedStorage::apply_window_metadata(WindowMetadataWrite const& write)
{
    i64 tab_ordinal = 0;
    for (auto tab_id : write.tabs_in_order) {
        TRY(database.try_execute_bound_statement(statements.update_tab_parent, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("session_id"sv, write.id));
            TRY(bind("tab_ordinal"sv, tab_ordinal));
            TRY(bind("id"sv, tab_id));
            return {};
        }));
        ++tab_ordinal;
    }

    TRY(database.try_execute_bound_statement(statements.update_session_active_tab_index, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("active_tab_index"sv, write.active_tab_index));
        TRY(bind("id"sv, write.id));
        return {};
    }));
    return {};
}

ErrorOr<void> SessionStore::PersistedStorage::insert_closed_unit_row(ClosedUnitWrite const& write)
{
    return database.try_execute_bound_statement(statements.insert_session, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("id"sv, write.id));
        TRY(bind("kind"sv, to_underlying(write.kind)));
        TRY(bind("closed_time"sv, write.closed_at));
        TRY(bind("close_sequence"sv, write.close_sequence));
        TRY(bind("source_window_id"sv, write.source_window_id));
        TRY(bind("source_tab_index"sv, write.source_tab_index));
        TRY(bind("origin"sv, to_underlying(write.origin)));
        TRY(bind("active_tab_index"sv, write.active_tab_index));
        return {};
    });
}

ErrorOr<void> SessionStore::PersistedStorage::delete_tab_rows(SessionTabId tab_id)
{
    return database.try_execute_bound_statement(statements.delete_tab, [&](auto& bind) -> ErrorOr<void> {
        return bind("id"sv, tab_id);
    });
}

ErrorOr<void> SessionStore::PersistedStorage::insert_open_window(SessionWindowId window_id)
{
    return database.try_execute_bound_statement(statements.insert_session, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("id"sv, window_id));
        TRY(bind("kind"sv, to_underlying(UnitKind::OpenWindow)));
        TRY(bind("closed_time"sv, 0));
        TRY(bind("close_sequence"sv, 0));
        TRY(bind("source_window_id"sv, Optional<SessionWindowId> {}));
        TRY(bind("source_tab_index"sv, Optional<i64> {}));
        TRY(bind("origin"sv, to_underlying(ClosedSessionUnit::Origin::CloseAction)));
        TRY(bind("active_tab_index"sv, 0));
        return {};
    });
}

ErrorOr<void> SessionStore::PersistedStorage::open_tab(OpenTabWrite const& write)
{
    return database.transaction([&]() -> ErrorOr<void> {
        TRY(database.try_execute_bound_statement(statements.insert_tab, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("id"sv, write.tab.id));
            TRY(bind("session_id"sv, write.tab.window_id));
            TRY(bind("tab_ordinal"sv, write.tab.tab_ordinal));
            TRY(bind("active_url"sv, persistable_active_url(write.tab.url)));
            TRY(bind("current_used_step_index"sv, write.tab.current_used_step_index));
            return {};
        }));
        return apply_window_metadata(write.window);
    });
}

ErrorOr<void> SessionStore::PersistedStorage::flush_tab(TabWrite const& write)
{
    return database.transaction([&]() -> ErrorOr<void> {
        return apply_tab_row_write(write);
    });
}

ErrorOr<void> SessionStore::PersistedStorage::write_windows_metadata(Vector<WindowMetadataWrite> const& writes)
{
    return database.transaction([&]() -> ErrorOr<void> {
        for (auto const& write : writes)
            TRY(apply_window_metadata(write));
        return {};
    });
}

ErrorOr<void> SessionStore::PersistedStorage::close_tab(ClosedTabWrite const& write)
{
    return database.transaction([&]() -> ErrorOr<void> {
        TRY(insert_closed_unit_row(write.unit));
        // Retried closes may no longer have their original tab row.
        TRY(database.try_execute_bound_statement(statements.insert_tab_if_missing, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("id"sv, write.tab.id));
            TRY(bind("session_id"sv, write.unit.id));
            TRY(bind("tab_ordinal"sv, 0));
            TRY(bind("active_url"sv, persistable_active_url(write.tab.url)));
            TRY(bind("current_used_step_index"sv, write.tab.current_used_step_index));
            return {};
        }));
        TRY(database.try_execute_bound_statement(statements.update_tab_parent, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("session_id"sv, write.unit.id));
            TRY(bind("tab_ordinal"sv, 0));
            TRY(bind("id"sv, write.tab.id));
            return {};
        }));
        TRY(apply_tab_row_write(write.tab));
        if (write.remaining_window.has_value())
            TRY(apply_window_metadata(*write.remaining_window));
        return prune_closed_units(UnitKind::ClosedTab, write.retention_limit);
    });
}

ErrorOr<void> SessionStore::PersistedStorage::close_window(ClosedWindowWrite const& write)
{
    return database.transaction([&]() -> ErrorOr<void> {
        i64 tab_ordinal = 0;
        for (auto const& member : write.members) {
            TRY(apply_tab_row_write(member));
            TRY(database.try_execute_bound_statement(statements.update_tab_parent, [&](auto& bind) -> ErrorOr<void> {
                TRY(bind("session_id"sv, write.unit.id));
                TRY(bind("tab_ordinal"sv, tab_ordinal));
                TRY(bind("id"sv, member.id));
                return {};
            }));
            ++tab_ordinal;
        }
        TRY(database.try_execute_bound_statement(statements.update_session_closed, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("kind"sv, to_underlying(write.unit.kind)));
            TRY(bind("closed_time"sv, write.unit.closed_at));
            TRY(bind("close_sequence"sv, write.unit.close_sequence));
            TRY(bind("source_window_id"sv, write.unit.source_window_id));
            TRY(bind("source_tab_index"sv, write.unit.source_tab_index));
            TRY(bind("origin"sv, to_underlying(write.unit.origin)));
            TRY(bind("active_tab_index"sv, write.unit.active_tab_index));
            TRY(bind("id"sv, write.unit.id));
            return {};
        }));
        return prune_closed_units(UnitKind::ClosedWindow, write.retention_limit);
    });
}

ErrorOr<void> SessionStore::PersistedStorage::discard_tab(DiscardTabWrite const& write)
{
    return database.transaction([&]() -> ErrorOr<void> {
        TRY(delete_tab_rows(write.id));
        if (write.remaining_window.has_value())
            TRY(apply_window_metadata(*write.remaining_window));
        return {};
    });
}

ErrorOr<void> SessionStore::PersistedStorage::clear_closed_units(ClearClosedUnitsWrite const& write)
{
    return database.transaction([&]() -> ErrorOr<void> {
        for (auto closed_unit : write.closed_units) {
            TRY(database.try_execute_bound_statement(statements.delete_session, [&](auto& bind) -> ErrorOr<void> {
                return bind("id"sv, closed_unit);
            }));
        }
        for (auto const& pending_tab : write.pending_tabs) {
            TRY(delete_tab_rows(pending_tab.id));
            if (pending_tab.remaining_window.has_value())
                TRY(apply_window_metadata(*pending_tab.remaining_window));
        }
        for (auto pending_window : write.pending_windows) {
            TRY(database.try_execute_bound_statement(statements.delete_session, [&](auto& bind) -> ErrorOr<void> {
                return bind("id"sv, pending_window);
            }));
        }
        return {};
    });
}

ErrorOr<void> SessionStore::PersistedStorage::recover_open_windows(RecoveryWrite const& write, IdAllocator& close_sequence_allocator)
{
    return database.transaction([&]() -> ErrorOr<void> {
        TRY(database.try_execute_bound_statement(statements.delete_empty_open_sessions, [&](auto& bind) -> ErrorOr<void> {
            return bind("open_window_kind"sv, to_underlying(UnitKind::OpenWindow));
        }));

        Vector<SessionWindowId> open_windows;
        TRY(database.try_execute_bound_statement(
            statements.select_open_sessions,
            [&](auto& bind) -> ErrorOr<void> {
                return bind("open_window_kind"sv, to_underlying(UnitKind::OpenWindow));
            },
            [&](Database::ResultRow& row) -> ErrorOr<void> {
                open_windows.append(TRY(row.read_integer<SessionWindowId>("id"sv)));
                return {};
            }));
        for (auto window_id : open_windows) {
            auto close_sequence = TRY(close_sequence_allocator.allocate());
            TRY(database.try_execute_bound_statement(statements.recover_open_session, [&](auto& bind) -> ErrorOr<void> {
                TRY(bind("closed_window_kind"sv, to_underlying(UnitKind::ClosedWindow)));
                TRY(bind("closed_time"sv, write.recovered_at));
                TRY(bind("close_sequence"sv, close_sequence));
                TRY(bind("origin"sv, to_underlying(ClosedSessionUnit::Origin::StartupRecovery)));
                TRY(bind("id"sv, window_id));
                return {};
            }));
        }
        TRY(prune_closed_units(UnitKind::ClosedTab, write.closed_tab_limit));
        return prune_closed_units(UnitKind::ClosedWindow, write.closed_window_limit);
    });
}

ErrorOr<void> SessionStore::PersistedStorage::delete_session_rows(SessionWindowId window_id)
{
    return database.try_execute_bound_statement(statements.delete_session, [&](auto& bind) -> ErrorOr<void> {
        return bind("id"sv, window_id);
    });
}

ErrorOr<void> SessionStore::PersistedStorage::prune_closed_units(UnitKind kind, u32 max_units)
{
    return database.try_execute_bound_statement(statements.prune_closed, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("kind"sv, to_underlying(kind)));
        TRY(bind("max_units"sv, max_units));
        return {};
    });
}

ErrorOr<SessionStore::IdAllocator> SessionStore::IdAllocator::create_from_maximum(Database::Database& database, Database::StatementID statement_id)
{
    auto maximum = TRY(database.try_execute_bound_statement_one<Optional<i64>>(
        statement_id,
        [](auto&) -> ErrorOr<void> { return {}; },
        [](Database::ResultRow& row) -> ErrorOr<Optional<i64>> {
            return row.read_optional_integer<i64>("maximum_id"sv);
        }));
    return IdAllocator { maximum.value_or(0) };
}

ErrorOr<i64> SessionStore::IdAllocator::allocate()
{
    Checked<i64> next_id = m_last_used;
    next_id += 1;
    if (next_id.has_overflow())
        return Error::from_string_literal("Id allocator has exhausted its id space");
    m_last_used = next_id.value();
    return m_last_used;
}

ErrorOr<SessionStore::IdAllocators> SessionStore::PersistedStorage::create_id_allocators()
{
    return IdAllocators {
        .session_ids = TRY(IdAllocator::create_from_maximum(database, statements.select_max_session_id)),
        .tab_ids = TRY(IdAllocator::create_from_maximum(database, statements.select_max_tab_id)),
        .close_sequences = TRY(IdAllocator::create_from_maximum(database, statements.select_max_close_sequence)),
    };
}

ErrorOr<SessionHistorySnapshot> SessionStore::PersistedStorage::load_tab_snapshot(SessionTabId tab_id, size_t current_used_step_index)
{
    return load_session_history_snapshot(database, statements.snapshot_statements, tab_id, current_used_step_index);
}

ErrorOr<Vector<SessionStore::ClosedUnit>> SessionStore::PersistedStorage::load_closed_units()
{
    // Buffered so the invalid-unit deletes below never run while this select is still stepping.
    struct SessionRow {
        Optional<i64> session_id;
        Optional<UnixDateTime> closed_time;
        Optional<SessionCloseSequence> close_sequence;
        Optional<SessionWindowId> source_window_id;
        Optional<i64> source_tab_index;
        Optional<ClosedSessionUnit::Origin> origin;
        // Empty when the stored index is not a usable one, which retires only this unit.
        Optional<i64> active_tab_index;
    };

    Vector<ClosedUnit> units;
    for (auto kind : { UnitKind::ClosedTab, UnitKind::ClosedWindow }) {
        Vector<SessionRow> session_rows;
        TRY(database.try_execute_bound_statement(
            statements.select_closed_sessions,
            [&](auto& bind) -> ErrorOr<void> {
                TRY(bind("kind"sv, to_underlying(kind)));
                TRY(bind("max_units"sv, kind == UnitKind::ClosedTab ? MAX_CLOSED_TABS : MAX_CLOSED_WINDOWS));
                return {};
            },
            [&](Database::ResultRow& row) -> ErrorOr<void> {
                auto active_tab_index = row.read_integer<i64>("active_tab_index"sv);
                auto close_sequence = row.read_integer<SessionCloseSequence>("close_sequence"sv);
                auto source_window_id = row.read_optional_integer<SessionWindowId>("source_window_id"sv);
                auto source_tab_index = row.read_optional_integer<i64>("source_tab_index"sv);
                auto origin = row.read_integer<i64>("origin"sv);
                auto session_id = row.read_integer<i64>("id"sv);
                auto closed_time = row.read_integer<i64>("closed_time"sv);

                Optional<ClosedSessionUnit::Origin> validated_origin;
                if (!origin.is_error()) {
                    if (origin.value() == to_underlying(ClosedSessionUnit::Origin::CloseAction))
                        validated_origin = ClosedSessionUnit::Origin::CloseAction;
                    else if (origin.value() == to_underlying(ClosedSessionUnit::Origin::StartupRecovery))
                        validated_origin = ClosedSessionUnit::Origin::StartupRecovery;
                }

                session_rows.append({
                    .session_id = !session_id.is_error() ? Optional<i64> { session_id.value() } : Optional<i64> {},
                    .closed_time = !closed_time.is_error() ? Optional<UnixDateTime> { UnixDateTime::from_milliseconds_since_epoch(closed_time.value()) } : Optional<UnixDateTime> {},
                    .close_sequence = !close_sequence.is_error() && close_sequence.value() >= 0 ? Optional<SessionCloseSequence> { close_sequence.value() } : Optional<SessionCloseSequence> {},
                    .source_window_id = !source_window_id.is_error() && source_window_id.value().value_or(0) > 0 ? source_window_id.value() : Optional<SessionWindowId> {},
                    .source_tab_index = !source_tab_index.is_error() && source_tab_index.value().value_or(-1) >= 0 ? source_tab_index.value() : Optional<i64> {},
                    .origin = validated_origin,
                    .active_tab_index = !active_tab_index.is_error() && active_tab_index.value() >= 0 ? Optional<i64> { active_tab_index.value() } : Optional<i64> {},
                });
                return {};
            }));

        for (auto const& session_row : session_rows) {
            if (!session_row.session_id.has_value())
                continue;

            Vector<ClosedTab> tabs;
            bool unit_invalid = false;
            auto tabs_result = database.try_execute_bound_statement(
                statements.select_session_tabs,
                [&](auto& bind) -> ErrorOr<void> {
                    return bind("session_id"sv, *session_row.session_id);
                },
                [&](Database::ResultRow& row) -> ErrorOr<void> {
                    auto row_result = [&]() -> ErrorOr<void> {
                        if (TRY(row.read_integer<size_t>("tab_ordinal"sv)) != tabs.size())
                            return Error::from_string_literal("Closed session tab ordinals are not contiguous");
                        auto current_used_step_index = TRY(row.read_integer<size_t>("current_used_step_index"sv));
                        auto url = URL::Parser::basic_parse(TRY(row.read_text("active_url"sv, MAX_ACTIVE_URL_BYTES)));
                        if (!url.has_value())
                            return Error::from_string_literal("Closed session tab has an unparseable active url");
                        tabs.append({
                            .active_url = url.release_value(),
                            .tab_id = TRY(row.read_integer<i64>("id"sv)),
                            .current_used_step_index = current_used_step_index,
                            .history = {},
                        });
                        return {};
                    }();
                    if (row_result.is_error())
                        unit_invalid = true;
                    return row_result;
                });

            // Only row-validation failures retire the unit.
            if (tabs_result.is_error() && !unit_invalid)
                return tabs_result.release_error();

            if (unit_invalid || tabs.is_empty() || !session_row.closed_time.has_value() || !session_row.close_sequence.has_value() || !session_row.origin.has_value() || !session_row.active_tab_index.has_value()) {
                // Cleanup is best-effort after the invalid unit leaves the mirror.
                (void)database.try_execute_bound_statement(statements.delete_session, [&](auto& bind) -> ErrorOr<void> {
                    return bind("id"sv, *session_row.session_id);
                });
                continue;
            }

            units.append({
                .session_id = *session_row.session_id,
                .kind = kind,
                .closed_time = *session_row.closed_time,
                .close_sequence = *session_row.close_sequence,
                // A crash can leave the persisted index pointing past tabs that closed before a flush.
                .active_tab_index = min(*session_row.active_tab_index, index_as_i64(tabs.size() - 1)),
                .source_window_id = session_row.source_window_id,
                .source_tab_index = session_row.source_tab_index,
                .origin = *session_row.origin,
                .tabs = move(tabs),
            });
        }
    }

    quick_sort(units, [](auto const& a, auto const& b) {
        if (a.close_sequence != b.close_sequence)
            return a.close_sequence < b.close_sequence;
        if (a.closed_time != b.closed_time)
            return a.closed_time < b.closed_time;
        return a.session_id < b.session_id;
    });
    return units;
}

ErrorOr<Vector<ClosedSessionTab>> SessionStore::PersistedStorage::take_closed_unit(SessionWindowId unit_id)
{
    Vector<ClosedSessionTab> tabs;
    TRY(database.transaction([&]() -> ErrorOr<void> {
        struct TabRow {
            SessionTabId tab_id { 0 };
            URL::URL active_url;
            size_t current_used_step_index { 0 };
        };
        Vector<TabRow> tab_rows;
        TRY(database.try_execute_bound_statement(
            statements.select_session_tabs,
            [&](auto& bind) -> ErrorOr<void> {
                return bind("session_id"sv, unit_id);
            },
            [&](Database::ResultRow& row) -> ErrorOr<void> {
                auto validated_row = [&]() -> ErrorOr<TabRow> {
                    auto tab_id = TRY(row.read_integer<i64>("id"sv));
                    auto current_used_step_index = TRY(row.read_integer<size_t>("current_used_step_index"sv));
                    auto url = URL::Parser::basic_parse(TRY(row.read_text("active_url"sv, MAX_ACTIVE_URL_BYTES)));
                    if (!url.has_value())
                        return Error::from_string_literal("Closed session tab row is not restorable");
                    return TabRow {
                        .tab_id = tab_id,
                        .active_url = url.release_value(),
                        .current_used_step_index = current_used_step_index,
                    };
                }();
                // Skip corrupt tabs without making the closed unit unreopenable.
                if (!validated_row.is_error())
                    tab_rows.append(validated_row.release_value());
                return {};
            }));

        for (auto const& tab_row : tab_rows) {
            auto history = load_session_history_snapshot(database, statements.snapshot_statements, tab_row.tab_id, tab_row.current_used_step_index);
            tabs.append({
                .active_url = tab_row.active_url,
                .history = history.is_error() ? Optional<SessionHistorySnapshot> {} : Optional<SessionHistorySnapshot> { history.release_value() },
            });
        }
        return database.try_execute_bound_statement(statements.delete_session, [&](auto& bind) -> ErrorOr<void> {
            return bind("id"sv, unit_id);
        });
    }));
    return tabs;
}

static UnixDateTime canonicalized_to_milliseconds(UnixDateTime time)
{
    // Match persisted millisecond precision when ordering closes.
    return UnixDateTime::from_milliseconds_since_epoch(time.offset_to_epoch().to_milliseconds());
}

static bool is_storable(Optional<SessionHistorySnapshot> const& history)
{
    if (!history.has_value())
        return false;

    auto result = validate_session_history_snapshot_storable(*history);
    if (result.is_error() && history_debug_enabled())
        dbgln("[History] Session snapshot degrades to URL-only: {}", result.error());
    return !result.is_error();
}

// Compare every persisted field because scroll and state updates preserve entry identity.
static bool session_history_nested_history_descriptors_match(Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> const& a, Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> const& b);

static bool serialized_directives_match(Vector<Web::ContentSecurityPolicy::Directives::SerializedDirective> const& a, Vector<Web::ContentSecurityPolicy::Directives::SerializedDirective> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].value != b[i].value)
            return false;
    }
    return true;
}

static bool serialized_policies_match(Vector<Web::ContentSecurityPolicy::SerializedPolicy> const& a, Vector<Web::ContentSecurityPolicy::SerializedPolicy> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (!serialized_directives_match(a[i].directives, b[i].directives)
            || a[i].disposition != b[i].disposition
            || a[i].source != b[i].source
            || a[i].self_origin != b[i].self_origin
            || a[i].pre_parsed_policy_string != b[i].pre_parsed_policy_string)
            return false;
    }
    return true;
}

static bool embedder_policies_match(Web::HTML::EmbedderPolicy const& a, Web::HTML::EmbedderPolicy const& b)
{
    return a.value == b.value
        && a.report_only_value == b.report_only_value
        && a.reporting_endpoint == b.reporting_endpoint
        && a.report_only_reporting_endpoint == b.report_only_reporting_endpoint;
}

static bool serialized_policy_containers_match(Web::HTML::SerializedPolicyContainer const& a, Web::HTML::SerializedPolicyContainer const& b)
{
    return serialized_policies_match(a.csp_list, b.csp_list)
        && embedder_policies_match(a.embedder_policy, b.embedder_policy)
        && a.referrer_policy == b.referrer_policy;
}

static bool history_policy_containers_match(Variant<Web::HTML::SerializedPolicyContainer, Web::HTML::DocumentState::Client> const& a, Variant<Web::HTML::SerializedPolicyContainer, Web::HTML::DocumentState::Client> const& b)
{
    if (auto const* a_serialized_policy_container = a.get_pointer<Web::HTML::SerializedPolicyContainer>()) {
        auto const* b_serialized_policy_container = b.get_pointer<Web::HTML::SerializedPolicyContainer>();
        return b_serialized_policy_container && serialized_policy_containers_match(*a_serialized_policy_container, *b_serialized_policy_container);
    }

    return a.has<Web::HTML::DocumentState::Client>() && b.has<Web::HTML::DocumentState::Client>();
}

static bool session_history_document_state_descriptors_match(Web::HTML::SessionHistoryDocumentStateDescriptor const& a, Web::HTML::SessionHistoryDocumentStateDescriptor const& b)
{
    return a.id == b.id
        && history_policy_containers_match(a.history_policy_container, b.history_policy_container)
        && a.request_referrer == b.request_referrer
        && a.request_referrer_policy == b.request_referrer_policy
        && a.initiator_origin == b.initiator_origin
        && a.origin == b.origin
        && a.about_base_url == b.about_base_url
        && a.resource == b.resource
        && a.reload_pending == b.reload_pending
        && a.ever_populated == b.ever_populated
        && a.navigable_target_name == b.navigable_target_name
        && session_history_nested_history_descriptors_match(a.nested_histories, b.nested_histories);
}

static bool session_history_entry_descriptors_match(Web::HTML::SessionHistoryEntryDescriptor const& a, Web::HTML::SessionHistoryEntryDescriptor const& b)
{
    return a.step == b.step
        && a.url == b.url
        && session_history_document_state_descriptors_match(a.document_state, b.document_state)
        && a.classic_history_api_state == b.classic_history_api_state
        && a.navigation_api_state == b.navigation_api_state
        && a.navigation_api_key == b.navigation_api_key
        && a.navigation_api_id == b.navigation_api_id
        && a.scroll_restoration_mode == b.scroll_restoration_mode
        && a.scroll_position_data.viewport_scroll_position == b.scroll_position_data.viewport_scroll_position;
}

static bool session_history_entry_descriptors_match(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& a, Vector<Web::HTML::SessionHistoryEntryDescriptor> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (!session_history_entry_descriptors_match(a[i], b[i]))
            return false;
    }
    return true;
}

static bool session_history_nested_history_descriptors_match(Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> const& a, Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || !session_history_entry_descriptors_match(a[i].entries, b[i].entries))
            return false;
    }
    return true;
}

static bool snapshots_match(Optional<SessionHistorySnapshot> const& a, Optional<SessionHistorySnapshot> const& b)
{
    if (a.has_value() != b.has_value())
        return false;
    if (!a.has_value())
        return true;
    if (a->current_used_step_index != b->current_used_step_index || a->used_steps != b->used_steps)
        return false;
    return session_history_entry_descriptors_match(a->entries, b->entries);
}

bool SessionStore::is_tab_worth_keeping(URL::URL const& url)
{
    if (url.scheme().is_empty())
        return false;
    return url != URL::about_blank() && url != URL::about_newtab();
}

u32 SessionStore::max_closed_units(UnitKind kind)
{
    return kind == UnitKind::ClosedTab ? MAX_CLOSED_TABS : MAX_CLOSED_WINDOWS;
}

ErrorOr<void> SessionStore::run_startup_recovery(UnixDateTime recovered_at)
{
    auto allocators = TRY(m_persisted_storage->create_id_allocators());
    m_session_id_allocator = allocators.session_ids;
    m_tab_id_allocator = allocators.tab_ids;
    m_close_sequence_allocator = allocators.close_sequences;

    TRY(m_persisted_storage->recover_open_windows({
                                                      .recovered_at = canonicalized_to_milliseconds(recovered_at),
                                                      .closed_tab_limit = max_closed_units(UnitKind::ClosedTab),
                                                      .closed_window_limit = max_closed_units(UnitKind::ClosedWindow),
                                                  },
        m_close_sequence_allocator));

    m_transient_storage.set_closed_units(TRY(m_persisted_storage->load_closed_units()));
    return {};
}

SessionStore::WindowMetadataWrite SessionStore::window_metadata_write(SessionWindowId window_id, OpenWindow const& window) const
{
    WindowMetadataWrite write { .id = window_id, .tabs_in_order = window.tabs, .active_tab_index = 0 };
    // Pending closes' rows still live in this window's unit; they keep trailing ordinals until
    // their retry demotes or discards them.
    for (auto const& pending : m_pending_tab_closes) {
        if (pending.tab.window_id == window_id)
            write.tabs_in_order.append(pending.tab_id);
    }
    write.active_tab_index = index_as_i64(window.tabs.find_first_index(window.active_tab).value_or(0));
    return write;
}

Optional<SessionStore::WindowMetadataWrite> SessionStore::remaining_window_write(SessionWindowId window_id, SessionTabId closing_tab) const
{
    if (auto const* window = m_transient_storage.find_window(window_id)) {
        auto write = window_metadata_write(window_id, *window);
        write.tabs_in_order.remove_all_matching([&](auto id) { return id == closing_tab; });
        write.active_tab_index = index_as_i64(write.tabs_in_order.find_first_index(window->active_tab).value_or(0));
        return write;
    }

    for (auto const& pending : m_pending_window_closes) {
        if (pending.window_id != window_id)
            continue;
        WindowMetadataWrite write { .id = window_id, .tabs_in_order = {}, .active_tab_index = 0 };
        for (auto const& member : pending.members) {
            if (member.tab_id != closing_tab)
                write.tabs_in_order.append(member.tab_id);
        }
        for (auto const& tab_pending : m_pending_tab_closes) {
            if (tab_pending.tab.window_id == window_id && tab_pending.tab_id != closing_tab && !write.tabs_in_order.contains_slow(tab_pending.tab_id))
                write.tabs_in_order.append(tab_pending.tab_id);
        }
        auto last_index = write.tabs_in_order.is_empty() ? 0 : index_as_i64(write.tabs_in_order.size() - 1);
        write.active_tab_index = min(pending.active_tab_index, last_index);
        return write;
    }

    return {};
}

SessionStore::TabWrite SessionStore::tab_write_for(SessionTabId tab_id, OpenTab const& tab, i64 tab_ordinal) const
{
    TabWrite write {
        .id = tab_id,
        .window_id = tab.window_id,
        .tab_ordinal = tab_ordinal,
        .url = tab.url,
        .current_used_step_index = index_as_i64(tab.current_used_step_index),
        .snapshot_disposition = SnapshotDisposition::Preserve,
        .snapshot = nullptr,
    };
    if (tab.dirty) {
        if (is_storable(tab.history)) {
            write.snapshot_disposition = SnapshotDisposition::Replace;
            write.snapshot = &*tab.history;
            write.current_used_step_index = index_as_i64(tab.history->current_used_step_index);
        } else {
            write.snapshot_disposition = SnapshotDisposition::Clear;
            write.current_used_step_index = 0;
        }
    }
    return write;
}

bool SessionStore::window_has_pending_tab_closes(SessionWindowId window_id) const
{
    for (auto const& pending : m_pending_tab_closes) {
        if (pending.tab.window_id == window_id)
            return true;
    }
    return false;
}

ErrorOr<void> SessionStore::drain_window_metadata()
{
    if (!m_persisted_storage.has_value())
        return {};

    Vector<WindowMetadataWrite> writes;
    for (auto const& retired : m_retired_window_metadata)
        writes.append(retired);
    Vector<SessionWindowId> window_ids;
    m_transient_storage.for_each_window([&](auto window_id, auto const& window) {
        if (!window.metadata_dirty)
            return;
        writes.append(window_metadata_write(window_id, window));
        window_ids.append(window_id);
    });
    if (writes.is_empty())
        return {};

    TRY(m_persisted_storage->write_windows_metadata(writes));
    m_retired_window_metadata.clear();
    for (auto window_id : window_ids)
        m_transient_storage.mark_window_metadata_clean(window_id);
    return {};
}

ErrorOr<SessionWindowId> SessionStore::window_opened()
{
    auto window_id = TRY(m_session_id_allocator.allocate());
    if (m_persisted_storage.has_value())
        TRY(m_persisted_storage->insert_open_window(window_id));
    m_transient_storage.open_window(window_id);
    return window_id;
}

void SessionStore::window_detached(SessionWindowId window_id)
{
    auto const* window = m_transient_storage.find_window(window_id);
    if (!window || !window->tabs.is_empty())
        return;

    if (!m_persisted_storage.has_value()) {
        m_transient_storage.remove_window(window_id);
        return;
    }

    ErrorOr<void> result;
    bool queue_detach = window_has_pending_tab_closes(window_id);
    if (!queue_detach) {
        result = drain_window_metadata();
        if (!result.is_error())
            result = m_persisted_storage->delete_session_rows(window_id);
        queue_detach = result.is_error();
    }
    if (queue_detach) {
        if (window->metadata_dirty)
            m_retired_window_metadata.append(window_metadata_write(window_id, *window));
        m_pending_window_closes.append({
            .window_id = window_id,
            .members = {},
            .closed_at = {},
            .discard = true,
        });
    }

    m_transient_storage.remove_window(window_id);
}

ErrorOr<SessionWindowId> SessionStore::default_window()
{
    if (!m_transient_storage.default_window.has_value())
        m_transient_storage.default_window = TRY(window_opened());
    return *m_transient_storage.default_window;
}

ErrorOr<SessionTabId> SessionStore::tab_opened(TabOpened opened)
{
    // A crash before the tab's first flush must not leave an unparseable URL that
    // startup recovery would treat as row corruption.
    if (opened.initial_url.scheme().is_empty())
        opened.initial_url = URL::about_blank();

    Optional<size_t> insertion_index;
    if (opened.insertion_index.has_value()) {
        insertion_index = index_as_size(*opened.insertion_index);
        if (!insertion_index.has_value())
            return Error::from_string_literal("Tab insertion index is outside the addressable range");
    }

    auto tab_id = TRY(m_tab_id_allocator.allocate());
    auto resolved_window_id = opened.window_id.has_value() ? *opened.window_id : TRY(default_window());
    auto const* window = m_transient_storage.find_window(resolved_window_id);
    if (!window)
        return tab_id;

    auto insert_at = min(insertion_index.value_or(window->tabs.size()), window->tabs.size());
    if (m_persisted_storage.has_value()) {
        TRY(drain_window_metadata());

        auto metadata = window_metadata_write(resolved_window_id, *window);
        metadata.tabs_in_order.insert(insert_at, tab_id);
        auto active_tab = opened.is_active == IsActive::Yes ? tab_id : window->active_tab;
        metadata.active_tab_index = index_as_i64(metadata.tabs_in_order.find_first_index(active_tab).value_or(0));

        TRY(m_persisted_storage->open_tab({
            .tab = {
                .id = tab_id,
                .window_id = resolved_window_id,
                .tab_ordinal = index_as_i64(insert_at),
                .url = opened.initial_url,
                .current_used_step_index = 0,
                .snapshot_disposition = SnapshotDisposition::Preserve,
                .snapshot = nullptr,
            },
            .window = move(metadata),
        }));
    }

    (void)m_transient_storage.open_tab(resolved_window_id, tab_id,
        OpenTab { .window_id = resolved_window_id, .url = move(opened.initial_url), .current_used_step_index = 0, .history = {}, .dirty = false }, insert_at);
    if (opened.is_active == IsActive::Yes)
        m_transient_storage.set_active_tab(tab_id);
    if (m_persisted_storage.has_value())
        m_transient_storage.mark_window_metadata_clean(resolved_window_id);
    return tab_id;
}

void SessionStore::update_tab_state(TabStateUpdate update)
{
    auto const* tab = m_transient_storage.find_tab(update.tab_id);
    if (!tab)
        return;

    // A clean persisted tab has dropped its snapshot, so there is nothing to compare a push against.
    auto cache_is_authoritative = !m_persisted_storage.has_value() || tab->dirty;
    if (cache_is_authoritative && tab->url == update.url && snapshots_match(tab->history, update.history))
        return;

    m_transient_storage.set_tab_state(update.tab_id, move(update.history), move(update.url), m_persisted_storage.has_value());
}

void SessionStore::active_tab_changed(SessionTabId tab_id)
{
    m_transient_storage.set_active_tab(tab_id);
}

void SessionStore::tab_order_changed(TabOrderChanged changed)
{
    m_transient_storage.set_tab_order(changed.window_id, changed.ordered_tabs);
}

void SessionStore::tab_moved(TabMoved moved)
{
    auto ordinal = index_as_size(moved.ordinal);
    if (!ordinal.has_value())
        return;

    auto old_window_id = m_transient_storage.move_tab(moved.tab_id, moved.new_window_id, *ordinal);
    if (!old_window_id.has_value())
        return;

    // On failure the next barrier drain rewrites every dirty window atomically.
    (void)drain_window_metadata();
}

ErrorOr<void> SessionStore::flush_tab(SessionTabId tab_id, OpenTab const& tab)
{
    auto write = tab_write_for(tab_id, tab, 0);
    TRY(m_persisted_storage->flush_tab(write));

    // Retain only the metadata needed to demote the persisted rows.
    m_transient_storage.mark_tab_flushed(tab_id, static_cast<size_t>(write.current_used_step_index));
    return {};
}

void SessionStore::append_closed_tab_unit(PendingTabClose const& pending)
{
    size_t mirror_step_index = pending.tab.current_used_step_index;
    if (pending.tab.dirty)
        mirror_step_index = is_storable(pending.tab.history) ? pending.tab.history->current_used_step_index : 0;

    ClosedUnit unit {
        .session_id = pending.unit_id,
        .kind = UnitKind::ClosedTab,
        .closed_time = pending.closed_at,
        .close_sequence = pending.close_sequence,
        .active_tab_index = 0,
        .source_window_id = pending.tab.window_id,
        .source_tab_index = pending.source_tab_index,
        .origin = ClosedSessionUnit::Origin::CloseAction,
        .tabs = {},
    };
    unit.tabs.append({
        .active_url = pending.tab.url,
        .tab_id = pending.tab_id,
        .current_used_step_index = mirror_step_index,
        .history = m_persisted_storage.has_value() ? Optional<SessionHistorySnapshot> {} : pending.tab.history,
    });
    m_transient_storage.append_closed_unit(move(unit));
}

void SessionStore::append_closed_window_unit(PendingWindowClose const& pending)
{
    ClosedUnit unit {
        .session_id = pending.window_id,
        .kind = UnitKind::ClosedWindow,
        .closed_time = pending.closed_at,
        .close_sequence = pending.close_sequence,
        .active_tab_index = min(pending.active_tab_index, pending.members.is_empty() ? 0 : index_as_i64(pending.members.size() - 1)),
        .source_window_id = pending.window_id,
        .source_tab_index = {},
        .origin = ClosedSessionUnit::Origin::CloseAction,
        .tabs = {},
    };
    for (auto const& member : pending.members) {
        size_t mirror_step_index = member.tab.current_used_step_index;
        if (member.tab.dirty)
            mirror_step_index = is_storable(member.tab.history) ? member.tab.history->current_used_step_index : 0;
        unit.tabs.append({
            .active_url = member.tab.url,
            .tab_id = member.tab_id,
            .current_used_step_index = mirror_step_index,
            .history = m_persisted_storage.has_value() ? Optional<SessionHistorySnapshot> {} : member.tab.history,
        });
    }
    m_transient_storage.append_closed_unit(move(unit));
}

ErrorOr<void> SessionStore::finish_tab_close(PendingTabClose const& pending)
{
    if (!is_tab_worth_keeping(pending.tab.url)) {
        if (m_persisted_storage.has_value()) {
            TRY(m_persisted_storage->discard_tab({
                .id = pending.tab_id,
                .remaining_window = remaining_window_write(pending.tab.window_id, pending.tab_id),
            }));
        }
        return {};
    }

    if (m_persisted_storage.has_value()) {
        TRY(m_persisted_storage->close_tab({
            .unit = {
                .id = pending.unit_id,
                .kind = UnitKind::ClosedTab,
                .closed_at = pending.closed_at,
                .close_sequence = pending.close_sequence,
                .source_window_id = pending.tab.window_id,
                .source_tab_index = pending.source_tab_index,
                .origin = ClosedSessionUnit::Origin::CloseAction,
                .active_tab_index = 0,
            },
            .tab = tab_write_for(pending.tab_id, pending.tab, 0),
            .remaining_window = remaining_window_write(pending.tab.window_id, pending.tab_id),
            .retention_limit = max_closed_units(UnitKind::ClosedTab),
        }));
    }
    append_closed_tab_unit(pending);
    m_transient_storage.prune_closed_units(UnitKind::ClosedTab, max_closed_units(UnitKind::ClosedTab));
    return {};
}

ErrorOr<void> SessionStore::finish_window_close(PendingWindowClose const& pending)
{
    bool any_tab_worth_keeping = false;
    for (auto const& member : pending.members)
        any_tab_worth_keeping |= is_tab_worth_keeping(member.tab.url);

    if (pending.members.is_empty() || !any_tab_worth_keeping) {
        if (m_persisted_storage.has_value())
            TRY(m_persisted_storage->delete_session_rows(pending.window_id));
        return {};
    }

    if (m_persisted_storage.has_value()) {
        ClosedWindowWrite write {
            .unit = {
                .id = pending.window_id,
                .kind = UnitKind::ClosedWindow,
                .closed_at = pending.closed_at,
                .close_sequence = pending.close_sequence,
                .source_window_id = pending.window_id,
                .source_tab_index = {},
                .origin = ClosedSessionUnit::Origin::CloseAction,
                .active_tab_index = min(pending.active_tab_index, index_as_i64(pending.members.size() - 1)),
            },
            .members = {},
            .retention_limit = max_closed_units(UnitKind::ClosedWindow),
        };
        for (auto const& member : pending.members)
            write.members.append(tab_write_for(member.tab_id, member.tab, 0));
        TRY(m_persisted_storage->close_window(write));
    }
    append_closed_window_unit(pending);
    m_transient_storage.prune_closed_units(UnitKind::ClosedWindow, max_closed_units(UnitKind::ClosedWindow));
    return {};
}

ErrorOr<void> SessionStore::tab_closed(TabClosed closed)
{
    auto const* tab = m_transient_storage.find_tab(closed.tab_id);
    if (!tab)
        return {};

    if (m_quitting) {
        ErrorOr<void> result;
        if (m_persisted_storage.has_value() && tab->dirty)
            result = flush_tab(closed.tab_id, *tab);
        m_transient_storage.remove_tab(closed.tab_id);
        return result;
    }

    Optional<i64> source_tab_index;
    if (auto index = m_transient_storage.tab_index(closed.tab_id); index.has_value()) {
        source_tab_index = index_as_i64(*index);
    }

    auto discard_without_undo = [&]() -> ErrorOr<void> {
        if (!m_persisted_storage.has_value()) {
            m_transient_storage.remove_tab(closed.tab_id);
            return {};
        }
        auto tab_window_id = tab->window_id;
        auto discard_result = drain_window_metadata();
        if (!discard_result.is_error())
            discard_result = m_persisted_storage->discard_tab({ .id = closed.tab_id, .remaining_window = remaining_window_write(tab_window_id, closed.tab_id) });
        if (discard_result.is_error()) {
            PendingTabClose discard {
                .tab_id = closed.tab_id,
                .unit_id = 0,
                .tab = *tab,
                .closed_at = canonicalized_to_milliseconds(closed.closed_at),
                .close_sequence = 0,
                .source_tab_index = source_tab_index,
                .discard = true,
            };
            m_pending_tab_closes.append(move(discard));
        }
        m_transient_storage.remove_tab(closed.tab_id);
        if (discard_result.is_error())
            return discard_result;
        m_transient_storage.mark_window_metadata_clean(tab_window_id);
        return {};
    };

    auto close_sequence = m_close_sequence_allocator.allocate();
    if (close_sequence.is_error()) {
        auto result = discard_without_undo();
        if (history_debug_enabled())
            dbgln("[History] Tab closed without undo: {}", close_sequence.error());
        return result;
    }

    // Closes must retire the transient tab even when no unit id is allocatable for the demotion, and
    // the closed tab's rows must be discarded so startup recovery cannot resurrect it.
    auto unit_id = m_session_id_allocator.allocate();
    if (unit_id.is_error()) {
        auto result = discard_without_undo();
        if (history_debug_enabled())
            dbgln("[History] Tab closed without undo: {}", unit_id.error());
        return result;
    }

    PendingTabClose close {
        .tab_id = closed.tab_id,
        .unit_id = unit_id.value(),
        .tab = *tab,
        .closed_at = canonicalized_to_milliseconds(closed.closed_at),
        .close_sequence = close_sequence.value(),
        .source_tab_index = source_tab_index,
        .discard = false,
    };
    auto window_id = close.tab.window_id;

    auto result = drain_window_metadata();
    if (!result.is_error())
        result = finish_tab_close(close);
    if (result.is_error() && m_persisted_storage.has_value()) {
        // A clean tab's only snapshot is its rows; reads can still work while writes fail, so
        // rescue it rather than let the retry depend on the rows surviving.
        if (!close.tab.dirty) {
            if (auto snapshot = m_persisted_storage->load_tab_snapshot(closed.tab_id, close.tab.current_used_step_index); !snapshot.is_error()) {
                close.tab.history = snapshot.release_value();
                close.tab.dirty = true;
            }
        }
        m_pending_tab_closes.append(move(close));
    }

    m_transient_storage.remove_tab(closed.tab_id);
    if (!result.is_error() && m_persisted_storage.has_value())
        m_transient_storage.mark_window_metadata_clean(window_id);
    return result;
}

void SessionStore::tab_detached(SessionTabId tab_id)
{
    auto const* tab = m_transient_storage.find_tab(tab_id);
    if (!tab)
        return;

    if (!m_persisted_storage.has_value()) {
        m_transient_storage.remove_tab(tab_id);
        return;
    }

    auto window_id = tab->window_id;
    auto result = drain_window_metadata();
    if (!result.is_error())
        result = m_persisted_storage->discard_tab({ .id = tab_id, .remaining_window = remaining_window_write(window_id, tab_id) });
    if (result.is_error()) {
        PendingTabClose discard {
            .tab_id = tab_id,
            .unit_id = 0,
            .tab = *tab,
            .closed_at = canonicalized_to_milliseconds(UnixDateTime::now()),
            .source_tab_index = {},
            .discard = true,
        };
        m_pending_tab_closes.append(move(discard));
    }

    m_transient_storage.remove_tab(tab_id);
    if (!result.is_error())
        m_transient_storage.mark_window_metadata_clean(window_id);
}

ErrorOr<void> SessionStore::window_closing(WindowClosing closing)
{
    auto const* window = m_transient_storage.find_window(closing.window_id);
    if (!window)
        return {};

    if (m_quitting) {
        if (m_persisted_storage.has_value()) {
            for (auto member_id : window->tabs) {
                if (auto const* tab = m_transient_storage.find_tab(member_id); tab && tab->dirty)
                    TRY(flush_tab(member_id, *tab));
            }
            if (window->metadata_dirty)
                m_retired_window_metadata.append(window_metadata_write(closing.window_id, *window));
        }
        m_transient_storage.remove_window(closing.window_id);
        return {};
    }

    auto closed_at = canonicalized_to_milliseconds(closing.closed_at);
    closing.active_tab_index = max(closing.active_tab_index, 0);

    auto make_pending_close = [&](SessionCloseSequence close_sequence, bool discard) {
        PendingWindowClose close {
            .window_id = closing.window_id,
            .members = {},
            .active_tab_index = closing.active_tab_index,
            .closed_at = closed_at,
            .close_sequence = close_sequence,
            .discard = discard,
        };
        for (auto member_id : window->tabs) {
            if (auto const* tab = m_transient_storage.find_tab(member_id))
                close.members.append({ .tab_id = member_id, .unit_id = 0, .tab = *tab, .closed_at = closed_at, .source_tab_index = {}, .discard = false });
        }
        return close;
    };

    auto close_sequence = m_close_sequence_allocator.allocate();
    if (close_sequence.is_error()) {
        auto close = make_pending_close(0, true);
        ErrorOr<void> result;
        if (m_persisted_storage.has_value())
            result = m_persisted_storage->delete_session_rows(closing.window_id);
        if (result.is_error() && m_persisted_storage.has_value()) {
            if (window->metadata_dirty)
                m_retired_window_metadata.append(window_metadata_write(closing.window_id, *window));
            m_pending_window_closes.append(move(close));
        }
        m_transient_storage.remove_window(closing.window_id);
        if (history_debug_enabled())
            dbgln("[History] Window closed without undo: {}", close_sequence.error());
        return result;
    }

    // Resolve pending child closes before changing the window unit's kind.
    bool topology_is_current = true;
    if (m_persisted_storage.has_value()) {
        topology_is_current = !drain_window_metadata().is_error();
        if (topology_is_current) {
            for (size_t i = 0; i < m_pending_tab_closes.size();) {
                auto const& pending = m_pending_tab_closes[i];
                if (pending.tab.window_id != closing.window_id) {
                    ++i;
                    continue;
                }
                ErrorOr<void> result;
                if (pending.discard)
                    result = m_persisted_storage->discard_tab({ .id = pending.tab_id, .remaining_window = remaining_window_write(closing.window_id, pending.tab_id) });
                else
                    result = finish_tab_close(pending);
                if (result.is_error())
                    ++i;
                else
                    m_pending_tab_closes.remove(i);
            }
        }
    }

    auto close = make_pending_close(close_sequence.value(), false);

    ErrorOr<void> result;
    bool queue_close = m_persisted_storage.has_value() && (!topology_is_current || window_has_pending_tab_closes(closing.window_id));
    if (!queue_close) {
        result = finish_window_close(close);
        queue_close = result.is_error() && m_persisted_storage.has_value();
    }
    if (queue_close) {
        // A retired window leaves the drain's reach; carry its stale topology until a drain commits.
        if (window->metadata_dirty)
            m_retired_window_metadata.append(window_metadata_write(closing.window_id, *window));
        m_pending_window_closes.append(move(close));
    }
    m_transient_storage.remove_window(closing.window_id);
    return result;
}

void SessionStore::application_quitting()
{
    m_quitting = true;
    flush_dirty_state();
}

void SessionStore::application_quit_aborted()
{
    m_quitting = false;
}

bool SessionStore::has_closed_units() const
{
    return m_transient_storage.has_closed_units();
}

ErrorOr<Optional<ClosedSessionUnit>> SessionStore::take_most_recently_closed()
{
    auto const* unit = m_transient_storage.last_closed_unit();
    if (!unit)
        return Optional<ClosedSessionUnit> {};

    ClosedSessionUnit result {
        .tabs = {},
        .active_tab_index = unit->active_tab_index,
        .was_window = unit->kind == UnitKind::ClosedWindow,
        .source_window_id = unit->source_window_id,
        .source_tab_index = unit->source_tab_index,
        .origin = unit->origin,
    };

    if (m_persisted_storage.has_value()) {
        result.tabs = TRY(m_persisted_storage->take_closed_unit(unit->session_id));
        (void)m_transient_storage.take_last_closed_unit();
    } else {
        auto taken = m_transient_storage.take_last_closed_unit();
        for (auto& tab : taken.tabs)
            result.tabs.append({ .active_url = move(tab.active_url), .history = move(tab.history) });
    }

    if (result.tabs.is_empty())
        return Optional<ClosedSessionUnit> {};
    result.active_tab_index = min(result.active_tab_index, index_as_i64(result.tabs.size() - 1));
    return Optional<ClosedSessionUnit> { move(result) };
}

Optional<SessionStore::TabStateUpdate> SessionStore::cached_tab_state_for_testing(SessionTabId tab_id) const
{
    auto const* tab = m_transient_storage.find_tab(tab_id);
    if (!tab)
        return {};
    return TabStateUpdate { .tab_id = tab_id, .history = tab->history, .url = tab->url };
}

ErrorOr<void> SessionStore::remove_entries_accessed_since(UnixDateTime since)
{
    auto closed_units = m_transient_storage.remove_closed_units_since(since);

    if (!m_persisted_storage.has_value()) {
        m_pending_tab_closes.remove_all_matching([&](auto const& pending) { return pending.closed_at >= since; });
        m_pending_window_closes.remove_all_matching([&](auto const& pending) { return pending.closed_at >= since; });
        return {};
    }

    if (!m_pending_clear_closed_units.has_value())
        m_pending_clear_closed_units = PendingClearClosedUnits {};
    auto& pending_clear = *m_pending_clear_closed_units;
    for (auto closed_unit : closed_units) {
        if (!pending_clear.closed_units.contains_slow(closed_unit))
            pending_clear.closed_units.append(closed_unit);
    }

    // Convert matching pending closes into deletion retries.
    for (auto& pending : m_pending_tab_closes) {
        if (pending.closed_at < since)
            continue;
        pending.discard = true;
        if (!pending_clear.pending_tabs.contains_slow(pending.tab_id))
            pending_clear.pending_tabs.append(pending.tab_id);
    }
    for (auto& pending : m_pending_window_closes) {
        if (pending.closed_at < since)
            continue;
        pending.discard = true;
        if (!pending_clear.pending_windows.contains_slow(pending.window_id))
            pending_clear.pending_windows.append(pending.window_id);
    }

    return retry_pending_clear_closed_units();
}

ErrorOr<void> SessionStore::retry_pending_clear_closed_units()
{
    VERIFY(m_persisted_storage.has_value());
    VERIFY(m_pending_clear_closed_units.has_value());

    auto const& pending_clear = *m_pending_clear_closed_units;
    ClearClosedUnitsWrite write { .closed_units = pending_clear.closed_units, .pending_tabs = {}, .pending_windows = {} };
    for (auto const& pending : m_pending_tab_closes) {
        if (pending_clear.pending_tabs.contains_slow(pending.tab_id))
            write.pending_tabs.append({ .id = pending.tab_id, .remaining_window = remaining_window_write(pending.tab.window_id, pending.tab_id) });
    }
    for (auto const& pending : m_pending_window_closes) {
        if (pending_clear.pending_windows.contains_slow(pending.window_id))
            write.pending_windows.append(pending.window_id);
    }

    TRY(drain_window_metadata());
    TRY(m_persisted_storage->clear_closed_units(write));

    m_pending_tab_closes.remove_all_matching([&](auto const& pending) { return pending_clear.pending_tabs.contains_slow(pending.tab_id); });
    m_pending_window_closes.remove_all_matching([&](auto const& pending) { return pending_clear.pending_windows.contains_slow(pending.window_id); });
    m_pending_clear_closed_units.clear();
    return {};
}

void SessionStore::flush_dirty_state()
{
    if (!m_persisted_storage.has_value())
        return;

    if (m_pending_clear_closed_units.has_value())
        (void)retry_pending_clear_closed_units();

    // Pending retries rewrite ordinals, so they only run once the persisted topology is current.
    bool any_close_completed = false;
    if (!drain_window_metadata().is_error()) {
        for (size_t i = 0; i < m_pending_tab_closes.size();) {
            auto const& pending = m_pending_tab_closes[i];
            ErrorOr<void> result;
            if (pending.discard) {
                result = m_persisted_storage->discard_tab({ .id = pending.tab_id, .remaining_window = remaining_window_write(pending.tab.window_id, pending.tab_id) });
            } else {
                result = finish_tab_close(pending);
                if (!result.is_error())
                    any_close_completed = true;
            }
            if (result.is_error())
                ++i;
            else
                m_pending_tab_closes.remove(i);
        }

        for (size_t i = 0; i < m_pending_window_closes.size();) {
            auto const& pending = m_pending_window_closes[i];
            // The persisted unit must stay open until every pending child row has left it.
            if (!pending.discard && window_has_pending_tab_closes(pending.window_id)) {
                ++i;
                continue;
            }
            ErrorOr<void> result;
            if (pending.discard) {
                result = m_persisted_storage->delete_session_rows(pending.window_id);
            } else {
                result = finish_window_close(pending);
                if (!result.is_error())
                    any_close_completed = true;
            }
            if (result.is_error())
                ++i;
            else
                m_pending_window_closes.remove(i);
        }
    }

    Vector<SessionTabId> dirty_tabs;
    m_transient_storage.for_each_tab([&](auto tab_id, auto const& tab) {
        if (tab.dirty)
            dirty_tabs.append(tab_id);
    });
    for (auto tab_id : dirty_tabs) {
        if (auto const* tab = m_transient_storage.find_tab(tab_id))
            (void)flush_tab(tab_id, *tab);
    }

    if (any_close_completed && on_closed_units_changed)
        on_closed_units_changed();
}

}
