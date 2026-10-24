/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibCore/System.h>
#include <LibDatabase/Database.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/StorageJar.h>

static NonnullRefPtr<WebView::CanonicalSessionHistoryEntry> initial_history_entry()
{
    return MUST(WebView::CanonicalSessionHistoryEntry::create_from_descriptor(Web::HTML::create_initial_session_history_entry_descriptor({}, {}, {})));
}

TEST_CASE(storage_round_trips_on_fresh_database)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

    auto jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));

    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16), Optional<Utf16String> {});
    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16, "bar"_utf16);
    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16), Optional<Utf16String> { "bar"_utf16 });

    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("WebStorage"sv)), Optional<u32> { 2u });
}

TEST_CASE(newer_storage_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('WebStorage', 99);"));

    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::DatabaseTooNew);
}

TEST_CASE(storage_schema_version_2_database_migrates_cleanly)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    // Databases written while schema version 2 shipped must still migrate, not be rejected as too new.
    TRY_OR_FAIL(database->execute_raw(R"#(
        CREATE TABLE WebStorage (
            storage_endpoint INTEGER,
            storage_key TEXT,
            bottle_key TEXT,
            bottle_value TEXT,
            last_access_time INTEGER,
            PRIMARY KEY(storage_endpoint, storage_key, bottle_key)
        );
    )#"));
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('WebStorage', 2);"));

    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

    auto jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));
    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16, "bar"_utf16);
    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16), Optional<Utf16String> { "bar"_utf16 });
}

TEST_CASE(storage_usage_tracks_item_sizes)
{
    auto jar = WebView::StorageJar::create();

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "key"_utf16, "value"_utf16);
    EXPECT_EQ(jar->usage("https://example.com"_string), 8u);

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "key"_utf16, "v"_utf16);
    EXPECT_EQ(jar->usage("https://example.com"_string), 4u);

    jar->remove_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "key"_utf16);
    EXPECT_EQ(jar->usage("https://example.com"_string), 0u);
}

TEST_CASE(storage_usage_counts_utf8_bytes_not_code_units)
{
    auto jar = WebView::StorageJar::create();

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "k"_utf16, "\u00e9"_utf16);
    EXPECT_EQ(jar->usage("https://example.com"_string), 3u);

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "k"_utf16, "\U0001f600"_utf16);
    EXPECT_EQ(jar->usage("https://example.com"_string), 5u);
}

TEST_CASE(legacy_cloning_a_canonical_traversable_creates_an_independent_copy)
{
    WebView::CanonicalTraversable source { {}, initial_history_entry() };
    WebView::CanonicalTraversable clone { {}, initial_history_entry() };

    source.session_storage().set_item(WebView::StorageEndpointType::SessionStorage, "https://a.example"_string, "key"_utf16, "source"_utf16);
    source.session_storage().set_item(WebView::StorageEndpointType::SessionStorage, "https://b.example"_string, "other"_utf16, "preserved"_utf16);

    clone.clone_session_storage_from(source);
    clone.session_storage().set_item(WebView::StorageEndpointType::SessionStorage, "https://a.example"_string, "key"_utf16, "clone"_utf16);

    EXPECT_EQ(source.session_storage().get_item(WebView::StorageEndpointType::SessionStorage, "https://a.example"_string, "key"_utf16), Optional<Utf16String> { "source"_utf16 });
    EXPECT_EQ(clone.session_storage().get_item(WebView::StorageEndpointType::SessionStorage, "https://a.example"_string, "key"_utf16), Optional<Utf16String> { "clone"_utf16 });
    EXPECT_EQ(clone.session_storage().get_item(WebView::StorageEndpointType::SessionStorage, "https://b.example"_string, "other"_utf16), Optional<Utf16String> { "preserved"_utf16 });
}

TEST_CASE(storage_quota_is_enforced_per_storage_key)
{
    auto jar = WebView::StorageJar::create();

    auto large_value = Utf16String::repeated('x', Web::StorageAPI::StorageEndpoint::LOCAL_STORAGE_QUOTA / 2 - 16);

    EXPECT(jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "a"_utf16, large_value).has<Optional<Utf16String>>());
    EXPECT(jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "b"_utf16, large_value).has<Optional<Utf16String>>());

    // A third value exceeds the quota for this storage key.
    EXPECT(jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "c"_utf16, large_value).has<WebView::StorageOperationError>());

    // A different storage key has its own quota.
    EXPECT(jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://other.example"_string, "a"_utf16, large_value).has<Optional<Utf16String>>());
}

