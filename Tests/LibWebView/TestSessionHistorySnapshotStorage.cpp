/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <LibTest/TestCase.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibWebView/SessionHistorySnapshotStorage.h>
#include <LibWebView/SessionStore.h>

using namespace WebView;

static URL::Origin::OpaqueData::Nonce sequential_nonce()
{
    URL::Origin::OpaqueData::Nonce nonce;
    for (size_t i = 0; i < nonce.size(); ++i)
        nonce[i] = static_cast<u8>(i + 1);
    return nonce;
}

static void expect_origin_round_trips(Optional<URL::Origin> const& origin)
{
    auto decoded = decode_origin(encode_origin(origin));
    EXPECT(!decoded.is_error());
    EXPECT(decoded.value() == origin);
}

static void expect_origin_rejected(PersistedOrigin const& persisted, StringView expected_message)
{
    auto decoded = decode_origin(persisted);
    EXPECT(decoded.is_error());
    EXPECT_EQ(decoded.error().string_literal(), expected_message);
}

TEST_CASE(origin_round_trips_empty)
{
    expect_origin_round_trips(Optional<URL::Origin> {});
    EXPECT_EQ(encode_origin(Optional<URL::Origin> {}).kind, 0);
}

TEST_CASE(origin_round_trips_opaque_standard)
{
    URL::Origin origin { URL::Origin::OpaqueData { .nonce = sequential_nonce(), .type = URL::Origin::OpaqueData::Type::Standard } };
    expect_origin_round_trips(origin);
    EXPECT_EQ(encode_origin(origin).kind, 1);
    EXPECT(encode_origin(origin).nonce == Optional<URL::Origin::OpaqueData::Nonce> { sequential_nonce() });
}

TEST_CASE(origin_round_trips_opaque_file)
{
    URL::Origin origin { URL::Origin::OpaqueData { .nonce = sequential_nonce(), .type = URL::Origin::OpaqueData::Type::File } };
    expect_origin_round_trips(origin);
    EXPECT_EQ(encode_origin(origin).kind, 2);
}

TEST_CASE(origin_round_trips_tuple_with_domain_host_and_port)
{
    URL::Origin origin { "https"_string, URL::Host { "www.example.com"_string }, static_cast<u16>(8080), URL::Host { "example.com"_string } };
    expect_origin_round_trips(origin);

    // is_same_origin ignores the domain, so pin it explicitly.
    EXPECT_EQ(encode_origin(origin).domain, Optional<String> { "example.com"_string });
    EXPECT_EQ(MUST(decode_origin(encode_origin(origin)))->domain()->serialize(), "example.com"_string);
}

TEST_CASE(origin_round_trips_tuple_with_ipv4_host_and_zero_port)
{
    auto ipv4 = IPv4Address::from_string("1.2.3.4"sv).value();
    URL::Origin origin { "http"_string, URL::Host { URL::Host::VariantType { ipv4 } }, static_cast<u16>(0) };
    expect_origin_round_trips(origin);

    // Port 0 is a real port, distinct from "no port" (stored as NULL).
    EXPECT_EQ(encode_origin(origin).port, Optional<u16> { 0 });
}

TEST_CASE(origin_round_trips_tuple_resource)
{
    URL::Origin origin { "resource"_string, URL::Host { String {} }, Optional<u16> {} };
    expect_origin_round_trips(origin);

    // The host is an active payload that happens to be empty, while an absent port is NULL.
    auto persisted = encode_origin(origin);
    EXPECT(persisted.host.has_value() && persisted.host->is_empty());
    EXPECT_EQ(persisted.port, Optional<u16> {});
}

TEST_CASE(origin_decode_normalizes_the_tuple_domain)
{
    auto decoded = MUST(decode_origin({ .kind = 3, .scheme = "https"_string, .host = "example.com"_string, .domain = "EXAMPLE.COM"_string }));
    EXPECT_EQ(decoded->domain()->serialize(), "example.com"_string);
}

TEST_CASE(origin_decode_accepts_an_ip_address_domain)
{
    // document.domain's equality path admits IP addresses.
    auto decoded = MUST(decode_origin({ .kind = 3, .scheme = "https"_string, .host = "0.0.0.0"_string, .domain = "0.0.0.0"_string }));
    EXPECT(decoded->domain()->has<IPv4Address>());
}

TEST_CASE(origin_decode_rejects_invalid_columns)
{
    auto tuple = [](StringView scheme, StringView host) {
        return PersistedOrigin { .kind = 3, .scheme = MUST(String::from_utf8(scheme)), .host = MUST(String::from_utf8(host)) };
    };

    expect_origin_rejected({ .kind = 99 }, "Persisted origin has an unknown kind"sv);
    expect_origin_rejected({ .kind = 1 }, "Persisted opaque origin has no nonce"sv);
    expect_origin_rejected({ .kind = 3, .scheme = "https"_string, .host = "example.com"_string, .domain = ""_string }, "Persisted tuple origin has an empty domain"sv);
    expect_origin_rejected({ .kind = 3, .scheme = "https"_string, .host = "example.com"_string, .domain = "[invalid"_string }, "Persisted tuple origin has an unparseable domain"sv);
    expect_origin_rejected({ .kind = 3, .scheme = "resource"_string, .host = ""_string, .domain = "example.com"_string }, "Persisted tuple origin has a domain with an empty host"sv);
    expect_origin_rejected({ .kind = 3, .host = "example.com"_string }, "Persisted tuple origin has an empty scheme"sv);
    expect_origin_rejected(tuple(""sv, "example.com"sv), "Persisted tuple origin has an empty scheme"sv);
    expect_origin_rejected({ .kind = 3, .scheme = "https"_string }, "Persisted tuple origin has no host"sv);
    expect_origin_rejected(tuple("https"sv, "[invalid"sv), "Persisted tuple origin has an unparseable host"sv);
    expect_origin_rejected(tuple("https"sv, ""sv), "Persisted network-scheme tuple origin has an empty host"sv);
    expect_origin_rejected(tuple("resource"sv, "example.com"sv), "Persisted resource tuple origin has a host or port"sv);
    expect_origin_rejected(tuple("file"sv, ""sv), "Persisted file tuple origin while file tuple origins are disabled"sv);
    expect_origin_rejected(tuple("gopher"sv, "example.com"sv), "Persisted tuple origin has a scheme that cannot form a tuple origin"sv);
}

static void expect_referrer_round_trips(Web::Fetch::Infrastructure::Request::ReferrerType const& referrer)
{
    auto decoded = decode_referrer(encode_referrer(referrer));
    EXPECT(!decoded.is_error());
    EXPECT(decoded.value() == referrer);
}

TEST_CASE(referrer_round_trips)
{
    using Referrer = Web::Fetch::Infrastructure::Request::Referrer;
    using ReferrerType = Web::Fetch::Infrastructure::Request::ReferrerType;

    expect_referrer_round_trips(ReferrerType { Referrer::Client });
    expect_referrer_round_trips(ReferrerType { Referrer::NoReferrer });
    expect_referrer_round_trips(ReferrerType { URL::Parser::basic_parse("https://ref.example/"sv).value() });
}

