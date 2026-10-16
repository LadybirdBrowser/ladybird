/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDatabase/Database.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/SessionStore.h>

using namespace WebView;

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

static Web::HTML::SessionHistoryEntryDescriptor make_entry(i32 step, StringView url, u64 document_state_id, u8 state_byte)
{
    Web::HTML::SessionHistoryEntryDescriptor entry;
    entry.step = step;
    entry.url = parse_url(url);
    entry.document_state.id = { 1, document_state_id };
    entry.document_state.ever_populated = true;
    entry.document_state.history_policy_container = Web::HTML::DocumentStateClient::Tag;
    entry.classic_history_api_state = Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::copy({ &state_byte, 1 })) };
    entry.navigation_api_state = Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::copy({ &state_byte, 1 })) };
    entry.directive_state_id = { 2, document_state_id };
    entry.directive_state_value = "text=target"_string;
    return entry;
}

static SessionHistorySnapshot make_snapshot(Vector<StringView> urls, u8 state_byte = 0x11)
{
    SessionHistorySnapshot snapshot;
    for (size_t i = 0; i < urls.size(); ++i) {
        snapshot.entries.append(make_entry(static_cast<i32>(i), urls[i], i + 1, state_byte));
        snapshot.used_steps.append(static_cast<i32>(i));
    }
    snapshot.current_used_step_index = urls.size() - 1;
    return snapshot;
}

static i64 select_i64(Database::Database& database, String const& sql)
{
    auto statement = MUST(database.prepare_statement(sql));
    i64 value = -1;
    database.execute_statement(statement, [&](auto statement_id) -> ErrorOr<void> {
        value = database.result_column<i64>(statement_id, 0);
        return {};
    });
    return value;
}

static NonnullRefPtr<Database::Database> create_session_database()
{
    auto database = MUST(Database::Database::create_memory_backed({ .foreign_keys = Database::Database::ForeignKeys::Yes }));
    MUST(SessionStore::migrate_schema(*database));
    return database;
}

static i64 count_entry_rows(Database::Database& database, i64 tab_id)
{
    return select_i64(database, MUST(String::formatted("SELECT COUNT(*) FROM SessionEntries INNER JOIN SessionHistories ON SessionEntries.history_id = SessionHistories.id WHERE SessionHistories.tab_id = {};", tab_id)));
}

static SessionTabId open_tab(SessionStore& store, SessionWindowId window, StringView url)
{
    return MUST(store.tab_opened({ .window_id = window, .initial_url = parse_url(url), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
}

// Takes the schema back to SESSIONS_SCHEMA_DOCUMENT_POLICY_VERSION, so every migration after it runs again.
static void rewind_sessions_schema_to_version_4(Database::Database& database)
{
    MUST(database.execute_raw("ALTER TABLE Sessions DROP COLUMN maximized;"sv));
    MUST(database.execute_raw("ALTER TABLE Sessions DROP COLUMN height;"sv));
    MUST(database.execute_raw("ALTER TABLE Sessions DROP COLUMN width;"sv));
    MUST(database.execute_raw("ALTER TABLE Sessions DROP COLUMN y;"sv));
    MUST(database.execute_raw("ALTER TABLE Sessions DROP COLUMN x;"sv));
    MUST(database.execute_raw("ALTER TABLE Sessions DROP COLUMN restore_at_launch;"sv));
    MUST(database.execute_raw("ALTER TABLE Sessions DROP COLUMN activation_sequence;"sv));
    MUST(database.execute_raw("DROP TABLE SessionLifecycle;"sv));
    MUST(database.execute_raw("ALTER TABLE SessionTabs DROP COLUMN pinned;"sv));
    MUST(database.execute_raw("UPDATE SchemaVersions SET version = 4 WHERE store = 'Sessions';"sv));
}

// What the frontend does at Quit: Every window closes in quitting mode, and the run then ends normally.
static void quit_cleanly(SessionStore& store, Vector<SessionWindowId> const& windows)
{
    store.application_quitting();
    for (auto window : windows)
        MUST(store.window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));
    store.mark_clean_exit();
}

TEST_CASE(fresh_database_migrates_to_the_latest_schema)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed({ .foreign_keys = Database::Database::ForeignKeys::Yes }));
    EXPECT_EQ(TRY_OR_FAIL(WebView::SessionStore::migrate_schema(*database)), Database::MigrationOutcome::Success);

    for (auto table : { "Sessions"sv, "SessionTabs"sv, "SessionUsedSteps"sv, "SessionHistories"sv, "SessionNestedHistories"sv, "SessionEntries"sv })
        EXPECT(TRY_OR_FAIL(database->table_exists(table)));

    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Sessions"sv)), Optional<u32> { 8u });
}

TEST_CASE(deleting_a_session_cascades_through_its_tabs)
{
    auto database = create_session_database();

    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (1, 0, 0, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (2, 1, 0, 'https://a.example/', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionUsedSteps (tab_id, step_ordinal, step) VALUES (2, 0, 0);"sv));

    TRY_OR_FAIL(database->execute_raw("DELETE FROM Sessions WHERE id = 1;"sv));

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 0);
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionUsedSteps;"_string), 0);
}

TEST_CASE(duplicate_snapshot_row_ordinals_are_rejected)
{
    auto database = create_session_database();

    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (1, 0, 0, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (2, 1, 0, 'https://a.example/', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionUsedSteps (tab_id, step_ordinal, step) VALUES (2, 0, 0);"sv));

    EXPECT(database->execute_raw("INSERT INTO SessionUsedSteps (tab_id, step_ordinal, step) VALUES (2, 0, 1);"sv).is_error());
}

TEST_CASE(newer_sessions_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('Sessions', 99);"sv));

    EXPECT_EQ(TRY_OR_FAIL(WebView::SessionStore::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
    EXPECT_EQ(TRY_OR_FAIL(WebView::SessionStore::migrate_schema(*database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::DatabaseTooNew);
}

TEST_CASE(version_4_database_migrates_to_the_latest_schema)
{
    auto database = create_session_database();
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (1, 0, 0, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (2, 1, 0, 'https://a.example/', 0);"sv));
    rewind_sessions_schema_to_version_4(*database);

    EXPECT_EQ(TRY_OR_FAIL(SessionStore::migrate_schema(*database)), Database::MigrationOutcome::Success);
    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Sessions"sv)), Optional<u32> { 8u });
    EXPECT_EQ(select_i64(*database, "SELECT pinned FROM SessionTabs WHERE id = 2;"_string), 0);
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE id = 1 AND width IS NULL;"_string), 1);
    EXPECT_EQ(select_i64(*database, "SELECT maximized FROM Sessions WHERE id = 1;"_string), 0);
}

TEST_CASE(pushed_tab_state_flushes_to_rows)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    store->flush_dirty_state();

    EXPECT_EQ(count_entry_rows(*database, tab), 2);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT current_used_step_index FROM SessionTabs WHERE id = {};", tab))), 1);

    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://c.example/"sv }), .url = parse_url("https://c.example/"sv) });
    store->flush_dirty_state();

    EXPECT_EQ(count_entry_rows(*database, tab), 1);
}

TEST_CASE(pushes_coalesce_and_clean_tabs_redirty_conservatively)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE Probe (x INTEGER);"sv));
    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER ProbeTabWrites AFTER UPDATE ON SessionTabs BEGIN INSERT INTO Probe VALUES (1); END;"sv));

    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    store->flush_dirty_state();
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Probe;"_string), 1);

    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    store->flush_dirty_state();
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Probe;"_string), 2);
}

TEST_CASE(a_changed_user_agent_initiated_flag_alone_flushes)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

    auto snapshot = make_snapshot({ "https://a.example/"sv });
    store->update_tab_state({ .tab_id = tab, .history = snapshot, .url = parse_url("https://a.example/"sv) });
    store->flush_dirty_state();
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionEntries WHERE user_agent_initiated = 1;"_string), 0);

    // The flag is the only difference from the flushed snapshot, and it still has to reach the rows.
    snapshot.entries[0].document_state.user_agent_initiated = Web::HTML::UserAgentInitiated::Yes;
    store->update_tab_state({ .tab_id = tab, .history = snapshot, .url = parse_url("https://a.example/"sv) });
    store->flush_dirty_state();
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionEntries WHERE user_agent_initiated = 1;"_string), 1);
}

TEST_CASE(unstorable_snapshot_flushes_url_only)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto snapshot = make_snapshot({ "https://a.example/"sv });
    snapshot.entries[0].classic_history_api_state = Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::create_zeroed((8uz * 1024 * 1024) + 1)) };

    store->update_tab_state({ .tab_id = tab, .history = move(snapshot), .url = parse_url("https://a.example/"sv) });
    store->flush_dirty_state();

    EXPECT_EQ(count_entry_rows(*database, tab), 0);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT COUNT(*) FROM SessionTabs WHERE id = {} AND active_url = 'https://a.example/';", tab))), 1);
}

TEST_CASE(oversized_active_url_reopens_as_about_blank)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto oversized_url = parse_url(MUST(String::formatted("data:,{}", MUST(String::repeated('a', (8uz * 1024 * 1024) + 1)))));
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = {}, .url = oversized_url });

    TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }));

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs WHERE active_url = 'about:blank';"_string), 1);

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "about:blank"sv);
}

