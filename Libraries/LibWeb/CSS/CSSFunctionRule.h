/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/CSSGroupingRule.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-mixins-1/#dictdef-functionparameter
struct FunctionParameter {
    Utf16FlyString name;
    Utf16String type;
    Optional<Utf16String> default_value;

    static FunctionParameter from_native_parameter(Parser::ValueParserFFI::FfiFunctionParameterView const&);
};

// https://drafts.csswg.org/css-mixins-1/#cssfunctionrule
class CSSFunctionRule : public CSSGroupingRule {
    WEB_WRAPPABLE(CSSFunctionRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSFunctionRule);

public:
    static GC::Ref<CSSFunctionRule> create(RustRule, CSSRuleList&);
    virtual ~CSSFunctionRule() override = default;

    Utf16String name() const;
    Vector<FunctionParameter> get_parameters() const;
    Utf16String return_type() const;

    Utf16String serialized() const override;

private:
    CSSFunctionRule(RustRule, CSSRuleList&);

    Parser::ValueParserFFI::FunctionSignature const& m_signature;
};

}