TEST_CASE(referrer_decode_rejects_invalid_columns)
{
    auto unknown_kind = decode_referrer({ .kind = 9 });
    EXPECT(unknown_kind.is_error());
    EXPECT_EQ(unknown_kind.error().string_literal(), "Persisted referrer has an unknown kind"sv);

    auto bad_url = decode_referrer({ .kind = 2, .url = "http://[invalid"_string });
    EXPECT(bad_url.is_error());
    EXPECT_EQ(bad_url.error().string_literal(), "Persisted referrer has an unparseable url"sv);
}

TEST_CASE(referrer_policy_round_trips)
{
    using RP = Web::ReferrerPolicy::ReferrerPolicy;
    Array policies { RP::EmptyString, RP::NoReferrer, RP::NoReferrerWhenDowngrade, RP::SameOrigin, RP::Origin, RP::StrictOrigin, RP::OriginWhenCrossOrigin, RP::StrictOriginWhenCrossOrigin, RP::UnsafeURL };
    for (auto policy : policies) {
        auto decoded = decode_referrer_policy(encode_referrer_policy(policy));
        EXPECT(!decoded.is_error());
        EXPECT(decoded.value() == policy);
    }

    auto unknown = decode_referrer_policy(99);
    EXPECT(unknown.is_error());
    EXPECT_EQ(unknown.error().string_literal(), "Persisted referrer policy has an unknown tag"sv);
}

TEST_CASE(resource_round_trips)
{
    auto empty = decode_resource(encode_resource(Web::HTML::DocumentResource { Empty {} }));
    EXPECT(!empty.is_error());
    EXPECT(empty.value().has<Empty>());

    auto srcdoc = decode_resource(encode_resource(Web::HTML::DocumentResource { "<p>hi</p>"_utf16 }));
    EXPECT(!srcdoc.is_error());
    EXPECT(srcdoc.value().has<Utf16String>());
    EXPECT_EQ(srcdoc.value().get<Utf16String>(), "<p>hi</p>"_utf16);
}

TEST_CASE(resource_drops_post_body)
{
    Web::HTML::POSTResource post {
        .request_body = MUST(ByteBuffer::copy("name=ladybird"sv.bytes())),
        .request_content_type = Web::HTML::POSTResource::RequestContentType::ApplicationXWWWFormUrlencoded,
    };

    // v1 restores a POST entry as a GET: the resource encodes as Empty and never decodes to a POST.
    auto persisted = encode_resource(Web::HTML::DocumentResource { move(post) });
    EXPECT_EQ(persisted.kind, 0);

    auto decoded = decode_resource(persisted);
    EXPECT(!decoded.is_error());
    EXPECT(decoded.value().has<Empty>());
}

TEST_CASE(resource_decode_rejects_unknown_kind)
{
    auto unknown = decode_resource({ .kind = 9 });
    EXPECT(unknown.is_error());
    EXPECT_EQ(unknown.error().string_literal(), "Persisted resource has an unknown kind"sv);
}

TEST_CASE(scroll_restoration_mode_round_trips)
{
    using Mode = Web::HTML::ScrollRestorationMode;
    for (auto mode : Array { Mode::Auto, Mode::Manual }) {
        auto decoded = decode_scroll_restoration_mode(encode_scroll_restoration_mode(mode));
        EXPECT(!decoded.is_error());
        EXPECT(decoded.value() == mode);
    }

    auto unknown = decode_scroll_restoration_mode(7);
    EXPECT(unknown.is_error());
    EXPECT_EQ(unknown.error().string_literal(), "Persisted scroll restoration mode has an unknown tag"sv);
}

TEST_CASE(embedder_policy_value_round_trips)
{
    using Value = Web::HTML::EmbedderPolicyValue;
    for (auto value : Array { Value::UnsafeNone, Value::RequireCorp, Value::Credentialless }) {
        auto decoded = decode_embedder_policy_value(encode_embedder_policy_value(value));
        EXPECT(!decoded.is_error());
        EXPECT(decoded.value() == value);
    }

    auto unknown = decode_embedder_policy_value(9);
    EXPECT(unknown.is_error());
    EXPECT_EQ(unknown.error().string_literal(), "Persisted embedder policy value has an unknown tag"sv);
}

TEST_CASE(csp_disposition_round_trips)
{
    using Disposition = Web::ContentSecurityPolicy::Policy::Disposition;
    for (auto disposition : Array { Disposition::Enforce, Disposition::Report }) {
        auto decoded = decode_csp_disposition(encode_csp_disposition(disposition));
        EXPECT(!decoded.is_error());
        EXPECT(decoded.value() == disposition);
    }

    auto unknown = decode_csp_disposition(9);
    EXPECT(unknown.is_error());
    EXPECT_EQ(unknown.error().string_literal(), "Persisted CSP policy disposition has an unknown tag"sv);
}

TEST_CASE(csp_source_round_trips)
{
    using Source = Web::ContentSecurityPolicy::Policy::Source;
    for (auto source : Array { Source::Header, Source::Meta }) {
        auto decoded = decode_csp_source(encode_csp_source(source));
        EXPECT(!decoded.is_error());
        EXPECT(decoded.value() == source);
    }

    auto unknown = decode_csp_source(9);
    EXPECT(unknown.is_error());
    EXPECT_EQ(unknown.error().string_literal(), "Persisted CSP policy source has an unknown tag"sv);
}

// The session store opens its connection with foreign keys enabled; the mapping's cascading deletes rely on it.
static NonnullRefPtr<Database::Database> make_database()
{
    return MUST(Database::Database::create_memory_backed({ .foreign_keys = Database::Database::ForeignKeys::Yes }));
}

static SessionHistorySnapshotStatements prepare_snapshot_tables(Database::Database& database)
{
    MUST(WebView::SessionStore::migrate_schema(database));
    return MUST(prepare_session_history_snapshot_statements(database));
}

// Snapshot rows are children of a live tab, so the enforced foreign keys need the parent rows first.
static void insert_tab_row(Database::Database& database, i64 tab_id)
{
    auto insert_session = MUST(database.prepare_statement("INSERT INTO Sessions (id, kind, closed_time, active_tab_index) VALUES (?, 0, 0, 0);"sv));
    database.execute_statement(insert_session, {}, tab_id);
    auto insert_tab = MUST(database.prepare_statement("INSERT INTO SessionTabs (id, session_id, tab_ordinal, active_url, current_used_step_index) VALUES (?, ?, 0, '', 0);"sv));
    database.execute_statement(insert_tab, {}, tab_id, tab_id);
}

// The production inserts let the database assign ids, so corrupt-row tests place theirs directly.
static void insert_history_row(Database::Database& database, i64 history_id, i64 tab_id)
{
    auto insert_history = MUST(database.prepare_statement("INSERT INTO SessionHistories (id, tab_id) VALUES (?, ?);"sv));
    database.execute_statement(insert_history, {}, history_id, tab_id);
}

static Web::HTML::CrossProcessId frame_id(u64 local_id)
{
    return { 2, local_id };
}

static void insert_nested_history_link(Database::Database& database, SessionHistorySnapshotStatements const& statements, i64 parent_history_id, i64 parent_entry_ordinal, i64 nested_ordinal, i64 child_history_id)
{
    database.execute_statement(statements.insert_nested_history_link, {},
        parent_history_id, parent_entry_ordinal, nested_ordinal, static_cast<i64>(1), static_cast<i64>(1), child_history_id);
}