TEST_CASE(oversized_active_url_survives_restart_as_about_blank)
{
    auto database = create_session_database();

    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto oversized_url = parse_url(MUST(String::formatted("data:,{}", MUST(String::repeated('a', (8uz * 1024 * 1024) + 1)))));
        auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        store->update_tab_state({ .tab_id = tab, .history = {}, .url = oversized_url });
        TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }));
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    EXPECT(store->has_closed_units());

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "about:blank"sv);
}

TEST_CASE(database_failure_keeps_previous_rows_and_retries)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
    store->flush_dirty_state();

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON SessionEntries BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    store->flush_dirty_state();

    EXPECT_EQ(count_entry_rows(*database, tab), 1);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT COUNT(*) FROM SessionTabs WHERE id = {} AND active_url = 'https://a.example/';", tab))), 1);

    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    store->flush_dirty_state();

    EXPECT_EQ(count_entry_rows(*database, tab), 2);
}

TEST_CASE(closing_a_tab_demotes_it)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto second = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = first, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    store->update_tab_state({ .tab_id = second, .history = make_snapshot({ "https://c.example/"sv }), .url = parse_url("https://c.example/"sv) });

    TRY_OR_FAIL(store->tab_closed({ .tab_id = first, .closed_at = UnixDateTime::now() }));

    EXPECT(store->has_closed_units());
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 1;"_string), 1);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT tab_ordinal FROM SessionTabs WHERE id = {};", second))), 0);

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(!unit->was_window);
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://b.example/"sv);
    EXPECT(unit->tabs[0].history.has_value());
    EXPECT_EQ(unit->tabs[0].history->entries.size(), 2uz);
    EXPECT_EQ(unit->tabs[0].history->current_used_step_index, 1uz);

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 1;"_string), 0);
    EXPECT_EQ(count_entry_rows(*database, first), 0);
}

TEST_CASE(a_pin_writes_through_and_survives_restart)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        auto pinned = open_tab(*store, window, "https://a.example/"sv);
        (void)open_tab(*store, window, "https://b.example/"sv);

        TRY_OR_FAIL(store->tab_pinned_changed(pinned, true));

        // No flush: The pin itself must already be in the row.
        EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT pinned FROM SessionTabs WHERE id = {};", pinned))), 1);
    }

    auto reopened_store = TRY_OR_FAIL(SessionStore::create(*database));
    auto unit = TRY_OR_FAIL(reopened_store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 2uz);
    EXPECT(unit->tabs[0].pinned);
    EXPECT(!unit->tabs[1].pinned);
}

TEST_CASE(a_flush_keeps_the_pin)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = open_tab(*store, window, "about:blank"sv);

    TRY_OR_FAIL(store->tab_pinned_changed(tab, true));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
    store->flush_dirty_state();

    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT pinned FROM SessionTabs WHERE id = {};", tab))), 1);
}

static i64 select_tab_column(Database::Database& database, StringView column, SessionTabId tab)
{
    return select_i64(database, MUST(String::formatted("SELECT {} FROM SessionTabs WHERE id = {};", column, tab)));
}

TEST_CASE(a_pin_is_saved_with_the_tab_order_it_needs)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window = TRY_OR_FAIL(store->window_opened());
    auto first = open_tab(*store, window, "https://a.example/"sv);
    auto second = open_tab(*store, window, "https://b.example/"sv);
    store->flush_dirty_state();

    // Pinning the second tab moves it to the front, and the frontend reports that order first.
    store->tab_order_changed({ .window_id = window, .ordered_tabs = { second, first } });
    TRY_OR_FAIL(store->tab_pinned_changed(second, true));

    // No flush: The order the pin needs must already be in the rows, along with the pin itself.
    EXPECT_EQ(select_tab_column(*database, "tab_ordinal"sv, second), 0);
    EXPECT_EQ(select_tab_column(*database, "tab_ordinal"sv, first), 1);
    EXPECT_EQ(select_tab_column(*database, "pinned"sv, second), 1);
    EXPECT_EQ(select_tab_column(*database, "pinned"sv, first), 0);
}

TEST_CASE(an_unpin_is_saved_with_the_tab_order_it_needs)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window = TRY_OR_FAIL(store->window_opened());
    auto first = open_tab(*store, window, "https://a.example/"sv);
    auto second = open_tab(*store, window, "https://b.example/"sv);
    auto third = open_tab(*store, window, "https://c.example/"sv);
    TRY_OR_FAIL(store->tab_pinned_changed(first, true));
    TRY_OR_FAIL(store->tab_pinned_changed(second, true));
    store->flush_dirty_state();

    // Unpinning the first tab moves it to the end of the pinned tabs.
    store->tab_order_changed({ .window_id = window, .ordered_tabs = { second, first, third } });
    TRY_OR_FAIL(store->tab_pinned_changed(first, false));

    EXPECT_EQ(select_tab_column(*database, "tab_ordinal"sv, second), 0);
    EXPECT_EQ(select_tab_column(*database, "tab_ordinal"sv, first), 1);
    EXPECT_EQ(select_tab_column(*database, "tab_ordinal"sv, third), 2);
    EXPECT_EQ(select_tab_column(*database, "pinned"sv, second), 1);
    EXPECT_EQ(select_tab_column(*database, "pinned"sv, first), 0);
}

TEST_CASE(a_failed_pin_write_is_retried_by_the_next_flush)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = open_tab(*store, window, "about:blank"sv);
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    store->flush_dirty_state();

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE UPDATE OF pinned ON SessionTabs BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_pinned_changed(tab, true).is_error());
    EXPECT_EQ(select_tab_column(*database, "pinned"sv, tab), 0);

    // The tab stays idle, so nothing but the retry writes its row again.
    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    store->flush_dirty_state();
    EXPECT_EQ(select_tab_column(*database, "pinned"sv, tab), 1);
    EXPECT_EQ(count_entry_rows(*database, tab), 2);
}

TEST_CASE(a_failed_unpin_write_is_retried_when_quitting)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = open_tab(*store, window, "about:blank"sv);
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    TRY_OR_FAIL(store->tab_pinned_changed(tab, true));
    store->flush_dirty_state();

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE UPDATE OF pinned ON SessionTabs BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_pinned_changed(tab, false).is_error());
    EXPECT_EQ(select_tab_column(*database, "pinned"sv, tab), 1);

    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    store->application_quitting();
    EXPECT_EQ(select_tab_column(*database, "pinned"sv, tab), 0);
    EXPECT_EQ(count_entry_rows(*database, tab), 2);
}

TEST_CASE(a_moved_tab_keeps_its_pin)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto first_window = TRY_OR_FAIL(store->window_opened());
    auto second_window = TRY_OR_FAIL(store->window_opened());
    auto tab = open_tab(*store, first_window, "https://a.example/"sv);

    TRY_OR_FAIL(store->tab_pinned_changed(tab, true));
    store->tab_moved({ .tab_id = tab, .new_window_id = second_window, .ordinal = 0 });

    // A pushed state makes the flush rewrite the moved tab's row.
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    store->flush_dirty_state();

    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT pinned FROM SessionTabs WHERE id = {};", tab))), 1);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT session_id FROM SessionTabs WHERE id = {};", tab))), second_window);
}

TEST_CASE(closed_tabs_and_windows_keep_their_pins)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window = TRY_OR_FAIL(store->window_opened());
    auto first = open_tab(*store, window, "https://a.example/"sv);
    auto second = open_tab(*store, window, "https://b.example/"sv);
    (void)open_tab(*store, window, "https://c.example/"sv);
    TRY_OR_FAIL(store->tab_pinned_changed(first, true));
    TRY_OR_FAIL(store->tab_pinned_changed(second, true));

    TRY_OR_FAIL(store->tab_closed({ .tab_id = second, .closed_at = UnixDateTime::now() }));
    auto tab_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(tab_unit.has_value());
    EXPECT(tab_unit->tabs[0].pinned);

    TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));
    auto window_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(window_unit.has_value());
    EXPECT_EQ(window_unit->tabs.size(), 2uz);
    EXPECT(window_unit->tabs[0].pinned);
    EXPECT(!window_unit->tabs[1].pinned);
}

