/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/StringBuilder.h>
#include <LibTest/TestCase.h>
#include <Libraries/LibDatabase/Database.h>
#include <Libraries/LibDatabase/ResultRow.h>

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
            [&](auto statement_id) -> ErrorOr<void> {
                result = database->result_column<String>(statement_id, 0);
                return {};
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
        [&](auto statement_id) -> ErrorOr<void> {
            result = database->result_column<double>(statement_id, 0);
            return {};
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
    auto outcome = database->execute_interruptible_statement(statement, [&](auto) -> ErrorOr<void> {
        ++row_count;
        database->interrupt();
        return {};
    });
    EXPECT_EQ(outcome, Database::Database::StatementExecutionOutcome::Interrupted);
    EXPECT_EQ(row_count, 1u);

    row_count = 0;
    outcome = database->execute_interruptible_statement(statement, [&](auto) -> ErrorOr<void> {
        ++row_count;
        return {};
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
    database->execute_statement(statement, [&](auto statement_id) -> ErrorOr<void> {
        busy_timeout = database->result_column<i32>(statement_id, 0);
        return {};
    });
    EXPECT_EQ(busy_timeout, 250);
}

static i32 read_foreign_keys_pragma(DB& database)
{
    auto statement = MUST(database.prepare_statement("PRAGMA foreign_keys;"sv));
    i32 value = -1;
    database.execute_statement(statement, [&](auto statement_id) -> ErrorOr<void> { value = database.result_column<i32>(statement_id, 0); return {}; });
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
    database.execute_statement(statement, [&](auto statement_id) -> ErrorOr<void> { count = database.result_column<i32>(statement_id, 0); return {}; });
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
    return MUST(database.prepare_statement("SELECT v FROM t WHERE i = :i;"sv));
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
        TRY_OR_FAIL(database->try_execute_bound_statement(
            select,
            [&](auto& bind) -> ErrorOr<void> { return bind("i"sv, key); },
            [&](Database::ResultRow& row) -> ErrorOr<void> { fn(row); return {}; }));
    };

    with_value(1, [&](Database::ResultRow& row) {
        EXPECT_EQ(TRY_OR_FAIL(row.read_text("v"sv, 1024)), "hello"_string);
        EXPECT(row.read_integer<i64>("v"sv).is_error());
        auto blob = row.read_blob("v"sv, 1024);
        EXPECT(blob.is_error());
        EXPECT_EQ(blob.error().string_literal(), "Column is not a blob"sv);
    });
    with_value(2, [&](Database::ResultRow& row) {
        EXPECT_EQ(TRY_OR_FAIL(row.read_integer<i64>("v"sv)), 42);
        auto text = row.read_text("v"sv, 1024);
        EXPECT(text.is_error());
        EXPECT_EQ(text.error().string_literal(), "Column is not text"sv);
        EXPECT(row.read_blob("v"sv, 1024).is_error());
    });
    with_value(3, [&](Database::ResultRow& row) {
        EXPECT_EQ(TRY_OR_FAIL(row.read_blob("v"sv, 1024)), MUST(ByteBuffer::copy("\x01\x02\x03"sv.bytes())));
        EXPECT(row.read_integer<i64>("v"sv).is_error());
        EXPECT(row.read_text("v"sv, 1024).is_error());
    });
    with_value(4, [&](Database::ResultRow& row) {
        // A SQL NULL is not any of the expected storage classes.
        EXPECT(row.read_integer<i64>("v"sv).is_error());
        EXPECT(row.read_text("v"sv, 1024).is_error());
        EXPECT(row.read_blob("v"sv, 1024).is_error());
    });
    with_value(5, [&](Database::ResultRow& row) {
        // A zero-length blob is a valid empty value, distinct from NULL.
        EXPECT(TRY_OR_FAIL(row.read_blob("v"sv, 1024)).is_empty());
        EXPECT_EQ(TRY_OR_FAIL(row.is_null("v"sv)), false);
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
        TRY_OR_FAIL(database->try_execute_bound_statement(
            select,
            [&](auto& bind) -> ErrorOr<void> { return bind("i"sv, key); },
            [&](Database::ResultRow& row) -> ErrorOr<void> { fn(row); return {}; }));
    };

    with_value(1, [&](Database::ResultRow& row) {
        auto oversized_text = row.read_text("v"sv, 4);
        EXPECT(oversized_text.is_error());
        EXPECT_EQ(oversized_text.error().string_literal(), "Text column exceeds the size limit"sv);
        EXPECT(!row.read_text("v"sv, 10).is_error());
    });
    with_value(2, [&](Database::ResultRow& row) {
        auto oversized_blob = row.read_blob("v"sv, 4);
        EXPECT(oversized_blob.is_error());
        EXPECT_EQ(oversized_blob.error().string_literal(), "Blob column exceeds the size limit"sv);
        EXPECT(!row.read_blob("v"sv, 10).is_error());
    });
}

TEST_CASE(named_binding_is_order_independent)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE named (a INTEGER NOT NULL, b TEXT NOT NULL, c INTEGER NOT NULL);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO named (a, b, c) VALUES (:a, :b, :c);"sv));

    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("c"sv, static_cast<i64>(30)));
        TRY(bind("b"sv, "two"_string));
        TRY(bind("a"sv, static_cast<i64>(10)));
        return {};
    }));

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT c, b, a FROM named;"sv));
    EXPECT_EQ(TRY_OR_FAIL(database->result_column_index(select, "c"sv)), 0);
    EXPECT_EQ(TRY_OR_FAIL(database->result_column_index(select, "b"sv)), 1);
    EXPECT_EQ(TRY_OR_FAIL(database->result_column_index(select, "a"sv)), 2);

    bool read = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(
        select,
        [](auto&) -> ErrorOr<void> { return {}; },
        [&](Database::ResultRow& row) -> ErrorOr<void> {
            read = true;
            EXPECT_EQ(TRY(row.read_integer<i64>("a"sv)), 10);
            EXPECT_EQ(TRY(row.read_text("b"sv, 1024)), "two"_string);
            EXPECT_EQ(TRY(row.read_integer<i64>("c"sv)), 30);
            return {};
        }));
    EXPECT(read);
}

