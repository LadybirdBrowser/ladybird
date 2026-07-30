/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibHTTP/Forward.h>
#include <LibRequests/CameFromCache.h>
#include <LibWebView/Export.h>

namespace WebView {

struct WEBVIEW_API DownloadRangeValidator {
    Optional<String> etag;
    Optional<String> last_modified;

    bool is_usable() const { return etag.has_value() || last_modified.has_value(); }

    Optional<String> if_range_value() const { return etag.has_value() ? etag : last_modified; }

    bool matches(DownloadRangeValidator const& fresh) const;
};

struct WEBVIEW_API DownloadRangeSupport {
    bool supports_ranges { false };

    Optional<u64> total_size;
    DownloadRangeValidator validator;

    bool can_resume() const { return supports_ranges && validator.is_usable(); }
};

WEBVIEW_API DownloadRangeSupport evaluate_range_support(HTTP::HeaderList const&, Optional<u32> response_code, Requests::CameFromCache);

WEBVIEW_API bool response_is_rate_limited(Optional<u32> response_code);

// FIXME: Accept the HTTP-date form of Retry-After once we have a parser for it.
WEBVIEW_API Optional<AK::Duration> parse_retry_after(HTTP::HeaderList const&);

struct DownloadSegmentRange {
    u64 start_offset { 0 };
    u64 end_offset { 0 };

    u64 size() const { return end_offset - start_offset + 1; }
};

struct DownloadSegmentationParameters {
    u32 maximum_connections { 4 };

    u64 minimum_size_to_split { 8 * MiB };
    u64 minimum_segment_size { 2 * MiB };
};

WEBVIEW_API Vector<DownloadSegmentRange> plan_download_segments(u64 total_size, u64 first_segment_next_offset, DownloadSegmentationParameters const& = {});

}
