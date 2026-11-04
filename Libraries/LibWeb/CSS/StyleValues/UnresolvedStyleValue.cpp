/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/ConservativeVector.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

namespace Web::CSS {

static StyleValueFFI::StyleValueData const* create_rust_style_value_from_source(Utf16String const& token_source, Parser::SubstitutionFunctionsPresence substitution_presence, UnresolvedStyleValue::SourceTextMode source_text_mode, bool contains_attr_tainted_values, StyleValue const* parsed_value)
{
    auto token_source_view = token_source.utf16_view();
    return StyleValueFFI::rust_style_value_create_unresolved_from_source(
        token_source_view.has_latin1_storage() ? token_source_view.latin1_span().data() : nullptr,
        token_source_view.has_latin1_storage() ? nullptr : reinterpret_cast<u16 const*>(token_source_view.utf16_span().data()),
        token_source_view.length_in_code_units(),
        to_underlying(source_text_mode),
        substitution_presence.attr,
        substitution_presence.dashed_function,
        substitution_presence.env,
        substitution_presence.if_,
        substitution_presence.inherit,
        substitution_presence.var,
        contains_attr_tainted_values,
        parsed_value ? StyleValueFFI::rust_style_value_retain(parsed_value->rust_style_value_data()) : nullptr);
}

ValueComparingNonnullRefPtr<UnresolvedStyleValue const> UnresolvedStyleValue::create_internal(Utf16String token_source, Parser::SubstitutionFunctionsPresence substitution_presence, SourceTextMode source_text_mode, bool contains_attr_tainted_values, RefPtr<StyleValue const> parsed_value)
{
    auto* data = create_rust_style_value_from_source(token_source, substitution_presence, source_text_mode, contains_attr_tainted_values, parsed_value.ptr());
    return adopt_ref(*new (nothrow) UnresolvedStyleValue(data));
}

ValueComparingNonnullRefPtr<UnresolvedStyleValue const> UnresolvedStyleValue::create(Utf16String token_source, Parser::SubstitutionFunctionsPresence substitution_presence, SourceTextMode source_text_mode, bool contains_attr_tainted_values)
{
    return create_internal(move(token_source), substitution_presence, source_text_mode, contains_attr_tainted_values, nullptr);
}

ValueComparingNonnullRefPtr<UnresolvedStyleValue const> UnresolvedStyleValue::create_attr_tainted_with_parsed_value(Utf16String token_source, Parser::SubstitutionFunctionsPresence substitution_presence, SourceTextMode source_text_mode, NonnullRefPtr<StyleValue const> parsed_value)
{
    VERIFY(!parsed_value->is_unresolved());
    return create_internal(move(token_source), substitution_presence, source_text_mode, true, move(parsed_value));
}

}
