/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/ImmutableBytes.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/Cache/CacheRequest.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/HeaderList.h>
#include <LibHTTP/NetworkIsolationKey.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>

struct TestCacheRequest final : public HTTP::CacheRequest {
    virtual bool is_revalidation_request() const override { return false; }
    virtual void notify_request_unblocked(Badge<HTTP::DiskCache>) override { ++unblocked_count; }
    virtual Optional<MonotonicTime> last_activity_time() const override { return activity; }

    size_t unblocked_count { 0 };
    Optional<MonotonicTime> activity;
};

static URL::URL parse_url(StringView url)
{
    return URL::Parser::basic_parse(url).release_value();
}

static LexicalPath test_cache_root()
{
    static Test::TemporaryDirectory const cache_root { "TestDiskCache"sv };
    return cache_root.path();
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

static Optional<HTTP::CacheEntryReader&> open_cache_entry(HTTP::DiskCache& disk_cache, TestCacheRequest& request, URL::URL const& url, HTTP::HeaderList const& request_headers, HTTP::CacheMode cache_mode = HTTP::CacheMode::Default)
{
    Optional<HTTP::CacheEntryReader&> reader;

    disk_cache.open_entry(request, test_partition(), url, "GET"sv, request_headers, cache_mode, HTTP::DiskCache::OpenMode::Read)
        .visit(
            [&](Optional<HTTP::CacheEntryReader&> cache_entry_reader) {
                reader = cache_entry_reader;
            },
            [](HTTP::DiskCache::CacheHasOpenEntry) {
                FAIL("Cache entry was unexpectedly open");
            });

    return reader;
}

static void store_cache_entry(HTTP::DiskCache& disk_cache, TestCacheRequest& request, URL::URL const& url, HTTP::HeaderList const& original_request_headers, NonnullRefPtr<HTTP::HeaderList> response_headers, StringView body)
{
    // Storage strips exempt fields from the supplied headers.
    auto request_headers = HTTP::HeaderList::create(original_request_headers.headers());

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data(body.bytes()));
    TRY_OR_FAIL(writer.flush(request_headers));
}

TEST_CASE(no_cache_request_for_fresh_entry_without_validators_goes_to_the_network)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = create_cacheable_request_headers();
    store_cache_entry(disk_cache, request, url, *request_headers, create_cacheable_response_headers(), "hello"sv);

    EXPECT(!open_cache_entry(disk_cache, request, url, *request_headers, HTTP::CacheMode::NoCache).has_value());

    auto validated_url = parse_url("https://example.com/validated"sv);
    auto validated_response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "ETag"sv, "\"v1\""sv },
    });
    store_cache_entry(disk_cache, request, validated_url, *request_headers, validated_response_headers, "hello"sv);

    auto reader = open_cache_entry(disk_cache, request, validated_url, *request_headers, HTTP::CacheMode::NoCache);
    VERIFY(reader.has_value());
    EXPECT_EQ(reader->revalidation_type(), HTTP::CacheEntryReader::RevalidationType::MustRevalidate);
    reader->revalidation_failed();
}

TEST_CASE(request_with_its_own_preconditions_does_not_revalidate_stored_response)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "ETag"sv, "\"old\""sv },
    });
    store_cache_entry(disk_cache, request, url, *request_headers, response_headers, "old"sv);

    auto conditional_request_headers = HTTP::HeaderList::create({
        { HTTP::TEST_CACHE_ENABLED_HEADER, "1"sv },
        { "If-None-Match"sv, "\"current\""sv },
    });
    EXPECT(!open_cache_entry(disk_cache, request, url, *conditional_request_headers, HTTP::CacheMode::NoCache).has_value());

    auto& writer = create_cache_entry(disk_cache, request, url, *conditional_request_headers);
    auto not_modified_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "ETag"sv, "\"current\""sv },
    });
    EXPECT(writer.write_status_and_reason(304, "Not Modified"_string, *conditional_request_headers, *not_modified_headers).is_error());

    auto reader = open_cache_entry(disk_cache, request, url, *request_headers);
    VERIFY(reader.has_value());
    EXPECT_EQ(reader->status_code(), 200u);
}

