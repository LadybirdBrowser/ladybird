/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDatabase/Database.h>
#include <LibTest/TestCase.h>
#include <LibWebView/SessionStore.h>

static NonnullRefPtr<Database::Database> create_session_database()
{
    auto database = MUST(Database::Database::create_memory_backed({ .foreign_keys = Database::Database::ForeignKeys::Yes }));
    MUST(WebView::SessionStore::migrate_schema(*database));
    return database;
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
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed({ .foreign_keys = Database::Database::ForeignKeys::Yes }));
    EXPECT_EQ(TRY_OR_FAIL(WebView::SessionStore::migrate_schema(*database)), Database::MigrationOutcome::Success);

    TRY_OR_FAIL(database->execute_raw("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (1, 0, 0, 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (2, 1, 0, 'https://a.example/', 0);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SessionUsedSteps (tab_id, step_ordinal, step) VALUES (2, 0, 0);"sv));

    TRY_OR_FAIL(database->execute_raw("DELETE FROM Sessions WHERE id = 1;"sv));

    for (auto table : { "SessionTabs"sv, "SessionUsedSteps"sv }) {
        i32 count = -1;
        auto statement = TRY_OR_FAIL(database->prepare_statement(MUST(String::formatted("SELECT COUNT(*) FROM {};", table))));
        database->execute_statement(statement, [&](auto statement_id) -> ErrorOr<void> {
            count = database->result_column<i32>(statement_id, 0);
            return {};
        });
        EXPECT_EQ(count, 0);
    }
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
