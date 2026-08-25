/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/CSS/Parser/RustSyntaxHandle.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

enum class LimitSingleComponentIdentToCustomIdent : u8 {
    No,
    Yes,
};
WEB_API Optional<RustSyntaxHandle> parse_as_syntax(Utf16View, LimitSingleComponentIdentToCustomIdent = LimitSingleComponentIdentToCustomIdent::No);
NonnullRefPtr<StyleValue const> parse_with_a_syntax(ParsingParams const&, Utf16View input, RustSyntaxHandle const& syntax);

}
