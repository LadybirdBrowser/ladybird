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

static bool stores_cookie(URL::URL const& url, HTTP::Cookie::ParsedCookie cookie, HTTP::Cookie::Source source)
{
    auto jar = WebView::CookieJar::create();
    jar->set_cookie(url, cookie, source, {});
    return !jar->get_all_cookies().is_empty();
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
        jar->set_cookie(parse_url("https://example.com/"sv), cookie, HTTP::Cookie::Source::Http, {});

        // The jar flushes dirty cookies to the database on destruction.
    }

    auto jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));

    auto cookies = jar->get_all_cookies();
    EXPECT_EQ(cookies.size(), 1uz);
    EXPECT_EQ(cookies[0].name, "foo"_string);
    EXPECT_EQ(cookies[0].value, "bar"_string);
}

TEST_CASE(http_cookie_prefixes_require_secure_http_provenance)
{
    auto http_url = parse_url("http://attacker.example.com/"sv);
    auto https_url = parse_url("https://example.com/"sv);

    EXPECT(!stores_cookie(http_url,
        HTTP::Cookie::ParsedCookie {
            .name = "__hTtP-session"_string,
            .value = "attacker"_string,
            .domain = "example.com"_string,
            .path = "/"_string,
        },
        HTTP::Cookie::Source::NonHttp));

    EXPECT(!stores_cookie(https_url,
        HTTP::Cookie::ParsedCookie {
            .name = "__Host-Http-session"_string,
            .value = "attacker"_string,
            .path = "/"_string,
            .secure_attribute_present = true,
        },
        HTTP::Cookie::Source::Http));

    EXPECT(!stores_cookie(https_url,
        HTTP::Cookie::ParsedCookie {
            .name = "__Host-Http-session"_string,
            .value = "attacker"_string,
            .secure_attribute_present = true,
            .http_only_attribute_present = true,
        },
        HTTP::Cookie::Source::Http));

    EXPECT(!stores_cookie(https_url,
        HTTP::Cookie::ParsedCookie {
            .name = {},
            .value = "__Http-session"_string,
            .path = "/"_string,
            .secure_attribute_present = true,
            .http_only_attribute_present = true,
        },
        HTTP::Cookie::Source::Http));

    HTTP::Cookie::Cookie structured_cookie {
        .name = "__Http-session"_string,
        .value = "attacker"_string,
        .path = "/"_string,
        .secure = true,
        .host_only = true,
    };
    auto jar = WebView::CookieJar::create();
    EXPECT(jar->set_cookie_from_devtools(https_url, {}, structured_cookie).is_error());
    structured_cookie.name = "__Host-Http-session"_string;
    EXPECT(jar->set_cookie_from_devtools(https_url, {}, structured_cookie).is_error());
    structured_cookie.http_only = true;
    EXPECT(!jar->set_cookie_from_devtools(https_url, {}, structured_cookie).is_error());

    EXPECT(stores_cookie(http_url,
        HTTP::Cookie::ParsedCookie {
            .name = "__Httq-session"_string,
            .value = "ordinary"_string,
            .domain = "example.com"_string,
            .path = "/"_string,
        },
        HTTP::Cookie::Source::NonHttp));
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
    database->execute_statement(statement, [&](auto statement_id) -> ErrorOr<void> { version = database->result_column<u32>(statement_id, 0); return {}; });
    EXPECT_EQ(version, Optional<u32> { 2u });

    auto jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));

    // A cookie stored before cookies were partitioned stays a first-party cookie.
    auto cookies = jar->get_all_cookies();
    EXPECT_EQ(cookies.size(), 1uz);
    EXPECT_EQ(cookies[0].name, "foo"_string);
    EXPECT_EQ(cookies[0].value, "bar"_string);
    EXPECT_EQ(cookies[0].domain, "example.com"_string);
    EXPECT(cookies[0].partition_key.is_empty());
}

TEST_CASE(newer_cookie_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('Cookies', 99);"sv));

    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::DatabaseTooNew);
}

TEST_CASE(third_party_cookies_are_partitioned_by_top_level_site)
{
    auto jar = WebView::CookieJar::create();
    auto url = parse_url("https://widget.example/"sv);
    auto context = [](StringView top_level_site, bool has_cross_site_ancestor = false) {
        return Optional<HTTP::Cookie::PartitionContext> { { Utf16String::from_utf8(top_level_site), has_cross_site_ancestor } };
    };
    auto first_top_level_site = context("https://first.example"sv);
    auto second_top_level_site = context("https://second.example"sv);
    auto widget_site = context("https://widget.example"sv);

    // The widget's site framed in another site's frame under the widget's own top-level document.
    auto widget_site_with_cross_site_ancestor = context("https://widget.example"sv, true);

    auto set_cookie = [&](StringView name, Optional<HTTP::Cookie::PartitionContext> const& partition_context) {
        HTTP::Cookie::ParsedCookie cookie {
            .name = MUST(String::from_utf8(name)),
            .value = "1"_string,
            .path = "/"_string,
        };
        jar->set_cookie(url, cookie, HTTP::Cookie::Source::Http, partition_context);
    };

    set_cookie("embedded"sv, first_top_level_site);
    set_cookie("first-party"sv, widget_site);
    set_cookie("nested"sv, widget_site_with_cross_site_ancestor);

    EXPECT_EQ(jar->get_cookie(url, HTTP::Cookie::Source::Http, first_top_level_site), "embedded=1"_string);
    EXPECT_EQ(jar->get_cookie(url, HTTP::Cookie::Source::Http, second_top_level_site), String {});
    EXPECT_EQ(jar->get_cookie(url, HTTP::Cookie::Source::Http, widget_site), "first-party=1"_string);

    // A context with a cross-site ancestor is third-party even under a top-level document of its own site.
    EXPECT_EQ(jar->get_cookie(url, HTTP::Cookie::Source::Http, widget_site_with_cross_site_ancestor), "nested=1"_string);

    // A context of nothing is a first-party context, as for the UI process's own requests.
    EXPECT_EQ(jar->get_cookie(url, HTTP::Cookie::Source::Http, {}), "first-party=1"_string);

    // A WebSocket handshake to the widget's site is a first-party request under the widget's top-level documents.
    EXPECT_EQ(jar->get_cookie(parse_url("wss://widget.example/"sv), HTTP::Cookie::Source::Http, widget_site), "first-party=1"_string);

    auto cookies = jar->get_all_cookies();
    EXPECT_EQ(cookies.size(), 3uz);
    for (auto const& cookie : cookies) {
        if (cookie.name == "embedded"sv)
            EXPECT_EQ(cookie.partition_key, "https://first.example"_utf16);
        else if (cookie.name == "nested"sv)
            EXPECT_EQ(cookie.partition_key, "https://widget.example"_utf16);
        else
            EXPECT(cookie.partition_key.is_empty());
    }
}
