/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <LibTest/TestCase.h>
#include <LibURL/Origin.h>
#include <LibWebView/SessionHistorySnapshotStorage.h>

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
