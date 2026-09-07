/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSKeyframeRule.h"
#include <LibGC/Heap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSKeyframeRule);

GC::Ref<CSSKeyframeRule> CSSKeyframeRule::create(RustRule rule)
{
    return GC::Heap::the().allocate<CSSKeyframeRule>(move(rule));
}

CSSKeyframeRule::CSSKeyframeRule(RustRule rule)
    : CSSRule(move(rule))
    , m_frame(*native_rule().payload().keyframe)
    , m_declarations(Parser::ValueParserFFI::rust_keyframe_declarations(&m_frame))
{
}

size_t CSSKeyframeRule::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(Base::external_memory_size(), m_declarations.external_memory_size());
}

GC::Ref<CSSStyleProperties> CSSKeyframeRule::style() const
{
    if (!m_style) {
        m_style = CSSStyleProperties::create(m_declarations.retain());
        m_style->set_parent_rule(const_cast<CSSKeyframeRule&>(*this));
    }
    return *m_style;
}

void CSSKeyframeRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
}

Utf16String CSSKeyframeRule::serialized() const
{
    Utf16StringBuilder builder;
    builder.appendff("{} {{ {} }}", key_text(), style()->serialized());
    return builder.to_string();
}

void CSSKeyframeRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Keys: {}\n"sv, key_text());
    dump_style_properties(builder, style(), indent_levels + 1);
}

}
