/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSNestedDeclarations.h"
#include <LibWeb/CSS/CSSStyleRule.h>

namespace Web::CSS {

SelectorList CSSNestedDeclarations::absolutized_selectors() const
{
    return matching_selectors_for_rule(native_rule());
}

RustDeclarationBlock CSSNestedDeclarations::declaration() const
{
    return RustDeclarationBlock { Parser::ValueParserFFI::rust_declaration_block_retain(native_rule().payload().declarations) };
}

GC::Ref<CSSStyleProperties> CSSNestedDeclarations::style() const
{
    return static_cast<CSSStyleProperties&>(*declarations());
}

CSSStyleRule const& CSSNestedDeclarations::parent_style_rule() const
{
    for (auto* parent = parent_rule(); parent; parent = parent->parent_rule()) {
        if (auto* style_rule = as_if<CSSStyleRule>(parent))
            return *style_rule;
    }

    dbgln("CSSNestedDeclarations has no parent style rule!");
    VERIFY_NOT_REACHED();
}

}