// Access times have microsecond resolution, so a moment "after" the last write needs a pause on both sides.
static UnixDateTime a_moment_later()
{
    MUST(Core::System::sleep_ms(2));
    auto since = UnixDateTime::now();
    MUST(Core::System::sleep_ms(2));
    return since;
}

TEST_CASE(set_emits_once_per_change)
{
    auto jar = WebView::StorageJar::create();
    Vector<WebView::StorageChange> changes;
    jar->on_change = [&changes](WebView::StorageChange const& change) { changes.append(change); };

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "a"_utf16, "1"_utf16);
    EXPECT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, WebView::StorageChange::Type::Set);
    EXPECT_EQ(changes[0].storage_endpoint, WebView::StorageEndpointType::LocalStorage);
    EXPECT_EQ(changes[0].storage_key, "https://example.com"_string);
    EXPECT_EQ(changes[0].key, Optional<Utf16String> { "a"_utf16 });
    EXPECT_EQ(changes[0].old_value, Optional<Utf16String> {});
    EXPECT_EQ(changes[0].new_value, Optional<Utf16String> { "1"_utf16 });

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "a"_utf16, "1"_utf16);
    EXPECT_EQ(changes.size(), 1u);

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "a"_utf16, "2"_utf16);
    EXPECT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[1].old_value, Optional<Utf16String> { "1"_utf16 });
    EXPECT_EQ(changes[1].new_value, Optional<Utf16String> { "2"_utf16 });

    // A refused write changes nothing, so it reports nothing.
    auto too_large = Utf16String::repeated('x', Web::StorageAPI::StorageEndpoint::LOCAL_STORAGE_QUOTA);
    EXPECT(jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "b"_utf16, too_large).has<WebView::StorageOperationError>());
    EXPECT_EQ(changes.size(), 2u);
}

TEST_CASE(remove_emits_only_for_existing)
{
    auto jar = WebView::StorageJar::create();
    Vector<WebView::StorageChange> changes;
    jar->on_change = [&changes](WebView::StorageChange const& change) { changes.append(change); };

    jar->remove_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "a"_utf16);
    EXPECT_EQ(changes.size(), 0u);

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "a"_utf16, "1"_utf16);
    jar->remove_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "a"_utf16);
    EXPECT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[1].type, WebView::StorageChange::Type::Removed);
    EXPECT_EQ(changes[1].key, Optional<Utf16String> { "a"_utf16 });
    EXPECT_EQ(changes[1].old_value, Optional<Utf16String> { "1"_utf16 });
    EXPECT_EQ(changes[1].new_value, Optional<Utf16String> {});
}

TEST_CASE(clear_emits_only_when_nonempty)
{
    auto jar = WebView::StorageJar::create();
    Vector<WebView::StorageChange> changes;
    jar->on_change = [&changes](WebView::StorageChange const& change) { changes.append(change); };

    jar->clear_storage_key(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string);
    EXPECT_EQ(changes.size(), 0u);

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "a"_utf16, "1"_utf16);
    jar->clear_storage_key(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string);
    EXPECT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[1].type, WebView::StorageChange::Type::Cleared);
    EXPECT_EQ(changes[1].storage_key, "https://example.com"_string);
    EXPECT_EQ(changes[1].key, Optional<Utf16String> {});
}

