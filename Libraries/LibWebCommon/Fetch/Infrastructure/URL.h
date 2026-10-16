/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/StringView.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Export.h>

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

[[nodiscard]] WEBCOMMON_API bool is_local_url(URL::URL const&);
[[nodiscard]] WEBCOMMON_API bool is_fetch_scheme(StringView);
[[nodiscard]] WEBCOMMON_API bool is_http_or_https_scheme(StringView);

}
