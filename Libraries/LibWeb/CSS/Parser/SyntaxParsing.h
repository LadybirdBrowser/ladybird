/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

enum class LimitSingleComponentIdentToCustomIdent : u8 {
    No,
    Yes,
};
WEB_API RefPtr<SyntaxNode> parse_as_syntax(Utf16View, LimitSingleComponentIdentToCustomIdent = LimitSingleComponentIdentToCustomIdent::No);
NonnullRefPtr<StyleValue const> parse_with_a_syntax(ParsingParams const&, Utf16View input, SyntaxNode const& syntax);

}