TEST_CASE(bind_rejects_unknown_and_nul_names)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE named (a INTEGER NOT NULL);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO named (a) VALUES (:a);"sv));

    auto unknown = database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
        return bind("missing"sv, static_cast<i64>(1));
    });
    EXPECT(unknown.is_error());
    EXPECT_EQ(unknown.error().string_literal(), "Unknown bound parameter name"sv);

    // The name up to the NUL ("a") is a real parameter, but the supplied name is not, so it must be
    // rejected rather than silently bound as ":a".
    auto embedded_nul = database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
        return bind(StringView { "a\0b", 3 }, static_cast<i64>(1));
    });
    EXPECT(embedded_nul.is_error());
    EXPECT_EQ(embedded_nul.error().string_literal(), "Bound parameter name contains an embedded NUL"sv);
}

TEST_CASE(bound_execute_requires_every_parameter)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (owner_id INTEGER NOT NULL, v INTEGER NOT NULL);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (owner_id, v) VALUES (:owner_id, :v);"sv));
    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("owner_id"sv, static_cast<i64>(1)));
        TRY(bind("v"sv, static_cast<i64>(42)));
        return {};
    }));

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t WHERE owner_id = :owner_id;"sv));
    auto missing = database->try_execute_bound_statement(select, [](auto&) -> ErrorOr<void> { return {}; });
    EXPECT(missing.is_error());
    EXPECT_EQ(missing.error().string_literal(), "Statement executed with an unbound parameter"sv);

    bool matched = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(select, [&](auto& bind) -> ErrorOr<void> { return bind("owner_id"sv, static_cast<i64>(1)); }, [&](Database::ResultRow&) -> ErrorOr<void> { matched = true; return {}; }));
    EXPECT(matched);
}

TEST_CASE(bound_execute_clears_state_between_cycles)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (a INTEGER NOT NULL, b INTEGER NOT NULL);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (a, b) VALUES (:a, :b);"sv));

    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("a"sv, static_cast<i64>(1)));
        TRY(bind("b"sv, static_cast<i64>(2)));
        return {};
    }));

    EXPECT(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
                       return bind("a"sv, static_cast<i64>(3));
                   })
            .is_error());

    EXPECT(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
                       TRY(bind("a"sv, static_cast<i64>(4)));
                       return bind("missing"sv, static_cast<i64>(0));
                   })
            .is_error());
    EXPECT(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
                       return bind("b"sv, static_cast<i64>(5));
                   })
            .is_error());

    EXPECT_EQ(count_rows(*database, "t"sv), 1);
}

