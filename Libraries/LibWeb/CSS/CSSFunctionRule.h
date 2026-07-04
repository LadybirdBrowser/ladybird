/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/Parser/Syntax.h>

namespace Web::CSS {

// NB: We use this struct internally instead of just using FunctionParameter so we can store the values in more
//     convenient types (i.e. not just strings)
struct FunctionParameterInternal {
    Utf16FlyString name;
    NonnullRefPtr<Parser::SyntaxNode> type;
    RefPtr<StyleValue const> default_value;

    void serialize(Utf16StringBuilder& builder) const;
};

// https://drafts.csswg.org/css-mixins-1/#dictdef-functionparameter
struct FunctionParameter {
    Utf16FlyString name;
    Utf16String type;
    Optional<Utf16String> default_value;

    static FunctionParameter from_internal_function_parameter(FunctionParameterInternal const&);
};

// https://drafts.csswg.org/css-mixins-1/#cssfunctionrule
class CSSFunctionRule : public CSSGroupingRule {
    WEB_PLATFORM_OBJECT(CSSFunctionRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSFunctionRule);

public:
    static GC::Ref<CSSFunctionRule> create(JS::Realm&, CSSRuleList&, Utf16FlyString name, Vector<FunctionParameterInternal> parameters, NonnullRefPtr<Parser::SyntaxNode> return_type);
    virtual ~CSSFunctionRule() override = default;

    virtual void initialize(JS::Realm&) override;

    Utf16FlyString const& qualified_layer_name() const { return parent_layer_internal_qualified_name(); }

    Utf16String name() const;
    Vector<FunctionParameter> get_parameters() const;
    Utf16String return_type() const;

    Utf16String serialized() const override;

    // https://drafts.csswg.org/css-mixins/#calling-context
    struct CallingContext {
        AbstractOrHypotheticalElement& element;
        Utf16View property_or_descriptor_name;

        // NB: This isn't in the spec but we include it here to avoid extra parameters
        ComputedProperties const* computed_style_for_custom_property_resolution;
    };
    NonnullRefPtr<StyleValue const> evaluate_a_custom_function(Parser::GuardedSubstitutionContexts&, Vector<Vector<Parser::ComponentValue>> const& arguments, CallingContext const&) const;

private:
    CSSFunctionRule(JS::Realm&, CSSRuleList&, Utf16FlyString name, Vector<FunctionParameterInternal> parameters, NonnullRefPtr<Parser::SyntaxNode> return_type);

    HashMap<Utf16FlyString, NonnullRefPtr<StyleValue const>> resolve_function_styles(OrderedHashMap<Utf16FlyString, StyleProperty>&& custom_properties, HashMap<Utf16FlyString, CustomPropertyRegistration> const&, CallingContext const&, Parser::GuardedSubstitutionContexts&) const;

    Utf16FlyString m_name;
    Vector<FunctionParameterInternal> m_parameters;
    NonnullRefPtr<Parser::SyntaxNode> m_return_type;
};

}
