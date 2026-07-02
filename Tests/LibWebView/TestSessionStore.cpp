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
    entry.document_state.history_policy_container = Web::HTML::DocumentState::Client::Tag;
    entry.classic_history_api_state = Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::copy({ &state_byte, 1 })) };
    entry.navigation_api_state = Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::copy({ &state_byte, 1 })) };
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

TEST_CASE(fresh_database_migrates_to_baseline)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed({ .foreign_keys = Database::Database::ForeignKeys::Yes }));
    EXPECT_EQ(TRY_OR_FAIL(WebView::SessionStore::migrate_schema(*database)), Database::MigrationOutcome::Success);

    for (auto table : { "Sessions"sv, "SessionTabs"sv, "SessionUsedSteps"sv, "SessionHistories"sv, "SessionNestedHistories"sv, "SessionEntries"sv })
        EXPECT(TRY_OR_FAIL(database->table_exists(table)));

    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Sessions"sv)), Optional<u32> { 1u });
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