TEST_CASE(revalidation_that_outgrows_entry_size_limit_removes_entry)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    disk_cache.set_maximum_disk_cache_size(800);
    TestCacheRequest request;

    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "no-cache"sv },
        { "ETag"sv, "\"v1\""sv },
    });
    store_cache_entry(disk_cache, request, url, *request_headers, response_headers, "hello"sv);

    auto reader = open_cache_entry(disk_cache, request, url, *request_headers);
    VERIFY(reader.has_value());
    EXPECT_EQ(reader->revalidation_type(), HTTP::CacheEntryReader::RevalidationType::MustRevalidate);

    reader->revalidation_succeeded(HTTP::HeaderList::create({ { "ETag"sv, "\"v1\""sv }, { "X-Large"sv, ByteString::repeated('x', 200) } }), UnixDateTime::now(), UnixDateTime::now());
    if (auto body_file = reader->take_body_file(); !body_file.is_error())
        MUST(Core::System::close(body_file.value().fd));

    EXPECT(!open_cache_entry(disk_cache, request, url, *request_headers).has_value());
}

TEST_CASE(writing_beyond_entry_size_limit_abandons_entry)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    disk_cache.set_maximum_disk_cache_size(800);
    TestCacheRequest request;

    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    auto chunk = ByteString::repeated('x', 60);
    TRY_OR_FAIL(writer.write_data(chunk.bytes()));
    EXPECT(writer.write_data(chunk.bytes()).is_error());

    auto& replacement_writer = create_cache_entry(disk_cache, request, url, *request_headers);
    replacement_writer.remove_incomplete_entry();

    EXPECT(!open_cache_entry(disk_cache, request, url, *request_headers).has_value());
}

TEST_CASE(fields_received_after_the_header_section_are_not_stored)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = create_cacheable_request_headers();
    auto lookup_request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("hello"sv.bytes()));

    response_headers->append({ "Vary"sv, "*"sv });
    response_headers->append({ "X-Trailer"sv, "value"sv });
    TRY_OR_FAIL(writer.flush(request_headers));

    auto reader = open_cache_entry(disk_cache, request, url, *lookup_request_headers);
    VERIFY(reader.has_value());
    EXPECT(!reader->response_headers().contains("Vary"sv));
    EXPECT(!reader->response_headers().contains("X-Trailer"sv));
}

TEST_CASE(unreadable_entry_is_removed)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://example.com/script.js"sv);
    auto request_headers = create_cacheable_request_headers();
    store_cache_entry(disk_cache, request, url, *request_headers, create_cacheable_response_headers(), "console.log('hello');"sv);

    auto cache_key = HTTP::create_cache_key(test_partition(), HTTP::serialize_url_for_cache_storage(url), "GET"sv);
    auto path = HTTP::path_for_cache_entry(disk_cache.cache_directory(), cache_key, {});
    {
        auto file = TRY_OR_FAIL(Core::File::open(path.string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        TRY_OR_FAIL(file->write_until_depleted("garbage"sv.bytes()));
    }

    EXPECT(!open_cache_entry(disk_cache, request, url, *request_headers).has_value());
    EXPECT(!FileSystem::exists(path.string()));
    EXPECT_EQ(disk_cache.estimate_cache_size_accessed_since(UnixDateTime::earliest()).total, 0u);
}

TEST_CASE(clearing_the_cache_removes_responses_still_being_written)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://example.com/resource"sv);
    auto request_headers = create_cacheable_request_headers();
    auto lookup_request_headers = create_cacheable_request_headers();
    auto response_headers = create_cacheable_response_headers();

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    TRY_OR_FAIL(writer.write_status_and_reason(200, "OK"_string, *request_headers, *response_headers));
    TRY_OR_FAIL(writer.write_data("hello"sv.bytes()));

    disk_cache.remove_entries_accessed_since(UnixDateTime::earliest());

    EXPECT(writer.flush(request_headers).is_error());
    EXPECT(!open_cache_entry(disk_cache, request, url, *lookup_request_headers).has_value());
}

