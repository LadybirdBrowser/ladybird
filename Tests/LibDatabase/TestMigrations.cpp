/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibDatabase/Database.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>

static bool column_exists(Database::Database& database, String const& table, String const& column)
{
    bool exists = false;
    auto statement = MUST(database.prepare_statement("SELECT 1 FROM pragma_table_info(?) WHERE name = ?;"sv));
    database.execute_statement(statement, [&](auto) { exists = true; }, table, column);
    return exists;
}

static u32 count_rows(Database::Database& database, StringView table)
{
    u32 count = 0;
    auto statement = MUST(database.prepare_statement(MUST(String::formatted("SELECT COUNT(*) FROM {};", table))));
    database.execute_statement(statement, [&](auto statement_id) { count = database.result_column<u32>(statement_id, 0); });
    return count;
}

TEST_CASE(fresh_database_replays_all_migrations)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    Database::Migration migrations[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Pets (name TEXT PRIMARY KEY);"sv },
        { .version = 2, .sql = "ALTER TABLE Pets ADD COLUMN species TEXT;"sv },
        { .version = 3, .sql = "ALTER TABLE Pets ADD COLUMN age INTEGER;"sv },
    };

    auto outcome = TRY_OR_FAIL(database->migrate("Pets"sv, migrations));
    EXPECT_EQ(outcome, Database::MigrationOutcome::Success);

    EXPECT(column_exists(*database, "Pets"_string, "species"_string));
    EXPECT(column_exists(*database, "Pets"_string, "age"_string));
    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Pets"sv)), Optional<u32> { 3u });
}

TEST_CASE(migrate_is_idempotent_at_latest_version)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    Database::Migration migrations[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Sentinel (id INTEGER); INSERT INTO Sentinel (id) VALUES (1);"sv },
    };

    EXPECT_EQ(TRY_OR_FAIL(database->migrate("Sentinel"sv, migrations)), Database::MigrationOutcome::Success);
    EXPECT_EQ(TRY_OR_FAIL(database->migrate("Sentinel"sv, migrations)), Database::MigrationOutcome::Success);

    EXPECT_EQ(count_rows(*database, "Sentinel"sv), 1u);
}

TEST_CASE(partial_upgrade_runs_only_newer_migrations)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    Database::Migration version_1[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Sentinel (id INTEGER); INSERT INTO Sentinel (id) VALUES (1);"sv },
    };
    EXPECT_EQ(TRY_OR_FAIL(database->migrate("Sentinel"sv, version_1)), Database::MigrationOutcome::Success);

    Database::Migration version_2[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Sentinel (id INTEGER); INSERT INTO Sentinel (id) VALUES (1);"sv },
        { .version = 2, .sql = "ALTER TABLE Sentinel ADD COLUMN extra INTEGER;"sv },
    };
    EXPECT_EQ(TRY_OR_FAIL(database->migrate("Sentinel"sv, version_2)), Database::MigrationOutcome::Success);

    EXPECT_EQ(count_rows(*database, "Sentinel"sv), 1u);
    EXPECT(column_exists(*database, "Sentinel"_string, "extra"_string));
    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Sentinel"sv)), Optional<u32> { 2u });
}

TEST_CASE(backfill_callback_can_bind_placeholders)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    Database::Migration migrations[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Events (name TEXT); INSERT INTO Events (name) VALUES ('launch');"sv },
        {
            .version = 2,
            .sql = "ALTER TABLE Events ADD COLUMN time INTEGER;"sv,
            .backfill = [](Database::Database& database) -> ErrorOr<void> {
                auto statement = TRY(database.prepare_statement("UPDATE Events SET time = ?;"sv));
                TRY(database.try_execute_statement(statement, {}, UnixDateTime::from_seconds_since_epoch(123)));
                return {};
            },
        },
    };

    EXPECT_EQ(TRY_OR_FAIL(database->migrate("Events"sv, migrations)), Database::MigrationOutcome::Success);

    Optional<UnixDateTime> time;
    auto statement = TRY_OR_FAIL(database->prepare_statement("SELECT time FROM Events WHERE name = 'launch';"sv));
    database->execute_statement(statement, [&](auto statement_id) { time = database->result_column<UnixDateTime>(statement_id, 0); });
    EXPECT_EQ(time, Optional<UnixDateTime> { UnixDateTime::from_seconds_since_epoch(123) });
}

TEST_CASE(newer_database_version_is_left_untouched)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('Pets', 99);"sv));

    Database::Migration migrations[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Pets (name TEXT PRIMARY KEY);"sv },
    };

    EXPECT_EQ(TRY_OR_FAIL(database->migrate("Pets"sv, migrations)), Database::MigrationOutcome::DatabaseTooNew);

    EXPECT(!TRY_OR_FAIL(database->table_exists("Pets"sv)));
    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Pets"sv)), Optional<u32> { 99u });
}

TEST_CASE(failed_migration_rolls_back_entire_transaction)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    Database::Migration invalid_sql[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Pets (name TEXT PRIMARY KEY);"sv },
        { .version = 2, .sql = "THIS IS NOT SQL;"sv },
    };

    EXPECT(database->migrate("Pets"sv, invalid_sql).is_error());
    EXPECT(!TRY_OR_FAIL(database->table_exists("Pets"sv)));
    EXPECT(!TRY_OR_FAIL(database->table_exists("SchemaVersions"sv)));

    Database::Migration version_1[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Pets (name TEXT PRIMARY KEY);"sv },
    };
    EXPECT_EQ(TRY_OR_FAIL(database->migrate("Pets"sv, version_1)), Database::MigrationOutcome::Success);

    Database::Migration failing_backfill[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Pets (name TEXT PRIMARY KEY);"sv },
        {
            .version = 2,
            .sql = "ALTER TABLE Pets ADD COLUMN species TEXT;"sv,
            .backfill = [](Database::Database&) -> ErrorOr<void> {
                return Error::from_string_literal("Backfill failed");
            },
        },
    };

    EXPECT(database->migrate("Pets"sv, failing_backfill).is_error());
    EXPECT(!column_exists(*database, "Pets"_string, "species"_string));
    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Pets"sv)), Optional<u32> { 1u });
}