TEST_CASE(transient_closed_tabs_keep_their_pins)
{
    auto store = SessionStore::create();
    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = open_tab(*store, window, "https://a.example/"sv);
    TRY_OR_FAIL(store->tab_pinned_changed(tab, true));

    TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }));
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(unit->tabs[0].pinned);
}

TEST_CASE(closed_tab_placement_survives_restart)
{
    auto database = create_session_database();
    SessionWindowId source_window = 0;

    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        source_window = TRY_OR_FAIL(store->window_opened());
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = source_window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto middle = TRY_OR_FAIL(store->tab_opened({ .window_id = source_window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = source_window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

        TRY_OR_FAIL(store->tab_closed({ .tab_id = middle, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }));
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto recovered_window = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(recovered_window.has_value());
    EXPECT(recovered_window->was_window);
    EXPECT_EQ(recovered_window->origin, ClosedSessionUnit::Origin::StartupRecovery);
    EXPECT(!recovered_window->source_window_id.has_value());

    auto closed_tab = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(closed_tab.has_value());
    EXPECT(!closed_tab->was_window);
    EXPECT_EQ(closed_tab->origin, ClosedSessionUnit::Origin::CloseAction);
    EXPECT_EQ(closed_tab->source_window_id, Optional<SessionWindowId> { source_window });
    EXPECT_EQ(closed_tab->source_tab_index, Optional<i64> { 1 });
    EXPECT_EQ(closed_tab->tabs[0].active_url.serialize(), "https://b.example/"sv);
}

TEST_CASE(close_sequence_orders_actions_with_the_same_timestamp)
{
    auto database = create_session_database();
    auto closed_at = UnixDateTime::from_milliseconds_since_epoch(1000);

    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto first_window = TRY_OR_FAIL(store->window_opened());
        auto second_window = TRY_OR_FAIL(store->window_opened());
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://window.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto closed_tab = TRY_OR_FAIL(store->tab_opened({ .window_id = second_window, .initial_url = parse_url("https://tab.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = second_window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

        TRY_OR_FAIL(store->tab_closed({ .tab_id = closed_tab, .closed_at = closed_at }));
        TRY_OR_FAIL(store->window_closing({ .window_id = first_window, .active_tab_index = 0, .closed_at = closed_at }));
        TRY_OR_FAIL(store->window_closing({ .window_id = second_window, .active_tab_index = 0, .closed_at = closed_at }));
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window_action = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(window_action.has_value());
    EXPECT(window_action->was_window);
    EXPECT_EQ(window_action->tabs[0].active_url.serialize(), "https://window.example/"sv);

    auto tab_action = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(tab_action.has_value());
    EXPECT(!tab_action->was_window);
    EXPECT_EQ(tab_action->tabs[0].active_url.serialize(), "https://tab.example/"sv);
}

TEST_CASE(closing_a_clean_tab_keeps_its_flushed_rows)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    store->flush_dirty_state();

    TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }));

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(unit->tabs[0].history.has_value());
    EXPECT_EQ(unit->tabs[0].history->entries.size(), 2uz);
    EXPECT_EQ(unit->tabs[0].history->current_used_step_index, 1uz);
}

TEST_CASE(closing_a_blank_tab_leaves_no_closed_unit)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }));

    EXPECT(!store->has_closed_units());
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 0);
}

TEST_CASE(closing_a_window_demotes_all_member_tabs)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto blank = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto third = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = first, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
    store->update_tab_state({ .tab_id = third, .history = make_snapshot({ "https://c.example/"sv }), .url = parse_url("https://c.example/"sv) });
    (void)blank;

    TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 2, .closed_at = UnixDateTime::now() }));

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 2;"_string), 1);

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(unit->was_window);
    EXPECT_EQ(unit->tabs.size(), 3uz);
    EXPECT_EQ(unit->active_tab_index, 2);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(unit->tabs[1].active_url.serialize(), "about:blank"sv);
    EXPECT(!unit->tabs[1].history.has_value());
    EXPECT(unit->tabs[2].history.has_value());
}

TEST_CASE(failed_demotion_retires_the_tab_and_retries_from_the_flush)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }).is_error());

    EXPECT(!store->has_closed_units());
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });

    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    store->flush_dirty_state();
    EXPECT(store->has_closed_units());

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
}

TEST_CASE(recovery_keeps_a_window_with_a_never_navigated_tab)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        auto navigated = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        store->update_tab_state({ .tab_id = navigated, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = {}, .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        store->flush_dirty_state();
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    EXPECT(store->has_closed_units());

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 2uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(unit->tabs[1].active_url.serialize(), "about:blank"sv);
}

TEST_CASE(closing_an_internal_page_keeps_it_reopenable)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto settings = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:settings"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto new_tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:newtab"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

    TRY_OR_FAIL(store->tab_closed({ .tab_id = settings, .closed_at = UnixDateTime::now() }));
    TRY_OR_FAIL(store->tab_closed({ .tab_id = new_tab, .closed_at = UnixDateTime::now() }));

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "about:settings"sv);
    EXPECT(!TRY_OR_FAIL(store->take_most_recently_closed()).has_value());
}

TEST_CASE(failed_window_demotion_retires_the_window_and_retries_from_the_flush)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE UPDATE ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }).is_error());

    EXPECT(!store->has_closed_units());
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 0;"_string), 1);

    size_t notified = 0;
    store->on_closed_units_changed = [&] { ++notified; };

    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    store->flush_dirty_state();
    EXPECT_EQ(notified, 1uz);

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(unit->was_window);
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
}

TEST_CASE(destruction_suppresses_close_retry_notification)
{
    auto database = create_session_database();
    bool notified = false;

    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });

        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE UPDATE ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        EXPECT(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }).is_error());
        store->on_closed_units_changed = [&] { notified = true; };
        TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    }

    EXPECT(!notified);
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    EXPECT(TRY_OR_FAIL(store->take_most_recently_closed()).has_value());
}

TEST_CASE(pending_tab_close_defers_its_windows_close)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }).is_error());

    TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 0;"_string), 1);
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 1);

    store->flush_dirty_state();
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 0;"_string), 1);
    EXPECT(!store->has_closed_units());

    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    store->flush_dirty_state();

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(!unit->was_window);
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT(unit->tabs[0].history.has_value());

    EXPECT(!TRY_OR_FAIL(store->take_most_recently_closed()).has_value());
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 0;"_string), 0);
}

TEST_CASE(failed_clean_tab_close_retries_with_its_history)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
    store->flush_dirty_state();

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }).is_error());
    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    store->flush_dirty_state();

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT(unit->tabs[0].history.has_value());
    EXPECT_EQ(unit->tabs[0].history->entries.size(), 1uz);
}

TEST_CASE(retried_closes_keep_reopen_order)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto second = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = first, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
    store->update_tab_state({ .tab_id = second, .history = make_snapshot({ "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_closed({ .tab_id = first, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }).is_error());
    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));

    TRY_OR_FAIL(store->tab_closed({ .tab_id = second, .closed_at = UnixDateTime::from_seconds_since_epoch(200) }));
    store->flush_dirty_state();

    auto newest = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(newest.has_value());
    EXPECT_EQ(newest->tabs[0].active_url.serialize(), "https://b.example/"sv);
    auto older = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(older.has_value());
    EXPECT_EQ(older->tabs[0].active_url.serialize(), "https://a.example/"sv);
}

TEST_CASE(crashed_pending_close_recovers_a_contiguous_window)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto middle = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto last = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)first;
        (void)last;

        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        EXPECT(store->tab_closed({ .tab_id = middle, .closed_at = UnixDateTime::now() }).is_error());
        store->flush_dirty_state();
    }
    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));

    // The pending row kept a trailing ordinal, so recovery accepts the window with the closed tab
    // resurrected inside it.
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(unit->was_window);
    EXPECT_EQ(unit->tabs.size(), 3uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(unit->tabs[1].active_url.serialize(), "https://c.example/"sv);
    EXPECT_EQ(unit->tabs[2].active_url.serialize(), "https://b.example/"sv);
}

