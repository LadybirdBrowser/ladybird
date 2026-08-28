/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/Dump.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMarginRule);

GC::Ref<CSSMarginRule> CSSMarginRule::create(Utf16FlyString name, GC::Ref<CSSStyleProperties> style)
{
    return GC::Heap::the().allocate<CSSMarginRule>(move(name), style);
}

CSSMarginRule::CSSMarginRule(Utf16FlyString name, GC::Ref<CSSStyleProperties> style)
    : CSSRule(Type::Margin)
    , m_name(name.to_ascii_lowercase())
    , m_style(style)
{
    m_style->set_parent_rule(*this);
}

Utf16String CSSMarginRule::serialized() const
{
    // AD-HOC: There is currently no spec for serializing CSSMarginRule.
    Utf16StringBuilder builder;
    builder.appendff("@{} {{ ", m_name);
    if (m_style->length() > 0) {
        builder.append(m_style->serialized());
        builder.append_ascii(' ');
    }
    builder.append_ascii('}');

    return builder.to_string();
}

void CSSMarginRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
}

void CSSMarginRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Name: {}\n", name());
    dump_style_properties(builder, style(), indent_levels + 1);
}

}
