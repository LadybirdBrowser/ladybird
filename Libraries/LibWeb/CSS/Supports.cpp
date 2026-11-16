/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibWeb/CSS/Query.h>
#include <LibWeb/CSS/Supports.h>

namespace Web::CSS {

bool supports_condition_matches(RustQueryHandle const& handle)
{
    auto result = Parser::ValueParserFFI::css_query_evaluate_supports(handle.data());
    if (result > to_underlying(MatchResult::Unknown)) {
        dbgln("supports_condition_matches: unexpected query handle kind");
        return false;
    }
    return result == 1;
}

}
