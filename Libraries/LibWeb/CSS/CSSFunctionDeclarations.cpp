/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFunctionDeclarations.h"
#include <LibGC/Heap.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFunctionDeclarations);

GC::Ref<CSSFunctionDeclarations> CSSFunctionDeclarations::create(Parser::Parser& parser, Vector<Parser::Declaration> const& declarations)
{
    return GC::Heap::the().allocate<CSSFunctionDeclarations>(parser.convert_to_descriptors<CSSFunctionDescriptors>(AtRuleID::Function, declarations));
}

CSSFunctionDeclarations::CSSFunctionDeclarations(GC::Ref<CSSFunctionDescriptors> style)
    : CSSRule(Type::FunctionDeclarations)
    , m_style(style)
{
}

void CSSFunctionDeclarations::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
}

String CSSFunctionDeclarations::serialized() const
{
    // https://drafts.csswg.org/css-mixins-1/#the-function-declarations-interface
    // The CSSFunctionDeclarations rule, like CSSNestedDeclarations, serializes as if its declaration block had been
    // serialized directly.
    return m_style->serialized();
}

void CSSFunctionDeclarations::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_descriptors(builder, m_style, indent_levels + 1);
}

}
