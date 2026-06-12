/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibDatabase/Database.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/CookieJar.h>

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

TEST_CASE(cookies_round_trip_on_fresh_database)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

    {
        auto jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));

        HTTP::Cookie::ParsedCookie cookie {
            .name = "foo"_string,
            .value = "bar"_string,
            .expiry_time_from_expires_attribute = UnixDateTime::now() + AK::Duration::from_seconds(3600),
        };
        jar->set_cookie(parse_url("https://example.com/"sv), cookie, HTTP::Cookie::Source::Http);

        // The jar flushes dirty cookies to the database on destruction.
    }

    auto jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));

    auto cookies = jar->get_all_cookies();
    EXPECT_EQ(cookies.size(), 1uz);
    EXPECT_EQ(cookies[0].name, "foo"_string);
    EXPECT_EQ(cookies[0].value, "bar"_string);
}

TEST_CASE(unversioned_cookie_table_is_stamped_and_preserved)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    // The Cookies table as created before schema versioning existed.
    TRY_OR_FAIL(database->execute_raw(R"#(
        CREATE TABLE Cookies (
            name TEXT,
            value TEXT,
            same_site INTEGER CHECK (same_site >= 0 AND same_site <= 3),
            creation_time INTEGER,
            last_access_time INTEGER,
            expiry_time INTEGER,
            domain TEXT,
            path TEXT,
            secure BOOLEAN,
            http_only BOOLEAN,
            host_only BOOLEAN,
            persistent BOOLEAN,
            PRIMARY KEY(name, domain, path)
        );
    )#"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Cookies VALUES ('foo', 'bar', 0, 0, 0, 4102444800000, 'example.com', '/', 0, 0, 0, 1);"sv));

    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

    Optional<u32> version;
    auto statement = TRY_OR_FAIL(database->prepare_statement("SELECT version FROM SchemaVersions WHERE store = 'Cookies';"sv));
    database->execute_statement(statement, [&](auto statement_id) { version = database->result_column<u32>(statement_id, 0); });
    EXPECT_EQ(version, Optional<u32> { 1u });

    auto jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));

    auto cookies = jar->get_all_cookies();
    EXPECT_EQ(cookies.size(), 1uz);
    EXPECT_EQ(cookies[0].name, "foo"_string);
    EXPECT_EQ(cookies[0].value, "bar"_string);
    EXPECT_EQ(cookies[0].domain, "example.com"_string);
}

TEST_CASE(newer_cookie_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('Cookies', 99);"sv));

    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::DatabaseTooNew);
}
