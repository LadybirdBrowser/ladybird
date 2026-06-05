/*
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibSyntax/Highlighter.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API SyntaxHighlighter : public Syntax::Highlighter {
public:
    SyntaxHighlighter() = default;
    virtual ~SyntaxHighlighter() override = default;

    virtual Syntax::Language language() const override { return Syntax::Language::CSS; }
    virtual void rehighlight(Palette const&) override;
};

}