TEST_CASE(result_column_index_rejects_ambiguous_and_missing_names)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (a INTEGER, b INTEGER);"sv)), {});
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (a, b) VALUES (1, 2);"sv)), {});

    auto duplicate = TRY_OR_FAIL(database->prepare_statement("SELECT a AS id, b AS id FROM t;"sv));
    bool checked = false;
    database->execute_statement(duplicate, [&](auto) -> ErrorOr<void> {
        checked = true;
        auto ambiguous = database->result_column_index(duplicate, "id"sv);
        EXPECT(ambiguous.is_error());
        EXPECT_EQ(ambiguous.error().string_literal(), "Ambiguous result column name"sv);
        auto missing = database->result_column_index(duplicate, "nope"sv);
        EXPECT(missing.is_error());
        EXPECT_EQ(missing.error().string_literal(), "Unknown result column name"sv);
        return {};
    });
    EXPECT(checked);
}

TEST_CASE(bound_execute_handles_a_zero_parameter_statement)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (v INTEGER NOT NULL);"sv)), {});
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v) VALUES (1);"sv)), {});

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    bool ran = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(select, [](auto&) -> ErrorOr<void> { return {}; }, [&](Database::ResultRow&) -> ErrorOr<void> { ran = true; return {}; }));
    EXPECT(ran);
}

TEST_CASE(utf16_text_round_trips_lonely_surrogates)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (s TEXT NOT NULL);"sv)), {});

    static constexpr char16_t code_units[] = { 'a', 0xd800, 'b' };
    auto text = Utf16String::from_utf16(Utf16View { code_units, 3 });

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (s) VALUES (:s);"sv));
    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
        return bind("s"sv, text);
    }));

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT s FROM t;"sv));
    bool read = false;
    database->execute_statement(select, [&](auto statement_id) -> ErrorOr<void> {
        read = true;
        EXPECT_EQ(database->result_column<Utf16String>(statement_id, 0), text);
        return {};
    });
    EXPECT(read);

    bool row_read = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(select, [](auto&) -> ErrorOr<void> { return {}; }, [&](Database::ResultRow& row) -> ErrorOr<void> {
        row_read = true;
        EXPECT_EQ(TRY(row.read_utf16_text("s"sv, 1024)), text);
        return {}; }));
    EXPECT(row_read);
}

TEST_CASE(utf16_text_read_rejects_malformed_bytes)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (s TEXT NOT NULL);"sv)), {});
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (s) VALUES (CAST(x'ff' AS TEXT));"sv)), {});

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT s FROM t;"sv));
    bool read = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(select, [](auto&) -> ErrorOr<void> { return {}; }, [&](Database::ResultRow& row) -> ErrorOr<void> {
        read = true;
        EXPECT(row.read_utf16_text("s"sv, 1024).is_error());
        return {}; }));
    EXPECT(read);
}

TEST_CASE(named_binding_supports_more_than_64_parameters)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());

    StringBuilder builder;
    builder.append("SELECT "sv);
    for (int i = 0; i < 70; ++i) {
        if (i != 0)
            builder.append(", "sv);
        builder.appendff(":p{}", i);
    }
    auto statement = TRY_OR_FAIL(database->prepare_statement(builder.string_view()));

    bool ran = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(statement, [&](auto& bind) -> ErrorOr<void> {
        for (int i = 0; i < 70; ++i)
            TRY(bind(ByteString::formatted("p{}", i), static_cast<i64>(i)));
        return {}; }, [&](Database::ResultRow&) -> ErrorOr<void> { ran = true; return {}; }));
    EXPECT(ran);

    EXPECT(database->try_execute_bound_statement(statement, [&](auto& bind) -> ErrorOr<void> {
                       for (int i = 0; i < 69; ++i)
                           TRY(bind(ByteString::formatted("p{}", i), static_cast<i64>(i)));
                       return {};
                   })
            .is_error());
}