TEST_CASE(topology_barrier_gates_opens_after_a_failed_move)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto first_window = TRY_OR_FAIL(store->window_opened());
    auto second_window = TRY_OR_FAIL(store->window_opened());
    auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto second = TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    (void)TRY_OR_FAIL(store->tab_opened({ .window_id = second_window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    (void)first;

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE UPDATE OF session_id ON SessionTabs BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    store->tab_moved({ .tab_id = second, .new_window_id = second_window, .ordinal = 1 });

    // The persisted topology is stale until the drain succeeds, so dependent opens must not
    // commit a single window's metadata.
    EXPECT(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://d.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }).is_error());

    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    (void)TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://d.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

    TRY_OR_FAIL(store->window_closing({ .window_id = second_window, .active_tab_index = 0, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }));
    TRY_OR_FAIL(store->window_closing({ .window_id = first_window, .active_tab_index = 0, .closed_at = UnixDateTime::from_seconds_since_epoch(200) }));

    auto first_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(first_unit.has_value());
    EXPECT_EQ(first_unit->tabs.size(), 2uz);
    EXPECT_EQ(first_unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(first_unit->tabs[1].active_url.serialize(), "https://d.example/"sv);

    auto second_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(second_unit.has_value());
    EXPECT_EQ(second_unit->tabs.size(), 2uz);
    EXPECT_EQ(second_unit->tabs[0].active_url.serialize(), "https://c.example/"sv);
    EXPECT_EQ(second_unit->tabs[1].active_url.serialize(), "https://b.example/"sv);
}

TEST_CASE(topology_barrier_gates_close_retries_after_a_failed_move)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto first_window = TRY_OR_FAIL(store->window_opened());
        auto second_window = TRY_OR_FAIL(store->window_opened());
        auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto last = TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = second_window, .initial_url = parse_url("https://d.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        EXPECT(store->tab_closed({ .tab_id = last, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }).is_error());
        TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));

        // Reparenting into the second window (session id 2) keeps failing, so the drain stays stale
        // while the pending close's own writes would succeed.
        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedMoveFailure BEFORE UPDATE OF session_id ON SessionTabs WHEN NEW.session_id = 2 BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        store->tab_moved({ .tab_id = first, .new_window_id = second_window, .ordinal = 1 });
        store->flush_dirty_state();
        EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 1;"_string), 0);
    }
    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedMoveFailure;"sv));

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto second_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(second_unit.has_value());
    EXPECT_EQ(second_unit->tabs.size(), 1uz);
    EXPECT_EQ(second_unit->tabs[0].active_url.serialize(), "https://d.example/"sv);

    auto first_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(first_unit.has_value());
    EXPECT_EQ(first_unit->tabs.size(), 3uz);
    EXPECT_EQ(first_unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(first_unit->tabs[1].active_url.serialize(), "https://b.example/"sv);
    EXPECT_EQ(first_unit->tabs[2].active_url.serialize(), "https://c.example/"sv);
}

TEST_CASE(retired_window_topology_drains_after_a_failed_move)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto first_window = TRY_OR_FAIL(store->window_opened());
        auto second_window = TRY_OR_FAIL(store->window_opened());
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto middle = TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = second_window, .initial_url = parse_url("https://d.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE UPDATE OF session_id ON SessionTabs BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        store->tab_moved({ .tab_id = middle, .new_window_id = second_window, .ordinal = 1 });
        TRY_OR_FAIL(store->window_closing({ .window_id = first_window, .active_tab_index = 0, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }));
        TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));

        // The drain must still cover the retired source window even though the window close itself
        // keeps failing.
        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedKindFailure BEFORE UPDATE OF kind ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        store->flush_dirty_state();
        EXPECT(!store->has_closed_units());
    }
    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedKindFailure;"sv));

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto second_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(second_unit.has_value());
    EXPECT_EQ(second_unit->tabs.size(), 2uz);
    EXPECT_EQ(second_unit->tabs[0].active_url.serialize(), "https://d.example/"sv);
    EXPECT_EQ(second_unit->tabs[1].active_url.serialize(), "https://b.example/"sv);

    auto first_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(first_unit.has_value());
    EXPECT_EQ(first_unit->tabs.size(), 2uz);
    EXPECT_EQ(first_unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(first_unit->tabs[1].active_url.serialize(), "https://c.example/"sv);
}

TEST_CASE(aborted_quit_still_drains_a_retired_windows_topology)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto first_window = TRY_OR_FAIL(store->window_opened());
        auto second_window = TRY_OR_FAIL(store->window_opened());
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto middle = TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = second_window, .initial_url = parse_url("https://d.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE UPDATE OF session_id ON SessionTabs BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        store->tab_moved({ .tab_id = middle, .new_window_id = second_window, .ordinal = 1 });
        store->application_quitting();
        TRY_OR_FAIL(store->window_closing({ .window_id = first_window, .active_tab_index = 0, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }));
        store->application_quit_aborted();

        TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
        store->flush_dirty_state();
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto second_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(second_unit.has_value());
    EXPECT_EQ(second_unit->tabs.size(), 2uz);
    EXPECT_EQ(second_unit->tabs[0].active_url.serialize(), "https://d.example/"sv);
    EXPECT_EQ(second_unit->tabs[1].active_url.serialize(), "https://b.example/"sv);

    auto first_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(first_unit.has_value());
    EXPECT_EQ(first_unit->tabs.size(), 2uz);
    EXPECT_EQ(first_unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(first_unit->tabs[1].active_url.serialize(), "https://c.example/"sv);
}

TEST_CASE(detaching_a_tab_discards_its_rows)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto middle = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

        store->update_tab_state({ .tab_id = middle, .history = make_snapshot({ "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
        store->flush_dirty_state();

        store->tab_detached(middle);
        EXPECT(!store->has_closed_units());
        EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 2);
        EXPECT_EQ(count_entry_rows(*database, middle), 0);
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(unit->was_window);
    EXPECT_EQ(unit->tabs.size(), 2uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(unit->tabs[1].active_url.serialize(), "https://c.example/"sv);
}

TEST_CASE(detaching_an_empty_window_discards_it_without_a_closed_unit)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    store->window_detached(window);

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions;"_string), 0);
    EXPECT(!store->has_closed_units());
}

TEST_CASE(failed_empty_window_detach_retries_from_the_flush)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE DELETE ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    store->window_detached(window);

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions;"_string), 1);
    EXPECT(!store->has_closed_units());

    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    store->flush_dirty_state();

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions;"_string), 0);
    EXPECT(!store->has_closed_units());
}

TEST_CASE(empty_window_detach_waits_for_a_pending_tab_close)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }).is_error());
    store->window_detached(window);

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 0;"_string), 1);
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 1);

    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    store->flush_dirty_state();

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(!unit->was_window);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 0;"_string), 0);
}

TEST_CASE(failed_detach_retries_from_the_flush)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto middle = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE DELETE ON SessionTabs BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        store->tab_detached(middle);
        EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 3);

        TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
        store->flush_dirty_state();
        EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 2);
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 2uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(unit->tabs[1].active_url.serialize(), "https://c.example/"sv);
}

TEST_CASE(tab_placement_and_activation_persist_with_registration)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)first;
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = 0, .is_active = SessionStore::IsActive::Yes }));
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 2uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://b.example/"sv);
    EXPECT_EQ(unit->tabs[1].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(unit->active_tab_index, 0);
}

TEST_CASE(closing_a_tab_commits_the_remaining_active_index)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto second = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        store->active_tab_changed(second);
        TRY_OR_FAIL(store->tab_closed({ .tab_id = first, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }));
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(window_unit.has_value());
    EXPECT(window_unit->was_window);
    EXPECT_EQ(window_unit->tabs.size(), 2uz);
    EXPECT_EQ(window_unit->tabs[0].active_url.serialize(), "https://b.example/"sv);
    EXPECT_EQ(window_unit->active_tab_index, 0);
}

TEST_CASE(child_close_retries_against_its_retired_windows_unit)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        auto middle = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        (void)TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        EXPECT(store->tab_closed({ .tab_id = middle, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }).is_error());
        TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::from_seconds_since_epoch(200) }));

        // The child retry succeeds against the retired window's still-open unit; the parent close
        // is then forced to keep failing.
        TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedKindFailure BEFORE UPDATE OF kind ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        store->flush_dirty_state();
        EXPECT(store->has_closed_units());
    }
    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedKindFailure;"sv));

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(window_unit.has_value());
    EXPECT(window_unit->was_window);
    EXPECT_EQ(window_unit->tabs.size(), 2uz);
    EXPECT_EQ(window_unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(window_unit->tabs[1].active_url.serialize(), "https://c.example/"sv);

    auto tab_unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(tab_unit.has_value());
    EXPECT(!tab_unit->was_window);
    EXPECT_EQ(tab_unit->tabs[0].active_url.serialize(), "https://b.example/"sv);
}

TEST_CASE(clearing_history_keeps_a_tombstone_when_deletion_fails)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }).is_error());
    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedDeleteFailure BEFORE DELETE ON SessionTabs BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));

    EXPECT(store->remove_entries_accessed_since(UnixDateTime {}).is_error());
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 1);

    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedDeleteFailure;"sv));
    store->flush_dirty_state();

    EXPECT(!store->has_closed_units());
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 0);
}

