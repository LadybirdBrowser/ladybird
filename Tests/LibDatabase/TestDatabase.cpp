/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <Libraries/LibDatabase/Database.h>

using DB = Database::Database;

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

static i32 read_foreign_keys_pragma(DB& database)
{
    auto statement = MUST(database.prepare_statement("PRAGMA foreign_keys;"sv));
    i32 value = -1;
    database.execute_statement(statement, [&](auto statement_id) { value = database.result_column<i32>(statement_id, 0); });
    return value;
}

TEST_CASE(foreign_keys_option_controls_pragma)
{
    auto default_database = TRY_OR_FAIL(DB::create_memory_backed());
    EXPECT_EQ(read_foreign_keys_pragma(default_database), 0);
    EXPECT(default_database->options().foreign_keys == DB::ForeignKeys::No);

    auto foreign_keys_database = TRY_OR_FAIL(DB::create_memory_backed({ .foreign_keys = DB::ForeignKeys::Yes }));
    EXPECT_EQ(read_foreign_keys_pragma(foreign_keys_database), 1);
    EXPECT(foreign_keys_database->options().foreign_keys == DB::ForeignKeys::Yes);
}

TEST_CASE(options_report_the_applied_journal_mode)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed({
        .journal_mode = DB::JournalMode::WriteAheadLog,
        .synchronous = DB::Synchronous::Extra,
        .foreign_keys = DB::ForeignKeys::Yes,
    }));
    EXPECT(database->options().journal_mode == DB::JournalMode::Memory);
    EXPECT(database->options().synchronous == DB::Synchronous::Extra);
    EXPECT(database->options().foreign_keys == DB::ForeignKeys::Yes);
}

static i32 count_rows(DB& database, StringView table)
{
    auto statement = MUST(database.prepare_statement(MUST(String::formatted("SELECT COUNT(*) FROM {};", table))));
    i32 count = -1;
    database.execute_statement(statement, [&](auto statement_id) { count = database.result_column<i32>(statement_id, 0); });
    return count;
}

TEST_CASE(foreign_key_cascade_follows_the_pragma)
{
    auto create_schema = [](DB& database) {
        database.execute_statement(MUST(database.prepare_statement("CREATE TABLE parent (id INTEGER PRIMARY KEY);"sv)), {});
        database.execute_statement(MUST(database.prepare_statement("CREATE TABLE child (id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL REFERENCES parent(id) ON DELETE CASCADE);"sv)), {});
        database.execute_statement(MUST(database.prepare_statement("INSERT INTO parent (id) VALUES (1);"sv)), {});
        database.execute_statement(MUST(database.prepare_statement("INSERT INTO child (id, parent_id) VALUES (10, 1);"sv)), {});
        database.execute_statement(MUST(database.prepare_statement("DELETE FROM parent WHERE id = 1;"sv)), {});
    };

    auto enabled = TRY_OR_FAIL(DB::create_memory_backed({ .foreign_keys = DB::ForeignKeys::Yes }));
    create_schema(enabled);
    EXPECT_EQ(count_rows(enabled, "child"sv), 0);

    auto disabled = TRY_OR_FAIL(DB::create_memory_backed());
    create_schema(disabled);
    EXPECT_EQ(count_rows(disabled, "child"sv), 1);
}

TEST_CASE(transaction_commits_on_success_and_rolls_back_on_error)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (v INTEGER);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v) VALUES (?);"sv));

    TRY_OR_FAIL(database->transaction([&]() -> ErrorOr<void> {
        TRY(database->try_execute_statement(insert, {}, 1));
        return {};
    }));
    EXPECT_EQ(count_rows(*database, "t"sv), 1);

    auto rolled_back = database->transaction([&]() -> ErrorOr<void> {
        TRY(database->try_execute_statement(insert, {}, 2));
        return Error::from_string_literal("deliberate failure");
    });
    EXPECT(rolled_back.is_error());
    EXPECT_EQ(count_rows(*database, "t"sv), 1);
}

