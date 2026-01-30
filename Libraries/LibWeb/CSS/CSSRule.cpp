/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSRulePrototype.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

CSSRule::CSSRule(JS::Realm& realm, Type type)
    : PlatformObject(realm)
    , m_type(type)
{
}

void CSSRule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_style_sheet);
    visitor.visit(m_parent_rule);
}

// https://www.w3.org/TR/cssom/#dom-cssrule-type
WebIDL::UnsignedShort CSSRule::type_for_bindings() const
{
    // NOTE: Types that aren't defined in the spec must return 0.
    // To do this, we arbitrarily make non-spec ones start at 100.
    auto type = to_underlying(m_type);
    if (type >= 100)
        return 0;
    return type;
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

    if (parent_rule == nullptr)
        set_parent_style_sheet(nullptr);
    else
        set_parent_style_sheet(parent_rule->parent_style_sheet());
    clear_caches();
}

void CSSRule::set_parent_style_sheet(CSSStyleSheet* parent_style_sheet)
{
    m_parent_style_sheet = parent_style_sheet;
    clear_caches();
}

void CSSRule::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("{}:\n", class_name());
}

void CSSRule::clear_caches()
{
    m_cached_layer_name.clear();
}

FlyString CSSRule::parent_layer_internal_qualified_name_slow_case() const
{
    Vector<FlyString> layer_names;
    for (auto* rule = parent_rule(); rule; rule = rule->parent_rule()) {
        switch (rule->type()) {
        case Type::Import:
            // @import is only a parent to style sheets, not to rules directly. It's handled below this loop.
            VERIFY_NOT_REACHED();
            break;

        case Type::LayerBlock: {
            auto& layer_block = as<CSSLayerBlockRule>(*rule);
            layer_names.append(layer_block.internal_name());
            break;
        }

            // Ignore everything else
            // Note that LayerStatement cannot have child rules so we still ignore it here.
        case Type::CounterStyle:
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
        case Type::Page:
        case Type::Margin:
            break;
        }
    }

    // If this style sheet is owned by a rule, include its qualified layer name.
    if (m_parent_style_sheet && m_parent_style_sheet->owner_rule()) {
        if (auto* import = as_if<CSSImportRule>(*m_parent_style_sheet->owner_rule())) {
            // https://drafts.csswg.org/css-cascade-5/#at-import
            // The layer is added to the layer order even if the import fails to load the stylesheet, but is subject to
            // any import conditions (just as if declared by an @layer rule wrapped in the appropriate conditional
            // group rules).
            if (auto layer_name = import->internal_layer_name(); layer_name.has_value() && import->matches()) {
                layer_names.append(layer_name.release_value());
                auto parent_qualified_layer_name = m_parent_style_sheet->owner_rule()->parent_layer_internal_qualified_name();
                if (!parent_qualified_layer_name.is_empty())
                    layer_names.append(move(parent_qualified_layer_name));
            }
        }
    }

    return MUST(String::join('.', layer_names.in_reverse()));
}

}
