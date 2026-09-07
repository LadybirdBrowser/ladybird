/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/Dump.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMarginRule);

GC::Ref<CSSMarginRule> CSSMarginRule::create(RustRule rule)
{
    return GC::Heap::the().allocate<CSSMarginRule>(move(rule));
}

CSSMarginRule::CSSMarginRule(RustRule rule)
    : CSSRule(move(rule))
    , m_name([&] {
        auto name = native_rule().payload().name;
        auto view = name.ascii ? Utf16View { StringView { reinterpret_cast<char const*>(name.ascii), name.length } } : Utf16View { reinterpret_cast<char16_t const*>(name.utf16), name.length };
        return Utf16FlyString::from_utf16(view);
    }())
    , m_declarations(Parser::ValueParserFFI::rust_declaration_block_retain(native_rule().payload().declarations))
{
}

size_t CSSMarginRule::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(Base::external_memory_size(), m_declarations.external_memory_size());
}

GC::Ref<CSSStyleProperties> CSSMarginRule::style() const
{
    if (!m_style) {
        m_style = CSSStyleProperties::create(m_declarations.retain());
        m_style->set_parent_rule(const_cast<CSSMarginRule&>(*this));
    }
    return *m_style;
}

Utf16String CSSMarginRule::serialized() const
{
    // AD-HOC: There is currently no spec for serializing CSSMarginRule.
    Utf16StringBuilder builder;
    builder.appendff("@{} {{ ", m_name);
    if (!m_declarations.is_empty()) {
        builder.append(style()->serialized());
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
