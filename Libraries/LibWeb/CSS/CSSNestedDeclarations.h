/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/Selector.h>

namespace Web::CSS {

class CSSNestedDeclarations final : public CSSRule {
    WEB_RUST_INTERFACE(CSSNestedDeclarations, css_nested_declarations)

public:
    SelectorList absolutized_selectors() const;
    RustDeclarationBlock declaration() const;

    GC::Ref<CSSStyleProperties> style() const;

    CSSStyleRule const& parent_style_rule() const;
};

template<>
inline bool CSSRule::fast_is<CSSNestedDeclarations>() const { return type() == CSSRule::Type::NestedDeclarations; }

}
