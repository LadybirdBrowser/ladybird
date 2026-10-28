/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf8View.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/HTML/Scripting/ConsoleInputCompletion.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

namespace Web::HTML {

// Enough for any list someone reads through, and a bound on the work for an object with a huge number of properties.
static constexpr size_t MAX_MATCHES = 1500;
static constexpr size_t MAX_PROPERTIES_EXAMINED = 100000;

// https://tc39.es/ecma262/#prod-ReservedWord
static constexpr Array RESERVED_WORDS = {
    "await"sv, "break"sv, "case"sv, "catch"sv, "class"sv, "const"sv, "continue"sv, "debugger"sv, "default"sv,
    "delete"sv, "do"sv, "else"sv, "enum"sv, "export"sv, "extends"sv, "false"sv, "finally"sv, "for"sv, "function"sv,
    "if"sv, "import"sv, "in"sv, "instanceof"sv, "new"sv, "null"sv, "return"sv, "super"sv, "switch"sv, "this"sv,
    "throw"sv, "true"sv, "try"sv, "typeof"sv, "var"sv, "void"sv, "while"sv, "with"sv, "yield"sv
};

static bool is_identifier_code_point(u32 code_point)
{
    return is_ascii_alphanumeric(code_point) || code_point == '_' || code_point == '$' || code_point >= 0x80;
}

static String string_from_code_points(ReadonlySpan<u32> code_points)
{
    StringBuilder builder;
    for (auto code_point : code_points)
        builder.append_code_point(code_point);
    return builder.to_string_without_validation();
}

// Whether the input ends inside a string, a template, or a comment, where what's typed isn't a name.
static bool ends_inside_literal_or_comment(ReadonlySpan<u32> code_points)
{
    enum class State : u8 {
        Code,
        SingleQuotedString,
        DoubleQuotedString,
        Template,
        LineComment,
        BlockComment,
    };

    auto state = State::Code;
    for (size_t i = 0; i < code_points.size(); ++i) {
        auto code_point = code_points[i];
        auto next = i + 1 < code_points.size() ? code_points[i + 1] : 0;

        switch (state) {
        case State::Code:
            if (code_point == '\'')
                state = State::SingleQuotedString;
            else if (code_point == '"')
                state = State::DoubleQuotedString;
            else if (code_point == '`')
                state = State::Template;
            else if (code_point == '/' && next == '/')
                state = State::LineComment;
            else if (code_point == '/' && next == '*') {
                state = State::BlockComment;
                ++i;
            }
            break;
        case State::SingleQuotedString:
        case State::DoubleQuotedString:
        case State::Template:
            if (code_point == '\\')
                ++i;
            else if ((state == State::SingleQuotedString && code_point == '\'') || (state == State::DoubleQuotedString && code_point == '"') || (state == State::Template && code_point == '`'))
                state = State::Code;
            break;
        case State::LineComment:
            if (code_point == '\n')
                state = State::Code;
            break;
        case State::BlockComment:
            if (code_point == '*' && next == '/') {
                state = State::Code;
                ++i;
            }
            break;
        }
    }

    return state != State::Code;
}

// The value of the property of `base` named `name`, as reading it would produce, without running any page script: A
// getter defined in script, or a proxy anywhere along the prototype chain, leaves the property unread.
static Optional<JS::Value> read_property_without_running_script(JS::VM& vm, JS::Value base, String const& name)
{
    if (base.is_nullish())
        return {};

    auto object = base.to_object(vm);
    if (object.is_error())
        return {};

    JS::PropertyKey key { Utf16FlyString::from_utf8(name) };
    for (GC::Ptr<JS::Object> holder = object.value(); holder;) {
        if (holder->is_proxy())
            return {};

        auto descriptor = holder->internal_get_own_property(key);
        if (descriptor.is_error())
            return {};

        if (descriptor.value().has_value()) {
            auto const& property = *descriptor.value();
            if (property.is_data_descriptor())
                return property.value.value_or(JS::js_undefined());

            auto getter = property.get.value_or(nullptr);
            if (!getter || !is<JS::NativeFunction>(*getter))
                return {};

            auto value = JS::call(vm, *getter, base);
            if (value.is_error())
                return {};
            return value.value();
        }

        auto prototype = holder->internal_get_prototype_of();
        if (prototype.is_error())
            return {};
        holder = prototype.value();
    }

    return {};
}

// The value of the global named `name`, read without running any page script.
static Optional<JS::Value> read_global_without_running_script(JS::VM& vm, JS::Realm& realm, String const& name)
{
    auto& declarative_record = realm.global_environment().declarative_record();
    auto binding_name = Utf16FlyString::from_utf8(name);
    if (declarative_record.bindings().contains_slow(binding_name)) {
        auto value = declarative_record.get_binding_value(vm, binding_name, false);
        if (value.is_error())
            return {};
        return value.value();
    }

    return read_property_without_running_script(vm, &realm.global_object(), name);
}

// The length of an object whose own keys include one per index, which listing its own keys would make every one of.
static Optional<double> indexed_key_count(JS::VM& vm, JS::Object& object)
{
    auto lists_index_keys = is<JS::Array>(object) || is<JS::TypedArrayBase>(object) || is<JS::StringObject>(object)
        || Bindings::impl_from<DOM::NodeList>(&object) || Bindings::impl_from<DOM::HTMLCollection>(&object);
    if (!lists_index_keys)
        return {};

    auto length = read_property_without_running_script(vm, &object, "length"_string);
    if (!length.has_value() || !length->is_number())
        return {};
    return length->as_double();
}

static bool is_array_index(StringView name)
{
    return !name.is_empty() && all_of(name, is_ascii_digit);
}

Optional<ConsoleInputCompletion> complete_console_input(JS::Realm& realm, StringView input)
{
    auto& vm = realm.vm();
    TemporaryExecutionContext execution_context { realm };

    Vector<u32> code_points;
    for (auto code_point : Utf8View { input })
        code_points.append(code_point);

    if (ends_inside_literal_or_comment(code_points))
        return {};

    // The input ends with the name being typed, perhaps empty after a period, and before it, any names that lead to the
    // object whose properties complete it: "document.body.cla", e.g.
    auto start = code_points.size();
    while (start > 0 && is_identifier_code_point(code_points[start - 1]))
        --start;
    auto match_prop = string_from_code_points(code_points.span().slice(start));

    // Exactly three periods before a name are a spread, as in "[...doc", which the name starts after.
    auto follows_spread = [&](size_t position) {
        return position >= 3 && code_points[position - 1] == '.' && code_points[position - 2] == '.' && code_points[position - 3] == '.'
            && (position == 3 || code_points[position - 4] != '.');
    };

    Vector<String> chain;
    while (start > 0 && code_points[start - 1] == '.' && !follows_spread(start)) {
        auto name_end = start - 1;
        auto name_start = name_end;
        while (name_start > 0 && is_identifier_code_point(code_points[name_start - 1]))
            --name_start;

        // A period after anything but a name, as in "f().x" or "a[0].x", follows a value that only evaluating the input
        // can produce.
        if (name_start == name_end)
            return {};

        chain.prepend(string_from_code_points(code_points.span().slice(name_start, name_end - name_start)));
        start = name_start;
    }

    if (chain.is_empty() && match_prop.is_empty())
        return {};

    auto const& first_name = chain.is_empty() ? match_prop : chain.first();
    if (is_ascii_digit(first_name.bytes_as_string_view()[0]))
        return {};

    JS::Value object = &realm.global_object();
    for (size_t i = 0; i < chain.size(); ++i) {
        auto value = i == 0 ? read_global_without_running_script(vm, realm, chain[i]) : read_property_without_running_script(vm, object, chain[i]);
        if (!value.has_value() || value->is_nullish())
            return {};
        object = *value;
    }

    // A name typed starting in lowercase matches without regard to case, so "doc" finds "Document" as well.
    auto matches_case_insensitively = is_ascii_lower_alpha(match_prop.bytes_as_string_view().is_empty() ? 0 : match_prop.bytes_as_string_view()[0]);
    auto lowercase_match_prop = match_prop.to_ascii_lowercase();
    auto matches_prop = [&](String const& name) {
        if (matches_case_insensitively)
            return name.to_ascii_lowercase().starts_with_bytes(lowercase_match_prop);
        return name.starts_with_bytes(match_prop);
    };

    Vector<String> matches;
    HashTable<String> seen;
    auto add_match = [&](String name) {
        if (matches.size() < MAX_MATCHES && seen.set(name) == HashSetResult::InsertedNewEntry)
            matches.append(move(name));
    };

    auto holder_or_error = object.to_object(vm);
    if (holder_or_error.is_error())
        return {};

    size_t properties_examined = 0;
    for (GC::Ptr<JS::Object> holder = holder_or_error.value(); holder && matches.size() < MAX_MATCHES;) {
        if (holder->is_proxy())
            break;

        // Listing the own keys of an object with more indices than the limit would make a key for each one first, and
        // an index never completes anyway, so its own keys are skipped. Its prototype still offers length.
        if (auto count = indexed_key_count(vm, *holder); count.has_value() && *count > MAX_PROPERTIES_EXAMINED) {
            auto prototype = holder->internal_get_prototype_of();
            if (prototype.is_error())
                break;
            holder = prototype.value();
            continue;
        }

        auto keys = holder->internal_own_property_keys();
        if (keys.is_error())
            break;

        properties_examined += keys.value().size();
        if (properties_examined > MAX_PROPERTIES_EXAMINED)
            break;

        for (auto key : keys.value()) {
            if (!key.is_string())
                continue;
            // An empty name completes nothing.
            auto name = key.as_string().utf16_string_view().to_utf8_but_should_be_ported_to_utf16();
            if (!name.is_empty() && !is_array_index(name) && matches_prop(name))
                add_match(move(name));
        }

        auto prototype = holder->internal_get_prototype_of();
        if (prototype.is_error())
            break;
        holder = prototype.value();
    }

    if (chain.is_empty()) {
        for (auto const& binding : realm.global_environment().declarative_record().bindings()) {
            auto name = binding.view().to_utf8_but_should_be_ported_to_utf16();
            if (matches_prop(name))
                add_match(move(name));
        }

        for (auto reserved_word : RESERVED_WORDS) {
            if (reserved_word.starts_with(match_prop))
                add_match(MUST(String::from_utf8(reserved_word)));
        }
    }

    // Names that start in lowercase come first, then the typed name itself, then the rest without regard to case.
    auto starts_in_uppercase = [](String const& name) { return is_ascii_upper_alpha(name.bytes_as_string_view()[0]); };
    quick_sort(matches, [&](String const& a, String const& b) {
        if (starts_in_uppercase(a) != starts_in_uppercase(b))
            return !starts_in_uppercase(a);
        if ((a == match_prop) != (b == match_prop))
            return a == match_prop;
        auto lowercase_a = a.to_ascii_lowercase();
        auto lowercase_b = b.to_ascii_lowercase();
        if (lowercase_a != lowercase_b)
            return lowercase_a.bytes_as_string_view() < lowercase_b.bytes_as_string_view();
        return a.bytes_as_string_view() > b.bytes_as_string_view();
    });

    return ConsoleInputCompletion { move(matches), move(match_prop) };
}

}
