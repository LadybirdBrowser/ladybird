/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLayerBlockRule.h"
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/Bindings/CSSLayerBlockRule.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSLayerBlockRule);

GC::Ref<CSSLayerBlockRule> CSSLayerBlockRule::create(RustRule rule, CSSRuleList& rules)
{
    return GC::Heap::the().allocate<CSSLayerBlockRule>(move(rule), rules);
}

CSSLayerBlockRule::CSSLayerBlockRule(RustRule rule, CSSRuleList& rules)
    : CSSGroupingRule(rules, move(rule))
    , m_names(*native_rule().payload().layer_names)
{
    VERIFY(Parser::ValueParserFFI::rust_layer_names_count(&m_names) == 1);
}

Utf16View CSSLayerBlockRule::name() const
{
    auto view = Parser::ValueParserFFI::rust_layer_names_at(&m_names, 0);
    return { reinterpret_cast<char16_t const*>(view.utf16), view.length };
}

Utf16FlyString const& CSSLayerBlockRule::internal_name() const
{
    return m_name_internal.ensure([&] {
        auto name = native_rule().internal_layer_name().release_value();
        return Utf16FlyString::from_utf16(name.utf16_view());
    });
}

size_t CSSLayerBlockRule::external_memory_size() const
{
    return Base::external_memory_size() + Parser::ValueParserFFI::rust_layer_names_external_memory_size(&m_names);
}

Utf16String CSSLayerBlockRule::serialized() const
{
    // AD-HOC: No spec yet, so this is based on the @media serialization algorithm.
    Utf16StringBuilder builder;
    builder.append_ascii("@layer"sv);
    if (!name().is_empty())
        builder.appendff(" {}", name());

    builder.append_ascii(" {\n"sv);
    // AD-HOC: All modern browsers omit the ending newline if there are no CSS rules, so let's do the same.
    if (css_rules().length() == 0) {
        builder.append_ascii('}');
        return builder.to_string();
    }

    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        if (i != 0)
            builder.append_ascii("\n"sv);
        builder.append_ascii("  "sv);
        builder.append(rule->serialized());
    }

    builder.append_ascii("\n}"sv);

    return builder.to_string();
}

void CSSLayerBlockRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Name: `{}` (internal `{}`)\n", name(), internal_name());
    dump_indent(builder, indent_levels + 1);
    builder.appendff("Rules ({}):\n", css_rules().length());
    for (auto& rule : css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

}