TEST_CASE(clearing_persisted_history_retries_when_deletion_fails)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));

        auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }));

        TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE DELETE ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
        EXPECT(store->remove_entries_accessed_since(UnixDateTime {}).is_error());
        EXPECT(!store->has_closed_units());
        EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 1;"_string), 1);

        auto newer_tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        TRY_OR_FAIL(store->tab_closed({ .tab_id = newer_tab, .closed_at = UnixDateTime::from_seconds_since_epoch(200) }));

        TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
        store->flush_dirty_state();

        EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 1;"_string), 1);
    }

    auto reopened_store = TRY_OR_FAIL(SessionStore::create(*database));
    auto unit = TRY_OR_FAIL(reopened_store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://b.example/"sv);
    EXPECT(!reopened_store->has_closed_units());
}

TEST_CASE(clearing_history_drops_pending_closes)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }).is_error());
    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));

    TRY_OR_FAIL(store->remove_entries_accessed_since(UnixDateTime {}));
    store->flush_dirty_state();

    EXPECT(!store->has_closed_units());
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 0);
}

TEST_CASE(quitting_flushes_without_demoting_and_recovery_restores)
{
    auto database = create_session_database();

    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });

        store->application_quitting();
        TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }));
        TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));

        EXPECT(!store->has_closed_units());
        EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 0;"_string), 1);
    }

    auto reopened_store = TRY_OR_FAIL(SessionStore::create(*database));
    EXPECT(reopened_store->has_closed_units());
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 2;"_string), 1);

    auto unit = TRY_OR_FAIL(reopened_store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(unit->was_window);
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://b.example/"sv);
    EXPECT(unit->tabs[0].history.has_value());
    EXPECT_EQ(unit->tabs[0].history->entries.size(), 2uz);
}

TEST_CASE(aborted_quit_resumes_demotion)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });

    store->application_quitting();
    store->application_quit_aborted();
    TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }));

    EXPECT(store->has_closed_units());
}

TEST_CASE(a_first_run_counts_as_following_a_clean_exit)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    EXPECT_EQ(store->consecutive_unclean_exits(), 0u);
}

TEST_CASE(a_corrupt_lifecycle_row_counts_as_following_a_clean_exit)
{
    auto database = create_session_database();
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionLifecycle (id, clean_exit, consecutive_unclean_exits) VALUES (1, 0, -1);"sv));

    EXPECT_EQ(TRY_OR_FAIL(SessionStore::create(*database))->consecutive_unclean_exits(), 0u);

    // The run start rewrote the row, so the next run counts the one before it as unclean.
    EXPECT_EQ(TRY_OR_FAIL(SessionStore::create(*database))->consecutive_unclean_exits(), 1u);
}

TEST_CASE(unclean_exits_count_up_until_a_clean_exit)
{
    auto database = create_session_database();

    // A store destroyed without mark_clean_exit() is what a crash leaves behind.
    (void)TRY_OR_FAIL(SessionStore::create(*database));
    EXPECT_EQ(TRY_OR_FAIL(SessionStore::create(*database))->consecutive_unclean_exits(), 1u);
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        EXPECT_EQ(store->consecutive_unclean_exits(), 2u);
        store->mark_clean_exit();
    }
    EXPECT_EQ(TRY_OR_FAIL(SessionStore::create(*database))->consecutive_unclean_exits(), 0u);
}

TEST_CASE(the_default_policy_keeps_leftover_windows_for_reopening)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, window, "https://a.example/"sv);
        quit_cleanly(*store, { window });
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    EXPECT(store->take_launch_plan().is_empty());
    EXPECT(store->has_closed_units());
}

TEST_CASE(everything_re_adopts_leftover_windows_in_place)
{
    auto database = create_session_database();
    SessionWindowId first_window = 0;
    SessionWindowId second_window = 0;
    SessionTabId a = 0;
    SessionTabId b = 0;
    SessionTabId c = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        first_window = TRY_OR_FAIL(store->window_opened());
        second_window = TRY_OR_FAIL(store->window_opened());
        a = open_tab(*store, first_window, "https://a.example/"sv);
        b = open_tab(*store, first_window, "https://b.example/"sv);
        c = open_tab(*store, second_window, "https://c.example/"sv);
        store->active_tab_changed(b);
        // The first window ends up frontmost.
        store->window_activated(second_window);
        store->window_activated(first_window);
        quit_cleanly(*store, { first_window, second_window });
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 2uz);
    EXPECT_EQ(plan[0].window_id, second_window);
    EXPECT_EQ(plan[0].tabs.size(), 1uz);
    EXPECT_EQ(plan[0].tabs[0].tab_id, c);
    EXPECT_EQ(plan[1].window_id, first_window);
    EXPECT_EQ(plan[1].tabs.size(), 2uz);
    EXPECT_EQ(plan[1].tabs[0].tab_id, a);
    EXPECT_EQ(plan[1].tabs[1].tab_id, b);
    EXPECT_EQ(plan[1].tabs[1].url.serialize(), "https://b.example/"sv);
    EXPECT_EQ(plan[1].active_tab_index, 1);
    EXPECT(!store->has_closed_units());

    // The plan is handed over once.
    EXPECT(store->take_launch_plan().is_empty());
}

TEST_CASE(a_never_selected_tab_keeps_its_history_across_relaunches)
{
    auto database = create_session_database();
    SessionWindowId window = 0;
    SessionTabId tab = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        window = TRY_OR_FAIL(store->window_opened());
        tab = open_tab(*store, window, "about:blank"sv);
        store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
        quit_cleanly(*store, { window });
    }

    // Relaunches that never select the tab never read or rewrite its rows.
    for (int relaunch = 0; relaunch < 2; ++relaunch) {
        auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
        EXPECT_EQ(store->take_launch_plan().size(), 1uz);
        quit_cleanly(*store, { window });
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    EXPECT_EQ(store->take_launch_plan().size(), 1uz);
    auto history = store->load_tab_history(tab);
    EXPECT(history.has_value());
    EXPECT_EQ(history->entries.size(), 2uz);
    EXPECT_EQ(history->current_used_step_index, 1uz);
}

TEST_CASE(pinned_only_splits_pinned_tabs_from_the_rest)
{
    auto database = create_session_database();
    SessionWindowId window = 0;
    SessionTabId first_pinned = 0;
    SessionTabId second_pinned = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        window = TRY_OR_FAIL(store->window_opened());
        first_pinned = open_tab(*store, window, "https://a.example/"sv);
        second_pinned = open_tab(*store, window, "https://b.example/"sv);
        auto unpinned = open_tab(*store, window, "https://c.example/"sv);
        TRY_OR_FAIL(store->tab_pinned_changed(first_pinned, true));
        TRY_OR_FAIL(store->tab_pinned_changed(second_pinned, true));
        store->active_tab_changed(unpinned);
        quit_cleanly(*store, { window });
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::PinnedOnly));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 1uz);
    EXPECT(plan[0].window_id != window);
    EXPECT_EQ(plan[0].tabs.size(), 2uz);
    EXPECT_EQ(plan[0].tabs[0].tab_id, first_pinned);
    EXPECT_EQ(plan[0].tabs[1].tab_id, second_pinned);
    EXPECT(plan[0].tabs[0].pinned);
    EXPECT(plan[0].tabs[1].pinned);

    // The unpinned remainder stays on the Reopen Closed Window stack, as the same unit.
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT kind FROM Sessions WHERE id = {};", window))), 2);
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(unit->was_window);
    EXPECT_EQ(unit->origin, ClosedSessionUnit::Origin::StartupRecovery);
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://c.example/"sv);
}

TEST_CASE(pinned_only_leaves_no_unit_for_a_window_with_only_pinned_tabs)
{
    auto database = create_session_database();
    SessionWindowId window = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        window = TRY_OR_FAIL(store->window_opened());
        auto first = open_tab(*store, window, "https://a.example/"sv);
        auto second = open_tab(*store, window, "https://b.example/"sv);
        TRY_OR_FAIL(store->tab_pinned_changed(first, true));
        TRY_OR_FAIL(store->tab_pinned_changed(second, true));
        quit_cleanly(*store, { window });
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::PinnedOnly));
    EXPECT_EQ(store->take_launch_plan().size(), 1uz);
    EXPECT(!store->has_closed_units());
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT COUNT(*) FROM Sessions WHERE id = {};", window))), 0);
}

TEST_CASE(pinned_only_keeps_a_window_without_pins_for_reopening)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, window, "https://a.example/"sv);
        quit_cleanly(*store, { window });
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::PinnedOnly));
    EXPECT(store->take_launch_plan().is_empty());
    EXPECT(store->has_closed_units());
}

