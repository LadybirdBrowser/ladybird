/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFunctionDeclarations.h"
#include <LibWeb/Bindings/CSSFunctionDeclarationsPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFunctionDeclarations);

GC::Ref<CSSFunctionDeclarations> CSSFunctionDeclarations::create(JS::Realm& realm, Parser::Parser& parser, Vector<Parser::Declaration> const& declarations)
{
    return realm.create<CSSFunctionDeclarations>(realm, parser.convert_to_descriptors<CSSFunctionDescriptors>(AtRuleID::Function, declarations));
}

CSSFunctionDeclarations::CSSFunctionDeclarations(JS::Realm& realm, GC::Ref<CSSFunctionDescriptors> style)
    : CSSRule(realm, Type::FunctionDeclarations)
    , m_style(style)
{
}

void CSSFunctionDeclarations::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSFunctionDeclarations);
    Base::initialize(realm);
}

void CSSFunctionDeclarations::visit_edges(Cell::Visitor& visitor)
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
