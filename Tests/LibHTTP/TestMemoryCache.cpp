/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ImmutableBytes.h>
#include <LibHTTP/Cache/MemoryCache.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/HeaderList.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>

static URL::URL parse_url(StringView url)
{
    return URL::Parser::basic_parse(url).release_value();
}

static NonnullRefPtr<HTTP::HeaderList> create_cacheable_request_headers()
{
    return HTTP::HeaderList::create({
        { "Accept"sv, "*/*"sv },
    });
}

static NonnullRefPtr<HTTP::HeaderList> create_cacheable_response_headers()
{
    return HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
    });
}

static NonnullRefPtr<HTTP::HeaderList> create_vary_user_agent_response_headers()
{
    return HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "Vary"sv, "User-Agent"sv },
    });
}

static Core::ImmutableBytes immutable_bytes(StringView bytes)
{
    return MUST(Core::ImmutableBytes::copy(bytes.bytes()));
}

TEST_CASE(permanent_redirects_without_explicit_freshness_are_reused)
{
    for (auto status_code : Array<u32, 2> { 301, 308 }) {
        auto cache = HTTP::MemoryCache::create();
        auto url = parse_url(MUST(String::formatted("https://example.com/redirect/{}", status_code)));
        auto request_headers = create_cacheable_request_headers();
        auto response_headers = HTTP::HeaderList::create({ { "Location"sv, "https://example.com/target"sv } });

        cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), status_code, "Permanent Redirect"sv, *response_headers);
        cache->finalize_entry(url, "GET"sv, *request_headers, status_code, *response_headers, immutable_bytes({}));

        auto entry = cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default);
        VERIFY(entry.has_value());
        EXPECT_EQ(entry->status_code, status_code);
    }
}

TEST_CASE(newer_response_replaces_entry_with_same_vary_key)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/script.js"sv);
    auto request_headers = create_cacheable_request_headers();
    auto old_response_headers = HTTP::HeaderList::create({ { "Cache-Control"sv, "max-age=60"sv }, { "Vary"sv, "Accept, User-Agent"sv } });
    auto new_response_headers = HTTP::HeaderList::create({ { "Cache-Control"sv, "max-age=60"sv }, { "Vary"sv, "User-Agent, Accept"sv } });

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *old_response_headers);
    cache->finalize_entry(url, "GET"sv, *request_headers, 200, *old_response_headers, immutable_bytes("old"sv));

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *new_response_headers);
    cache->finalize_entry(url, "GET"sv, *request_headers, 200, *new_response_headers, immutable_bytes("new"sv));

    auto entry = cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default);
    VERIFY(entry.has_value());
    EXPECT_EQ(entry->response_body.bytes(), "new"sv.bytes());
}

TEST_CASE(not_modified_responses_are_not_stored)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = HTTP::HeaderList::create({ { "If-None-Match"sv, "\"v1\""sv } });
    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "ETag"sv, "\"v1\""sv },
    });

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 304, "Not Modified"sv, *response_headers);
    cache->finalize_entry(url, "GET"sv, *request_headers, 304, *response_headers, immutable_bytes({}));

    EXPECT(!cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default).has_value());
}

TEST_CASE(a_request_with_preconditions_is_not_answered_from_a_stored_response)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "ETag"sv, "\"v1\""sv },
    });

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->finalize_entry(url, "GET"sv, *request_headers, 200, *response_headers, immutable_bytes("v1"sv));
    VERIFY(cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default).has_value());

    for (auto name : { "If-None-Match"sv, "If-Modified-Since"sv, "If-Match"sv, "If-Unmodified-Since"sv, "If-Range"sv }) {
        auto conditional_request_headers = HTTP::HeaderList::create({ { "Accept"sv, "*/*"sv }, { name, "\"v1\""sv } });
        EXPECT(!cache->open_entry(url, "GET"sv, *conditional_request_headers, HTTP::CacheMode::Default).has_value());
    }
}

TEST_CASE(invalidation_removes_stored_and_pending_responses)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->finalize_entry(url, "GET"sv, *request_headers, 200, *response_headers, immutable_bytes("old"sv));
    cache->create_entry(url, "HEAD"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);

    cache->invalidate(url);
    cache->finalize_entry(url, "HEAD"sv, *request_headers, 200, *response_headers, immutable_bytes({}));

    EXPECT(!cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default).has_value());
    EXPECT(!cache->open_entry(url, "HEAD"sv, *request_headers, HTTP::CacheMode::Default).has_value());
}

