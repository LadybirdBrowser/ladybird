/*
 * Copyright (c) 2020, Hunter Salyer <thefalsehonesty@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Error.h>

namespace JS {

class MarkupGenerator {
public:
    static ErrorOr<String> html_from_source(StringView);
    static ErrorOr<String> html_from_value(Value);
    static ErrorOr<String> html_from_error(Error const&, bool);

private:
    enum class StyleType {
        Invalid,
        String,
        Number,
        KeywordBold,
        Punctuation,
        Operator,
        Keyword,
        ControlKeyword,
        Identifier,
        ObjectType,
    };

    static ErrorOr<void> value_to_html(Value, StringBuilder& output_html, HashTable<Object*>& seen_objects);
    static ErrorOr<void> array_to_html(Array const&, StringBuilder& output_html, HashTable<Object*>&);
    static ErrorOr<void> object_to_html(Object const&, StringBuilder& output_html, HashTable<Object*>&);
    static ErrorOr<void> function_to_html(Object const&, StringBuilder& output_html, HashTable<Object*>&);
    static ErrorOr<void> date_to_html(Object const&, StringBuilder& output_html, HashTable<Object*>&);
    static ErrorOr<void> error_to_html(Error const&, StringBuilder& output_html, bool in_promise);
    static ErrorOr<void> trace_to_html(TracebackFrame const&, StringBuilder& output_html);

    static StringView style_from_style_type(StyleType);
    static StyleType style_type_for_token(Token);
    static ErrorOr<String> open_style_type(StyleType type);
    static ErrorOr<String> wrap_string_in_style(StringView source, StyleType type);
};

}
