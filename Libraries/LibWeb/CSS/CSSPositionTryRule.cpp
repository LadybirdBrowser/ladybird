/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/CSSPositionTryDescriptors.h>
#include <LibWeb/CSS/CSSPositionTryRule.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSPositionTryRule);

GC::Ref<CSSPositionTryRule> CSSPositionTryRule::create(RustRule rule)
{
    return GC::Heap::the().allocate<CSSPositionTryRule>(move(rule));
}

CSSPositionTryRule::CSSPositionTryRule(RustRule rule)
    : CSSRule(move(rule))
    , m_name([&] {
        auto name = native_rule().payload().name;
        auto view = name.ascii ? Utf16View { StringView { reinterpret_cast<char const*>(name.ascii), name.length } } : Utf16View { reinterpret_cast<char16_t const*>(name.utf16), name.length };
        return Utf16FlyString::from_utf16(view);
    }())
    , m_declarations(Parser::ValueParserFFI::rust_declaration_block_retain(native_rule().payload().declarations))
{
}

size_t CSSPositionTryRule::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(Base::external_memory_size(), m_declarations.external_memory_size());
}

// https://drafts.csswg.org/css-anchor-position-1/#dom-csspositiontryrule-name
Utf16String CSSPositionTryRule::name() const
{
    // Its name attribute represents the name declared in the rule's prelude.
    // INTEROP: Other browsers return the name serialized as an identifier.
    return serialize_an_identifier_to_utf16(m_name);
}

// https://drafts.csswg.org/css-anchor-position-1/#dom-csspositiontryrule-style
GC::Ref<CSSPositionTryDescriptors> CSSPositionTryRule::style() const
{
    // On getting, it must return a CSSPositionTryDescriptors object for the @position-try at-rule, with the following
    // properties:
    //   computed flag: Unset
    //   readonly flag: Unset
    //   declarations: The declared descriptors in the rule, in specified order.
    //   parent CSS rule: The context object
    //   owner node: Null
    if (!m_style) {
        m_style = CSSPositionTryDescriptors::create(m_declarations.retain());
        m_style->set_parent_rule(const_cast<CSSPositionTryRule&>(*this));
    }
    return *m_style;
}

Utf16String CSSPositionTryRule::serialized() const
{
    // AD-HOC: There is no spec for serializing CSSPositionTryRule. This matches other browsers.
    Utf16StringBuilder builder;
    builder.append_ascii("@position-try "sv);
    builder.append(name());
    builder.append_ascii(" { "sv);
    if (!m_declarations.is_empty()) {
        builder.append(style()->serialized());
        builder.append_ascii(' ');
    }
    builder.append_ascii('}');
    return builder.to_string();
}

void CSSPositionTryRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
}

void CSSPositionTryRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Name: {}\n", m_name);
    dump_style_properties(builder, style(), indent_levels + 1);
}

}
