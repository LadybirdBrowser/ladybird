/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSFunctionDescriptors.h>
#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-mixins-1/#cssfunctiondeclarations
class CSSFunctionDeclarations final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSFunctionDeclarations, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSFunctionDeclarations);

public:
    [[nodiscard]] static GC::Ref<CSSFunctionDeclarations> create(JS::Realm&, Parser::Parser&, Vector<Parser::Declaration> const&);

    virtual ~CSSFunctionDeclarations() override = default;

    GC::Ref<CSSFunctionDescriptors> style() { return m_style; }

private:
    CSSFunctionDeclarations(JS::Realm&, GC::Ref<CSSFunctionDescriptors>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    GC::Ref<CSSFunctionDescriptors> m_style;
};

template<>
inline bool CSSRule::fast_is<CSSFunctionDeclarations>() const { return type() == CSSRule::Type::FunctionDeclarations; }

}
