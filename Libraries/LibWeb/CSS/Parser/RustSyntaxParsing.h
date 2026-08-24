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
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

class Parser;

enum class PreservePropertySourceText {
    No,
    Yes,
};

enum class RuleNesting {
    No,
    Yes,
};

class RustSyntaxParser {
public:
    static Optional<Rule> parse_rule(Parser&, ReadonlySpan<RuleContext>, RuleNesting);
    static Vector<Rule> parse_stylesheet(Parser&);
    static Vector<RuleOrListOfDeclarations> parse_block_contents(Parser&, ReadonlySpan<RuleContext>, PreservePropertySourceText = PreservePropertySourceText::No);
    static Vector<RuleOrListOfDeclarations> parse_block_contents(Parser&, Utf16View, ReadonlySpan<RuleContext>, PreservePropertySourceText = PreservePropertySourceText::No);
    static RefPtr<StyleValue const> parse_descriptor(Parser&, AtRuleID, DescriptorNameAndID const&);
    static Vector<ComponentValue> component_values(ValueParserFFI::FfiSyntaxParseData const&, size_t start, size_t count);
    static void set_token_position(Token&, SourcePosition, SourcePosition);
};

}
