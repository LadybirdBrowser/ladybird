/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibTest/TestCase.h>
#include <LibURL/URL.h>
#include <LibWebView/DownloadPresentation.h>
#include <LibWebView/DownloadSegmentation.h>

static NonnullRefPtr<HTTP::HeaderList> headers(Vector<HTTP::Header> headers)
{
    return HTTP::HeaderList::create(move(headers));
}

static Vector<HTTP::Header> ok_headers()
{
    return {
        { "Content-Length", "1048576" },
        { "Accept-Ranges", "bytes" },
        { "ETag", "\"abc123\"" },
    };
}

static WebView::DownloadRangeSupport evaluate(Vector<HTTP::Header> header_list, Optional<u32> response_code = 200u, Requests::CameFromCache came_from_cache = Requests::CameFromCache::No)
{
    return WebView::evaluate_range_support(headers(move(header_list)), response_code, came_from_cache);
}

TEST_CASE(range_support_for_a_well_behaved_response)
{
    auto support = evaluate(ok_headers());
    EXPECT(support.supports_ranges);
    EXPECT_EQ(support.total_size, 1048576u);
    EXPECT_EQ(support.validator.etag, "\"abc123\""_string);
    EXPECT(support.can_resume());
}

TEST_CASE(range_support_requires_accept_ranges)
{
    EXPECT(!evaluate({ { "Content-Length", "1048576" } }).supports_ranges);
    EXPECT(!evaluate({ { "Content-Length", "1048576" }, { "Accept-Ranges", "none" } }).supports_ranges);
    EXPECT(evaluate({ { "Content-Length", "1048576" }, { "Accept-Ranges", "none, bytes" } }).supports_ranges);
    EXPECT(evaluate({ { "Content-Length", "1048576" }, { "Accept-Ranges", "BYTES" } }).supports_ranges);
}

TEST_CASE(range_support_requires_a_known_non_zero_length)
{
    EXPECT(!evaluate({ { "Accept-Ranges", "bytes" } }).supports_ranges);
    EXPECT(!evaluate({ { "Content-Length", "0" }, { "Accept-Ranges", "bytes" } }).supports_ranges);
}

TEST_CASE(range_support_requires_an_unencoded_body)
{
    auto with_header = [](StringView name, StringView value) {
        auto header_list = ok_headers();
        header_list.append({ name, value });
        return evaluate(move(header_list));
    };

    EXPECT(!with_header("Content-Encoding"sv, "gzip"sv).supports_ranges);
    EXPECT(!with_header("Content-Encoding"sv, "identity, gzip"sv).supports_ranges);
    EXPECT(with_header("Content-Encoding"sv, "identity"sv).supports_ranges);
    EXPECT(!with_header("Transfer-Encoding"sv, "chunked"sv).supports_ranges);
    EXPECT(with_header("Transfer-Encoding"sv, "identity"sv).supports_ranges);
}

TEST_CASE(range_support_requires_a_fresh_complete_response)
{
    EXPECT(!evaluate(ok_headers(), 206u).supports_ranges);
    EXPECT(!evaluate(ok_headers(), 302u).supports_ranges);
    EXPECT(!evaluate(ok_headers(), {}).supports_ranges);
    EXPECT(!evaluate(ok_headers(), 200u, Requests::CameFromCache::Yes).supports_ranges);
}

TEST_CASE(range_validators)
{
    auto weak = evaluate({ { "Content-Length", "1048576" }, { "Accept-Ranges", "bytes" }, { "ETag", "W/\"abc123\"" } });
    EXPECT(weak.supports_ranges);
    EXPECT(!weak.validator.etag.has_value());
    EXPECT(!weak.can_resume());

    auto last_modified = evaluate({ { "Content-Length", "1048576" }, { "Accept-Ranges", "bytes" }, { "Last-Modified", "Wed, 21 Oct 2015 07:28:00 GMT" } });
    EXPECT(last_modified.can_resume());
    EXPECT_EQ(last_modified.validator.if_range_value(), "Wed, 21 Oct 2015 07:28:00 GMT"_string);

    auto both = evaluate({ { "Content-Length", "1048576" }, { "Accept-Ranges", "bytes" }, { "ETag", "\"abc123\"" }, { "Last-Modified", "Wed, 21 Oct 2015 07:28:00 GMT" } });
    EXPECT_EQ(both.validator.if_range_value(), "\"abc123\""_string);
}