static Web::HTML::SessionHistoryEntryDescriptor make_entry(i32 step, StringView url, u64 document_state_id, u8 classic_byte, u8 navigation_byte, StringView key, StringView id, Optional<Web::CSSPixelPoint> scroll)
{
    Web::HTML::SessionHistoryEntryDescriptor entry;
    entry.step = step;
    entry.url = URL::Parser::basic_parse(url).value();
    entry.document_state.id = { 1, document_state_id };
    entry.document_state.ever_populated = true;
    entry.document_state.history_policy_container = Web::HTML::DocumentState::Client::Tag;
    entry.classic_history_api_state = Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::copy({ &classic_byte, 1 })) };
    entry.navigation_api_state = Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::copy({ &navigation_byte, 1 })) };
    entry.navigation_api_key = Utf16String::from_utf8(key);
    entry.navigation_api_id = Utf16String::from_utf8(id);
    entry.scroll_restoration_mode = Web::HTML::ScrollRestorationMode::Auto;
    entry.scroll_position_data.viewport_scroll_position = scroll;
    return entry;
}

static void insert_minimal_entry(Database::Database& database, SessionHistorySnapshotStatements const& statements, i64 history_id, i64 entry_ordinal, i64 document_state_id = 1, bool ever_populated = true)
{
    Optional<String> const absent_text;
    Optional<i32> const absent_scroll;
    Optional<u16> const absent_port;

    database.execute_statement(statements.insert_entry, {},
        history_id, entry_ordinal, static_cast<i64>(0), MUST(String::from_utf8("https://x.example/"sv)), static_cast<i64>(1), document_state_id,
        ByteString {}, ByteString {}, String {}, String {},
        static_cast<i64>(0), absent_scroll, absent_scroll,
        static_cast<i64>(0), absent_text, absent_text, absent_text, absent_port, absent_text,
        static_cast<i64>(0), absent_text, absent_text, absent_text, absent_port, absent_text,
        static_cast<i64>(0), absent_text, static_cast<i64>(0),
        static_cast<i64>(0), absent_text,
        absent_text, String {}, ever_populated, false);
}

static i32 count_tab_rows(Database::Database& database, StringView table, i64 tab_id)
{
    auto statement = MUST(database.prepare_statement(MUST(String::formatted("SELECT COUNT(*) FROM {} WHERE tab_id = ?;", table))));
    i32 count = -1;
    database.execute_statement(
        statement,
        [&](auto statement_id) -> ErrorOr<void> {
            count = database.result_column<i32>(statement_id, 0);
            return {};
        },
        tab_id);
    return count;
}

static i32 count_rows(Database::Database& database, StringView table)
{
    auto statement = MUST(database.prepare_statement(MUST(String::formatted("SELECT COUNT(*) FROM {};", table))));
    i32 count = -1;
    database.execute_statement(statement, [&](auto statement_id) -> ErrorOr<void> {
        count = database.result_column<i32>(statement_id, 0);
        return {};
    });
    return count;
}

static i64 select_i64(Database::Database& database, StringView sql)
{
    auto statement = MUST(database.prepare_statement(sql));
    i64 value = 0;
    database.execute_statement(statement, [&](auto statement_id) -> ErrorOr<void> {
        value = database.result_column<i64>(statement_id, 0);
        return {};
    });
    return value;
}

static i32 count_history_owned_rows(Database::Database& database, StringView table, i64 tab_id)
{
    auto statement = MUST(database.prepare_statement(MUST(String::formatted("SELECT COUNT(*) FROM {} INNER JOIN SessionHistories ON {}.history_id = SessionHistories.id WHERE SessionHistories.tab_id = ?;", table, table))));
    i32 count = -1;
    database.execute_statement(
        statement,
        [&](auto statement_id) -> ErrorOr<void> {
            count = database.result_column<i32>(statement_id, 0);
            return {};
        },
        tab_id);
    return count;
}

static Web::HTML::SerializedPolicyContainer make_policy_container()
{
    Web::ContentSecurityPolicy::SerializedPolicy enforced_policy {
        .directives = {
            { .name = "script-src"_utf16, .value = { "'self'"_utf16, "https://cdn.example"_utf16 } },
            { .name = "upgrade-insecure-requests"_utf16, .value = {} },
        },
        .disposition = Web::ContentSecurityPolicy::Policy::Disposition::Enforce,
        .source = Web::ContentSecurityPolicy::Policy::Source::Header,
        .self_origin = URL::Origin { "https"_string, URL::Host { "a.example"_string }, static_cast<u16>(443), URL::Host { "a.example"_string } },
        .pre_parsed_policy_string = "script-src 'self' https://cdn.example; upgrade-insecure-requests"_string,
    };
    Web::ContentSecurityPolicy::SerializedPolicy report_only_policy {
        .directives = {
            { .name = "img-src"_utf16, .value = { "'none'"_utf16 } },
        },
        .disposition = Web::ContentSecurityPolicy::Policy::Disposition::Report,
        .source = Web::ContentSecurityPolicy::Policy::Source::Meta,
        .self_origin = URL::Origin { URL::Origin::OpaqueData { .nonce = sequential_nonce(), .type = URL::Origin::OpaqueData::Type::Standard } },
        .pre_parsed_policy_string = "img-src 'none'"_string,
    };
    return {
        .csp_list = { move(enforced_policy), move(report_only_policy) },
        .embedder_policy = {
            .value = Web::HTML::EmbedderPolicyValue::RequireCorp,
            .report_only_value = Web::HTML::EmbedderPolicyValue::Credentialless,
            .reporting_endpoint = "coep"_utf16,
            .report_only_reporting_endpoint = "coep-report"_utf16,
        },
        .referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::NoReferrer,
    };
}

static void expect_policy_containers_equal(Web::HTML::SerializedPolicyContainer const& restored, Web::HTML::SerializedPolicyContainer const& original)
{
    EXPECT_EQ(restored.csp_list.size(), original.csp_list.size());
    for (size_t i = 0; i < min(restored.csp_list.size(), original.csp_list.size()); ++i) {
        auto const& restored_policy = restored.csp_list[i];
        auto const& original_policy = original.csp_list[i];
        EXPECT(restored_policy.disposition == original_policy.disposition);
        EXPECT(restored_policy.source == original_policy.source);
        EXPECT(restored_policy.self_origin == original_policy.self_origin);
        EXPECT_EQ(restored_policy.pre_parsed_policy_string, original_policy.pre_parsed_policy_string);
        EXPECT_EQ(restored_policy.directives.size(), original_policy.directives.size());
        for (size_t j = 0; j < min(restored_policy.directives.size(), original_policy.directives.size()); ++j) {
            EXPECT_EQ(restored_policy.directives[j].name, original_policy.directives[j].name);
            EXPECT(restored_policy.directives[j].value == original_policy.directives[j].value);
        }
    }
    EXPECT(restored.embedder_policy.value == original.embedder_policy.value);
    EXPECT(restored.embedder_policy.report_only_value == original.embedder_policy.report_only_value);
    EXPECT_EQ(restored.embedder_policy.reporting_endpoint, original.embedder_policy.reporting_endpoint);
    EXPECT_EQ(restored.embedder_policy.report_only_reporting_endpoint, original.embedder_policy.report_only_reporting_endpoint);
    EXPECT(restored.referrer_policy == original.referrer_policy);
}

