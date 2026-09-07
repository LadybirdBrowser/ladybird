/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFunctionDeclarations.h"
#include <LibGC/Heap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFunctionDeclarations);

GC::Ref<CSSFunctionDeclarations> CSSFunctionDeclarations::create(Parser::Parser&, Parser::DeclarationList const& declarations)
{
    return GC::Heap::the().allocate<CSSFunctionDeclarations>(declarations.descriptors().share());
}

CSSFunctionDeclarations::CSSFunctionDeclarations(RustDescriptorBlock descriptors)
    : CSSRule(Type::FunctionDeclarations)
    , m_descriptors(move(descriptors))
{
}

size_t CSSFunctionDeclarations::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(Base::external_memory_size(), m_descriptors.external_memory_size());
}

GC::Ref<CSSFunctionDescriptors> CSSFunctionDeclarations::style() const
{
    if (!m_style) {
        m_style = CSSFunctionDescriptors::create(m_descriptors.retain());
        m_style->set_parent_rule(const_cast<CSSFunctionDeclarations&>(*this));
    }
    return *m_style;
}

void CSSFunctionDeclarations::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
}

Utf16String CSSFunctionDeclarations::serialized() const
{
    // https://drafts.csswg.org/css-mixins-1/#the-function-declarations-interface
    // The CSSFunctionDeclarations rule, like CSSNestedDeclarations, serializes as if its declaration block had been
    // serialized directly.
    return style()->serialized();
}

void CSSFunctionDeclarations::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_descriptors(builder, style(), indent_levels + 1);
}

}
