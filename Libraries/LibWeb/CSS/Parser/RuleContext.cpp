/*
 * Copyright (c) 2025-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/RuleContext.h>

namespace Web::CSS::Parser {

RuleContext rule_context_type_for_rule(CSSRule::Type rule_type)
{
    switch (rule_type) {
    case CSSRule::Type::Container:
        return RuleContext::AtContainer;
    case CSSRule::Type::CounterStyle:
        return RuleContext::AtCounterStyle;
    case CSSRule::Type::Style:
        return RuleContext::Style;
    case CSSRule::Type::Media:
        return RuleContext::AtMedia;
    case CSSRule::Type::FontFace:
        return RuleContext::AtFontFace;
    case CSSRule::Type::FontFeatureValues:
        return RuleContext::AtFontFeatureValues;
    case CSSRule::Type::Function:
        return RuleContext::AtFunction;
    case CSSRule::Type::Keyframes:
        return RuleContext::AtKeyframes;
    case CSSRule::Type::Keyframe:
        return RuleContext::Keyframe;
    case CSSRule::Type::Supports:
        return RuleContext::AtSupports;
    case CSSRule::Type::Scope:
        return RuleContext::AtScope;
    case CSSRule::Type::LayerBlock:
        return RuleContext::AtLayer;
    case CSSRule::Type::Margin:
        return RuleContext::Margin;
    case CSSRule::Type::NestedDeclarations:
        return RuleContext::Style;
    case CSSRule::Type::Page:
        return RuleContext::AtPage;
    case CSSRule::Type::Property:
        return RuleContext::AtProperty;
        // Other types shouldn't be trying to create a context.
    case CSSRule::Type::Import:
    case CSSRule::Type::LayerStatement:
    case CSSRule::Type::Namespace:
    case CSSRule::Type::FunctionDeclarations:
        break;
    }
    VERIFY_NOT_REACHED();
}

}