TEST_CASE(snapshot_round_trips_flat_entries)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 42);

    auto rich_entry = make_entry(0, "https://a.example/"sv, 1, 0x11, 0x22, "keyA"sv, "idA"sv, Web::CSSPixelPoint { Web::CSSPixels::from_raw(7), Web::CSSPixels::from_raw(-9) });
    rich_entry.scroll_restoration_mode = Web::HTML::ScrollRestorationMode::Manual;
    rich_entry.document_state.origin = URL::Origin { "https"_string, URL::Host { "a.example"_string }, static_cast<u16>(8443), URL::Host { "a.example"_string } };
    rich_entry.document_state.initiator_origin = URL::Origin::create_opaque();
    rich_entry.document_state.request_referrer = Web::Fetch::Infrastructure::Request::ReferrerType { URL::Parser::basic_parse("https://ref.example/"sv).value() };
    rich_entry.document_state.request_referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::StrictOrigin;
    rich_entry.document_state.resource = "<p>srcdoc</p>"_utf16;
    rich_entry.document_state.about_base_url = URL::Parser::basic_parse("https://base.example/"sv);
    rich_entry.document_state.navigable_target_name = "main"_utf16;
    rich_entry.document_state.ever_populated = true;
    rich_entry.document_state.reload_pending = true;

    auto plain_entry = make_entry(1, "https://b.example/"sv, 2, 0x33, 0x44, "keyB"sv, "idB"sv, {});
    plain_entry.document_state.initiator_origin = URL::Origin { "https"_string, URL::Host { "b.example"_string }, Optional<u16> {}, URL::Host { "b.example"_string } };

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(rich_entry));
    snapshot.entries.append(move(plain_entry));
    snapshot.used_steps = { 0, 1 };
    snapshot.current_used_step_index = 1;

    TRY_OR_FAIL(store_session_history_snapshot(*database, statements, 42, snapshot));
    auto loaded = TRY_OR_FAIL(load_session_history_snapshot(*database, statements, 42, 1));

    EXPECT_EQ(loaded.entries.size(), 2uz);
    EXPECT_EQ(loaded.current_used_step_index, 1uz);
    EXPECT(loaded.used_steps == snapshot.used_steps);
    for (size_t i = 0; i < snapshot.entries.size(); ++i) {
        auto const& original = snapshot.entries[i];
        auto const& restored = loaded.entries[i];
        auto const& original_state = original.document_state;
        auto const& restored_state = restored.document_state;
        EXPECT(restored.step == original.step);
        EXPECT(restored.url == original.url);
        EXPECT(restored.classic_history_api_state == original.classic_history_api_state);
        EXPECT(restored.navigation_api_state == original.navigation_api_state);
        EXPECT(restored.navigation_api_key == original.navigation_api_key);
        EXPECT(restored.navigation_api_id == original.navigation_api_id);
        EXPECT(restored.scroll_restoration_mode == original.scroll_restoration_mode);
        EXPECT_EQ(restored.scroll_position_data.viewport_scroll_position, original.scroll_position_data.viewport_scroll_position);
        EXPECT(restored_state.id == original_state.id);
        EXPECT(restored_state.origin == original_state.origin);
        EXPECT(restored_state.initiator_origin == original_state.initiator_origin);
        EXPECT(restored_state.request_referrer == original_state.request_referrer);
        EXPECT(restored_state.request_referrer_policy == original_state.request_referrer_policy);
        EXPECT(restored_state.about_base_url == original_state.about_base_url);
        EXPECT(restored_state.navigable_target_name == original_state.navigable_target_name);
        EXPECT(restored_state.ever_populated == original_state.ever_populated);
        EXPECT(restored_state.reload_pending == original_state.reload_pending);
        EXPECT(restored_state.resource.has<Utf16String>() == original_state.resource.has<Utf16String>());
        if (original_state.resource.has<Utf16String>())
            EXPECT(restored_state.resource.get<Utf16String>() == original_state.resource.get<Utf16String>());
    }

    // is_same_origin ignores the domain, so pin each domain column explicitly.
    EXPECT_EQ(loaded.entries[0].document_state.origin->domain()->serialize(), "a.example"_string);
    EXPECT_EQ(loaded.entries[1].document_state.initiator_origin->domain()->serialize(), "b.example"_string);
}

TEST_CASE(snapshot_load_rejects_non_contiguous_entries)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 1);
    insert_history_row(*database, 1, 1);
    database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), static_cast<i64>(0), static_cast<i64>(0));
    insert_minimal_entry(*database, statements, 1, 0);
    insert_minimal_entry(*database, statements, 1, 2);

    auto loaded = load_session_history_snapshot(*database, statements, 1, 0);
    EXPECT(loaded.is_error());
    EXPECT_EQ(loaded.error().string_literal(), "Session history entries are not contiguously ordered"sv);
}

TEST_CASE(snapshot_load_rejects_out_of_range_current_index)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 1);
    insert_history_row(*database, 1, 1);
    database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), static_cast<i64>(0), static_cast<i64>(0));
    insert_minimal_entry(*database, statements, 1, 0);

    auto loaded = load_session_history_snapshot(*database, statements, 1, 5);
    EXPECT(loaded.is_error());
    EXPECT_EQ(loaded.error().string_literal(), "Session history current step index is out of range"sv);
}

TEST_CASE(snapshot_load_rejects_inconsistent_used_steps)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    // One entry at step 0, but used_steps claims a step (1) that no entry provides.
    insert_tab_row(*database, 1);
    insert_history_row(*database, 1, 1);
    database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), static_cast<i64>(0), static_cast<i64>(0));
    database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), static_cast<i64>(1), static_cast<i64>(1));
    insert_minimal_entry(*database, statements, 1, 0);

    auto loaded = load_session_history_snapshot(*database, statements, 1, 0);
    EXPECT(loaded.is_error());
    EXPECT_EQ(loaded.error().string_literal(), "Session history snapshot is structurally invalid"sv);
}

TEST_CASE(snapshot_round_trips_policy_containers)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 21);

    auto container_entry = make_entry(1, "data:text/html,x"sv, 2, 0, 0, ""sv, ""sv, {});
    container_entry.document_state.history_policy_container = make_policy_container();

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(make_entry(0, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {}));
    snapshot.entries.append(move(container_entry));
    snapshot.used_steps = { 0, 1 };
    snapshot.current_used_step_index = 1;

    TRY_OR_FAIL(store_session_history_snapshot(*database, statements, 21, snapshot));
    auto loaded = TRY_OR_FAIL(load_session_history_snapshot(*database, statements, 21, 1));

    EXPECT_EQ(loaded.entries.size(), 2uz);
    EXPECT(loaded.entries[0].document_state.history_policy_container.has<Web::HTML::DocumentState::Client>());
    EXPECT_EQ(count_history_owned_rows(*database, "SessionPolicyContainers"sv, 21), 1);

    auto const* container = loaded.entries[1].document_state.history_policy_container.get_pointer<Web::HTML::SerializedPolicyContainer>();
    EXPECT(container != nullptr);
    if (container) {
        expect_policy_containers_equal(*container, make_policy_container());

        // is_same_origin ignores the domain, so pin the self-origin domain column explicitly.
        EXPECT_EQ(container->csp_list[0].self_origin.domain()->serialize(), "a.example"_string);
    }
}

