/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2022-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSRulePrototype.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>

namespace Web::CSS {

CSSRule::CSSRule(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void CSSRule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_style_sheet);
    visitor.visit(m_parent_rule);
}

// https://www.w3.org/TR/cssom/#dom-cssrule-csstext
String CSSRule::css_text() const
{
    // The cssText attribute must return a serialization of the CSS rule.
    return serialized();
}

// https://www.w3.org/TR/cssom/#dom-cssrule-csstext
void CSSRule::set_css_text(StringView)
{
    // On setting the cssText attribute must do nothing.
}

void CSSRule::set_parent_rule(CSSRule* parent_rule)
{
    m_parent_rule = parent_rule;
    clear_caches();
}

void CSSRule::set_parent_style_sheet(CSSStyleSheet* parent_style_sheet)
{
    m_parent_style_sheet = parent_style_sheet;
    clear_caches();
}

void CSSRule::clear_caches()
{
    m_cached_layer_name.clear();
}

FlyString const& CSSRule::parent_layer_internal_qualified_name_slow_case() const
{
    Vector<FlyString> layer_names;
    for (auto* rule = parent_rule(); rule; rule = rule->parent_rule()) {
        switch (rule->type()) {
        case Type::Import:
            // TODO: Handle `layer(foo)` in import rules once we implement that.
            break;

        case Type::LayerBlock: {
            auto& layer_block = static_cast<CSSLayerBlockRule const&>(*rule);
            layer_names.append(layer_block.internal_name());
            break;
        }

            // Ignore everything else
            // Note that LayerStatement cannot have child rules so we still ignore it here.
        case Type::LayerStatement:
        case Type::Style:
        case Type::Media:
        case Type::FontFace:
        case Type::Keyframes:
        case Type::Keyframe:
        case Type::Namespace:
        case Type::Supports:
        case Type::NestedDeclarations:
        case Type::Property:
            break;
        }
    }

    m_cached_layer_name = MUST(String::join("."sv, layer_names.in_reverse()));
    return m_cached_layer_name.value();
}

}