TEST_CASE(range_validator_matching)
{
    auto validator = [](Optional<String> etag, Optional<String> last_modified) {
        return WebView::DownloadRangeValidator { move(etag), move(last_modified) };
    };

    auto recorded = validator("\"abc123\""_string, "Wed, 21 Oct 2015 07:28:00 GMT"_string);

    EXPECT(recorded.matches(recorded));
    EXPECT(!recorded.matches(validator("\"def456\""_string, "Wed, 21 Oct 2015 07:28:00 GMT"_string)));

    // An agreeing ETag outweighs a disagreeing Last-Modified, and vice versa when no ETag is available.
    EXPECT(recorded.matches(validator("\"abc123\""_string, "Thu, 22 Oct 2015 07:28:00 GMT"_string)));
    EXPECT(!recorded.matches(validator({}, "Thu, 22 Oct 2015 07:28:00 GMT"_string)));
    EXPECT(recorded.matches(validator({}, "Wed, 21 Oct 2015 07:28:00 GMT"_string)));

    // A response carrying no validators in common with ours is tolerated.
    EXPECT(recorded.matches(validator({}, {})));
    EXPECT(validator({}, {}).matches(recorded));

    auto last_modified_only = validator({}, "Wed, 21 Oct 2015 07:28:00 GMT"_string);
    EXPECT(last_modified_only.matches(recorded));
    EXPECT(!last_modified_only.matches(validator({}, "Thu, 22 Oct 2015 07:28:00 GMT"_string)));
    EXPECT(last_modified_only.matches(validator("\"abc123\""_string, {})));
}

static void expect_segments_tile(Vector<WebView::DownloadSegmentRange> const& segments, u64 total_size)
{
    EXPECT(!segments.is_empty());
    EXPECT_EQ(segments.first().start_offset, 0u);
    EXPECT_EQ(segments.last().end_offset, total_size - 1);

    for (size_t i = 0; i < segments.size(); ++i) {
        EXPECT(segments[i].start_offset <= segments[i].end_offset);
        if (i > 0)
            EXPECT_EQ(segments[i].start_offset, segments[i - 1].end_offset + 1);
    }
}

TEST_CASE(segments_are_not_planned_for_small_downloads)
{
    WebView::DownloadSegmentationParameters parameters { .maximum_connections = 4, .minimum_size_to_split = 8 * MiB, .minimum_segment_size = 2 * MiB };

    EXPECT(WebView::plan_download_segments(0, 0, parameters).is_empty());
    EXPECT(WebView::plan_download_segments(5 * MiB, 0, parameters).is_empty());
    EXPECT(WebView::plan_download_segments(8 * MiB - 1, 0, parameters).is_empty());
    EXPECT(!WebView::plan_download_segments(8 * MiB, 0, parameters).is_empty());

    EXPECT(WebView::plan_download_segments(8 * MiB, 7 * MiB, parameters).is_empty());
    EXPECT(WebView::plan_download_segments(8 * MiB, 8 * MiB, parameters).is_empty());
}

TEST_CASE(segment_count_is_capped_by_the_minimum_segment_size)
{
    WebView::DownloadSegmentationParameters parameters { .maximum_connections = 4, .minimum_size_to_split = 8 * MiB, .minimum_segment_size = 2 * MiB };

    EXPECT_EQ(WebView::plan_download_segments(8 * MiB, 0, parameters).size(), 4u);
    EXPECT_EQ(WebView::plan_download_segments(100 * MiB, 0, parameters).size(), 4u);

    EXPECT_EQ(WebView::plan_download_segments(20 * MiB, 14 * MiB, parameters).size(), 3u);
    EXPECT_EQ(WebView::plan_download_segments(20 * MiB, 16 * MiB, parameters).size(), 2u);
}

TEST_CASE(segments_tile_the_whole_file)
{
    WebView::DownloadSegmentationParameters parameters { .maximum_connections = 4, .minimum_size_to_split = 8 * MiB, .minimum_segment_size = 2 * MiB };

    auto segments = WebView::plan_download_segments(100 * MiB + 7, 0, parameters);
    expect_segments_tile(segments, 100 * MiB + 7);
    EXPECT_EQ(segments.size(), 4u);
    EXPECT_EQ(segments.last().size(), segments.first().size() + 3);

    auto resumed = WebView::plan_download_segments(100 * MiB, 30 * MiB, parameters);
    expect_segments_tile(resumed, 100 * MiB);
    EXPECT_EQ(resumed.first().start_offset, 0u);
    EXPECT(resumed.first().size() > resumed[1].size());
}