TEST_CASE(remove_items_accessed_since_emits_reset_per_affected_bottle)
{
    auto jar = WebView::StorageJar::create();
    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://a.example"_string, "k"_utf16, "1"_utf16);
    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://b.example"_string, "k"_utf16, "2"_utf16);
    jar->set_item(WebView::StorageEndpointType::SessionStorage, "https://b.example"_string, "k"_utf16, "3"_utf16);

    auto since = a_moment_later();
    jar->mark_accessed(WebView::StorageEndpointType::LocalStorage, "https://b.example"_string);

    Vector<WebView::StorageChange> changes;
    jar->on_change = [&changes](WebView::StorageChange const& change) { changes.append(change); };
    jar->remove_items_accessed_since(since);

    EXPECT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, WebView::StorageChange::Type::Reset);
    EXPECT_EQ(changes[0].storage_endpoint, WebView::StorageEndpointType::LocalStorage);
    EXPECT_EQ(changes[0].storage_key, "https://b.example"_string);
    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://a.example"_string, "k"_utf16), Optional<Utf16String> { "1"_utf16 });
    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://b.example"_string, "k"_utf16), Optional<Utf16String> {});
    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::SessionStorage, "https://b.example"_string, "k"_utf16), Optional<Utf16String> { "3"_utf16 });
}

static void get_item_does_not_mark_accessed_in(WebView::StorageJar& jar)
{
    jar.set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "k"_utf16, "1"_utf16);
    auto since = a_moment_later();

    EXPECT_EQ(jar.get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "k"_utf16), Optional<Utf16String> { "1"_utf16 });
    jar.remove_items_accessed_since(since);
    EXPECT_EQ(jar.get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "k"_utf16), Optional<Utf16String> { "1"_utf16 });

    jar.mark_accessed(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string);
    jar.remove_items_accessed_since(since);
    EXPECT_EQ(jar.get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "k"_utf16), Optional<Utf16String> {});
}

TEST_CASE(get_item_does_not_mark_accessed_transient)
{
    auto jar = WebView::StorageJar::create();
    get_item_does_not_mark_accessed_in(*jar);
}

TEST_CASE(get_item_does_not_mark_accessed_persisted)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::Success);
    auto jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));
    get_item_does_not_mark_accessed_in(*jar);
}

static void bottle_usage_is_per_endpoint_in(WebView::StorageJar& jar)
{
    jar.set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "key"_utf16, "value"_utf16);
    jar.set_item(WebView::StorageEndpointType::SessionStorage, "https://example.com"_string, "k"_utf16, "\u00e9"_utf16);

    EXPECT_EQ(jar.bottle_usage(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string), 8u);
    EXPECT_EQ(jar.bottle_usage(WebView::StorageEndpointType::SessionStorage, "https://example.com"_string), 3u);
    EXPECT_EQ(jar.bottle_usage(WebView::StorageEndpointType::LocalStorage, "https://other.example"_string), 0u);
    EXPECT_EQ(jar.usage("https://example.com"_string), 11u);
}

TEST_CASE(bottle_usage_is_per_endpoint_transient)
{
    auto jar = WebView::StorageJar::create();
    bottle_usage_is_per_endpoint_in(*jar);
}

TEST_CASE(bottle_usage_is_per_endpoint_persisted)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::Success);
    auto jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));
    bottle_usage_is_per_endpoint_in(*jar);
}

static void get_all_items_returns_every_pair_in(WebView::StorageJar& jar)
{
    jar.set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "a"_utf16, "1"_utf16);
    jar.set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "b"_utf16, "2"_utf16);
    jar.set_item(WebView::StorageEndpointType::SessionStorage, "https://example.com"_string, "c"_utf16, "3"_utf16);

    auto items = jar.get_all_items(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string);
    EXPECT_EQ(items.size(), 2u);
    HashMap<Utf16String, Utf16String> by_key;
    for (auto const& item : items)
        by_key.set(item.key, item.value);
    EXPECT_EQ(by_key.get("a"_utf16), Optional<Utf16String> { "1"_utf16 });
    EXPECT_EQ(by_key.get("b"_utf16), Optional<Utf16String> { "2"_utf16 });
    EXPECT(jar.get_all_items(WebView::StorageEndpointType::LocalStorage, "https://other.example"_string).is_empty());
}

TEST_CASE(get_all_items_returns_every_pair_transient)
{
    auto jar = WebView::StorageJar::create();
    get_all_items_returns_every_pair_in(*jar);
}

TEST_CASE(get_all_items_returns_every_pair_persisted)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::Success);
    auto jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));
    get_all_items_returns_every_pair_in(*jar);
}
