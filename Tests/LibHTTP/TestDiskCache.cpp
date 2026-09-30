/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCore/ImmutableBytes.h>
#include <LibCore/StandardPaths.h>
#include <LibHTTP/Cache/CacheRequest.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/HeaderList.h>
#include <LibHTTP/NetworkIsolationKey.h>
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

static LexicalPath test_cache_root()
{
    return LexicalPath::join(Core::StandardPaths::cache_directory(), "Ladybird"sv);
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

static Utf16String partition_for(HTTP::NetworkIsolationKey const& key)
{
    return key.disk_cache_partition().release_value();
}

static Utf16String test_partition()
{
    return partition_for({
        .top_level_site = "https://example.com"_utf16,
        .frame_site = "https://example.com"_utf16,
    });
}

static HTTP::CacheEntryWriter& create_cache_entry(HTTP::DiskCache& disk_cache, TestCacheRequest& request, URL::URL const& url, HTTP::HeaderList const& request_headers, Utf16String const& partition = test_partition())
{
    Optional<HTTP::CacheEntryWriter&> writer;

    disk_cache.create_entry(request, partition, url, "GET"sv, request_headers, UnixDateTime::now())
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
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://example.com/script.js"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("console.log('hello');"sv.bytes()));
    TRY_OR_FAIL(writer.flush(request_headers, response_headers));

    auto bytecode = TRY_OR_FAIL(ByteBuffer::copy("bytecode"sv.bytes()));
    EXPECT(TRY_OR_FAIL(disk_cache.store_associated_data(test_partition(), url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));

    auto retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(test_partition(), url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    VERIFY(retrieved_bytecode.has_value());
    EXPECT_EQ(retrieved_bytecode->bytes(), bytecode.bytes());

    auto retrieved_bytecode_file = TRY_OR_FAIL(disk_cache.retrieve_associated_data_file(test_partition(), url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    VERIFY(retrieved_bytecode_file.has_value());
    auto mapped_bytecode = TRY_OR_FAIL(Core::ImmutableBytes::map_from_fd_range_and_close(retrieved_bytecode_file->fd, "bytecode"sv, retrieved_bytecode_file->offset, retrieved_bytecode_file->size));
    EXPECT_EQ(mapped_bytecode.bytes(), bytecode.bytes());

    disk_cache.remove_entries_accessed_since(UnixDateTime::earliest());

    retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(test_partition(), url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    EXPECT(!retrieved_bytecode.has_value());
}

TEST_CASE(replacing_cache_entry_removes_associated_data)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://example.com/script.js"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("console.log('old');"sv.bytes()));
    TRY_OR_FAIL(writer.flush(request_headers, response_headers));

    auto bytecode = TRY_OR_FAIL(ByteBuffer::copy("bytecode"sv.bytes()));
    EXPECT(TRY_OR_FAIL(disk_cache.store_associated_data(test_partition(), url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));

    auto retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(test_partition(), url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    VERIFY(retrieved_bytecode.has_value());

    auto replacement_request_headers = create_cacheable_request_headers();
    auto replacement_response_headers = create_cacheable_response_headers();
    auto& replacement_writer = create_cache_entry(disk_cache, request, url, *replacement_request_headers);
    TRY_OR_FAIL(replacement_writer.write_status_and_reason(200, "OK"_string, *replacement_request_headers, *replacement_response_headers));
    TRY_OR_FAIL(replacement_writer.write_data("console.log('new');"sv.bytes()));
    TRY_OR_FAIL(replacement_writer.flush(replacement_request_headers, replacement_response_headers));

    retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(test_partition(), url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    EXPECT(!retrieved_bytecode.has_value());
}

TEST_CASE(flush_returns_mappable_body_file)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://example.com/script.js"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("console.log('hello');"sv.bytes()));

    auto body_file = TRY_OR_FAIL(writer.flush_and_take_body_file(request_headers, response_headers));
    auto body = TRY_OR_FAIL(Core::ImmutableBytes::map_from_fd_range_and_close(body_file.fd, "cache body"sv, body_file.offset, body_file.size));
    EXPECT_EQ(body.bytes(), "console.log('hello');"sv.bytes());
}

TEST_CASE(replacing_cache_entry_keeps_existing_body_mapping_stable)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://example.com/script.js"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("console.log('old');"sv.bytes()));

    auto old_body_file = TRY_OR_FAIL(writer.flush_and_take_body_file(request_headers, response_headers));
    auto old_body = TRY_OR_FAIL(Core::ImmutableBytes::map_from_fd_range_and_close(old_body_file.fd, "old cache body"sv, old_body_file.offset, old_body_file.size));
    EXPECT_EQ(old_body.bytes(), "console.log('old');"sv.bytes());

    auto replacement_request_headers = create_cacheable_request_headers();
    auto replacement_response_headers = create_cacheable_response_headers();
    auto& replacement_writer = create_cache_entry(disk_cache, request, url, *replacement_request_headers);
    TRY_OR_FAIL(replacement_writer.write_status_and_reason(200, "OK"_string, *replacement_request_headers, *replacement_response_headers));
    TRY_OR_FAIL(replacement_writer.write_data("console.log('new');"sv.bytes()));
    TRY_OR_FAIL(replacement_writer.flush(replacement_request_headers, replacement_response_headers));

    EXPECT_EQ(old_body.bytes(), "console.log('old');"sv.bytes());
}

TEST_CASE(associated_data_round_trips_with_explicit_vary_key)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
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
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers).value();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("console.log('hello');"sv.bytes()));
    TRY_OR_FAIL(writer.flush(request_headers, response_headers));

    auto bytecode = TRY_OR_FAIL(ByteBuffer::copy("bytecode"sv.bytes()));
    EXPECT(!TRY_OR_FAIL(disk_cache.store_associated_data(test_partition(), url, "GET"sv, *mismatched_request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));
    EXPECT(TRY_OR_FAIL(disk_cache.store_associated_data(test_partition(), url, "GET"sv, *mismatched_request_headers, vary_key, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));

    auto retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(test_partition(), url, "GET"sv, *mismatched_request_headers, vary_key, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    VERIFY(retrieved_bytecode.has_value());
    EXPECT_EQ(retrieved_bytecode->bytes(), bytecode.bytes());
}

TEST_CASE(associated_data_participates_in_cache_eviction)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
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
    EXPECT(!TRY_OR_FAIL(disk_cache.store_associated_data(test_partition(), url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));

    auto retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(test_partition(), url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    EXPECT(!retrieved_bytecode.has_value());
}

TEST_CASE(cache_partitions_do_not_share_entries)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://cdn.example.net/library.js"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto partition = test_partition();
    auto other_top_level_site = partition_for({
        .top_level_site = "https://other.example"_utf16,
        .frame_site = "https://example.com"_utf16,
    });
    auto other_frame_site = partition_for({
        .top_level_site = "https://example.com"_utf16,
        .frame_site = "https://other.example"_utf16,
    });
    auto subframe_document = partition_for({
        .top_level_site = "https://example.com"_utf16,
        .frame_site = "https://example.com"_utf16,
        .is_subframe_document = true,
    });
    auto cross_site_main_frame_navigation = partition_for({
        .top_level_site = "https://example.com"_utf16,
        .frame_site = "https://example.com"_utf16,
        .is_cross_site_main_frame_navigation = true,
    });

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers, partition);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("console.log('hello');"sv.bytes()));
    TRY_OR_FAIL(writer.flush(request_headers, response_headers));

    auto bytecode = TRY_OR_FAIL(ByteBuffer::copy("bytecode"sv.bytes()));
    for (auto const& other_partition : { other_top_level_site, other_frame_site, subframe_document, cross_site_main_frame_navigation }) {
        EXPECT(!TRY_OR_FAIL(disk_cache.store_associated_data(other_partition, url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));

        disk_cache.open_entry(request, other_partition, url, "GET"sv, *request_headers, HTTP::CacheMode::Default, HTTP::DiskCache::OpenMode::Read)
            .visit(
                [&](Optional<HTTP::CacheEntryReader&> reader) {
                    EXPECT(!reader.has_value());
                },
                [](HTTP::DiskCache::CacheHasOpenEntry) {});
    }

    EXPECT(TRY_OR_FAIL(disk_cache.store_associated_data(partition, url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));
    for (auto const& other_partition : { other_top_level_site, other_frame_site, subframe_document, cross_site_main_frame_navigation })
        EXPECT(!TRY_OR_FAIL(disk_cache.retrieve_associated_data(other_partition, url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode)).has_value());

    auto retrieved_bytecode = TRY_OR_FAIL(disk_cache.retrieve_associated_data(partition, url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
    VERIFY(retrieved_bytecode.has_value());
    EXPECT_EQ(retrieved_bytecode->bytes(), bytecode.bytes());

    disk_cache.remove_entries_accessed_since(UnixDateTime::earliest());
}

static Optional<HTTP::CacheEntryReader&> open_stale_cache_entry(HTTP::DiskCache& disk_cache, TestCacheRequest& request, URL::URL const& url, StringView cache_control, StringView age_in_seconds, HTTP::CacheMode cache_mode)
{
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, cache_control },
        { "ETag"sv, "\"v1\""sv },
    });

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    MUST(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    MUST(writer.write_data("data"sv.bytes()));
    MUST(writer.flush(request_headers, response_headers));

    auto stale_request_headers = HTTP::HeaderList::create({
        { HTTP::TEST_CACHE_ENABLED_HEADER, "1"sv },
        { HTTP::TEST_CACHE_REQUEST_TIME_OFFSET, age_in_seconds },
    });

    Optional<HTTP::CacheEntryReader&> reader;
    disk_cache.open_entry(request, test_partition(), url, "GET"sv, *stale_request_headers, cache_mode, HTTP::DiskCache::OpenMode::Read)
        .visit(
            [&](Optional<HTTP::CacheEntryReader&> cache_entry_reader) {
                reader = cache_entry_reader;
            },
            [](HTTP::DiskCache::CacheHasOpenEntry) {
                FAIL("Cache entry was unexpectedly open");
            });

    return reader;
}

TEST_CASE(must_revalidate_overrides_stale_while_revalidate)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto reader = open_stale_cache_entry(disk_cache, request, parse_url("https://example.com/must-revalidate-and-swr"sv), "max-age=60, stale-while-revalidate=30, must-revalidate"sv, "70"sv, HTTP::CacheMode::Default);
    VERIFY(reader.has_value());
    EXPECT_EQ(reader->revalidation_type(), HTTP::CacheEntryReader::RevalidationType::MustRevalidate);
}

TEST_CASE(force_cache_reuses_stale_must_revalidate_entries)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto reader = open_stale_cache_entry(disk_cache, request, parse_url("https://example.com/force-cache"sv), "max-age=60, must-revalidate"sv, "120"sv, HTTP::CacheMode::ForceCache);
    VERIFY(reader.has_value());
    EXPECT_EQ(reader->revalidation_type(), HTTP::CacheEntryReader::RevalidationType::None);

    reader = open_stale_cache_entry(disk_cache, request, parse_url("https://example.com/only-if-cached"sv), "max-age=60, must-revalidate"sv, "120"sv, HTTP::CacheMode::OnlyIfCached);
    VERIFY(reader.has_value());
    EXPECT_EQ(reader->revalidation_type(), HTTP::CacheEntryReader::RevalidationType::None);
}
