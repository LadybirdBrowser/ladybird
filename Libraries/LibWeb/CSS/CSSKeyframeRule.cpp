/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSKeyframeRule.h"
#include <LibWeb/Bindings/CSSKeyframeRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/Dump.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSKeyframeRule);

GC::Ref<CSSKeyframeRule> CSSKeyframeRule::create(JS::Realm& realm, Percentage key, CSSStyleProperties& declarations)
{
    return realm.create<CSSKeyframeRule>(realm, key, declarations);
}

CSSKeyframeRule::CSSKeyframeRule(JS::Realm& realm, Percentage key, CSSStyleProperties& declarations)
    : CSSRule(realm, Type::Keyframe)
    , m_key(key)
    , m_declarations(declarations)
{
    m_declarations->set_parent_rule(*this);
}

void CSSKeyframeRule::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_declarations);
}

void CSSKeyframeRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSKeyframeRule);
    Base::initialize(realm);
}

// https://drafts.csswg.org/css-animations/#dom-csskeyframerule-keytext
WebIDL::ExceptionOr<void> CSSKeyframeRule::set_key_text(String const& key_text)
{
    // On setting, if the value does not match the <keyframe-selector> grammar, throw a SyntaxError.
    auto trimmed = key_text.bytes_as_string_view().trim_whitespace();

    Optional<Percentage> new_key;

    if (trimmed.equals_ignoring_ascii_case("from"sv)) {
        new_key = Percentage(0);
    } else if (trimmed.equals_ignoring_ascii_case("to"sv)) {
        new_key = Percentage(100);
    } else if (trimmed.ends_with('%')) {
        auto number_part = trimmed.substring_view(0, trimmed.length() - 1);
        auto maybe_value = number_part.to_number<double>();
        if (maybe_value.has_value() && *maybe_value >= 0.0 && *maybe_value <= 100.0)
            new_key = Percentage(*maybe_value);
    }

    if (!new_key.has_value())
        return WebIDL::SyntaxError::create(realm(), "Invalid keyframe selector"_utf16);

    m_key = *new_key;
    return {};
}

String CSSKeyframeRule::serialized() const
{
    StringBuilder builder;
    builder.appendff("{}% {{ {} }}", key().value(), style()->serialized());
    return MUST(builder.to_string());
}

void CSSKeyframeRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Key: {}\n"sv, key_text());
    dump_style_properties(builder, style(), indent_levels + 1);
}

}