TEST_CASE(snapshot_round_trips_empty_policy_container)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 22);

    // The about:newtab shape: a stored container whose CSP list is empty and whose fields are defaults.
    auto entry = make_entry(0, "about:newtab"sv, 1, 0, 0, ""sv, ""sv, {});
    entry.document_state.history_policy_container = Web::HTML::SerializedPolicyContainer {};

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(entry));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    TRY_OR_FAIL(store_session_history_snapshot(*database, statements, 22, snapshot));
    EXPECT_EQ(count_history_owned_rows(*database, "SessionPolicyContainers"sv, 22), 1);
    EXPECT_EQ(count_rows(*database, "SessionCspPolicies"sv), 0);

    auto loaded = TRY_OR_FAIL(load_session_history_snapshot(*database, statements, 22, 0));
    auto const* container = loaded.entries[0].document_state.history_policy_container.get_pointer<Web::HTML::SerializedPolicyContainer>();
    EXPECT(container != nullptr);
    if (container)
        expect_policy_containers_equal(*container, Web::HTML::SerializedPolicyContainer {});
}

TEST_CASE(snapshot_delete_cascades_policy_container_rows)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 23);

    auto entry = make_entry(0, "data:text/html,x"sv, 1, 0, 0, ""sv, ""sv, {});
    entry.document_state.history_policy_container = make_policy_container();

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(entry));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    TRY_OR_FAIL(store_session_history_snapshot(*database, statements, 23, snapshot));
    EXPECT_EQ(count_rows(*database, "SessionCspDirectiveValues"sv), 3);

    TRY_OR_FAIL(delete_session_history_snapshot(*database, statements, 23));
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 23), 0);
    EXPECT_EQ(count_history_owned_rows(*database, "SessionPolicyContainers"sv, 23), 0);
    EXPECT_EQ(count_rows(*database, "SessionCspPolicies"sv), 0);
    EXPECT_EQ(count_rows(*database, "SessionCspDirectives"sv), 0);
    EXPECT_EQ(count_rows(*database, "SessionCspDirectiveValues"sv), 0);
}

TEST_CASE(snapshot_store_rejects_oversized_policy_container_text)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    auto entry = make_entry(0, "data:text/html,x"sv, 1, 0, 0, ""sv, ""sv, {});
    auto container = make_policy_container();
    container.csp_list[0].pre_parsed_policy_string = MUST(String::repeated('a', (8uz * 1024 * 1024) + 1));
    entry.document_state.history_policy_container = move(container);

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(entry));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    auto result = store_session_history_snapshot(*database, statements, 24, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session history entry text column exceeds the maximum size"sv);
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 24), 0);
    EXPECT_EQ(count_history_owned_rows(*database, "SessionPolicyContainers"sv, 24), 0);
}

TEST_CASE(snapshot_store_leaves_no_partial_rows_on_rejection)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    auto rejected_entry = make_entry(1, "https://b.example/"sv, 2, 0, 0, ""sv, ""sv, {});
    rejected_entry.classic_history_api_state = Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::create_zeroed((8uz * 1024 * 1024) + 1)) };

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(make_entry(0, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {}));
    snapshot.entries.append(move(rejected_entry));
    snapshot.used_steps = { 0, 1 };
    snapshot.current_used_step_index = 1;

    auto result = store_session_history_snapshot(*database, statements, 7, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 7), 0);
    EXPECT_EQ(count_tab_rows(*database, "SessionUsedSteps"sv, 7), 0);
}

TEST_CASE(snapshot_store_rejects_oversized_state_record)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    // One byte past the load-time cap: stored, this snapshot would fail its own bounded read.
    auto entry = make_entry(0, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {});
    entry.classic_history_api_state = Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::create_zeroed((8uz * 1024 * 1024) + 1)) };

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(entry));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    auto result = store_session_history_snapshot(*database, statements, 11, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session history entry classic history state exceeds the maximum size"sv);
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 11), 0);
    EXPECT_EQ(count_tab_rows(*database, "SessionUsedSteps"sv, 11), 0);
}

TEST_CASE(snapshot_store_rejects_oversized_text_column)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    auto entry = make_entry(0, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {});
    entry.document_state.resource = Utf16String::repeated('a', (8uz * 1024 * 1024) + 1);

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(entry));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    auto result = store_session_history_snapshot(*database, statements, 12, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session history entry text column exceeds the maximum size"sv);
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 12), 0);
    EXPECT_EQ(count_tab_rows(*database, "SessionUsedSteps"sv, 12), 0);
}

TEST_CASE(snapshot_store_rejects_oversized_snapshot)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    WebView::SessionHistorySnapshot snapshot;
    for (i32 step = 0; step < 8; ++step) {
        auto entry = make_entry(step, "https://a.example/"sv, static_cast<u64>(step) + 1, 0, 0, ""sv, ""sv, {});
        entry.classic_history_api_state = Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::create_zeroed(8uz * 1024 * 1024)) };
        snapshot.entries.append(move(entry));
        snapshot.used_steps.append(step);
    }
    snapshot.current_used_step_index = snapshot.used_steps.size() - 1;

    auto result = store_session_history_snapshot(*database, statements, 25, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session history snapshot exceeds the maximum size"sv);
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 25), 0);
}

TEST_CASE(snapshot_store_rolls_back_with_the_caller_transaction)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(make_entry(0, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {}));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    insert_tab_row(*database, 9);

    auto result = database->transaction([&]() -> ErrorOr<void> {
        TRY(store_session_history_snapshot(*database, statements, 9, snapshot));
        return Error::from_string_literal("caller aborted the undo unit");
    });
    EXPECT(result.is_error());
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 9), 0);
    EXPECT_EQ(count_tab_rows(*database, "SessionUsedSteps"sv, 9), 0);
}

TEST_CASE(snapshot_store_rejects_structurally_inconsistent_snapshot)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    // Entries at steps 0 and 1, but a used-steps list that omits step 1: reopen would reject this.
    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(make_entry(0, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {}));
    snapshot.entries.append(make_entry(1, "https://b.example/"sv, 2, 0, 0, ""sv, ""sv, {}));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    auto result = store_session_history_snapshot(*database, statements, 13, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session history snapshot is structurally invalid"sv);
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 13), 0);
    EXPECT_EQ(count_tab_rows(*database, "SessionUsedSteps"sv, 13), 0);
}

TEST_CASE(snapshot_store_rejects_current_entry_without_document_state)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(make_entry(0, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {}));
    snapshot.entries[0].document_state.ever_populated = false;
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    auto result = store_session_history_snapshot(*database, statements, 14, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session history snapshot's current entry has no document state"sv);
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 14), 0);
    EXPECT_EQ(count_tab_rows(*database, "SessionUsedSteps"sv, 14), 0);
}

TEST_CASE(snapshot_load_rejects_current_entry_without_document_state)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 1);
    insert_history_row(*database, 1, 1);
    database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), static_cast<i64>(0), static_cast<i64>(0));
    insert_minimal_entry(*database, statements, 1, 0, 1, false);

    auto loaded = load_session_history_snapshot(*database, statements, 1, 0);
    EXPECT(loaded.is_error());
    EXPECT_EQ(loaded.error().string_literal(), "Session history snapshot's current entry has no document state"sv);
}

static void insert_minimal_policy_container(Database::Database& database, i64 id, i64 entry_ordinal, i64 referrer_policy = 0, i64 embedder_policy_value = 0)
{
    auto insert_container = MUST(database.prepare_statement(R"#(
        INSERT INTO SessionPolicyContainers (
            id, history_id, entry_ordinal, referrer_policy, embedder_policy_value,
            embedder_policy_report_only_value, embedder_policy_reporting_endpoint,
            embedder_policy_report_only_reporting_endpoint)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?);
    )#"sv));
    database.execute_statement(insert_container, {},
        id, static_cast<i64>(1), entry_ordinal, referrer_policy,
        embedder_policy_value, static_cast<i64>(0), String {}, String {});
}

