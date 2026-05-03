/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibHTTP/Cache/CacheRequest.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/HeaderList.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>

struct TestCacheRequest final : public HTTP::CacheRequest {
    virtual bool is_revalidation_request() const override { return false; }
    virtual void notify_request_unblocked(Badge<HTTP::DiskCache>) override { }
};

static URL::URL parse_url(StringView url)
{
    return URL::Parser::basic_parse(url).release_value();
}

static NonnullRefPtr<HTTP::HeaderList> create_cacheable_request_headers()
{
    return HTTP::HeaderList::create({
        { HTTP::TEST_CACHE_ENABLED_HEADER, "1"sv },
    });
}

static NonnullRefPtr<HTTP::HeaderList> create_cacheable_response_headers()
{
    return HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
    });
}

static HTTP::CacheEntryWriter& create_cache_entry(HTTP::DiskCache& disk_cache, TestCacheRequest& request, URL::URL const& url, HTTP::HeaderList const& request_headers)
{
    Optional<HTTP::CacheEntryWriter&> writer;

    disk_cache.create_entry(request, url, "GET"sv, request_headers, UnixDateTime::now())
        .visit(
            [&](Optional<HTTP::CacheEntryWriter&> cache_entry_writer) {
                writer = cache_entry_writer;
            },
            [](HTTP::DiskCache::CacheHasOpenEntry) {
                FAIL("Cache entry was unexpectedly open");
            });

    VERIFY(writer.has_value());
    return *writer;
}

TEST_CASE(associated_data_round_trips_with_cache_entry)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing));
    TestCacheRequest request;

    auto url = parse_url("https://example.com/script.js"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("console.log('hello');"sv.bytes()));
    TRY_OR_FAIL(writer.flush(request_headers, response_headers));

    auto bytecode = TRY_OR_FAIL(ByteBuffer::copy("bytecode"sv.bytes()));
    EXPECT(TRY_OR_FAIL(disk_cache.store_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));

    auto retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    VERIFY(retrieved_bytecode.has_value());
    EXPECT_EQ(retrieved_bytecode->bytes(), bytecode.bytes());

    disk_cache.remove_entries_accessed_since(UnixDateTime::earliest());

    retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    EXPECT(!retrieved_bytecode.has_value());
}

TEST_CASE(replacing_cache_entry_removes_associated_data)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing));
    TestCacheRequest request;

    auto url = parse_url("https://example.com/script.js"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("console.log('old');"sv.bytes()));
    TRY_OR_FAIL(writer.flush(request_headers, response_headers));

    auto bytecode = TRY_OR_FAIL(ByteBuffer::copy("bytecode"sv.bytes()));
    EXPECT(TRY_OR_FAIL(disk_cache.store_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));

    auto retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    VERIFY(retrieved_bytecode.has_value());

    auto replacement_request_headers = create_cacheable_request_headers();
    auto replacement_response_headers = create_cacheable_response_headers();
    auto& replacement_writer = create_cache_entry(disk_cache, request, url, *replacement_request_headers);
    TRY_OR_FAIL(replacement_writer.write_status_and_reason(200, "OK"_string, *replacement_request_headers, *replacement_response_headers));
    TRY_OR_FAIL(replacement_writer.write_data("console.log('new');"sv.bytes()));
    TRY_OR_FAIL(replacement_writer.flush(replacement_request_headers, replacement_response_headers));

    retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    EXPECT(!retrieved_bytecode.has_value());
}

TEST_CASE(associated_data_round_trips_with_explicit_vary_key)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing));
    TestCacheRequest request;

    auto url = parse_url("https://example.com/script.js"sv);
    auto request_headers = HTTP::HeaderList::create({
        { HTTP::TEST_CACHE_ENABLED_HEADER, "1"sv },
        { "Origin"sv, "https://origin.example"sv },
    });
    auto mismatched_request_headers = create_cacheable_request_headers();
    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "Vary"sv, "Origin"sv },
    });
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers);

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("console.log('hello');"sv.bytes()));
    TRY_OR_FAIL(writer.flush(request_headers, response_headers));

    auto bytecode = TRY_OR_FAIL(ByteBuffer::copy("bytecode"sv.bytes()));
    EXPECT(!TRY_OR_FAIL(disk_cache.store_associated_data(url, "GET"sv, *mismatched_request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));
    EXPECT(TRY_OR_FAIL(disk_cache.store_associated_data(url, "GET"sv, *mismatched_request_headers, vary_key, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));

    auto retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(url, "GET"sv, *mismatched_request_headers, vary_key, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    VERIFY(retrieved_bytecode.has_value());
    EXPECT_EQ(retrieved_bytecode->bytes(), bytecode.bytes());
}

TEST_CASE(associated_data_participates_in_cache_eviction)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing));
    TestCacheRequest request;

    auto url = parse_url("https://example.com/script.js"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("console.log('hello');"sv.bytes()));
    TRY_OR_FAIL(writer.flush(request_headers, response_headers));

    disk_cache.set_maximum_disk_cache_size(80);
    auto bytecode = TRY_OR_FAIL(ByteBuffer::create_zeroed(100));
    EXPECT(!TRY_OR_FAIL(disk_cache.store_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));

    auto retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    EXPECT(!retrieved_bytecode.has_value());
}
