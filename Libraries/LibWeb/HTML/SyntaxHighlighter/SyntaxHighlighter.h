/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibSyntax/Highlighter.h>
#include <LibWeb/Export.h>

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

class WEB_API SyntaxHighlighter : public Syntax::Highlighter {
public:
    SyntaxHighlighter() = default;
    virtual ~SyntaxHighlighter() override = default;

    virtual Syntax::Language language() const override { return Syntax::Language::HTML; }
    virtual void rehighlight(Palette const&) override;

    static constexpr u64 JS_TOKEN_START_VALUE = 1000;
    static constexpr u64 CSS_TOKEN_START_VALUE = 2000;
};

}