static void insert_minimal_csp_policy(Database::Database& database, i64 id, i64 container_id, i64 policy_ordinal, i64 disposition = 0, i64 source = 0, i64 self_origin_kind = 3)
{
    auto insert_policy = MUST(database.prepare_statement(R"#(
        INSERT INTO SessionCspPolicies (
            id, container_id, policy_ordinal, disposition, source,
            self_origin_kind, self_origin_nonce, self_origin_scheme, self_origin_host,
            self_origin_port, self_origin_domain, pre_parsed_policy_string)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )#"sv));
    database.execute_statement(insert_policy, {},
        id, container_id, policy_ordinal, disposition, source,
        self_origin_kind, Optional<String> {}, "https"_string, "x.example"_string, Optional<u16> {}, Optional<String> {},
        String {});
}

template<typename Setup>
static void expect_load_error(Setup setup, StringView expected_message)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);
    insert_tab_row(*database, 1);
    insert_history_row(*database, 1, 1);
    database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), static_cast<i64>(0), static_cast<i64>(0));
    insert_minimal_entry(*database, statements, 1, 0);
    setup(*database, statements);

    auto loaded = load_session_history_snapshot(*database, statements, 1, 0);
    EXPECT(loaded.is_error());
    EXPECT_EQ(loaded.error().string_literal(), expected_message);
}

TEST_CASE(snapshot_load_rejects_a_half_null_scroll_position)
{
    // A scroll position is one value across two columns, so one coordinate alone is corruption rather than an entry
    // that merely never scrolled.
    expect_load_error([](auto& database, auto&) {
        MUST(database.execute_raw("UPDATE SessionEntries SET scroll_x_raw = 5;"sv));
    },
        "Session history entry has only one scroll coordinate"sv);

    expect_load_error([](auto& database, auto&) {
        MUST(database.execute_raw("UPDATE SessionEntries SET scroll_y_raw = 5;"sv));
    },
        "Session history entry has only one scroll coordinate"sv);
}

TEST_CASE(snapshot_load_rejects_a_nonce_that_is_not_the_nonce_length)
{
    // Too long fails the bounded read before it is copied; too short fails the length check.
    expect_load_error([](auto& database, auto&) {
        MUST(database.execute_raw("UPDATE SessionEntries SET origin_kind = 1, origin_nonce = x'000102030405060708090a0b0c0d0e0f10';"sv));
    },
        "Blob column exceeds the size limit"sv);

    expect_load_error([](auto& database, auto&) {
        MUST(database.execute_raw("UPDATE SessionEntries SET origin_kind = 1, origin_nonce = x'000102030405060708090a0b0c0d0e';"sv));
    },
        "Persisted opaque origin nonce is not the nonce length"sv);
}

TEST_CASE(snapshot_load_rejects_oversized_snapshot)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);
    insert_tab_row(*database, 1);
    insert_history_row(*database, 1, 1);

    for (i64 step = 0; step < 8; ++step) {
        database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), step, step);
        insert_minimal_entry(*database, statements, 1, step, step + 1);
        TRY_OR_FAIL(database->execute_raw(ByteString::formatted("UPDATE SessionEntries SET step = {}, classic_state = zeroblob(8388608) WHERE history_id = 1 AND entry_ordinal = {};", step, step)));
    }

    auto loaded = load_session_history_snapshot(*database, statements, 1, 7);
    EXPECT(loaded.is_error());
    EXPECT_EQ(loaded.error().string_literal(), "Session history snapshot exceeds the maximum size"sv);
}

TEST_CASE(snapshot_load_rejects_invalid_policy_container_rows)
{
    auto insert_directive = [](Database::Database& database, SessionHistorySnapshotStatements const&, i64 id, i64 policy_id, i64 directive_ordinal) {
        auto statement = MUST(database.prepare_statement("INSERT INTO SessionCspDirectives (id, policy_id, directive_ordinal, name) VALUES (?, ?, ?, ?);"sv));
        database.execute_statement(statement, {}, id, policy_id, directive_ordinal, "img-src"_string);
    };
    auto insert_value = [](Database::Database& database, SessionHistorySnapshotStatements const& statements, i64 directive_id, i64 value_ordinal) {
        database.execute_statement(statements.insert_csp_directive_value, {}, directive_id, value_ordinal, "'none'"_string);
    };

    expect_load_error([](auto& database, auto&) {
        insert_minimal_policy_container(database, 1, 0, 99);
    },
        "Persisted referrer policy has an unknown tag"sv);

    expect_load_error([](auto& database, auto&) {
        insert_minimal_policy_container(database, 1, 0, 0, 9);
    },
        "Persisted embedder policy value has an unknown tag"sv);

    expect_load_error([](auto& database, auto&) {
        insert_minimal_policy_container(database, 1, 0);
        insert_minimal_csp_policy(database, 1, 1, 0, 9);
    },
        "Persisted CSP policy disposition has an unknown tag"sv);

    expect_load_error([](auto& database, auto&) {
        insert_minimal_policy_container(database, 1, 0);
        insert_minimal_csp_policy(database, 1, 1, 0, 0, 9);
    },
        "Persisted CSP policy source has an unknown tag"sv);

    expect_load_error([](auto& database, auto&) {
        insert_minimal_policy_container(database, 1, 0);
        insert_minimal_csp_policy(database, 1, 1, 0, 0, 0, 0);
    },
        "Persisted CSP policy has an empty self origin"sv);

    expect_load_error([&](auto& database, auto&) {
        insert_minimal_policy_container(database, 1, 0);
        insert_minimal_csp_policy(database, 1, 1, 0);
        insert_minimal_csp_policy(database, 2, 1, 2);
    },
        "Session history CSP policies are not contiguously ordered"sv);

    expect_load_error([&](auto& database, auto& statements) {
        insert_minimal_policy_container(database, 1, 0);
        insert_minimal_csp_policy(database, 1, 1, 0);
        insert_directive(database, statements, 1, 1, 0);
        insert_directive(database, statements, 2, 1, 2);
    },
        "Session history CSP directives are not contiguously ordered"sv);

    expect_load_error([&](auto& database, auto& statements) {
        insert_minimal_policy_container(database, 1, 0);
        insert_minimal_csp_policy(database, 1, 1, 0);
        insert_directive(database, statements, 1, 1, 0);
        insert_value(database, statements, 1, 0);
        insert_value(database, statements, 1, 2);
    },
        "Session history CSP directive values are not contiguously ordered"sv);
}

TEST_CASE(snapshot_load_rejects_policy_container_for_missing_entry)
{
    // Foreign keys are deliberately unenforced here so the dangling container row is insertable.
    auto database = MUST(Database::Database::create_memory_backed());
    auto statements = prepare_snapshot_tables(*database);

    insert_history_row(*database, 1, 1);
    database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), static_cast<i64>(0), static_cast<i64>(0));
    insert_minimal_entry(*database, statements, 1, 0);
    insert_minimal_policy_container(*database, 1, 5);

    auto loaded = load_session_history_snapshot(*database, statements, 1, 0);
    EXPECT(loaded.is_error());
    EXPECT_EQ(loaded.error().string_literal(), "Session history policy container references a missing entry"sv);
}

