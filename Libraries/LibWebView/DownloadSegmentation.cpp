/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
#include <LibHTTP/HeaderList.h>
#include <LibWebView/DownloadSegmentation.h>

namespace WebView {

static String header_value_to_string(StringView value)
{
    return String::from_utf8_with_replacement_character(value, String::WithBOMHandling::No);
}

static bool header_list_contains_only(Optional<ByteString> const& header, StringView expected)
{
    if (!header.has_value())
        return true;

    auto matches = true;

    header->view().for_each_split_view(","sv, SplitBehavior::Nothing, [&](StringView token) {
        if (!token.trim_whitespace().equals_ignoring_ascii_case(expected))
            matches = false;
    });

    return matches;
}

static bool header_list_contains(Optional<ByteString> const& header, StringView expected)
{
    if (!header.has_value())
        return false;

    auto contains = false;

    header->view().for_each_split_view(","sv, SplitBehavior::Nothing, [&](StringView token) {
        if (token.trim_whitespace().equals_ignoring_ascii_case(expected))
            contains = true;
    });

    return contains;
}

static DownloadRangeValidator extract_range_validator(HTTP::HeaderList const& response_headers)
{
    DownloadRangeValidator validator;

    if (auto etag = response_headers.get("ETag"sv); etag.has_value()) {
        auto value = etag->view().trim_whitespace();

        // Weak entity tags must not be used as an `If-Range` validator, as they may compare equal across
        // representations whose bytes differ. See RFC 9110 section 13.1.5.
        if (!value.is_empty() && !value.starts_with("W/"sv))
            validator.etag = header_value_to_string(value);
    }

    if (auto last_modified = response_headers.get("Last-Modified"sv); last_modified.has_value()) {
        auto value = last_modified->view().trim_whitespace();
        if (!value.is_empty())
            validator.last_modified = header_value_to_string(value);
    }

    return validator;
}

bool DownloadRangeValidator::matches(DownloadRangeValidator const& fresh) const
{
    // Compare by the strongest validator both responses carry.
    if (etag.has_value() && fresh.etag.has_value())
        return *etag == *fresh.etag;

    if (last_modified.has_value() && fresh.last_modified.has_value())
        return *last_modified == *fresh.last_modified;

    return true;
}

DownloadRangeSupport evaluate_range_support(HTTP::HeaderList const& response_headers, Optional<u32> response_code, Requests::CameFromCache came_from_cache)
{
    DownloadRangeSupport support;

    if (auto extracted_length = response_headers.extract_length(); extracted_length.has<u64>())
        support.total_size = extracted_length.get<u64>();

    support.validator = extract_range_validator(response_headers);

    if (response_code != 200u)
        return support;
    if (came_from_cache == Requests::CameFromCache::Yes)
        return support;

    if (!support.total_size.has_value() || *support.total_size == 0)
        return support;

    if (!header_list_contains(response_headers.get("Accept-Ranges"sv), "bytes"sv))
        return support;

    // If the response body is encoded, the byte offsets we would ask for are offsets into the encoded bytes, while
    // the offsets we write at are offsets into the bytes we are handed after decoding. Those are not the same thing,
    // so there is no correct range to request.
    if (!header_list_contains_only(response_headers.get("Content-Encoding"sv), "identity"sv))
        return support;
    if (!header_list_contains_only(response_headers.get("Transfer-Encoding"sv), "identity"sv))
        return support;

    support.supports_ranges = true;
    return support;
}

bool response_is_rate_limited(Optional<u32> response_code)
{
    if (!response_code.has_value())
        return false;

    return *response_code == 429 || *response_code == 503;
}

Optional<AK::Duration> parse_retry_after(HTTP::HeaderList const& response_headers)
{
    auto retry_after = response_headers.get("Retry-After"sv);
    if (!retry_after.has_value())
        return {};

    auto seconds = retry_after->view().trim_whitespace().to_number<u32>();
    if (!seconds.has_value())
        return {};

    return AK::Duration::from_seconds(*seconds);
}

Vector<DownloadSegmentRange> plan_download_segments(u64 total_size, u64 first_segment_next_offset, DownloadSegmentationParameters const& parameters)
{
    if (parameters.maximum_connections < 2 || parameters.minimum_segment_size == 0)
        return {};
    if (total_size < parameters.minimum_size_to_split)
        return {};
    if (first_segment_next_offset >= total_size)
        return {};

    auto remaining_size = total_size - first_segment_next_offset;
    if (remaining_size / parameters.minimum_segment_size < 2)
        return {};

    auto segment_count = min(static_cast<u64>(parameters.maximum_connections), remaining_size / parameters.minimum_segment_size);
    auto segment_size = remaining_size / segment_count;

    Vector<DownloadSegmentRange> segments;
    segments.ensure_capacity(segment_count);

    for (u64 i = 0; i < segment_count; ++i) {
        auto start_offset = i == 0 ? 0 : first_segment_next_offset + (i * segment_size);
        auto end_offset = i == segment_count - 1 ? total_size - 1 : first_segment_next_offset + ((i + 1) * segment_size) - 1;

        segments.unchecked_append({ start_offset, end_offset });
    }

    return segments;
}

}
