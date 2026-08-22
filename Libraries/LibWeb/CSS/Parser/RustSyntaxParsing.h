/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Parser/RuleContext.h>
#include <LibWeb/CSS/Parser/Types.h>

namespace Web::CSS::Parser {

class Parser;

enum class PreservePropertySourceText {
    No,
    Yes,
};

class RustSyntaxParser {
public:
    static Vector<Rule> parse_stylesheet(Parser&);
    static Vector<RuleOrListOfDeclarations> parse_block_contents(Parser&, ReadonlySpan<RuleContext>, PreservePropertySourceText = PreservePropertySourceText::No);
    static void set_token_position(Token&, SourcePosition, SourcePosition);
};

}