TEST_CASE(snapshot_round_trips_cross_process_ids_past_the_signed_range)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 31);

    constexpr u64 just_past_i64_max = static_cast<u64>(NumericLimits<i64>::max()) + 1;
    constexpr u64 all_ones = NumericLimits<u64>::max();

    Web::HTML::SessionHistoryNestedHistoryDescriptor frame;
    frame.id = { all_ones, just_past_i64_max };
    frame.entries.append(make_entry(0, "https://frame.example/"sv, 6, 0x61, 0x62, ""sv, ""sv, {}));

    auto root_entry = make_entry(0, "https://a.example/"sv, 1, 0x11, 0x12, ""sv, ""sv, {});
    root_entry.document_state.id = { just_past_i64_max, all_ones };
    root_entry.document_state.nested_histories.append(move(frame));

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(root_entry));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    TRY_OR_FAIL(store_session_history_snapshot(*database, statements, 31, snapshot));

    // The id columns hold the signed bit pattern, so a component past i64's range lands as a negative cell.
    EXPECT_EQ(select_i64(*database, "SELECT document_state_namespace_id FROM SessionEntries WHERE url = 'https://a.example/';"sv), NumericLimits<i64>::min());
    EXPECT_EQ(select_i64(*database, "SELECT document_state_local_id FROM SessionEntries WHERE url = 'https://a.example/';"sv), -1);
    EXPECT_EQ(select_i64(*database, "SELECT nested_history_namespace_id FROM SessionNestedHistories;"sv), -1);
    EXPECT_EQ(select_i64(*database, "SELECT nested_history_local_id FROM SessionNestedHistories;"sv), NumericLimits<i64>::min());

    auto loaded = TRY_OR_FAIL(load_session_history_snapshot(*database, statements, 31, 0));

    EXPECT_EQ(loaded.entries.size(), 1uz);
    EXPECT_EQ(loaded.entries[0].document_state.id, (Web::HTML::CrossProcessId { just_past_i64_max, all_ones }));

    auto const& nested = loaded.entries[0].document_state.nested_histories;
    EXPECT_EQ(nested.size(), 1uz);
    EXPECT_EQ(nested[0].id, (Web::HTML::CrossProcessId { all_ones, just_past_i64_max }));
    EXPECT_EQ(nested[0].entries.size(), 1uz);
}

TEST_CASE(snapshot_round_trips_nested_histories)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 30);

    Web::HTML::SessionHistoryNestedHistoryDescriptor inner_frame;
    inner_frame.id = frame_id(3);
    inner_frame.entries.append(make_entry(1, "https://inner.example/"sv, 6, 0x61, 0x62, ""sv, ""sv, {}));

    auto middle_entry = make_entry(0, "https://frame.example/"sv, 4, 0x41, 0x42, ""sv, ""sv, {});
    middle_entry.document_state.nested_histories.append(move(inner_frame));

    auto container_entry = make_entry(1, "about:srcdoc"sv, 5, 0x51, 0x52, ""sv, ""sv, {});
    container_entry.document_state.history_policy_container = make_policy_container();

    Web::HTML::SessionHistoryNestedHistoryDescriptor frame_a;
    frame_a.id = frame_id(1);
    frame_a.entries.append(move(middle_entry));
    frame_a.entries.append(move(container_entry));

    Web::HTML::SessionHistoryNestedHistoryDescriptor frame_b;
    frame_b.id = frame_id(2);

    auto root_entry = make_entry(0, "https://a.example/"sv, 1, 0x11, 0x12, ""sv, ""sv, {});
    root_entry.document_state.nested_histories.append(move(frame_a));
    root_entry.document_state.nested_histories.append(move(frame_b));

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(root_entry));
    snapshot.entries.append(make_entry(1, "https://b.example/"sv, 2, 0x21, 0x22, ""sv, ""sv, {}));
    snapshot.used_steps = { 0, 1 };
    snapshot.current_used_step_index = 1;

    TRY_OR_FAIL(store_session_history_snapshot(*database, statements, 30, snapshot));
    auto loaded = TRY_OR_FAIL(load_session_history_snapshot(*database, statements, 30, 1));

    EXPECT_EQ(loaded.entries.size(), 2uz);
    EXPECT(loaded.entries[1].document_state.nested_histories.is_empty());

    auto const& nested = loaded.entries[0].document_state.nested_histories;
    EXPECT_EQ(nested.size(), 2uz);
    EXPECT_EQ(nested[0].id, frame_id(1));
    EXPECT_EQ(nested[1].id, frame_id(2));
    EXPECT(nested[1].entries.is_empty());

    EXPECT_EQ(nested[0].entries.size(), 2uz);
    EXPECT_EQ(nested[0].entries[0].url.serialize(), "https://frame.example/"_string);
    EXPECT_EQ(nested[0].entries[0].classic_history_api_state.data.bytes()[0], 0x41);

    auto const& inner = nested[0].entries[0].document_state.nested_histories;
    EXPECT_EQ(inner.size(), 1uz);
    EXPECT_EQ(inner[0].id, frame_id(3));
    EXPECT_EQ(inner[0].entries.size(), 1uz);
    EXPECT_EQ(inner[0].entries[0].url.serialize(), "https://inner.example/"_string);
    EXPECT_EQ(inner[0].entries[0].step, 1);

    auto const* container = nested[0].entries[1].document_state.history_policy_container.get_pointer<Web::HTML::SerializedPolicyContainer>();
    EXPECT(container != nullptr);
    if (container)
        expect_policy_containers_equal(*container, make_policy_container());
}

TEST_CASE(snapshot_store_rejects_over_deep_nesting)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    auto entry = make_entry(0, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {});
    for (int i = 0; i < 17; ++i) {
        auto parent = make_entry(0, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {});
        Web::HTML::SessionHistoryNestedHistoryDescriptor nested;
        nested.id = frame_id(1);
        nested.entries.append(move(entry));
        parent.document_state.nested_histories.append(move(nested));
        entry = move(parent);
    }

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(entry));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    auto result = store_session_history_snapshot(*database, statements, 31, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session history nests deeper than the supported budget"sv);
    EXPECT_EQ(count_rows(*database, "SessionHistories"sv), 0);
    EXPECT_EQ(count_tab_rows(*database, "SessionUsedSteps"sv, 31), 0);
}