TEST_CASE(closing_the_last_window_restores_it_at_launch)
{
    auto database = create_session_database();
    SessionWindowId window = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, window, "https://a.example/"sv);
        (void)open_tab(*store, window, "https://b.example/"sv);
        TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 1, .closed_at = UnixDateTime::now() }));
        store->mark_clean_exit();
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 1uz);
    EXPECT_EQ(plan[0].window_id, window);
    EXPECT_EQ(plan[0].tabs.size(), 2uz);
    EXPECT_EQ(plan[0].active_tab_index, 1);
    EXPECT(!store->has_closed_units());
}

TEST_CASE(pinned_only_restores_the_last_windows_pins)
{
    auto database = create_session_database();
    SessionWindowId window = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        window = TRY_OR_FAIL(store->window_opened());
        auto pinned = open_tab(*store, window, "https://a.example/"sv);
        (void)open_tab(*store, window, "https://b.example/"sv);
        TRY_OR_FAIL(store->tab_pinned_changed(pinned, true));
        TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));
        store->mark_clean_exit();
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::PinnedOnly));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 1uz);
    EXPECT_EQ(plan[0].tabs.size(), 1uz);
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://b.example/"sv);
}

TEST_CASE(closing_one_of_two_windows_doesnt_restore_it_at_launch)
{
    auto database = create_session_database();
    SessionWindowId second_window = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto first_window = TRY_OR_FAIL(store->window_opened());
        second_window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, first_window, "https://a.example/"sv);
        (void)open_tab(*store, second_window, "https://b.example/"sv);
        TRY_OR_FAIL(store->window_closing({ .window_id = first_window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));
        quit_cleanly(*store, { second_window });
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 1uz);
    EXPECT_EQ(plan[0].window_id, second_window);
    EXPECT(store->has_closed_units());
}

TEST_CASE(opening_a_window_cancels_the_last_windows_restore)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, window, "https://a.example/"sv);
        TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));
        // A window that's still empty at the quit is discarded at the next launch, so only the flag's fate matters.
        (void)TRY_OR_FAIL(store->window_opened());
        store->mark_clean_exit();
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    EXPECT(store->take_launch_plan().is_empty());
    EXPECT(store->has_closed_units());
}

TEST_CASE(reopening_the_last_window_cancels_its_restore)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, window, "https://a.example/"sv);
        TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));
        EXPECT(TRY_OR_FAIL(store->take_most_recently_closed()).has_value());
        store->mark_clean_exit();
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    EXPECT(store->take_launch_plan().is_empty());
}

TEST_CASE(clearing_history_cancels_the_last_windows_restore)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, window, "https://a.example/"sv);
        TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));
        TRY_OR_FAIL(store->remove_entries_accessed_since(UnixDateTime::epoch()));
        store->mark_clean_exit();
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    EXPECT(store->take_launch_plan().is_empty());
    EXPECT(!store->has_closed_units());
}

TEST_CASE(a_second_unclean_exit_in_a_row_stops_launch_restore)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, window, "https://a.example/"sv);
        // Destroyed without mark_clean_exit(): a crash.
    }
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
        EXPECT_EQ(store->take_launch_plan().size(), 1uz);
        // And a second crash.
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    EXPECT(store->take_launch_plan().is_empty());
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->origin, ClosedSessionUnit::Origin::StartupRecovery);
}

TEST_CASE(a_clean_exit_resumes_launch_restore)
{
    auto database = create_session_database();
    // Two crashes in a row.
    for (int run = 0; run < 2; ++run) {
        auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, window, "https://a.example/"sv);
    }
    SessionWindowId window = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
        EXPECT(store->take_launch_plan().is_empty());
        window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, window, "https://b.example/"sv);
        quit_cleanly(*store, { window });
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 1uz);
    EXPECT_EQ(plan[0].window_id, window);
}

TEST_CASE(everything_moves_pinned_tabs_ahead_of_unpinned_ones)
{
    auto database = create_session_database();
    SessionWindowId window = 0;
    SessionTabId a = 0;
    SessionTabId b = 0;
    SessionTabId c = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        window = TRY_OR_FAIL(store->window_opened());
        a = open_tab(*store, window, "https://a.example/"sv);
        b = open_tab(*store, window, "https://b.example/"sv);
        c = open_tab(*store, window, "https://c.example/"sv);
        store->active_tab_changed(b);
        quit_cleanly(*store, { window });
    }

    // A saved session whose last tab is pinned, with the unpinned tabs still ahead of it.
    TRY_OR_FAIL(database->execute_raw(ByteString::formatted("UPDATE SessionTabs SET pinned = 1 WHERE id = {};", c)));

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 1uz);
    EXPECT_EQ(plan[0].tabs.size(), 3uz);
    EXPECT_EQ(plan[0].tabs[0].tab_id, c);
    EXPECT(plan[0].tabs[0].pinned);
    EXPECT_EQ(plan[0].tabs[1].tab_id, a);
    EXPECT_EQ(plan[0].tabs[2].tab_id, b);
    EXPECT_EQ(plan[0].active_tab_index, 2);

    // The corrected order is saved too.
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT tab_ordinal FROM SessionTabs WHERE id = {};", c))), 0);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT tab_ordinal FROM SessionTabs WHERE id = {};", b))), 2);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT active_tab_index FROM Sessions WHERE id = {};", window))), 2);
}

TEST_CASE(a_corrupt_tab_row_drops_only_that_tab_from_the_launch_plan)
{
    auto database = create_session_database();
    SessionTabId a = 0;
    SessionTabId b = 0;
    SessionTabId c = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        a = open_tab(*store, window, "https://a.example/"sv);
        b = open_tab(*store, window, "https://b.example/"sv);
        c = open_tab(*store, window, "https://c.example/"sv);
        quit_cleanly(*store, { window });
    }
    TRY_OR_FAIL(database->execute_raw(MUST(String::formatted("UPDATE SessionTabs SET active_url = 'not a url' WHERE id = {};", b)).to_byte_string()));

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 1uz);
    EXPECT_EQ(plan[0].tabs.size(), 2uz);
    EXPECT_EQ(plan[0].tabs[0].tab_id, a);
    EXPECT_EQ(plan[0].tabs[1].tab_id, c);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT COUNT(*) FROM SessionTabs WHERE id = {};", b))), 0);
}

TEST_CASE(a_window_whose_every_tab_row_is_corrupt_leaves_no_launch_plan_entry)
{
    auto database = create_session_database();
    SessionWindowId emptied = 0;
    SessionTabId kept = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        emptied = TRY_OR_FAIL(store->window_opened());
        auto lost = open_tab(*store, emptied, "https://lost.example/"sv);
        auto other = TRY_OR_FAIL(store->window_opened());
        kept = open_tab(*store, other, "https://kept.example/"sv);
        quit_cleanly(*store, { emptied, other });
        TRY_OR_FAIL(database->execute_raw(MUST(String::formatted("UPDATE SessionTabs SET active_url = 'not a url' WHERE id = {};", lost)).to_byte_string()));
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 1uz);
    EXPECT_EQ(plan[0].tabs.size(), 1uz);
    EXPECT_EQ(plan[0].tabs[0].tab_id, kept);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT COUNT(*) FROM Sessions WHERE id = {};", emptied))), 0);
}

TEST_CASE(pinned_only_with_a_corrupt_row_in_a_window_without_pins_drops_only_that_tab)
{
    auto database = create_session_database();
    SessionTabId a = 0;
    SessionTabId b = 0;
    SessionTabId c = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        a = open_tab(*store, window, "https://a.example/"sv);
        b = open_tab(*store, window, "https://b.example/"sv);
        c = open_tab(*store, window, "https://c.example/"sv);
        quit_cleanly(*store, { window });
    }
    TRY_OR_FAIL(database->execute_raw(MUST(String::formatted("UPDATE SessionTabs SET active_url = 'not a url' WHERE id = {};", b)).to_byte_string()));

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::PinnedOnly));
    EXPECT(store->take_launch_plan().is_empty());
    EXPECT(store->has_closed_units());
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 2uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://a.example/"sv);
    EXPECT_EQ(unit->tabs[1].active_url.serialize(), "https://c.example/"sv);
    (void)a;
    (void)c;
}

TEST_CASE(pinned_only_maps_the_active_tab_into_the_pinned_window_and_the_remainder)
{
    auto database = create_session_database();
    SessionTabId p1 = 0;
    SessionTabId p2 = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        p1 = open_tab(*store, window, "https://p1.example/"sv);
        (void)open_tab(*store, window, "https://u1.example/"sv);
        p2 = open_tab(*store, window, "https://p2.example/"sv);
        (void)open_tab(*store, window, "https://u2.example/"sv);
        TRY_OR_FAIL(store->tab_pinned_changed(p1, true));
        TRY_OR_FAIL(store->tab_pinned_changed(p2, true));
        store->active_tab_changed(p2);
        quit_cleanly(*store, { window });
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::PinnedOnly));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 1uz);
    EXPECT_EQ(plan[0].tabs.size(), 2uz);
    EXPECT_EQ(plan[0].tabs[0].tab_id, p1);
    EXPECT_EQ(plan[0].tabs[1].tab_id, p2);
    EXPECT_EQ(plan[0].active_tab_index, 1);

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 2uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://u1.example/"sv);
    EXPECT_EQ(unit->tabs[1].active_url.serialize(), "https://u2.example/"sv);
}

