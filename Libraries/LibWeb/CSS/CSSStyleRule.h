/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CSSStyleRule.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/Selector.h>

namespace Web::CSS {

class CSSStyleRule final : public CSSGroupingRule {
    WEB_RUST_INTERFACE(CSSStyleRule, css_style_rule)

public:
    SelectorList selectors() const;
    SelectorList absolutized_selectors() const;
    RustDeclarationBlock declaration() const;

    Utf16String selector_text() const;
    void set_selector_text(Utf16View);

    GC::Ref<CSSStyleProperties> style() const;
};

template<>
inline bool CSSRule::fast_is<CSSStyleRule>() const { return type() == CSSRule::Type::Style; }

}
