/*
 * Copyright (c) 2020, Hunter Salyer <thefalsehonesty@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/StringBuilder.h>
#include <AK/TypeCasts.h>
#include <LibJS/Lexer.h>
#include <LibJS/MarkupGenerator.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/DatePrototype.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

ErrorOr<String> MarkupGenerator::html_from_source(StringView source)
{
    StringBuilder builder;
    auto lexer = Lexer(source);
    for (auto token = lexer.next(); token.type() != TokenType::Eof; token = lexer.next()) {
        TRY(builder.try_append(token.trivia()));
        TRY(builder.try_append(TRY(wrap_string_in_style(token.value(), style_type_for_token(token)))));
    }
    return builder.to_string();
}

ErrorOr<String> MarkupGenerator::html_from_value(Value value)
{
    StringBuilder output_html;
    HashTable<Object*> seen_objects;
    TRY(value_to_html(value, output_html, seen_objects));
    return output_html.to_string();
}

ErrorOr<String> MarkupGenerator::html_from_error(Error const& object, bool in_promise)
{
    StringBuilder output_html;
    TRY(error_to_html(object, output_html, in_promise));
    return output_html.to_string();
}

ErrorOr<void> MarkupGenerator::value_to_html(Value value, StringBuilder& output_html, HashTable<Object*>& seen_objects)
{
    if (value.is_empty()) {
        TRY(output_html.try_append("&lt;empty&gt;"sv));
        return {};
    }

    if (value.is_object()) {
        if (seen_objects.contains(&value.as_object())) {
            // FIXME: Maybe we should only do this for circular references,
            //        not for all reoccurring objects.
            TRY(output_html.try_appendff("&lt;already printed Object {:p}&gt;", &value.as_object()));
            return {};
        }
        seen_objects.set(&value.as_object());
    }

    if (value.is_object()) {
        auto& object = value.as_object();
        if (is<Array>(object))
            return array_to_html(static_cast<Array const&>(object), output_html, seen_objects);
        TRY(output_html.try_append(TRY(wrap_string_in_style(object.class_name(), StyleType::ObjectType))));
        if (object.is_function())
            return function_to_html(object, output_html, seen_objects);
        if (is<Date>(object))
            return date_to_html(object, output_html, seen_objects);
        return object_to_html(object, output_html, seen_objects);
    }

    if (value.is_string())
        TRY(output_html.try_append(TRY(open_style_type(StyleType::String))));
    else if (value.is_number())
        TRY(output_html.try_append(TRY(open_style_type(StyleType::Number))));
    else if (value.is_boolean() || value.is_nullish())
        TRY(output_html.try_append(TRY(open_style_type(StyleType::KeywordBold))));

    if (value.is_string())
        TRY(output_html.try_append('"'));
    TRY(output_html.try_append(escape_html_entities(value.to_string_without_side_effects())));
    if (value.is_string())
        TRY(output_html.try_append('"'));

    TRY(output_html.try_append("</span>"sv));
    return {};
}

ErrorOr<void> MarkupGenerator::array_to_html(Array const& array, StringBuilder& html_output, HashTable<Object*>& seen_objects)
{
    TRY(html_output.try_append(TRY(wrap_string_in_style("[ "sv, StyleType::Punctuation))));
    bool first = true;
    for (auto it = array.indexed_properties().begin(false); it != array.indexed_properties().end(); ++it) {
        if (!first)
            TRY(html_output.try_append(TRY(wrap_string_in_style(", "sv, StyleType::Punctuation))));
        first = false;
        // FIXME: Exception check
        TRY(value_to_html(array.get(it.index()).release_value(), html_output, seen_objects));
    }
    TRY(html_output.try_append(TRY(wrap_string_in_style(" ]"sv, StyleType::Punctuation))));
    return {};
}

ErrorOr<void> MarkupGenerator::object_to_html(Object const& object, StringBuilder& html_output, HashTable<Object*>& seen_objects)
{
    TRY(html_output.try_append(TRY(wrap_string_in_style("{ "sv, StyleType::Punctuation))));
    bool first = true;
    for (auto& entry : object.indexed_properties()) {
        if (!first)
            TRY(html_output.try_append(TRY(wrap_string_in_style(", "sv, StyleType::Punctuation))));
        first = false;
        TRY(html_output.try_append(TRY(wrap_string_in_style(String::number(entry.index()), StyleType::Number))));
        TRY(html_output.try_append(TRY(wrap_string_in_style(": "sv, StyleType::Punctuation))));
        // FIXME: Exception check
        TRY(value_to_html(object.get(entry.index()).release_value(), html_output, seen_objects));
    }

    if (!object.indexed_properties().is_empty() && object.shape().property_count())
        TRY(html_output.try_append(TRY(wrap_string_in_style(", "sv, StyleType::Punctuation))));

    size_t index = 0;
    for (auto& it : object.shape().property_table()) {
        TRY(html_output.try_append(TRY(wrap_string_in_style(TRY(String::formatted("\"{}\"", escape_html_entities(it.key.to_display_string()))), StyleType::String))));
        TRY(html_output.try_append(TRY(wrap_string_in_style(": "sv, StyleType::Punctuation))));
        TRY(value_to_html(object.get_direct(it.value.offset), html_output, seen_objects));
        if (index != object.shape().property_count() - 1)
            TRY(html_output.try_append(TRY(wrap_string_in_style(", "sv, StyleType::Punctuation))));
        ++index;
    }

    TRY(html_output.try_append(TRY(wrap_string_in_style(" }"sv, StyleType::Punctuation))));
    return {};
}

ErrorOr<void> MarkupGenerator::function_to_html(Object const& function, StringBuilder& html_output, HashTable<Object*>&)
{
    TRY(html_output.try_appendff("[{}]", function.class_name()));
    return {};
}

ErrorOr<void> MarkupGenerator::date_to_html(Object const& date, StringBuilder& html_output, HashTable<Object*>&)
{
    TRY(html_output.try_appendff("Date {}", to_date_string(static_cast<Date const&>(date).date_value())));
    return {};
}

ErrorOr<void> MarkupGenerator::trace_to_html(TracebackFrame const& traceback_frame, StringBuilder& html_output)
{
    auto function_name = escape_html_entities(traceback_frame.function_name);
    auto [line, column, _] = traceback_frame.source_range().start;
    auto get_filename_from_path = [&](StringView filename) -> StringView {
        auto last_slash_index = filename.find_last('/');
        return last_slash_index.has_value() ? filename.substring_view(*last_slash_index + 1) : filename;
    };
    auto filename = escape_html_entities(get_filename_from_path(traceback_frame.source_range().filename()));
    auto trace = TRY(String::formatted("at {} ({}:{}:{})", function_name, filename, line, column));

    TRY(html_output.try_appendff("&nbsp;&nbsp;{}<br>", trace));
    return {};
}

ErrorOr<void> MarkupGenerator::error_to_html(Error const& error, StringBuilder& html_output, bool in_promise)
{
    auto& vm = error.vm();
    auto name = error.get_without_side_effects(vm.names.name).value_or(js_undefined());
    auto message = error.get_without_side_effects(vm.names.message).value_or(js_undefined());
    auto name_string = name.to_string_without_side_effects();
    auto message_string = message.to_string_without_side_effects();
    auto uncaught_message = TRY(String::formatted("Uncaught {}[{}]: ", in_promise ? "(in promise) " : "", name_string));

    TRY(html_output.try_append(TRY(wrap_string_in_style(uncaught_message, StyleType::Invalid))));
    TRY(html_output.try_appendff("{}<br>", message_string.is_empty() ? "\"\"" : escape_html_entities(message_string)));

    for (size_t i = 0; i < error.traceback().size() - min(error.traceback().size(), 3); i++) {
        auto& traceback_frame = error.traceback().at(i);
        TRY(trace_to_html(traceback_frame, html_output));
    }
    return {};
}

StringView MarkupGenerator::style_from_style_type(StyleType type)
{
    switch (type) {
    case StyleType::Invalid:
        return "color: red;"sv;
    case StyleType::String:
        return "color: -libweb-palette-syntax-string;"sv;
    case StyleType::Number:
        return "color: -libweb-palette-syntax-number;"sv;
    case StyleType::KeywordBold:
        return "color: -libweb-palette-syntax-keyword; font-weight: bold;"sv;
    case StyleType::Punctuation:
        return "color: -libweb-palette-syntax-punctuation;"sv;
    case StyleType::Operator:
        return "color: -libweb-palette-syntax-operator;"sv;
    case StyleType::Keyword:
        return "color: -libweb-palette-syntax-keyword;"sv;
    case StyleType::ControlKeyword:
        return "color: -libweb-palette-syntax-control-keyword;"sv;
    case StyleType::Identifier:
        return "color: -libweb-palette-syntax-identifier;"sv;
    case StyleType::ObjectType:
        return "padding: 2px; background-color: #ddf; color: black; font-weight: bold;"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

MarkupGenerator::StyleType MarkupGenerator::style_type_for_token(Token token)
{
    switch (token.category()) {
    case TokenCategory::Invalid:
        return StyleType::Invalid;
    case TokenCategory::Number:
        return StyleType::Number;
    case TokenCategory::String:
        return StyleType::String;
    case TokenCategory::Punctuation:
        return StyleType::Punctuation;
    case TokenCategory::Operator:
        return StyleType::Operator;
    case TokenCategory::Keyword:
        switch (token.type()) {
        case TokenType::BoolLiteral:
        case TokenType::NullLiteral:
            return StyleType::KeywordBold;
        default:
            return StyleType::Keyword;
        }
    case TokenCategory::ControlKeyword:
        return StyleType::ControlKeyword;
    case TokenCategory::Identifier:
        return StyleType::Identifier;
    default:
        dbgln("Unknown style type for token {}", token.name());
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<String> MarkupGenerator::open_style_type(StyleType type)
{
    return String::formatted("<span style=\"{}\">", style_from_style_type(type));
}

ErrorOr<String> MarkupGenerator::wrap_string_in_style(StringView source, StyleType type)
{
    return String::formatted("<span style=\"{}\">{}</span>", style_from_style_type(type), escape_html_entities(source));
}

}
