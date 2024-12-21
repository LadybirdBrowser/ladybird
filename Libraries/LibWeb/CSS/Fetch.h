/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::CSS {

enum class CorsMode {
    NoCors,
    Cors,
};

// https://drafts.csswg.org/css-values-4/#fetch-a-style-resource
void fetch_a_style_resource(String const& url, CSSStyleSheet const&, Fetch::Infrastructure::Request::Destination, CorsMode, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction process_response);

}