TEST_CASE(a_transient_store_has_no_launch_plan)
{
    auto store = SessionStore::create();
    EXPECT(store->take_launch_plan().is_empty());
    EXPECT(!store->load_tab_history(1).has_value());
}

static constexpr SessionWindowGeometry test_geometry { .x = 10, .y = 20, .width = 800, .height = 600, .maximized = true };

TEST_CASE(window_geometry_waits_for_a_flush)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window = TRY_OR_FAIL(store->window_opened());
    (void)open_tab(*store, window, "https://a.example/"sv);

    store->window_geometry_changed(window, { .x = 10, .y = 20, .width = 800, .height = 600, .maximized = false });
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT COUNT(*) FROM Sessions WHERE id = {} AND width IS NULL;", window))), 1);

    store->flush_dirty_state();
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT width FROM Sessions WHERE id = {};", window))), 800);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT maximized FROM Sessions WHERE id = {};", window))), 0);
}

TEST_CASE(a_closed_window_keeps_its_geometry)
{
    auto database = create_session_database();
    SessionWindowId first_window = 0;
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        first_window = TRY_OR_FAIL(store->window_opened());
        auto second_window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, first_window, "https://a.example/"sv);
        (void)open_tab(*store, second_window, "https://b.example/"sv);
        store->window_geometry_changed(first_window, test_geometry);
        TRY_OR_FAIL(store->window_closing({ .window_id = first_window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));
        quit_cleanly(*store, { second_window });
    }

    // After a relaunch, the unit's geometry comes from its row.
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    Optional<ClosedSessionUnit> first_window_unit;
    for (;;) {
        auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
        if (!unit.has_value())
            break;
        if (unit->tabs[0].active_url.serialize() == "https://a.example/"sv)
            first_window_unit = unit.release_value();
    }
    EXPECT(first_window_unit.has_value());
    EXPECT(first_window_unit->geometry.has_value());
    EXPECT(*first_window_unit->geometry == test_geometry);
}

TEST_CASE(transient_closed_windows_keep_their_geometry)
{
    auto store = SessionStore::create();
    auto window = TRY_OR_FAIL(store->window_opened());
    (void)open_tab(*store, window, "https://a.example/"sv);
    store->window_geometry_changed(window, test_geometry);
    TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::now() }));

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(unit->geometry.has_value());
    EXPECT(*unit->geometry == test_geometry);
}

TEST_CASE(launch_plans_keep_window_geometry)
{
    for (auto policy : { SessionLaunchPolicy::Everything, SessionLaunchPolicy::PinnedOnly }) {
        auto database = create_session_database();
        {
            auto store = TRY_OR_FAIL(SessionStore::create(*database));
            auto window = TRY_OR_FAIL(store->window_opened());
            auto tab = open_tab(*store, window, "https://a.example/"sv);
            TRY_OR_FAIL(store->tab_pinned_changed(tab, true));
            store->window_geometry_changed(window, test_geometry);
            quit_cleanly(*store, { window });
        }

        auto store = TRY_OR_FAIL(SessionStore::create(*database, policy));
        auto plan = store->take_launch_plan();
        EXPECT_EQ(plan.size(), 1uz);
        EXPECT(plan[0].geometry.has_value());
        EXPECT(*plan[0].geometry == test_geometry);

        // The window the plan keeps has its row hold the geometry too.
        EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT COUNT(*) FROM Sessions WHERE id = {} AND x = 10 AND y = 20 AND width = 800 AND height = 600 AND maximized = 1;", plan[0].window_id))), 1);
    }
}

TEST_CASE(a_corrupt_saved_geometry_is_dropped_instead_of_failing_recovery)
{
    auto database = create_session_database();
    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        (void)open_tab(*store, window, "https://a.example/"sv);
        store->window_geometry_changed(window, test_geometry);
        store->flush_dirty_state();
        TRY_OR_FAIL(database->execute_raw("UPDATE Sessions SET width = 'wide' WHERE kind = 0;"sv));
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database, SessionLaunchPolicy::Everything));
    auto plan = store->take_launch_plan();
    EXPECT_EQ(plan.size(), 1uz);
    EXPECT(!plan[0].geometry.has_value());
}

TEST_CASE(a_pending_tab_close_in_a_closing_window_leaves_the_saved_geometry)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window = TRY_OR_FAIL(store->window_opened());
    (void)open_tab(*store, window, "https://a.example/"sv);
    auto second = open_tab(*store, window, "https://b.example/"sv);
    store->window_geometry_changed(window, test_geometry);
    store->flush_dirty_state();

    // The tab close fails and the window close stays pending, so the tab's close is retried against a window that's
    // already closing.
    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedTabFailure BEFORE INSERT ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedKindFailure BEFORE UPDATE OF kind ON Sessions BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_closed({ .tab_id = second, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }).is_error());
    (void)store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::from_seconds_since_epoch(101) });

    // The row's width is changed behind the store's back, so a retry that rewrote the window's geometry would show.
    TRY_OR_FAIL(database->execute_raw("UPDATE Sessions SET width = 999;"sv));
    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedTabFailure;"sv));
    store->flush_dirty_state();

    EXPECT_EQ(select_i64(*database, "SELECT width FROM Sessions WHERE kind = 0;"_string), 999);
}

TEST_CASE(retention_keeps_the_newest_closed_units)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    for (size_t i = 0; i < 27; ++i) {
        auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url(MUST(String::formatted("https://tab{}.example/", i))) });
        TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::from_seconds_since_epoch(static_cast<i64>(i + 1)) }));
    }
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 1;"_string), 25);

    for (size_t i = 0; i < 6; ++i) {
        auto window = TRY_OR_FAIL(store->window_opened());
        auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
        TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 0, .closed_at = UnixDateTime::from_seconds_since_epoch(static_cast<i64>(100 + i)) }));
    }
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 2;"_string), 5);

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM SessionTabs;"_string), 30);
}

TEST_CASE(remove_entries_accessed_since_deletes_closed_units)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    for (i64 time : { 100, 200 }) {
        auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
        store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
        TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::from_seconds_since_epoch(time) }));
    }

    TRY_OR_FAIL(store->remove_entries_accessed_since(UnixDateTime::from_seconds_since_epoch(150)));

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 1;"_string), 1);
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(!store->has_closed_units());
}

TEST_CASE(create_requires_foreign_keys_enabled)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    TRY_OR_FAIL(SessionStore::migrate_schema(*database));

    auto result = SessionStore::create(*database);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session storage relies on cascading deletes; open the connection with foreign keys enabled"sv);
}

TEST_CASE(transient_store_round_trips_in_memory)
{
    auto store = SessionStore::create();

    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv, "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }));

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT(unit->tabs[0].history.has_value());
    EXPECT_EQ(unit->tabs[0].history->entries.size(), 2uz);
    EXPECT(!store->has_closed_units());
}