TEST_CASE(abandoned_response_is_not_stored)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->abandon_entry(url, "GET"sv, *request_headers, *response_headers, HTTP::MemoryCache::StoredResponse::Keep);
    cache->finalize_entry(url, "GET"sv, *request_headers, 200, *response_headers, immutable_bytes("partial"sv));

    EXPECT(!cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default).has_value());
}

TEST_CASE(abandoning_a_response_leaves_other_pending_responses)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->abandon_entry(url, "GET"sv, *request_headers, *response_headers, HTTP::MemoryCache::StoredResponse::Keep);
    cache->finalize_entry(url, "GET"sv, *request_headers, 200, *response_headers, immutable_bytes("complete"sv));

    auto entry = cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default);
    VERIFY(entry.has_value());
    EXPECT_EQ(entry->response_body.bytes(), "complete"sv.bytes());
}

TEST_CASE(abandoning_a_response_matches_its_vary_headers)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/resource"sv);
    auto abandoned_request_headers = HTTP::HeaderList::create({
        { "Accept"sv, "*/*"sv },
        { "User-Agent"sv, "Abandoned"sv },
    });
    auto completed_request_headers = HTTP::HeaderList::create({
        { "Accept"sv, "*/*"sv },
        { "User-Agent"sv, "Completed"sv },
    });
    auto response_headers = create_vary_user_agent_response_headers();

    cache->create_entry(url, "GET"sv, *abandoned_request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->create_entry(url, "GET"sv, *completed_request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->abandon_entry(url, "GET"sv, *abandoned_request_headers, *response_headers, HTTP::MemoryCache::StoredResponse::Keep);
    cache->finalize_entry(url, "GET"sv, *abandoned_request_headers, 200, *response_headers, immutable_bytes("partial"sv));
    cache->finalize_entry(url, "GET"sv, *completed_request_headers, 200, *response_headers, immutable_bytes("complete"sv));

    EXPECT(!cache->open_entry(url, "GET"sv, *abandoned_request_headers, HTTP::CacheMode::Default).has_value());
    EXPECT(cache->open_entry(url, "GET"sv, *completed_request_headers, HTTP::CacheMode::Default).has_value());
}

TEST_CASE(non_storable_response_retires_stored_response)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/data"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();
    auto no_store_response_headers = HTTP::HeaderList::create({ { "Cache-Control"sv, "no-store"sv } });

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->finalize_entry(url, "GET"sv, *request_headers, 200, *response_headers, immutable_bytes("old"sv));
    EXPECT(cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default).has_value());

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *no_store_response_headers);
    cache->finalize_entry(url, "GET"sv, *request_headers, 200, *no_store_response_headers, immutable_bytes("current"sv));

    EXPECT(!cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default).has_value());
}

TEST_CASE(responses_varying_on_cookie_are_not_reused)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/account"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "Vary"sv, "Cookie"sv },
    });

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->finalize_entry(url, "GET"sv, *request_headers, 200, *response_headers, immutable_bytes("private"sv));

    EXPECT(!cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default).has_value());
}

TEST_CASE(no_store_permanent_redirects_are_not_reused)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/redirect"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "no-store"sv },
        { "Location"sv, "https://example.com/target"sv },
    });

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 301, "Moved Permanently"sv, *response_headers);
    cache->finalize_entry(url, "GET"sv, *request_headers, 301, *response_headers, immutable_bytes({}));

    EXPECT(!cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default).has_value());
}

TEST_CASE(force_cache_reuses_stale_must_revalidate_entries)
{
    auto cache = HTTP::MemoryCache::create();
    auto url = parse_url("https://example.com/must-revalidate"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = HTTP::HeaderList::create({
        { "Age"sv, "120"sv },
        { "Cache-Control"sv, "max-age=60, must-revalidate"sv },
        { "ETag"sv, "\"v1\""sv },
    });

    cache->create_entry(url, "GET"sv, *request_headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->finalize_entry(url, "GET"sv, *request_headers, 200, *response_headers, immutable_bytes("data"sv));

    EXPECT(cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::ForceCache).has_value());
    EXPECT(cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::OnlyIfCached).has_value());
    EXPECT(!cache->open_entry(url, "GET"sv, *request_headers, HTTP::CacheMode::Default).has_value());
}