TEST_CASE(connection_specific_cache_policy_does_not_allow_storage)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;

    auto url = parse_url("https://example.com/redirect"sv);
    auto request_headers = create_cacheable_request_headers();
    auto response_headers = HTTP::HeaderList::create({
        { "Connection"sv, "Cache-Control"sv },
        { "Cache-Control"sv, "max-age=3600"sv },
        { "Location"sv, "https://example.com/target"sv },
    });

    auto& writer = create_cache_entry(disk_cache, request, url, *request_headers);
    EXPECT(writer.write_status_and_reason(302, "Found"_string, *request_headers, *response_headers).is_error());
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

    auto body_file = TRY_OR_FAIL(writer.flush_and_take_body_file(request_headers));
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

    auto old_body_file = TRY_OR_FAIL(writer.flush_and_take_body_file(request_headers));
    auto old_body = TRY_OR_FAIL(Core::ImmutableBytes::map_from_fd_range_and_close(old_body_file.fd, "old cache body"sv, old_body_file.offset, old_body_file.size));
    EXPECT_EQ(old_body.bytes(), "console.log('old');"sv.bytes());

    auto replacement_request_headers = create_cacheable_request_headers();
    auto replacement_response_headers = create_cacheable_response_headers();
    auto& replacement_writer = create_cache_entry(disk_cache, request, url, *replacement_request_headers);
    TRY_OR_FAIL(replacement_writer.write_status_and_reason(200, "OK"_string, *replacement_request_headers, *replacement_response_headers));
    TRY_OR_FAIL(replacement_writer.write_data("console.log('new');"sv.bytes()));
    TRY_OR_FAIL(replacement_writer.flush(replacement_request_headers));

    EXPECT_EQ(old_body.bytes(), "console.log('old');"sv.bytes());
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
    TRY_OR_FAIL(writer.flush(request_headers));

    for (auto const& other_partition : { other_top_level_site, other_frame_site, subframe_document, cross_site_main_frame_navigation }) {
        disk_cache.open_entry(request, other_partition, url, "GET"sv, *request_headers, HTTP::CacheMode::Default, HTTP::DiskCache::OpenMode::Read)
            .visit(
                [&](Optional<HTTP::CacheEntryReader&> reader) {
                    EXPECT(!reader.has_value());
                },
                [](HTTP::DiskCache::CacheHasOpenEntry) { });
    }

    EXPECT(open_cache_entry(disk_cache, request, url, *request_headers).has_value());

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
    MUST(writer.flush(request_headers));

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

// Legacy disk SHA-1 prefix: cc4122efe2277ef8, with test_partition() and GET.
TEST_CASE(legacy_disk_hash_collision_does_not_alias_responses)
{
    auto disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request;
    auto a = parse_url("https://b.example/a15233b41c58b0cc"sv);
    auto b = parse_url("https://a.example/3a0fafd1c5bfae49"sv);
    auto headers = create_cacheable_request_headers();
    auto response_a = create_cacheable_response_headers();
    response_a->append({ "X-Identity"sv, "A"sv });
    auto response_b = create_cacheable_response_headers();
    response_b->append({ "X-Identity"sv, "B"sv });
    store_cache_entry(disk_cache, request, a, *headers, response_a, "response A"sv);
    auto unexpected = open_cache_entry(disk_cache, request, b, *headers);
    EXPECT(!unexpected.has_value());
    if (unexpected.has_value())
        disk_cache.close_entries_read_by(request);
    store_cache_entry(disk_cache, request, b, *headers, response_b, "response B"sv);
    for (auto which : { 0, 1 }) {
        auto const& url = which == 0 ? a : b;
        auto expected = which == 0 ? "response A"sv : "response B"sv;
        auto reader = open_cache_entry(disk_cache, request, url, *headers);
        VERIFY(reader.has_value());
        EXPECT_EQ(reader->response_headers().get("X-Identity"sv), Optional<ByteString> { which == 0 ? "A" : "B" });
        auto file = TRY_OR_FAIL(reader->take_body_file());
        auto body = TRY_OR_FAIL(Core::ImmutableBytes::map_from_fd_range_and_close(file.fd, "collision body"sv, file.offset, file.size));
        EXPECT_EQ(body.bytes(), expected.bytes());
    }
    disk_cache.invalidate(test_partition(), a);
    EXPECT(!open_cache_entry(disk_cache, request, a, *headers).has_value());
    EXPECT(open_cache_entry(disk_cache, request, b, *headers).has_value());
}