TEST_CASE(result_row_reads_columns_by_name)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (n INTEGER NOT NULL, s TEXT NOT NULL, b BLOB NOT NULL);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (n, s, b) VALUES (:n, :s, :b);"sv));
    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("n"sv, static_cast<i64>(7)));
        TRY(bind("s"sv, "hi"_string));
        TRY(bind("b"sv, ByteString { "\x01\x02", 2 }));
        return {};
    }));

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT b, s, n FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void> { return {}; };
    bool read = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        read = true;
        EXPECT_EQ(TRY(row.read_integer<i64>("n"sv)), 7);
        EXPECT_EQ(TRY(row.read_text("s"sv, 1024)), "hi"_string);
        EXPECT_EQ(TRY(row.read_blob("b"sv, 1024)), MUST(ByteBuffer::copy("\x01\x02"sv.bytes())));
        return {};
    }));
    EXPECT(read);

    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT(row.read_integer<i64>("nope"sv).is_error());
        EXPECT(row.read_integer<i64>("s"sv).is_error());
        return {};
    }));
}

TEST_CASE(typed_integer_reads_check_range_and_storage_class)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (v INTEGER, s TEXT NOT NULL);"sv));

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v, s) VALUES (:v, :s);"sv));
    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT v, s FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void> { return {}; };

    auto store = [&](i64 value) {
        TRY_OR_FAIL(database->execute_raw("DELETE FROM t;"sv));
        TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("v"sv, value));
            TRY(bind("s"sv, "text"_string));
            return {};
        }));
    };

    store(static_cast<i64>(NumericLimits<i32>::max()) + 1);
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT(row.read_integer<i32>("v"sv).is_error());
        EXPECT_EQ(TRY(row.read_integer<u32>("v"sv)), static_cast<u32>(NumericLimits<i32>::max()) + 1);
        EXPECT_EQ(TRY(row.read_integer<i64>("v"sv)), static_cast<i64>(NumericLimits<i32>::max()) + 1);
        return {};
    }));

    store(static_cast<i64>(NumericLimits<u32>::max()) + 1);
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT(row.read_integer<u32>("v"sv).is_error());
        return {};
    }));

    store(-1);
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT(row.read_integer<u32>("v"sv).is_error());
        EXPECT(row.read_integer<u64>("v"sv).is_error());
        EXPECT_EQ(TRY(row.read_integer<i32>("v"sv)), -1);
        return {};
    }));

    store(0);
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT(row.read_integer<i64>("s"sv).is_error());
        EXPECT(row.read_optional_integer<i64>("s"sv).is_error());
        EXPECT_EQ(TRY(row.is_null("v"sv)), false);
        EXPECT_EQ(TRY(row.read_optional_integer<i64>("v"sv)), Optional<i64> { 0 });
        return {};
    }));

    auto select_max = TRY_OR_FAIL(database->prepare_statement("SELECT MAX(v) AS maximum FROM t WHERE v > 100;"sv));
    TRY_OR_FAIL(database->try_execute_bound_statement(select_max, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT_EQ(TRY(row.is_null("maximum"sv)), true);
        EXPECT_EQ(TRY(row.read_optional_integer<i64>("maximum"sv)), Optional<i64> {});
        EXPECT(row.read_integer<i64>("maximum"sv).is_error());
        return {};
    }));
}

TEST_CASE(borrowed_bytes_bind_as_blobs)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (b BLOB);"sv));

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (b) VALUES (:b);"sv));
    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT b, typeof(b) AS kind, b IS NULL AS absent FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void> { return {}; };

    auto payload = MUST(ByteBuffer::copy("\x01\x02\x03"sv.bytes()));
    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
        return bind("b"sv, payload.bytes());
    }));
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT_EQ(TRY(row.read_text("kind"sv, 16)), "blob"_string);
        EXPECT_EQ(TRY(row.read_bool("absent"sv)), false);
        EXPECT_EQ(TRY(row.read_blob("b"sv, 1024)), payload);
        return {};
    }));

    auto store = [&](ReadonlyBytes bytes) {
        TRY_OR_FAIL(database->execute_raw("DELETE FROM t;"sv));
        TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
            return bind("b"sv, bytes);
        }));
    };

    ByteBuffer const empty_buffer {};
    for (auto bytes : { ReadonlyBytes {}, empty_buffer.bytes() }) {
        store(bytes);
        TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
            EXPECT_EQ(TRY(row.read_text("kind"sv, 16)), "blob"_string);
            EXPECT_EQ(TRY(row.read_bool("absent"sv)), false);
            EXPECT(TRY(row.read_blob("b"sv, 1024)).is_empty());
            return {};
        }));
    }
}