// A column with no declared type has NONE affinity, so each bound value keeps its storage class.
static Database::StatementID prepare_untyped_value_table(DB& database)
{
    database.execute_statement(MUST(database.prepare_statement("CREATE TABLE t (i INTEGER PRIMARY KEY, v);"sv)), {});
    return MUST(database.prepare_statement("SELECT v FROM t WHERE i = ?;"sv));
}

TEST_CASE(checked_reads_reject_wrong_storage_class)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    auto select = prepare_untyped_value_table(*database);

    auto insert_value = MUST(database->prepare_statement("INSERT INTO t (i, v) VALUES (?, ?);"sv));
    auto insert_null = MUST(database->prepare_statement("INSERT INTO t (i) VALUES (?);"sv));

    database->execute_statement(insert_value, {}, 1, "hello"_string);
    database->execute_statement(insert_value, {}, 2, static_cast<i64>(42));
    database->execute_statement(insert_value, {}, 3, ByteString { "\x01\x02\x03", 3 });
    database->execute_statement(insert_null, {}, 4);
    database->execute_statement(insert_value, {}, 5, ByteString {});

    auto with_value = [&](i32 key, auto fn) {
        database->execute_statement(select, [&](auto statement_id) { fn(statement_id); }, key);
    };

    with_value(1, [&](auto id) {
        auto text = database->result_text_column_bounded(id, 0, 1024);
        EXPECT(!text.is_error());
        EXPECT_EQ(text.value(), ByteString { "hello" });
        EXPECT(database->result_i64_checked(id, 0).is_error());
        EXPECT(database->result_blob_column_bounded(id, 0, 1024).is_error());
    });
    with_value(2, [&](auto id) {
        auto integer = database->result_i64_checked(id, 0);
        EXPECT(!integer.is_error());
        EXPECT_EQ(integer.value(), 42);
        EXPECT(database->result_text_column_bounded(id, 0, 1024).is_error());
        EXPECT(database->result_blob_column_bounded(id, 0, 1024).is_error());
    });
    with_value(3, [&](auto id) {
        ByteString const expected_blob { "\x01\x02\x03", 3 };
        auto blob = database->result_blob_column_bounded(id, 0, 1024);
        EXPECT(!blob.is_error());
        EXPECT_EQ(blob.value(), expected_blob);
        EXPECT(database->result_i64_checked(id, 0).is_error());
        EXPECT(database->result_text_column_bounded(id, 0, 1024).is_error());
    });
    with_value(4, [&](auto id) {
        // A SQL NULL is not any of the expected storage classes.
        EXPECT(database->result_i64_checked(id, 0).is_error());
        EXPECT(database->result_text_column_bounded(id, 0, 1024).is_error());
        EXPECT(database->result_blob_column_bounded(id, 0, 1024).is_error());
    });
    with_value(5, [&](auto id) {
        // A zero-length blob is a valid empty value, distinct from NULL.
        auto blob = database->result_blob_column_bounded(id, 0, 1024);
        EXPECT(!blob.is_error());
        EXPECT(blob.value().is_empty());
    });
}

TEST_CASE(bounded_reads_reject_oversized_cells)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    auto select = prepare_untyped_value_table(*database);

    auto insert_value = MUST(database->prepare_statement("INSERT INTO t (i, v) VALUES (?, ?);"sv));
    database->execute_statement(insert_value, {}, 1, "abcdefghij"_string);
    database->execute_statement(insert_value, {}, 2, ByteString { "0123456789", 10 });

    auto with_value = [&](i32 key, auto fn) {
        database->execute_statement(select, [&](auto statement_id) { fn(statement_id); }, key);
    };

    with_value(1, [&](auto id) {
        auto too_small = database->result_text_column_bounded(id, 0, 4);
        EXPECT(too_small.is_error());
        EXPECT(too_small.error() == DB::ColumnReadError::TooLarge);
        EXPECT(!database->result_text_column_bounded(id, 0, 10).is_error());
    });
    with_value(2, [&](auto id) {
        auto too_small = database->result_blob_column_bounded(id, 0, 4);
        EXPECT(too_small.is_error());
        EXPECT(too_small.error() == DB::ColumnReadError::TooLarge);
        EXPECT(!database->result_blob_column_bounded(id, 0, 10).is_error());
    });
}
