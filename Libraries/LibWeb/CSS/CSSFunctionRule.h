/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/Parser/Syntax.h>

namespace Web::CSS {

// NB: We use this struct internally instead of just using FunctionParameter so we can store the values in more
//     convenient types (i.e. not just strings)
struct FunctionParameterInternal {
    FlyString name;
    NonnullOwnPtr<Parser::SyntaxNode> type;
    Optional<Vector<Parser::ComponentValue>> default_value;

    void serialize(StringBuilder& builder) const;
};

// https://drafts.csswg.org/css-mixins-1/#dictdef-functionparameter
struct FunctionParameter {
    FlyString name;
    String type;
    Optional<String> default_value;

    static FunctionParameter from_internal_function_parameter(FunctionParameterInternal const&);
};

// https://drafts.csswg.org/css-mixins-1/#cssfunctionrule
class CSSFunctionRule : public CSSGroupingRule {
    WEB_PLATFORM_OBJECT(CSSFunctionRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSFunctionRule);

public:
    static GC::Ref<CSSFunctionRule> create(JS::Realm&, CSSRuleList&, FlyString name, Vector<FunctionParameterInternal> parameters, NonnullOwnPtr<Parser::SyntaxNode> return_type);
    virtual ~CSSFunctionRule() override = default;

    virtual void initialize(JS::Realm&) override;

    FlyString name() const { return m_name; }
    Vector<FunctionParameter> get_parameters() const;
    String return_type() const;

    String serialized() const override;

private:
    CSSFunctionRule(JS::Realm&, CSSRuleList&, FlyString name, Vector<FunctionParameterInternal> parameters, NonnullOwnPtr<Parser::SyntaxNode> return_type);

    FlyString m_name;
    Vector<FunctionParameterInternal> m_parameters;
    NonnullOwnPtr<Parser::SyntaxNode> m_return_type;
};

}