TEST_CASE(optional_placeholders_bind_null_when_empty)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (v INTEGER, s TEXT);"sv));

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v, s) VALUES (:v, :s);"sv));
    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT v, s FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void> { return {}; };

    auto store = [&](Optional<u32> value, Optional<String> text) {
        TRY_OR_FAIL(database->execute_raw("DELETE FROM t;"sv));
        TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("v"sv, value));
            TRY(bind("s"sv, text));
            return {};
        }));
    };

    store(7u, "text"_string);
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT_EQ(TRY(row.is_null("v"sv)), false);
        EXPECT_EQ(TRY(row.read_optional_integer<u32>("v"sv)), Optional<u32> { 7 });
        EXPECT_EQ(TRY(row.read_optional_text("s"sv, 64)), Optional<String> { "text"_string });
        return {};
    }));

    store({}, {});
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT_EQ(TRY(row.is_null("v"sv)), true);
        EXPECT_EQ(TRY(row.read_optional_integer<u32>("v"sv)), Optional<u32> {});
        EXPECT_EQ(TRY(row.read_optional_text("s"sv, 64)), Optional<String> {});
        return {};
    }));

    store(0u, String {});
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT_EQ(TRY(row.is_null("v"sv)), false);
        EXPECT_EQ(TRY(row.read_optional_integer<u32>("v"sv)), Optional<u32> { 0 });
        EXPECT_EQ(TRY(row.read_optional_text("s"sv, 64)), Optional<String> { String {} });
        return {};
    }));
}

TEST_CASE(column_name_cache_survives_a_reprepare)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (a INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (a) VALUES (7);"sv));

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT * FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void> { return {}; };

    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT_EQ(TRY(row.read_integer<i64>("a"sv)), 7);
        return {};
    }));

    TRY_OR_FAIL(database->execute_raw("ALTER TABLE t RENAME COLUMN a TO b;"sv));
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void> {
        EXPECT_EQ(TRY(row.read_integer<i64>("b"sv)), 7);
        EXPECT(row.read_integer<i64>("a"sv).is_error());
        return {};
    }));
}

TEST_CASE(one_row_execution_maps_the_single_row)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (1, 10), (2, 20);"sv));

    auto select_by_id = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t WHERE id = :id;"sv));
    auto read_v = [](Database::ResultRow& row) -> ErrorOr<i64> {
        return row.read_integer<i64>("v"sv);
    };

    auto value = database->try_execute_bound_statement_one<i64>(select_by_id, [](auto& bind) -> ErrorOr<void> { return bind("id"sv, static_cast<i64>(2)); }, read_v);
    EXPECT_EQ(TRY_OR_FAIL(move(value)), 20);

    auto none = database->try_execute_bound_statement_one<i64>(select_by_id, [](auto& bind) -> ErrorOr<void> { return bind("id"sv, static_cast<i64>(99)); }, read_v);
    EXPECT(none.is_error());
    EXPECT_EQ(none.error().string_literal(), "Statement returned no rows"sv);

    auto select_all = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    auto many = database->try_execute_bound_statement_one<i64>(select_all, [](auto&) -> ErrorOr<void> { return {}; }, read_v);
    EXPECT(many.is_error());
    EXPECT_EQ(many.error().string_literal(), "Statement returned more than one row"sv);

    auto again = database->try_execute_bound_statement_one<i64>(select_by_id, [](auto& bind) -> ErrorOr<void> { return bind("id"sv, static_cast<i64>(1)); }, read_v);
    EXPECT_EQ(TRY_OR_FAIL(move(again)), 10);
}

TEST_CASE(one_row_execution_reads_a_returning_clause)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v) VALUES (:v) RETURNING id;"sv));
    auto read_id = [](Database::ResultRow& row) -> ErrorOr<i64> {
        return row.read_integer<i64>("id"sv);
    };

    auto first = database->try_execute_bound_statement_one<i64>(insert, [](auto& bind) -> ErrorOr<void> { return bind("v"sv, static_cast<i64>(10)); }, read_id);
    EXPECT_EQ(TRY_OR_FAIL(move(first)), 1);

    auto second = database->try_execute_bound_statement_one<i64>(insert, [](auto& bind) -> ErrorOr<void> { return bind("v"sv, static_cast<i64>(20)); }, read_id);
    EXPECT_EQ(TRY_OR_FAIL(move(second)), 2);
}