TEST_CASE(preexisting_unversioned_table_is_stamped_by_baseline_replay)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE Pets (name TEXT PRIMARY KEY);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Pets (name) VALUES ('Mochi');"sv));

    Database::Migration migrations[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Pets (name TEXT PRIMARY KEY);"sv },
        { .version = 2, .sql = "ALTER TABLE Pets ADD COLUMN species TEXT;"sv },
    };

    EXPECT_EQ(TRY_OR_FAIL(database->migrate("Pets"sv, migrations)), Database::MigrationOutcome::Success);

    EXPECT_EQ(count_rows(*database, "Pets"sv), 1u);
    EXPECT(column_exists(*database, "Pets"_string, "species"_string));
    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Pets"sv)), Optional<u32> { 2u });
}

TEST_CASE(check_only_never_commits)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE Pets (name TEXT PRIMARY KEY);"sv));

    Database::Migration migrations[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Pets (name TEXT PRIMARY KEY);"sv },
        { .version = 2, .sql = "ALTER TABLE Pets ADD COLUMN species TEXT;"sv },
    };

    auto outcome = TRY_OR_FAIL(database->migrate("Pets"sv, migrations, Database::MigrationMode::CheckOnly));
    EXPECT_EQ(outcome, Database::MigrationOutcome::Success);

    // CheckOnly rolls everything back: the pending migration is not applied and SchemaVersions is not created.
    EXPECT(!column_exists(*database, "Pets"_string, "species"_string));
    EXPECT(!TRY_OR_FAIL(database->table_exists("SchemaVersions"sv)));

    outcome = TRY_OR_FAIL(database->migrate("Pets"sv, migrations));
    EXPECT_EQ(outcome, Database::MigrationOutcome::Success);

    EXPECT(column_exists(*database, "Pets"_string, "species"_string));
    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Pets"sv)), Optional<u32> { 2u });
}

TEST_CASE(check_only_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('Pets', 99);"sv));

    Database::Migration migrations[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Pets (name TEXT PRIMARY KEY);"sv },
    };

    auto outcome = TRY_OR_FAIL(database->migrate("Pets"sv, migrations, Database::MigrationMode::CheckOnly));
    EXPECT_EQ(outcome, Database::MigrationOutcome::DatabaseTooNew);

    EXPECT(!TRY_OR_FAIL(database->table_exists("Pets"sv)));
    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Pets"sv)), Optional<u32> { 99u });
}

TEST_CASE(multiple_stores_track_versions_independently)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    Database::Migration pets_migrations[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Pets (name TEXT PRIMARY KEY);"sv },
    };
    EXPECT_EQ(TRY_OR_FAIL(database->migrate("Pets"sv, pets_migrations)), Database::MigrationOutcome::Success);

    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('Toys', 99);"sv));

    Database::Migration toys_migrations[] = {
        { .version = 1, .sql = "CREATE TABLE IF NOT EXISTS Toys (name TEXT PRIMARY KEY);"sv },
    };
    EXPECT_EQ(TRY_OR_FAIL(database->migrate("Toys"sv, toys_migrations)), Database::MigrationOutcome::DatabaseTooNew);

    EXPECT(TRY_OR_FAIL(database->table_exists("Pets"sv)));
    EXPECT(!TRY_OR_FAIL(database->table_exists("Toys"sv)));
    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Pets"sv)), Optional<u32> { 1u });
    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Toys"sv)), Optional<u32> { 99u });
}

TEST_CASE(reopened_database_resumes_from_recorded_version)
{
    auto database_directory = ByteString::formatted(
        "{}/ladybird-migrations-test-{}",
        Core::StandardPaths::tempfile_directory(),
        generate_random_uuid());
    TRY_OR_FAIL(Core::Directory::create(database_directory, Core::Directory::CreateDirectories::Yes));

    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(database_directory, FileSystem::RecursionMode::Allowed));
    });

    // Deliberately not IF NOT EXISTS, so this test fails if migration 1 is ever replayed.
    auto migration_1_sql = "CREATE TABLE Pets (name TEXT PRIMARY KEY);"sv;

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "Migrations"sv));

        Database::Migration migrations[] = {
            { .version = 1, .sql = migration_1_sql },
        };
        EXPECT_EQ(TRY_OR_FAIL(database->migrate("Pets"sv, migrations)), Database::MigrationOutcome::Success);
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "Migrations"sv));

        Database::Migration migrations[] = {
            { .version = 1, .sql = migration_1_sql },
            { .version = 2, .sql = "ALTER TABLE Pets ADD COLUMN species TEXT;"sv },
        };
        EXPECT_EQ(TRY_OR_FAIL(database->migrate("Pets"sv, migrations)), Database::MigrationOutcome::Success);

        EXPECT(column_exists(*database, "Pets"_string, "species"_string));
        EXPECT_EQ(TRY_OR_FAIL(database->schema_version("Pets"sv)), Optional<u32> { 2u });
    }
}
