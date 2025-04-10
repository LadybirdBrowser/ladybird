/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::CSS {

enum class CorsMode {
    NoCors,
    Cors,
};

using StyleResourceURL = Variant<::URL::URL, CSS::URL>;

// https://drafts.csswg.org/css-values-4/#fetch-a-style-resource
void fetch_a_style_resource(StyleResourceURL const& url, CSSStyleSheet const&, Fetch::Infrastructure::Request::Destination, CorsMode, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction process_response);

}
