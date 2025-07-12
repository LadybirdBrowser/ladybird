/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace Web::ContentSecurityPolicy::Directives {

struct SourceExpressionParseResult {
    Optional<StringView> scheme_part;
    Optional<StringView> host_part;
    Optional<StringView> port_part;
    Optional<StringView> path_part;
    Optional<StringView> keyword_source;
    Optional<StringView> base64_value;
    Optional<StringView> hash_algorithm;
};

enum class Production {
    SchemeSource,
    HostSource,
    KeywordSource,
    NonceSource,
    HashSource,
};

Optional<SourceExpressionParseResult> parse_source_expression(Production, StringView);

}