TEST_CASE(different_resources_do_not_share_writer_activity_or_locks)
{
    auto disk_cache = TRY_OR_FAIL(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest request_a;
    TestCacheRequest request_b;
    auto a = parse_url("https://b.example/a15233b41c58b0cc"sv);
    auto b = parse_url("https://a.example/3a0fafd1c5bfae49"sv);
    auto headers = create_cacheable_request_headers();
    request_a.activity = MonotonicTime::now();
    auto& writer_a = create_cache_entry(disk_cache, request_a, a, *headers);
    EXPECT(disk_cache.last_activity_time_of_open_entries(test_partition(), a, "GET"sv) == request_a.activity);
    EXPECT(!disk_cache.last_activity_time_of_open_entries(test_partition(), b, "GET"sv).has_value());
    // This helper fails if B is queued behind A, as happens with the legacy collision.
    auto& writer_b = create_cache_entry(disk_cache, request_b, b, *headers);
    TRY_OR_FAIL(writer_a.write_status_and_reason(200, "OK"_string, *headers, *create_cacheable_response_headers()));
    TRY_OR_FAIL(writer_b.write_status_and_reason(200, "OK"_string, *headers, *create_cacheable_response_headers()));
    disk_cache.invalidate(test_partition(), a);
    EXPECT(writer_a.write_data("A"sv.bytes()).is_error());
    TRY_OR_FAIL(writer_b.write_data("B"sv.bytes()));
    TRY_OR_FAIL(writer_b.flush(create_cacheable_request_headers()));
    EXPECT(open_cache_entry(disk_cache, request_b, b, *headers).has_value());
}

TEST_CASE(waiters_are_resumed_only_when_their_exact_resource_closes)
{
    Core::EventLoop event_loop;
    auto disk_cache = TRY_OR_FAIL(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    TestCacheRequest holder_a;
    TestCacheRequest holder_b;
    TestCacheRequest waiter_a;
    TestCacheRequest waiter_b;
    auto a = parse_url("https://b.example/a15233b41c58b0cc"sv);
    auto b = parse_url("https://a.example/3a0fafd1c5bfae49"sv);
    auto headers = create_cacheable_request_headers();
    auto& writer_a = create_cache_entry(disk_cache, holder_a, a, *headers);
    auto& writer_b = create_cache_entry(disk_cache, holder_b, b, *headers);
    for (auto which : { 0, 1 }) {
        auto& waiter = which == 0 ? waiter_a : waiter_b;
        auto const& url = which == 0 ? a : b;
        auto result = disk_cache.create_entry(waiter, test_partition(), url, "GET"sv, *headers, UnixDateTime::now());
        EXPECT(result.has<HTTP::DiskCache::CacheHasOpenEntry>());
    }
    writer_a.remove_incomplete_entry();
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(waiter_a.unblocked_count, 1u);
    EXPECT_EQ(waiter_b.unblocked_count, 0u);
    writer_b.remove_incomplete_entry();
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(waiter_b.unblocked_count, 1u);
}

TEST_CASE(cache_file_for_another_url_is_not_served)
{
    auto disk_cache = TRY_OR_FAIL(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    auto url = parse_url("https://example.com/a"sv);
    auto other_url = "https://example.com/b"_string;
    auto headers = create_cacheable_request_headers();
    TestCacheRequest request;
    store_cache_entry(disk_cache, request, url, *headers, create_cacheable_response_headers(), "body"sv);
    auto cache_key = HTTP::create_cache_key(test_partition(), HTTP::serialize_url_for_cache_storage(url), "GET"sv);
    auto path = HTTP::path_for_cache_entry(disk_cache.cache_directory(), cache_key, {});
    {
        auto file = TRY_OR_FAIL(Core::File::open(path.string(), Core::File::OpenMode::ReadWrite));
        auto header = TRY_OR_FAIL(file->read_value<HTTP::CacheHeader>());
        header.url_hash = other_url.hash();
        (void)TRY_OR_FAIL(file->seek(0, SeekMode::SetPosition));
        TRY_OR_FAIL(file->write_value(header));
        TRY_OR_FAIL(file->write_until_depleted(other_url));
    }
    EXPECT(!open_cache_entry(disk_cache, request, url, *headers).has_value());
    EXPECT(!FileSystem::exists(path.string()));
}

TEST_CASE(cache_file_for_another_key_is_not_served)
{
    auto disk_cache = TRY_OR_FAIL(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, test_cache_root())).release_value();
    auto url = parse_url("https://example.com/a"sv);
    auto headers = create_cacheable_request_headers();
    TestCacheRequest request;
    store_cache_entry(disk_cache, request, url, *headers, create_cacheable_response_headers(), "body"sv);
    auto cache_key = HTTP::create_cache_key(test_partition(), HTTP::serialize_url_for_cache_storage(url), "GET"sv);
    auto path = HTTP::path_for_cache_entry(disk_cache.cache_directory(), cache_key, {});
    {
        auto file = TRY_OR_FAIL(Core::File::open(path.string(), Core::File::OpenMode::ReadWrite));
        auto header = TRY_OR_FAIL(file->read_value<HTTP::CacheHeader>());
        header.vary_key.bytes.last() ^= 1;
        (void)TRY_OR_FAIL(file->seek(0, SeekMode::SetPosition));
        TRY_OR_FAIL(file->write_value(header));
    }
    EXPECT(!open_cache_entry(disk_cache, request, url, *headers).has_value());
    EXPECT(!FileSystem::exists(path.string()));
}