// Legacy memory SHA-1 prefix: 5955c546f462acaa, for URL + GET.
TEST_CASE(legacy_memory_hash_collision_does_not_alias_responses)
{
    auto cache = HTTP::MemoryCache::create();
    auto a = parse_url("https://b.example/a3e7f95e82b770fa"sv);
    auto b = parse_url("https://a.example/59179468b3084f39"sv);
    auto headers = create_cacheable_request_headers();
    auto response_a = create_cacheable_response_headers();
    response_a->append({ "X-Identity"sv, "A"sv });
    auto response_b = create_cacheable_response_headers();
    response_b->append({ "X-Identity"sv, "B"sv });
    cache->create_entry(a, "GET"sv, *headers, UnixDateTime::now(), 200, "OK"sv, *response_a);
    cache->finalize_entry(a, "GET"sv, *headers, 200, *response_a, immutable_bytes("response A"sv));
    EXPECT(!cache->open_entry(b, "GET"sv, *headers, HTTP::CacheMode::Default).has_value());
    cache->create_entry(b, "GET"sv, *headers, UnixDateTime::now(), 200, "OK"sv, *response_b);
    cache->finalize_entry(b, "GET"sv, *headers, 200, *response_b, immutable_bytes("response B"sv));
    for (auto which : { 0, 1 }) {
        auto entry = cache->open_entry(which == 0 ? a : b, "GET"sv, *headers, HTTP::CacheMode::Default);
        VERIFY(entry.has_value());
        EXPECT_EQ(entry->response_body.bytes(), (which == 0 ? "response A"sv : "response B"sv).bytes());
        EXPECT_EQ(entry->response_headers->get("X-Identity"sv), Optional<ByteString> { which == 0 ? "A" : "B" });
    }
    cache->invalidate(a);
    EXPECT(!cache->open_entry(a, "GET"sv, *headers, HTTP::CacheMode::Default).has_value());
    EXPECT(cache->open_entry(b, "GET"sv, *headers, HTTP::CacheMode::Default).has_value());
}

TEST_CASE(colliding_resources_keep_pending_responses_and_eviction_independent)
{
    auto usage = adopt_ref(*new HTTP::MemoryCacheUsage);
    auto cache = HTTP::MemoryCache::create(usage);
    auto a = parse_url("https://b.example/a3e7f95e82b770fa"sv);
    auto b = parse_url("https://a.example/59179468b3084f39"sv);
    auto headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();
    cache->create_entry(a, "GET"sv, *headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->create_entry(b, "GET"sv, *headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->finalize_entry(b, "GET"sv, *headers, 200, *response_headers, immutable_bytes("B"sv));
    cache->abandon_entry(a, "GET"sv, *headers, *response_headers, HTTP::MemoryCache::StoredResponse::Retire);
    cache->finalize_entry(a, "GET"sv, *headers, 200, *response_headers, immutable_bytes("abandoned"sv));
    EXPECT(!cache->open_entry(a, "GET"sv, *headers, HTTP::CacheMode::Default).has_value());
    auto b_entry = cache->open_entry(b, "GET"sv, *headers, HTTP::CacheMode::Default);
    VERIFY(b_entry.has_value());
    EXPECT_EQ(b_entry->response_body.bytes(), "B"sv.bytes());
    cache->create_entry(a, "GET"sv, *headers, UnixDateTime::now(), 200, "OK"sv, *response_headers);
    cache->finalize_entry(a, "GET"sv, *headers, 200, *response_headers, immutable_bytes("A"sv));
    for (auto const& use : cache->complete_entry_uses()) {
        if (use.cache_key.url == HTTP::serialize_url_for_cache_storage(b))
            cache->evict_complete_entry(use.cache_key, use.vary_key);
    }
    EXPECT(cache->open_entry(a, "GET"sv, *headers, HTTP::CacheMode::Default).has_value());
    EXPECT(!cache->open_entry(b, "GET"sv, *headers, HTTP::CacheMode::Default).has_value());
    cache->invalidate(a);
    EXPECT_EQ(usage->bytes_in_memory, 0u);
    EXPECT_EQ(usage->mapped_bodies, 0u);
}
