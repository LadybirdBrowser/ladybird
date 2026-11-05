/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

static StyleValueFFI::FfiUtf16View ffi_view(Utf16View view)
{
    return {
        .latin1 = view.has_latin1_storage() ? view.latin1_span().data() : nullptr,
        .utf16 = view.has_latin1_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

Utf16String serialize_an_identifier_to_utf16(Utf16View ident)
{
    return Utf16String::adopt_raw(StyleValueFFI::rust_serialize_an_identifier(ffi_view(ident)));
}

static Utf16String serialize_a_string_to_utf16(Utf16View string)
{
    return Utf16String::adopt_raw(StyleValueFFI::rust_serialize_a_string(ffi_view(string)));
}

static Utf16String serialize_a_url_to_utf16(Utf16View url)
{
    return Utf16String::adopt_raw(StyleValueFFI::rust_serialize_a_url(ffi_view(url)));
}

static Utf16String serialize_a_number_to_utf16(double value)
{
    return Utf16String::adopt_raw(StyleValueFFI::rust_serialize_a_number(value));
}

void serialize_an_identifier(StringBuilder& builder, Utf16View ident)
{
    builder.append(serialize_an_identifier_to_utf16(ident));
}

void serialize_an_identifier(Utf16StringBuilder& builder, Utf16View ident)
{
    builder.append(serialize_an_identifier_to_utf16(ident));
}

void serialize_a_string(StringBuilder& builder, Utf16View string)
{
    builder.append(serialize_a_string_to_utf16(string));
}

void serialize_a_string(Utf16StringBuilder& builder, Utf16View string)
{
    builder.append(serialize_a_string_to_utf16(string));
}

void serialize_a_url(StringBuilder& builder, Utf16View url)
{
    builder.append(serialize_a_url_to_utf16(url));
}

void serialize_a_url(Utf16StringBuilder& builder, Utf16View url)
{
    builder.append(serialize_a_url_to_utf16(url));
}

void serialize_a_number(StringBuilder& builder, double value)
{
    builder.append(serialize_a_number_to_utf16(value));
}

void serialize_a_number(Utf16StringBuilder& builder, double value)
{
    builder.append(serialize_a_number_to_utf16(value));
}

String serialize_an_identifier(Utf16View ident)
{
    return serialize_an_identifier_to_utf16(ident).to_utf8();
}

String serialize_a_string(Utf16View string)
{
    return serialize_a_string_to_utf16(string).to_utf8();
}

String serialize_a_url(Utf16View url)
{
    return serialize_a_url_to_utf16(url).to_utf8();
}

}
