/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibSyntax/Highlighter.h>

namespace JS {

class JS_API SyntaxHighlighter : public Syntax::Highlighter {
public:
    SyntaxHighlighter() = default;
    virtual ~SyntaxHighlighter() override = default;

    virtual Syntax::Language language() const override { return Syntax::Language::JavaScript; }
    virtual void rehighlight(Palette const&) override;
};

}