TEST_CASE(snapshot_load_rejects_invalid_nested_history_rows)
{
    // Two histories where each is the other's child: no history is left rootless.
    expect_load_error([](auto& database, auto& statements) {
        insert_history_row(database, 2, 1);
        insert_minimal_entry(database, statements, 2, 0);
        insert_nested_history_link(database, statements, 1, 0, 0, 2);
        insert_nested_history_link(database, statements, 2, 0, 0, 1);
    },
        "Session history snapshot lacks a unique root history"sv);

    // A second linkless history is a second root.
    expect_load_error([](auto& database, auto&) {
        insert_history_row(database, 2, 1);
    },
        "Session history snapshot lacks a unique root history"sv);

    // A cycle pair beside the root is unreachable from it.
    expect_load_error([](auto& database, auto& statements) {
        insert_history_row(database, 2, 1);
        insert_history_row(database, 3, 1);
        insert_minimal_entry(database, statements, 2, 0);
        insert_minimal_entry(database, statements, 3, 0);
        insert_nested_history_link(database, statements, 2, 0, 0, 3);
        insert_nested_history_link(database, statements, 3, 0, 0, 2);
    },
        "Session history has an unreachable nested history"sv);

    // A link whose parent history belongs to another tab.
    expect_load_error([](auto& database, auto& statements) {
        insert_tab_row(database, 2);
        insert_history_row(database, 2, 2);
        insert_minimal_entry(database, statements, 2, 0);
        insert_history_row(database, 3, 1);
        insert_nested_history_link(database, statements, 2, 0, 0, 3);
    },
        "Session history nested history link references another tab"sv);

    // A link whose child history belongs to another tab.
    expect_load_error([](auto& database, auto& statements) {
        insert_tab_row(database, 2);
        insert_history_row(database, 2, 2);
        insert_nested_history_link(database, statements, 1, 0, 0, 2);
    },
        "Session history nested history link references another tab"sv);

    // Nested ordinals with a gap under one parent entry.
    expect_load_error([](auto& database, auto& statements) {
        insert_history_row(database, 2, 1);
        insert_history_row(database, 3, 1);
        insert_nested_history_link(database, statements, 1, 0, 0, 2);
        insert_nested_history_link(database, statements, 1, 0, 2, 3);
    },
        "Session history nested histories are not contiguously ordered"sv);

    // Entry ordinals with a gap within a nested history.
    expect_load_error([](auto& database, auto& statements) {
        insert_history_row(database, 2, 1);
        insert_minimal_entry(database, statements, 2, 0);
        insert_minimal_entry(database, statements, 2, 2);
        insert_nested_history_link(database, statements, 1, 0, 0, 2);
    },
        "Session history entries are not contiguously ordered"sv);

    // A chain of nested histories past the depth budget.
    expect_load_error([](auto& database, auto& statements) {
        for (i64 history_id = 2; history_id <= 18; ++history_id) {
            insert_history_row(database, history_id, 1);
            insert_minimal_entry(database, statements, history_id, 0);
        }
        for (i64 history_id = 1; history_id < 18; ++history_id)
            insert_nested_history_link(database, statements, history_id, 0, 0, history_id + 1);
    },
        "Session history nests deeper than the supported budget"sv);
}

TEST_CASE(snapshot_load_rejects_nested_history_for_missing_parent_entry)
{
    // Foreign keys are deliberately unenforced here so the dangling link row is insertable.
    auto database = MUST(Database::Database::create_memory_backed());
    auto statements = prepare_snapshot_tables(*database);

    insert_history_row(*database, 1, 1);
    insert_history_row(*database, 2, 1);
    database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), static_cast<i64>(0), static_cast<i64>(0));
    insert_minimal_entry(*database, statements, 1, 0);
    insert_nested_history_link(*database, statements, 1, 5, 0, 2);

    auto loaded = load_session_history_snapshot(*database, statements, 1, 0);
    EXPECT(loaded.is_error());
    EXPECT_EQ(loaded.error().string_literal(), "Session history nested history references a missing parent entry"sv);
}

TEST_CASE(snapshot_load_rejects_a_negative_nested_history_ordinal)
{
    auto database = MUST(Database::Database::create_memory_backed());
    auto statements = prepare_snapshot_tables(*database);

    insert_history_row(*database, 1, 1);
    insert_history_row(*database, 2, 1);
    database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), static_cast<i64>(0), static_cast<i64>(0));
    insert_minimal_entry(*database, statements, 1, 0);
    insert_nested_history_link(*database, statements, 1, -1, 0, 2);

    // An ordinal indexes a vector, so the typed read rejects a negative cell rather than letting it wrap.
    auto loaded = load_session_history_snapshot(*database, statements, 1, 0);
    EXPECT(loaded.is_error());
    EXPECT_EQ(loaded.error().string_literal(), "Column value is out of range"sv);
}

TEST_CASE(snapshot_store_rejects_unreachable_used_step)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    // Used step 1 has no top-level entry, so neither restore nor storage may accept the snapshot.
    Web::HTML::SessionHistoryNestedHistoryDescriptor frame;
    frame.id = frame_id(1);
    frame.entries.append(make_entry(1, "https://nested.example/"sv, 2, 0, 0, ""sv, ""sv, {}));

    auto root_entry = make_entry(5, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {});
    root_entry.document_state.nested_histories.append(move(frame));

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(root_entry));
    snapshot.used_steps = { 1, 5 };
    snapshot.current_used_step_index = 1;

    auto result = store_session_history_snapshot(*database, statements, 15, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session history snapshot has a used step that is not reachable"sv);
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 15), 0);
    EXPECT_EQ(count_tab_rows(*database, "SessionUsedSteps"sv, 15), 0);
}

TEST_CASE(snapshot_store_survives_a_maximum_history_row_id)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 1);
    insert_history_row(*database, 9223372036854775807, 1);

    insert_tab_row(*database, 2);
    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(make_entry(0, "https://a.example/"sv, 1, 0x11, 0x12, ""sv, ""sv, {}));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    TRY_OR_FAIL(store_session_history_snapshot(*database, statements, 2, snapshot));
    EXPECT_EQ(count_tab_rows(*database, "SessionHistories"sv, 2), 1);
}

TEST_CASE(snapshot_store_rejects_over_budget_entries)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    WebView::SessionHistorySnapshot snapshot;
    for (i32 step = 0; step < 8193; ++step)
        snapshot.entries.append(make_entry(step, "https://a.example/"sv, static_cast<u64>(step) + 1, 0, 0, ""sv, ""sv, {}));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    auto result = store_session_history_snapshot(*database, statements, 16, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session history snapshot has more entries than the supported budget"sv);
    EXPECT_EQ(count_history_owned_rows(*database, "SessionEntries"sv, 16), 0);
}

TEST_CASE(snapshot_store_rejects_over_budget_histories)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    auto root_entry = make_entry(0, "https://a.example/"sv, 1, 0, 0, ""sv, ""sv, {});
    for (u64 i = 0; i < 4096; ++i) {
        Web::HTML::SessionHistoryNestedHistoryDescriptor frame;
        frame.id = frame_id(i + 1);
        root_entry.document_state.nested_histories.append(move(frame));
    }

    WebView::SessionHistorySnapshot snapshot;
    snapshot.entries.append(move(root_entry));
    snapshot.used_steps = { 0 };
    snapshot.current_used_step_index = 0;

    auto result = store_session_history_snapshot(*database, statements, 17, snapshot);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Session history snapshot has more histories than the supported budget"sv);
    EXPECT_EQ(count_tab_rows(*database, "SessionHistories"sv, 17), 0);
}

TEST_CASE(snapshot_load_rejects_an_over_budget_used_step_flood)
{
    auto database = make_database();
    auto statements = prepare_snapshot_tables(*database);

    insert_tab_row(*database, 1);
    insert_history_row(*database, 1, 1);
    insert_minimal_entry(*database, statements, 1, 0);
    for (i64 i = 0; i < 8193; ++i)
        database->execute_statement(statements.insert_used_step, {}, static_cast<i64>(1), i, i);

    auto loaded = load_session_history_snapshot(*database, statements, 1, 0);
    EXPECT(loaded.is_error());
    EXPECT_EQ(loaded.error().string_literal(), "Statement returned more rows than the caller allowed"sv);
}
