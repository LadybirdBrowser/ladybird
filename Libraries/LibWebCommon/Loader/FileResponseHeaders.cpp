/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Time.h>
#include <LibCore/MimeData.h>
#include <LibWebCommon/Loader/FileResponseHeaders.h>

namespace Web {

NonnullRefPtr<HTTP::HeaderList> response_headers_for_file(StringView path, size_t content_length, Optional<time_t> const& modified_time)
{
    // For file:// and resource:// URLs, we have to guess the MIME type, since there's no HTTP header to tell us what
    // it is. We insert a fake Content-Type header here, so that clients can use it to learn the MIME type.
    auto mime_type = Core::guess_mime_type_based_on_filename(path);

    auto response_headers = HTTP::HeaderList::create({
        { "Access-Control-Allow-Origin"sv, "null"sv },
        { "Content-Type"sv, mime_type },
        { "Content-Length"sv, ByteString::number(content_length) },
    });

    if (modified_time.has_value()) {
        auto const datetime = AK::UnixDateTime::from_seconds_since_epoch(modified_time.value());
        response_headers->set({ "Last-Modified"sv, datetime.to_byte_string("%a, %d %b %Y %H:%M:%S GMT"sv, AK::UnixDateTime::LocalTime::No) });
    }

    return response_headers;
}

}
