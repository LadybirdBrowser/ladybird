/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <Libraries/LibDatabase/Database.h>

TEST_CASE(string_can_contain_null_bytes)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    database->execute_statement(TRY_OR_FAIL(database->prepare_statement(R"#(
        CREATE TABLE WebStorage (
            key TEXT PRIMARY KEY,
            value TEXT
        );
    )#"sv)),
        {});

    auto get_item_statement = TRY_OR_FAIL(database->prepare_statement("SELECT value FROM WebStorage WHERE key = ?;"sv));
    auto set_item_statement = TRY_OR_FAIL(database->prepare_statement("INSERT OR REPLACE INTO WebStorage VALUES (?, ?);"sv));
    auto delete_item_statement = TRY_OR_FAIL(database->prepare_statement("DELETE FROM WebStorage WHERE key = ?;"sv));

    auto get_item = [&](String const& key) {
        Optional<String> result;
        database->execute_statement(
            get_item_statement,
            [&](auto statement_id) {
                result = database->result_column<String>(statement_id, 0);
            },
            key);
        return result;
    };

    auto set_item = [&](String const& key, String const& value) {
        database->execute_statement(
            set_item_statement,
            {},
            key,
            value);
    };

    auto remove_item = [&](String const& key) {
        database->execute_statement(
            delete_item_statement,
            {},
            key);
    };

    EXPECT_EQ(get_item("my_key"_string), Optional<String> {});
    set_item("my_key"_string, "my_value"_string);
    EXPECT_EQ(get_item("my_key"_string), Optional<String> { "my_value"_string });
    set_item("my_key"_string, "my_value_with_\0_null"_string);
    EXPECT_EQ(get_item("my_key"_string), Optional<String> { "my_value_with_\0_null"_string });
    remove_item("my_key"_string);
    EXPECT_EQ(get_item("my_key"_string), Optional<String> {});
}

TEST_CASE(double_values_can_be_bound_and_read)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    auto statement = TRY_OR_FAIL(database->prepare_statement("SELECT ?;"sv));

    double result = 0;
    database->execute_statement(
        statement,
        [&](auto statement_id) {
            result = database->result_column<double>(statement_id, 0);
        },
        3.25);

    EXPECT_EQ(result, 3.25);
}

TEST_CASE(interrupted_statement_can_be_reused)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    auto statement = TRY_OR_FAIL(database->prepare_statement(R"#(
        SELECT 1
        UNION ALL SELECT 2
        UNION ALL SELECT 3;
    )#"sv));

    size_t row_count = 0;
    auto outcome = database->execute_interruptible_statement(statement, [&](auto) {
        ++row_count;
        database->interrupt();
    });
    EXPECT_EQ(outcome, Database::Database::StatementExecutionOutcome::Interrupted);
    EXPECT_EQ(row_count, 1u);

    row_count = 0;
    outcome = database->execute_interruptible_statement(statement, [&](auto) {
        ++row_count;
    });
    EXPECT_EQ(outcome, Database::Database::StatementExecutionOutcome::Completed);
    EXPECT_EQ(row_count, 3u);
}

TEST_CASE(busy_timeout_can_be_configured)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    TRY_OR_FAIL(database->set_busy_timeout(250));

    auto statement = TRY_OR_FAIL(database->prepare_statement("PRAGMA busy_timeout;"sv));
    i32 busy_timeout = 0;
    database->execute_statement(statement, [&](auto statement_id) {
        busy_timeout = database->result_column<i32>(statement_id, 0);
    });
    EXPECT_EQ(busy_timeout, 250);
}