TEST_CASE(bounded_execution_stops_before_the_callback_past_the_budget)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (1, 10), (2, 20), (3, 30);"sv));

    auto select_all = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    size_t callback_invocations = 0;
    auto result = database->try_execute_bound_statement(
        select_all,
        2uz,
        [](auto&) -> ErrorOr<void> { return {}; },
        [&](Database::ResultRow&) -> ErrorOr<void> {
            ++callback_invocations;
            return {};
        });
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Statement returned more rows than the caller allowed"sv);
    EXPECT_EQ(callback_invocations, 2uz);
}

TEST_CASE(collect_bounded_statement_maps_rows_under_the_budget)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));

    auto select_all = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void> { return {}; };
    auto read_v = [](Database::ResultRow& row) -> ErrorOr<i64> {
        return row.read_integer<i64>("v"sv);
    };

    auto empty = TRY_OR_FAIL(database->try_collect_bound_statement<i64>(select_all, 0uz, no_binds, read_v));
    EXPECT(empty.is_empty());

    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (1, 10), (2, 20);"sv));
    auto zero_budget = database->try_collect_bound_statement<i64>(select_all, 0uz, no_binds, read_v);
    EXPECT(zero_budget.is_error());
    EXPECT_EQ(zero_budget.error().string_literal(), "Statement returned more rows than the caller allowed"sv);

    auto at_limit = TRY_OR_FAIL(database->try_collect_bound_statement<i64>(select_all, 2uz, no_binds, read_v));
    EXPECT_EQ(at_limit.size(), 2uz);
    EXPECT_EQ(at_limit[0], 10);
    EXPECT_EQ(at_limit[1], 20);

    auto past_limit = database->try_collect_bound_statement<i64>(select_all, 1uz, no_binds, read_v);
    EXPECT(past_limit.is_error());
    EXPECT_EQ(past_limit.error().string_literal(), "Statement returned more rows than the caller allowed"sv);

    auto mapper_failure = database->try_collect_bound_statement<i64>(select_all, 2uz, no_binds,
        [](Database::ResultRow&) -> ErrorOr<i64> { return Error::from_string_literal("mapper failed"); });
    EXPECT(mapper_failure.is_error());
    EXPECT_EQ(mapper_failure.error().string_literal(), "mapper failed"sv);

    auto again = TRY_OR_FAIL(database->try_collect_bound_statement<i64>(select_all, 2uz, no_binds, read_v));
    EXPECT_EQ(again.size(), 2uz);
}

TEST_CASE(collect_set_deduplicates_but_budget_counts_rows)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (1, 5), (2, 5), (3, 7);"sv));

    auto select_all = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void> { return {}; };
    auto read_v = [](Database::ResultRow& row) -> ErrorOr<i64> {
        return row.read_integer<i64>("v"sv);
    };

    auto values = TRY_OR_FAIL(database->try_collect_bound_statement_set<i64>(select_all, 3uz, no_binds, read_v));
    EXPECT_EQ(values.size(), 2uz);
    EXPECT(values.contains(5));
    EXPECT(values.contains(7));

    auto over_budget = database->try_collect_bound_statement_set<i64>(select_all, 2uz, no_binds, read_v);
    EXPECT(over_budget.is_error());
    EXPECT_EQ(over_budget.error().string_literal(), "Statement returned more rows than the caller allowed"sv);
}

TEST_CASE(integral_binds_widen_to_64_bit)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (id, v) VALUES (?, ?);"sv));
    auto select_by_id = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t WHERE id = ?;"sv));
    auto stored_value = [&](i64 id) {
        Optional<i64> value;
        database->execute_statement(select_by_id, [&](auto statement_id) -> ErrorOr<void> {
            value = database->result_column<i64>(statement_id, 0);
            return {}; }, id);
        return value;
    };

    TRY_OR_FAIL(database->try_execute_statement(insert, {}, static_cast<i64>(1), NumericLimits<u32>::max()));
    EXPECT_EQ(stored_value(1), Optional<i64> { 4294967295 });

    TRY_OR_FAIL(database->try_execute_statement(insert, {}, static_cast<i64>(2), NumericLimits<i64>::min()));
    EXPECT_EQ(stored_value(2), Optional<i64> { NumericLimits<i64>::min() });

    TRY_OR_FAIL(database->try_execute_statement(insert, {}, static_cast<i64>(3), NumericLimits<i64>::max()));
    EXPECT_EQ(stored_value(3), Optional<i64> { NumericLimits<i64>::max() });
}
