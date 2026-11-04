/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSGroupingRule : public CSSRule {
    WEB_RUST_INTERFACE(CSSGroupingRule, css_grouping_rule)

public:
    CSSRuleList& css_rules() const;
};

template<>
inline bool CSSRule::fast_is<CSSGroupingRule>() const
{
    switch (type()) {
    case Type::Style:
    case Type::Media:
    case Type::Supports:
    case Type::Container:
    case Type::Scope:
    case Type::LayerBlock:
    case Type::Page:
    case Type::Function:
        return true;
    default:
        return false;
    }
}

}
