/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibURL/Forward.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#local-scheme
// A local scheme is "about", "blob", or "data".
inline constexpr Array LOCAL_SCHEMES = {
    "about"sv, "blob"sv, "data"sv
};

// https://fetch.spec.whatwg.org/#http-scheme
// An HTTP(S) scheme is "http" or "https".
inline constexpr Array HTTP_SCHEMES = {
    "http"sv, "https"sv
};

// https://fetch.spec.whatwg.org/#fetch-scheme
// A fetch scheme is "about", "blob", "data", "file", or an HTTP(S) scheme.
inline constexpr Array FETCH_SCHEMES = {
    "about"sv, "blob"sv, "data"sv, "file"sv, "http"sv, "https"sv,

    // AD-HOC: Internal fetch schemes:
    "resource"sv
};

// https://fetch.spec.whatwg.org/#data-url-struct
struct DataURL {
    MimeSniff::MimeType mime_type;
    ByteBuffer body;
};

[[nodiscard]] bool is_local_url(URL::URL const&);
[[nodiscard]] bool is_fetch_scheme(StringView);
[[nodiscard]] bool is_http_or_https_scheme(StringView);
ErrorOr<DataURL> process_data_url(URL::URL const&);

}
