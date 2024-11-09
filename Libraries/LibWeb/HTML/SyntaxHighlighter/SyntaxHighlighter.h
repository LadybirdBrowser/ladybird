/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibSyntax/Highlighter.h>

namespace Web::HTML {

enum class AugmentedTokenKind : u32 {
    AttributeName,
    AttributeValue,
    OpenTag,
    CloseTag,
    Comment,
    Doctype,
    __Count,
};

class SyntaxHighlighter : public Syntax::Highlighter {
public:
    SyntaxHighlighter() = default;
    virtual ~SyntaxHighlighter() override = default;

    virtual bool is_identifier(u64) const override;
    virtual bool is_navigatable(u64) const override;

    virtual Syntax::Language language() const override { return Syntax::Language::HTML; }
    virtual Optional<StringView> comment_prefix() const override { return "<!--"sv; }
    virtual Optional<StringView> comment_suffix() const override { return "-->"sv; }
    virtual void rehighlight(Palette const&) override;

    static constexpr u64 JS_TOKEN_START_VALUE = 1000;
    static constexpr u64 CSS_TOKEN_START_VALUE = 2000;

protected:
    virtual Vector<MatchingTokenPair> matching_token_pairs_impl() const override;
    virtual bool token_types_equal(u64, u64) const override;
};

}
