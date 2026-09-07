/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/Bindings/CSSRule.h>
#include <LibWeb/CSS/CSSContainerRule.h>
#include <LibWeb/CSS/CSSCounterStyleRule.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSFunctionDeclarations.h>
#include <LibWeb/CSS/CSSFunctionRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSKeyframeRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSNamespaceRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSPageRule.h>
#include <LibWeb/CSS/CSSPropertyRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSScopeRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

CSSRule::~CSSRule() = default;

CSSStyleSheet* CSSRule::parent_style_sheet_for_bindings() const
{
    return m_parent_style_sheet ? &m_parent_style_sheet->cssom_sheet() : nullptr;
}

CSSRule::CSSRule(RustRule rule)
    : m_type(rule.type())
    , m_native_rule(move(rule))
{
    auto payload = m_native_rule.payload();
    if (payload.has_source_position) {
        VERIFY(payload.start_line <= NumericLimits<u32>::max());
        VERIFY(payload.start_column <= NumericLimits<u32>::max());
        m_source_position = SourcePosition { static_cast<u32>(payload.start_line), static_cast<u32>(payload.start_column) };
    }
}

GC::Ref<CSSRule> CSSRule::create(RustRule rule, GC::Ptr<DOM::Document> document)
{
    GC::Ptr<CSSRuleList> children;
    if (auto* native_children = Parser::ValueParserFFI::rust_rule_children(rule.handle()); native_children && rule.type() != Type::Keyframes)
        children = CSSRuleList::create(RustRuleList { Parser::ValueParserFFI::rust_rule_list_retain(native_children) }, document);
    switch (rule.type()) {
    case Type::Style:
        return CSSStyleRule::create(move(rule), *children);
    case Type::Media:
        return CSSMediaRule::create(move(rule), *children);
    case Type::Supports:
        return CSSSupportsRule::create(move(rule), *children);
    case Type::Container:
        return CSSContainerRule::create(move(rule), *children);
    case Type::Scope:
        return CSSScopeRule::create(move(rule), *children);
    case Type::LayerBlock:
        return CSSLayerBlockRule::create(move(rule), *children);
    case Type::Page:
        return CSSPageRule::create(move(rule), *children);
    case Type::Function:
        return CSSFunctionRule::create(move(rule), *children);
    case Type::Import:
        return CSSImportRule::create(*StyleSheetImport::create(move(rule), document));
    case Type::FontFace:
        return CSSFontFaceRule::create(move(rule));
    case Type::Keyframes:
        return CSSKeyframesRule::create(move(rule));
    case Type::Keyframe:
        return CSSKeyframeRule::create(move(rule));
    case Type::Margin:
        return CSSMarginRule::create(move(rule));
    case Type::Namespace:
        return CSSNamespaceRule::create(move(rule));
    case Type::CounterStyle:
        return CSSCounterStyleRule::create(move(rule));
    case Type::FontFeatureValues:
        return CSSFontFeatureValuesRule::create(move(rule));
    case Type::LayerStatement:
        return CSSLayerStatementRule::create(move(rule));
    case Type::NestedDeclarations:
        return CSSNestedDeclarations::create(move(rule));
    case Type::Property:
        return CSSPropertyRule::create(move(rule));
    case Type::FunctionDeclarations:
        return CSSFunctionDeclarations::create(move(rule));
    }
    VERIFY_NOT_REACHED();
}

void CSSRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_style_sheet);
    visitor.visit(m_parent_cssom_sheet);
    visitor.visit(m_parent_rule);
}

size_t CSSRule::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(Base::external_memory_size(), m_native_rule.external_memory_size());
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
Utf16String CSSRule::css_text() const
{
    // The cssText attribute must return a serialization of the CSS rule.
    return serialized();
}

// https://www.w3.org/TR/cssom/#dom-cssrule-csstext
void CSSRule::set_css_text(Utf16View)
{
    // On setting the cssText attribute must do nothing.
}

void CSSRule::set_parent_rule(CSSRule* parent_rule)
{
    clear_caches();
    m_parent_rule = parent_rule;

    if (parent_rule == nullptr)
        set_parent_style_sheet(nullptr);
    else
        set_parent_style_sheet(parent_rule->parent_style_sheet());
    clear_caches();
}

void CSSRule::set_parent_style_sheet(StyleSheetState* parent_style_sheet)
{
    clear_caches();
    m_parent_style_sheet = parent_style_sheet;
    m_parent_cssom_sheet = parent_style_sheet ? &parent_style_sheet->cssom_sheet() : nullptr;
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

Utf16FlyString CSSRule::parent_layer_internal_qualified_name_slow_case() const
{
    Vector<Utf16FlyString> layer_names;
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
        case Type::Container:
        case Type::CounterStyle:
        case Type::LayerStatement:
        case Type::Style:
        case Type::Media:
        case Type::FontFace:
        case Type::FontFeatureValues:
        case Type::Function:
        case Type::FunctionDeclarations:
        case Type::Keyframes:
        case Type::Keyframe:
        case Type::Namespace:
        case Type::Supports:
        case Type::NestedDeclarations:
        case Type::Property:
        case Type::Page:
        case Type::Margin:
        case Type::Scope:
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

    Utf16StringBuilder builder;
    bool first = true;
    for (auto const& layer_name : layer_names.in_reverse()) {
        if (!first)
            builder.append_ascii('.');
        first = false;
        builder.append(layer_name);
    }
    auto qualified_name = builder.to_string();
    return Utf16FlyString::from_utf16(qualified_name.utf16_view());
}

}
