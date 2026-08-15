/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Font/UnicodeRange.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

void escape_a_character(StringBuilder&, u32 character);
void escape_a_character_as_code_point(StringBuilder&, u32 character);
WEB_API void serialize_an_identifier(StringBuilder&, Utf16View ident);
WEB_API void serialize_an_identifier(Utf16StringBuilder&, Utf16View ident);
void serialize_a_string(StringBuilder&, StringView string);
void serialize_a_string(StringBuilder&, Utf16View string);
void serialize_a_string(Utf16StringBuilder&, Utf16View string);
WEB_API void serialize_a_url(StringBuilder&, Utf16View url);
void serialize_a_url(Utf16StringBuilder&, Utf16View url);
WEB_API void serialize_a_number(StringBuilder&, double value);
WEB_API void serialize_a_number(Utf16StringBuilder&, double value);

String serialize_an_identifier(Utf16View ident);
Utf16String serialize_an_identifier_to_utf16(Utf16View ident);
String serialize_a_string(Utf16View string);
String serialize_a_url(Utf16View url);
String serialize_a_number(double value);

Utf16String serialize_a_css_declaration_to_utf16(StringView property, Utf16View value, Important = Important::No);
Utf16String serialize_a_css_declaration_to_utf16(Utf16View property, Utf16View value, Important = Important::No);

void serialize_a_series_of_component_values(Utf16StringBuilder&, ReadonlySpan<Parser::ComponentValue>);
Utf16String serialize_a_series_of_component_values(ReadonlySpan<Parser::ComponentValue>);
String serialize_a_series_of_component_values_preserving_original_source_text(ReadonlySpan<Parser::ComponentValue>);
String serialize_a_positional_value_list(ReadonlySpan<ValueComparingNonnullRefPtr<StyleValue const>> values, SerializationMode mode);

}