TEST_CASE(segments_are_not_planned_without_room_for_two_connections)
{
    WebView::DownloadSegmentationParameters parameters { .maximum_connections = 1, .minimum_size_to_split = 8 * MiB, .minimum_segment_size = 2 * MiB };
    EXPECT(WebView::plan_download_segments(100 * MiB, 0, parameters).is_empty());

    parameters = { .maximum_connections = 4, .minimum_size_to_split = 8 * MiB, .minimum_segment_size = 0 };
    EXPECT(WebView::plan_download_segments(100 * MiB, 0, parameters).is_empty());
}

TEST_CASE(rate_limited_responses)
{
    EXPECT(WebView::response_is_rate_limited(429u));
    EXPECT(WebView::response_is_rate_limited(503u));
    EXPECT(!WebView::response_is_rate_limited(200u));
    EXPECT(!WebView::response_is_rate_limited(500u));
    EXPECT(!WebView::response_is_rate_limited({}));
}

TEST_CASE(retry_after)
{
    auto retry_after = [](Vector<HTTP::Header> header_list) {
        return WebView::parse_retry_after(headers(move(header_list)));
    };

    EXPECT_EQ(retry_after({ { "Retry-After", "30" } }), AK::Duration::from_seconds(30));
    EXPECT_EQ(retry_after({ { "Retry-After", "  30  " } }), AK::Duration::from_seconds(30));
    EXPECT_EQ(retry_after({ { "Retry-After", "0" } }), AK::Duration::from_seconds(0));

    EXPECT(!retry_after({}).has_value());
    EXPECT(!retry_after({ { "Retry-After", "" } }).has_value());
    EXPECT(!retry_after({ { "Retry-After", "-5" } }).has_value());

    EXPECT(!retry_after({ { "Retry-After", "Wed, 21 Oct 2015 07:28:00 GMT" } }).has_value());
}

TEST_CASE(estimated_time_remaining)
{
    WebView::FileDownloader::Download download {
        .id = 1,
        .is_private = WebView::IsPrivate::No,
        .url = URL::about_blank(),
        .destination = LexicalPath { "/tmp/file"sv },
        .status = WebView::FileDownloader::DownloadStatus::InProgress,
        .downloaded_size = 400,
        .total_size = 1000,
        .error = {},
        .can_resume = true,
        .connection_count = 1,
        .bytes_per_second = {},
        .is_waiting_to_retry = false,
        .created_time = {},
    };

    EXPECT(!download.estimated_time_remaining().has_value());

    download.bytes_per_second = 100;
    EXPECT_EQ(download.estimated_time_remaining(), AK::Duration::from_seconds(6));

    download.downloaded_size = 1000;
    EXPECT(!download.estimated_time_remaining().has_value());

    download.downloaded_size = 400;
    download.total_size = {};
    EXPECT(!download.estimated_time_remaining().has_value());

    download.total_size = 1000;
    download.bytes_per_second = 0;
    EXPECT(!download.estimated_time_remaining().has_value());
}

TEST_CASE(time_remaining_text)
{
    auto text = [](i64 seconds) {
        return WebView::download_time_remaining_text(AK::Duration::from_seconds(seconds));
    };

    EXPECT_EQ(text(0), "1 sec left"_string);
    EXPECT_EQ(text(45), "45 sec left"_string);
    EXPECT_EQ(text(59), "59 sec left"_string);

    EXPECT_EQ(text(60), "1 min left"_string);
    EXPECT_EQ(text(61), "2 min left"_string);
    EXPECT_EQ(text(59 * 60), "59 min left"_string);

    EXPECT_EQ(text(60 * 60), "1 hr 0 min left"_string);
    EXPECT_EQ(text(90 * 60), "1 hr 30 min left"_string);
    EXPECT_EQ(text(7199), "2 hr 0 min left"_string);
    EXPECT_EQ(text(25 * 60 * 60), "25 hr 0 min left"_string);
}

TEST_CASE(rate_text)
{
    EXPECT_EQ(WebView::download_rate_text(512), "512 B/s"_string);
    EXPECT_EQ(WebView::download_rate_text(1536), "1.5 KiB/s"_string);
    EXPECT_EQ(WebView::download_rate_text(3 * MiB), "3.0 MiB/s"_string);
}
