/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLayerStatementRule.h"
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/Bindings/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSLayerStatementRule);

GC::Ref<CSSLayerStatementRule> CSSLayerStatementRule::create(RustRule rule)
{
    return GC::Heap::the().allocate<CSSLayerStatementRule>(move(rule));
}

CSSLayerStatementRule::CSSLayerStatementRule(RustRule rule)
    : CSSRule(move(rule))
    , m_names(*native_rule().payload().layer_names)
{
}

Vector<Utf16String> CSSLayerStatementRule::name_list() const
{
    Vector<Utf16String> names;
    auto count = Parser::ValueParserFFI::rust_layer_names_count(&m_names);
    names.ensure_capacity(count);
    for (size_t index = 0; index < count; ++index)
        names.unchecked_append(Utf16String::from_utf16(name_at(index)));
    return names;
}

Utf16View CSSLayerStatementRule::name_at(size_t index) const
{
    auto view = Parser::ValueParserFFI::rust_layer_names_at(&m_names, index);
    return { reinterpret_cast<char16_t const*>(view.utf16), view.length };
}

size_t CSSLayerStatementRule::external_memory_size() const
{
    return Base::external_memory_size() + Parser::ValueParserFFI::rust_layer_names_external_memory_size(&m_names);
}

Utf16String CSSLayerStatementRule::serialized() const
{
    // AD-HOC: No spec yet.
    Utf16StringBuilder builder;
    builder.append_ascii("@layer "sv);
    auto count = Parser::ValueParserFFI::rust_layer_names_count(&m_names);
    for (size_t i = 0; i < count; ++i) {
        if (i > 0)
            builder.append_ascii(", "sv);
        builder.append(name_at(i));
    }
    builder.append_ascii(';');
    return builder.to_string();
}

void CSSLayerStatementRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.append("Names: "sv);
    builder.join(", "sv, name_list());
}

}