TEST_CASE(corrupt_closed_rows_are_dropped_at_startup)
{
    auto database = create_session_database();

    // Non-contiguous tab ordinals (0, 2) mark the first unit corrupt; the second is valid; the
    // third has a step index of the wrong storage class.
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (500, 1, 5000, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (501, 500, 0, 'https://a.example/', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (502, 500, 2, 'https://b.example/', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (600, 1, 6000, 99);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (601, 600, 0, 'https://c.example/', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (700, 7, 7000, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (800, 1, 8000, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (801, 800, 0, 'https://d.example/', 'corrupt');"sv));

    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE id = 500;"_string), 0);
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE id = 800;"_string), 0);

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://c.example/"sv);
    // The out-of-range persisted index was clamped instead of dropping the unit.
    EXPECT_EQ(unit->active_tab_index, 0);

    EXPECT(!store->has_closed_units());
}

TEST_CASE(negative_active_tab_index_retires_only_its_own_unit)
{
    auto database = create_session_database();

    // The invalid middle unit must not abort loading the surrounding valid units.
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (500, 1, 5000, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (501, 500, 0, 'https://older.example/', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (600, 1, 6000, -1);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (601, 600, 0, 'https://corrupt.example/', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (700, 1, 7000, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (701, 700, 0, 'https://newer.example/', 0);"sv));

    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE id = 600;"_string), 0);

    auto newer = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(newer.has_value());
    EXPECT_EQ(newer->tabs.size(), 1uz);
    EXPECT_EQ(newer->tabs[0].active_url.serialize(), "https://newer.example/"sv);

    auto older = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(older.has_value());
    EXPECT_EQ(older->tabs.size(), 1uz);
    EXPECT_EQ(older->tabs[0].active_url.serialize(), "https://older.example/"sv);

    EXPECT(!store->has_closed_units());
}

TEST_CASE(non_integer_closed_time_retires_only_its_own_unit)
{
    auto database = create_session_database();

    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (500, 1, 5000, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (501, 500, 0, 'https://older.example/', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (600, 1, 'corrupt', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (601, 600, 0, 'https://corrupt.example/', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (700, 1, 7000, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (701, 700, 0, 'https://newer.example/', 0);"sv));

    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE id = 600;"_string), 0);
    auto newer = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(newer.has_value());
    EXPECT_EQ(newer->tabs[0].active_url.serialize(), "https://newer.example/"sv);
    auto older = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(older.has_value());
    EXPECT_EQ(older->tabs[0].active_url.serialize(), "https://older.example/"sv);
    EXPECT(!store->has_closed_units());
}

TEST_CASE(tampered_oversized_active_url_skips_only_its_tab)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto earlier = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = earlier, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
    TRY_OR_FAIL(store->tab_closed({ .tab_id = earlier, .closed_at = UnixDateTime::from_seconds_since_epoch(100) }));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto tampered = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto sibling = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tampered, .history = make_snapshot({ "https://b.example/"sv }), .url = parse_url("https://b.example/"sv) });
    store->update_tab_state({ .tab_id = sibling, .history = make_snapshot({ "https://c.example/"sv }), .url = parse_url("https://c.example/"sv) });
    TRY_OR_FAIL(store->window_closing({ .window_id = window, .active_tab_index = 1, .closed_at = UnixDateTime::from_seconds_since_epoch(200) }));

    // Bypass the writer to insert an over-cap cell.
    TRY_OR_FAIL(database->execute_raw(ByteString::formatted("UPDATE SessionTabs SET active_url = '{}' WHERE id = {};", ByteString::repeated('a', (8uz * 1024 * 1024) + 1), tampered)));

    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT(unit->was_window);
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->active_tab_index, 0);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://c.example/"sv);
    EXPECT(unit->tabs[0].history.has_value());
    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM Sessions WHERE kind = 2;"_string), 0);

    auto next = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(next.has_value());
    EXPECT_EQ(next->tabs.size(), 1uz);
    EXPECT_EQ(next->tabs[0].active_url.serialize(), "https://a.example/"sv);
}

TEST_CASE(tampered_only_tab_retires_an_empty_closed_unit)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = {}, .initial_url = parse_url("about:blank"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->update_tab_state({ .tab_id = tab, .history = make_snapshot({ "https://a.example/"sv }), .url = parse_url("https://a.example/"sv) });
    TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }));
    TRY_OR_FAIL(database->execute_raw(ByteString::formatted("UPDATE SessionTabs SET active_url = 'http://[invalid' WHERE id = {};", tab)));

    EXPECT(!TRY_OR_FAIL(store->take_most_recently_closed()).has_value());
    EXPECT(!store->has_closed_units());
}

TEST_CASE(moving_a_tab_writes_both_windows_ordinals_at_once)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto first_window = TRY_OR_FAIL(store->window_opened());
    auto second_window = TRY_OR_FAIL(store->window_opened());
    auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto second = TRY_OR_FAIL(store->tab_opened({ .window_id = first_window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto third = TRY_OR_FAIL(store->tab_opened({ .window_id = second_window, .initial_url = parse_url("https://c.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

    store->tab_moved({ .tab_id = first, .new_window_id = second_window, .ordinal = 0 });

    // No flush: the move itself must leave every ordinal contiguous in both windows.
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT session_id FROM SessionTabs WHERE id = {};", first))), second_window);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT tab_ordinal FROM SessionTabs WHERE id = {};", first))), 0);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT tab_ordinal FROM SessionTabs WHERE id = {};", third))), 1);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT tab_ordinal FROM SessionTabs WHERE id = {};", second))), 0);
}

TEST_CASE(moving_the_active_tab_within_its_window_keeps_it_active)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto second = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    (void)second;

    store->active_tab_changed(first);
    store->tab_moved({ .tab_id = first, .new_window_id = window, .ordinal = 1 });
    store->flush_dirty_state();

    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT active_tab_index FROM Sessions WHERE id = {};", window))), 1);
}

TEST_CASE(window_metadata_flushes_order_and_active_tab)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto second = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

    store->tab_order_changed({ .window_id = window, .ordered_tabs = { second, first } });
    store->active_tab_changed(first);
    store->flush_dirty_state();

    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT tab_ordinal FROM SessionTabs WHERE id = {};", second))), 0);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT tab_ordinal FROM SessionTabs WHERE id = {};", first))), 1);
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT active_tab_index FROM Sessions WHERE id = {};", window))), 1);
}

TEST_CASE(unchanged_window_metadata_does_not_flush)
{
    auto database = create_session_database();
    auto store = TRY_OR_FAIL(SessionStore::create(*database));

    auto window = TRY_OR_FAIL(store->window_opened());
    auto first = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    auto second = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://b.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));
    store->active_tab_changed(first);
    store->flush_dirty_state();

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE ProbeWindowWrites (x INTEGER);"sv));
    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER ProbeWindowWrites AFTER UPDATE ON Sessions BEGIN INSERT INTO ProbeWindowWrites VALUES (1); END;"sv));

    store->tab_order_changed({ .window_id = window, .ordered_tabs = { first, second } });
    store->active_tab_changed(first);
    store->flush_dirty_state();

    EXPECT_EQ(select_i64(*database, "SELECT COUNT(*) FROM ProbeWindowWrites;"_string), 0);
}

TEST_CASE(take_on_empty_store_returns_nothing)
{
    auto store = SessionStore::create();
    EXPECT(!TRY_OR_FAIL(store->take_most_recently_closed()).has_value());
    EXPECT(!store->has_closed_units());
}

TEST_CASE(window_opened_errors_when_the_id_space_is_exhausted)
{
    auto database = create_session_database();

    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (9223372036854775807, 1, 5000, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (1, 9223372036854775807, 0, 'https://a.example/', 0);"sv));

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    EXPECT(store->window_opened().is_error());
}

TEST_CASE(exhausted_id_close_discards_the_tab_instead_of_resurrecting_it)
{
    auto database = create_session_database();

    // The seeded maximum leaves exactly one session id for the window; the close's unit allocation fails.
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (9223372036854775806, 1, 5000, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (1, 9223372036854775806, 0, 'https://z.example/', 0);"sv));

    {
        auto store = TRY_OR_FAIL(SessionStore::create(*database));
        auto window = TRY_OR_FAIL(store->window_opened());
        auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

        TRY_OR_FAIL(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }));
        EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT COUNT(*) FROM SessionTabs WHERE id = {};", tab))), 0);
    }

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto unit = TRY_OR_FAIL(store->take_most_recently_closed());
    EXPECT(unit.has_value());
    EXPECT_EQ(unit->tabs.size(), 1uz);
    EXPECT_EQ(unit->tabs[0].active_url.serialize(), "https://z.example/"sv);
    EXPECT(!store->has_closed_units());
}

TEST_CASE(exhausted_id_close_queues_the_discard_when_the_write_fails)
{
    auto database = create_session_database();

    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (9223372036854775806, 1, 5000, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (1, 9223372036854775806, 0, 'https://z.example/', 0);"sv));

    auto store = TRY_OR_FAIL(SessionStore::create(*database));
    auto window = TRY_OR_FAIL(store->window_opened());
    auto tab = TRY_OR_FAIL(store->tab_opened({ .window_id = window, .initial_url = parse_url("https://a.example/"sv), .insertion_index = {}, .is_active = SessionStore::IsActive::No }));

    TRY_OR_FAIL(database->execute_raw("CREATE TRIGGER InjectedFailure BEFORE DELETE ON SessionTabs BEGIN SELECT RAISE(ABORT, 'injected'); END;"sv));
    EXPECT(store->tab_closed({ .tab_id = tab, .closed_at = UnixDateTime::now() }).is_error());
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT COUNT(*) FROM SessionTabs WHERE id = {};", tab))), 1);

    TRY_OR_FAIL(database->execute_raw("DROP TRIGGER InjectedFailure;"sv));
    store->flush_dirty_state();
    EXPECT_EQ(select_i64(*database, MUST(String::formatted("SELECT COUNT(*) FROM SessionTabs WHERE id = {};", tab))), 0);
}
