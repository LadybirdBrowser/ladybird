/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>

namespace Web::ContentSecurityPolicy::Directives {

struct SourceExpressionParseResult {
    Optional<Utf16View> scheme_part;
    Optional<Utf16View> host_part;
    Optional<Utf16View> port_part;
    Optional<Utf16View> path_part;
    Optional<Utf16View> keyword_source;
    Optional<Utf16View> base64_value;
    Optional<Utf16View> hash_algorithm;
};

enum class Production {
    SchemeSource,
    HostSource,
    KeywordSource,
    NonceSource,
    HashSource,
};

Optional<SourceExpressionParseResult> parse_source_expression(Production, Utf16View);

}
