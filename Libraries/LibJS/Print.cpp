/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Concepts.h>
#include <AK/Stream.h>
#include <LibJS/Print.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/AsyncGenerator.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/DatePrototype.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GeneratorObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/Collator.h>
#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/Intl/DisplayNames.h>
#include <LibJS/Runtime/Intl/DurationFormat.h>
#include <LibJS/Runtime/Intl/ListFormat.h>
#include <LibJS/Runtime/Intl/Locale.h>
#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/Intl/PluralRules.h>
#include <LibJS/Runtime/Intl/RelativeTimeFormat.h>
#include <LibJS/Runtime/Intl/Segmenter.h>
#include <LibJS/Runtime/Intl/Segments.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/ProxyObject.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/ShadowRealm.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/StringPrototype.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/WeakMap.h>
#include <LibJS/Runtime/WeakRef.h>
#include <LibJS/Runtime/WeakSet.h>

namespace {

static ErrorOr<String> escape_for_string_literal(StringView string)
{
    StringBuilder builder;
    for (auto byte : string.bytes()) {
        switch (byte) {
        case '\r':
            TRY(builder.try_append("\\r"sv));
            continue;
        case '\v':
            TRY(builder.try_append("\\v"sv));
            continue;
        case '\f':
            TRY(builder.try_append("\\f"sv));
            continue;
        case '\b':
            TRY(builder.try_append("\\b"sv));
            continue;
        case '\n':
            TRY(builder.try_append("\\n"sv));
            continue;
        case '\\':
            TRY(builder.try_append("\\\\"sv));
            continue;
        default:
            TRY(builder.try_append(byte));
            continue;
        }
    }

    return builder.to_string();
}

ErrorOr<void> print_value(JS::PrintContext&, JS::Value value, HashTable<JS::Object*>& seen_objects);

template<typename T>
ErrorOr<void> print_value(JS::PrintContext& print_context, JS::ThrowCompletionOr<T> value_or_error, HashTable<JS::Object*>& seen_objects)
{
    if (value_or_error.is_error()) {
        auto error = value_or_error.release_error();

        // We can't explicitly check for OOM because InternalError does not store the ErrorType
        VERIFY(error.value().has_value());
        VERIFY(error.value()->is_object());
        VERIFY(is<JS::InternalError>(error.value()->as_object()));

        return Error::from_errno(ENOMEM);
    }

    return print_value(print_context, value_or_error.release_value(), seen_objects);
}

ErrorOr<String> strip_ansi(StringView format_string)
{
    if (format_string.is_empty())
        return String();

    StringBuilder builder;
    size_t i;
    for (i = 0; i < format_string.length() - 1; ++i) {
        if (format_string[i] == '\033' && format_string[i + 1] == '[') {
            while (i < format_string.length() && format_string[i] != 'm')
                ++i;
        } else {
            TRY(builder.try_append(format_string[i]));
        }
    }
    if (i < format_string.length())
        TRY(builder.try_append(format_string[i]));
    return builder.to_string();
}

template<typename... Args>
ErrorOr<void> js_out(JS::PrintContext& print_context, CheckedFormatString<Args...> format_string, Args const&... args)
{
    if (print_context.strip_ansi) {
        auto format_string_without_ansi = TRY(strip_ansi(format_string.view()));
        TRY(print_context.stream.write_formatted(format_string_without_ansi, args...));
    } else {
        TRY(print_context.stream.write_formatted(format_string.view(), args...));
    }

    return {};
}

ErrorOr<void> print_type(JS::PrintContext& print_context, StringView name)
{
    return js_out(print_context, "[\033[36;1m{}\033[0m]", name);
}

ErrorOr<void> print_separator(JS::PrintContext& print_context, bool& first)
{
    TRY(js_out(print_context, first ? " "sv : ", "sv));
    first = false;
    return {};
}

ErrorOr<void> print_array(JS::PrintContext& print_context, JS::Array const& array, HashTable<JS::Object*>& seen_objects)
{
    TRY(js_out(print_context, "["));
    bool first = true;
    size_t printed_count = 0;
    for (auto it = array.indexed_properties().begin(false); it != array.indexed_properties().end(); ++it) {
        TRY(print_separator(print_context, first));
        auto value_or_error = array.get(it.index());
        // The V8 repl doesn't throw an exception here, and instead just
        // prints 'undefined'. We may choose to replicate that behavior in
        // the future, but for now lets just catch the error
        if (value_or_error.is_error())
            return {};
        auto value = value_or_error.release_value();
        TRY(print_value(print_context, value, seen_objects));
        if (++printed_count > 100 && it != array.indexed_properties().end()) {
            TRY(js_out(print_context, ", ..."));
            break;
        }
    }
    if (!first)
        TRY(js_out(print_context, " "));
    TRY(js_out(print_context, "]"));
    return {};
}

ErrorOr<void> print_object(JS::PrintContext& print_context, JS::Object const& object, HashTable<JS::Object*>& seen_objects)
{
    TRY(js_out(print_context, "{}{{", object.class_name()));
    bool first = true;
    static constexpr size_t max_number_of_new_objects = 20; // Arbitrary limit
    size_t original_num_seen_objects = seen_objects.size();

    auto maybe_completion = object.enumerate_object_properties([&](JS::Value property_key) -> Optional<JS::Completion> {
        // The V8 repl doesn't throw an exception on accessing properties, and instead just
        // prints 'undefined'. We may choose to replicate that behavior in
        // the future, but for now lets just catch the error
        auto error = print_separator(print_context, first);
        if (error.is_error())
            return JS::js_undefined();
        error = js_out(print_context, "\033[33;1m");
        if (error.is_error())
            return JS::js_undefined();
        error = print_value(print_context, property_key, seen_objects);
        // NOTE: Ignore this error to always print out "reset" ANSI sequence
        error = js_out(print_context, "\033[0m: ");
        if (error.is_error())
            return JS::js_undefined();
        auto maybe_property_key = JS::PropertyKey::from_value(print_context.vm, property_key);
        if (maybe_property_key.is_error())
            return JS::js_undefined();
        auto value_or_error = object.get(maybe_property_key.value());
        if (value_or_error.is_error())
            return JS::js_undefined();
        auto value = value_or_error.release_value();
        error = print_value(print_context, value, seen_objects);
        // FIXME: Come up with a better way to structure the data so that we don't care about this limit
        if (seen_objects.size() > original_num_seen_objects + max_number_of_new_objects)
            return JS::js_undefined(); // Stop once we've seen a ton of objects, to prevent spamming the console.
        if (error.is_error())
            return JS::js_undefined();
        return {};
    });
    // Swallow Error/undefined from printing properties
    if (maybe_completion.has_value())
        return {};

    if (!first)
        TRY(js_out(print_context, " "));
    TRY(js_out(print_context, "}}"));

    return {};
}

ErrorOr<void> print_function(JS::PrintContext& print_context, JS::FunctionObject const& function_object, HashTable<JS::Object*>&)
{
    if (is<JS::ECMAScriptFunctionObject>(function_object)) {
        auto const& ecmascript_function_object = static_cast<JS::ECMAScriptFunctionObject const&>(function_object);
        switch (ecmascript_function_object.kind()) {
        case JS::FunctionKind::Normal:
            TRY(print_type(print_context, "Function"sv));
            break;
        case JS::FunctionKind::Generator:
            TRY(print_type(print_context, "GeneratorFunction"sv));
            break;
        case JS::FunctionKind::Async:
            TRY(print_type(print_context, "AsyncFunction"sv));
            break;
        case JS::FunctionKind::AsyncGenerator:
            TRY(print_type(print_context, "AsyncGeneratorFunction"sv));
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    } else {
        TRY(print_type(print_context, function_object.class_name()));
    }
    if (is<JS::ECMAScriptFunctionObject>(function_object))
        TRY(js_out(print_context, " {}", static_cast<JS::ECMAScriptFunctionObject const&>(function_object).name()));
    else if (is<JS::NativeFunction>(function_object))
        TRY(js_out(print_context, " {}", static_cast<JS::NativeFunction const&>(function_object).name()));
    return {};
}

ErrorOr<void> print_date(JS::PrintContext& print_context, JS::Date const& date, HashTable<JS::Object*>&)
{
    TRY(print_type(print_context, "Date"sv));
    TRY(js_out(print_context, " \033[34;1m{}\033[0m", JS::to_date_string(date.date_value())));
    return {};
}

ErrorOr<void> print_error(JS::PrintContext& print_context, JS::Object const& object, HashTable<JS::Object*>& seen_objects)
{
    auto name = object.get_without_side_effects(print_context.vm.names.name).value_or(JS::js_undefined());
    auto message = object.get_without_side_effects(print_context.vm.names.message).value_or(JS::js_undefined());
    if (name.is_accessor() || message.is_accessor()) {
        TRY(print_value(print_context, &object, seen_objects));
    } else {
        auto name_string = name.to_string_without_side_effects();
        auto message_string = message.to_string_without_side_effects();
        TRY(print_type(print_context, name_string));
        if (!message_string.is_empty())
            TRY(js_out(print_context, " \033[31;1m{}\033[0m", message_string));
    }
    return {};
}

ErrorOr<void> print_regexp_object(JS::PrintContext& print_context, JS::RegExpObject const& regexp_object, HashTable<JS::Object*>&)
{
    TRY(print_type(print_context, "RegExp"sv));
    TRY(js_out(print_context, " \033[34;1m/{}/{}\033[0m", regexp_object.escape_regexp_pattern(), regexp_object.flags()));
    return {};
}

ErrorOr<void> print_proxy_object(JS::PrintContext& print_context, JS::ProxyObject const& proxy_object, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Proxy"sv));
    TRY(js_out(print_context, "\n  target: "));
    TRY(print_value(print_context, &proxy_object.target(), seen_objects));
    TRY(js_out(print_context, "\n  handler: "));
    TRY(print_value(print_context, &proxy_object.handler(), seen_objects));
    return {};
}

ErrorOr<void> print_map(JS::PrintContext& print_context, JS::Map const& map, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Map"sv));
    TRY(js_out(print_context, " {{"));
    bool first = true;
    for (auto const& entry : map) {
        TRY(print_separator(print_context, first));
        TRY(print_value(print_context, entry.key, seen_objects));
        TRY(js_out(print_context, " => "));
        TRY(print_value(print_context, entry.value, seen_objects));
    }
    if (!first)
        TRY(js_out(print_context, " "));
    TRY(js_out(print_context, "}}"));
    return {};
}

ErrorOr<void> print_set(JS::PrintContext& print_context, JS::Set const& set, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Set"sv));
    TRY(js_out(print_context, " {{"));
    bool first = true;
    for (auto const& entry : set) {
        TRY(print_separator(print_context, first));
        TRY(print_value(print_context, entry.key, seen_objects));
    }
    if (!first)
        TRY(js_out(print_context, " "));
    TRY(js_out(print_context, "}}"));
    return {};
}

ErrorOr<void> print_weak_map(JS::PrintContext& print_context, JS::WeakMap const& weak_map, HashTable<JS::Object*>&)
{
    TRY(print_type(print_context, "WeakMap"sv));
    TRY(js_out(print_context, " ({})", weak_map.values().size()));
    // Note: We could tell you what's actually inside, but not in insertion order.
    return {};
}

ErrorOr<void> print_weak_set(JS::PrintContext& print_context, JS::WeakSet const& weak_set, HashTable<JS::Object*>&)
{
    TRY(print_type(print_context, "WeakSet"sv));
    TRY(js_out(print_context, " ({})", weak_set.values().size()));
    // Note: We could tell you what's actually inside, but not in insertion order.
    return {};
}

ErrorOr<void> print_weak_ref(JS::PrintContext& print_context, JS::WeakRef const& weak_ref, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "WeakRef"sv));
    TRY(js_out(print_context, " "));
    TRY(print_value(print_context, weak_ref.value().visit([](Empty) -> JS::Value { return JS::js_undefined(); }, [](auto value) -> JS::Value { return value; }), seen_objects));
    return {};
}

ErrorOr<void> print_promise(JS::PrintContext& print_context, JS::Promise const& promise, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Promise"sv));
    switch (promise.state()) {
    case JS::Promise::State::Pending:
        TRY(js_out(print_context, "\n  state: "));
        TRY(js_out(print_context, "\033[36;1mPending\033[0m"));
        break;
    case JS::Promise::State::Fulfilled:
        TRY(js_out(print_context, "\n  state: "));
        TRY(js_out(print_context, "\033[32;1mFulfilled\033[0m"));
        TRY(js_out(print_context, "\n  result: "));
        TRY(print_value(print_context, promise.result(), seen_objects));
        break;
    case JS::Promise::State::Rejected:
        TRY(js_out(print_context, "\n  state: "));
        TRY(js_out(print_context, "\033[31;1mRejected\033[0m"));
        TRY(js_out(print_context, "\n  result: "));
        TRY(print_value(print_context, promise.result(), seen_objects));
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    return {};
}

ErrorOr<void> print_array_buffer(JS::PrintContext& print_context, JS::ArrayBuffer const& array_buffer, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "ArrayBuffer"sv));

    auto byte_length = array_buffer.byte_length();
    TRY(js_out(print_context, "\n  byteLength: "));
    TRY(print_value(print_context, JS::Value((double)byte_length), seen_objects));
    if (array_buffer.is_detached()) {
        TRY(js_out(print_context, "\n  Detached"));
        return {};
    }

    if (byte_length == 0)
        return {};

    auto& buffer = array_buffer.buffer();
    TRY(js_out(print_context, "\n"));
    for (size_t i = 0; i < byte_length; ++i) {
        TRY(js_out(print_context, "{:02x}", buffer[i]));
        if (i + 1 < byte_length) {
            if ((i + 1) % 32 == 0)
                TRY(js_out(print_context, "\n"));
            else if ((i + 1) % 16 == 0)
                TRY(js_out(print_context, "  "));
            else
                TRY(js_out(print_context, " "));
        }
    }

    return {};
}

ErrorOr<void> print_shadow_realm(JS::PrintContext& print_context, JS::ShadowRealm const&, HashTable<JS::Object*>&)
{
    // Not much we can show here that would be useful. Realm pointer address?!
    TRY(print_type(print_context, "ShadowRealm"sv));
    return {};
}

ErrorOr<void> print_generator(JS::PrintContext& print_context, JS::GeneratorObject const& generator, HashTable<JS::Object*>&)
{
    TRY(print_type(print_context, generator.class_name()));
    return {};
}

ErrorOr<void> print_async_generator(JS::PrintContext& print_context, JS::AsyncGenerator const& generator, HashTable<JS::Object*>&)
{
    TRY(print_type(print_context, generator.class_name()));
    return {};
}

template<Arithmetic T>
ErrorOr<void> print_number(JS::PrintContext& print_context, T number)
{
    TRY(js_out(print_context, "\033[35;1m"));
    TRY(js_out(print_context, "{}", number));
    TRY(js_out(print_context, "\033[0m"));
    return {};
}

ErrorOr<void> print_typed_array(JS::PrintContext& print_context, JS::TypedArrayBase const& typed_array_base, HashTable<JS::Object*>& seen_objects)
{
    auto& array_buffer = *typed_array_base.viewed_array_buffer();

    auto typed_array_record = JS::make_typed_array_with_buffer_witness_record(typed_array_base, JS::ArrayBuffer::Order::SeqCst);
    TRY(print_type(print_context, typed_array_base.class_name()));

    TRY(js_out(print_context, "\n  buffer: "));
    TRY(print_type(print_context, "ArrayBuffer"sv));
    TRY(js_out(print_context, " @ {:p}", &array_buffer));

    if (JS::is_typed_array_out_of_bounds(typed_array_record)) {
        TRY(js_out(print_context, "\n  <out of bounds>"));
        return {};
    }

    auto length = JS::typed_array_length(typed_array_record);

    TRY(js_out(print_context, "\n  length: "));
    TRY(print_value(print_context, JS::Value(length), seen_objects));
    TRY(js_out(print_context, "\n  byteLength: "));
    TRY(print_value(print_context, JS::Value(JS::typed_array_byte_length(typed_array_record)), seen_objects));

    TRY(js_out(print_context, "\n"));
    // FIXME: Find a better way to print typed arrays to the console.
    // The current solution is limited to 100 lines, is hard to read, and hampers debugging.
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    if (is<JS::ClassName>(typed_array_base)) {                                           \
        TRY(js_out(print_context, "[ "));                                                \
        auto& typed_array = static_cast<JS::ClassName const&>(typed_array_base);         \
        auto data = typed_array.data();                                                  \
        size_t printed_count = 0;                                                        \
        for (size_t i = 0; i < length; ++i) {                                            \
            if (i > 0)                                                                   \
                TRY(js_out(print_context, ", "));                                        \
            TRY(print_number(print_context, data[i]));                                   \
            if (++printed_count > 100 && i < length) {                                   \
                TRY(js_out(print_context, ", ..."));                                     \
                break;                                                                   \
            }                                                                            \
        }                                                                                \
        TRY(js_out(print_context, " ]"));                                                \
        return {};                                                                       \
    }
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
    VERIFY_NOT_REACHED();
}

ErrorOr<void> print_data_view(JS::PrintContext& print_context, JS::DataView const& data_view, HashTable<JS::Object*>& seen_objects)
{
    auto view_record = JS::make_data_view_with_buffer_witness_record(data_view, JS::ArrayBuffer::Order::SeqCst);
    TRY(print_type(print_context, "DataView"sv));

    TRY(js_out(print_context, "\n  buffer: "));
    TRY(print_type(print_context, "ArrayBuffer"sv));
    TRY(js_out(print_context, " @ {:p}", data_view.viewed_array_buffer()));

    if (JS::is_view_out_of_bounds(view_record)) {
        TRY(js_out(print_context, "\n  <out of bounds>"));
        return {};
    }

    TRY(js_out(print_context, "\n  byteLength: "));
    TRY(print_value(print_context, JS::Value(JS::get_view_byte_length(view_record)), seen_objects));
    TRY(js_out(print_context, "\n  byteOffset: "));
    TRY(print_value(print_context, JS::Value(data_view.byte_offset()), seen_objects));
    return {};
}

ErrorOr<void> print_intl_display_names(JS::PrintContext& print_context, JS::Intl::DisplayNames const& display_names, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Intl.DisplayNames"sv));
    TRY(js_out(print_context, "\n  locale: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(display_names.vm(), display_names.locale()), seen_objects));
    TRY(js_out(print_context, "\n  type: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(display_names.vm(), display_names.type_string()), seen_objects));
    TRY(js_out(print_context, "\n  style: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(display_names.vm(), display_names.style_string()), seen_objects));
    TRY(js_out(print_context, "\n  fallback: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(display_names.vm(), display_names.fallback_string()), seen_objects));
    if (display_names.has_language_display()) {
        TRY(js_out(print_context, "\n  languageDisplay: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(display_names.vm(), display_names.language_display_string()), seen_objects));
    }
    return {};
}

ErrorOr<void> print_intl_locale(JS::PrintContext& print_context, JS::Intl::Locale const& locale, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Intl.Locale"sv));
    TRY(js_out(print_context, "\n  locale: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(locale.vm(), locale.locale()), seen_objects));
    if (locale.has_calendar()) {
        TRY(js_out(print_context, "\n  calendar: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(locale.vm(), locale.calendar()), seen_objects));
    }
    if (locale.has_case_first()) {
        TRY(js_out(print_context, "\n  caseFirst: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(locale.vm(), locale.case_first()), seen_objects));
    }
    if (locale.has_collation()) {
        TRY(js_out(print_context, "\n  collation: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(locale.vm(), locale.collation()), seen_objects));
    }
    if (locale.has_hour_cycle()) {
        TRY(js_out(print_context, "\n  hourCycle: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(locale.vm(), locale.hour_cycle()), seen_objects));
    }
    if (locale.has_numbering_system()) {
        TRY(js_out(print_context, "\n  numberingSystem: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(locale.vm(), locale.numbering_system()), seen_objects));
    }
    TRY(js_out(print_context, "\n  numeric: "));
    TRY(print_value(print_context, JS::Value(locale.numeric()), seen_objects));
    return {};
}

ErrorOr<void> print_intl_list_format(JS::PrintContext& print_context, JS::Intl::ListFormat const& list_format, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Intl.ListFormat"sv));
    TRY(js_out(print_context, "\n  locale: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(list_format.vm(), list_format.locale()), seen_objects));
    TRY(js_out(print_context, "\n  type: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(list_format.vm(), list_format.type_string()), seen_objects));
    TRY(js_out(print_context, "\n  style: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(list_format.vm(), list_format.style_string()), seen_objects));
    return {};
}

ErrorOr<void> print_intl_number_format(JS::PrintContext& print_context, JS::Intl::NumberFormat const& number_format, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Intl.NumberFormat"sv));
    TRY(js_out(print_context, "\n  locale: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.locale()), seen_objects));
    TRY(js_out(print_context, "\n  numberingSystem: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.numbering_system()), seen_objects));
    TRY(js_out(print_context, "\n  style: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.style_string()), seen_objects));
    if (number_format.has_currency()) {
        TRY(js_out(print_context, "\n  currency: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.currency()), seen_objects));
    }
    if (number_format.has_currency_display()) {
        TRY(js_out(print_context, "\n  currencyDisplay: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.currency_display_string()), seen_objects));
    }
    if (number_format.has_currency_sign()) {
        TRY(js_out(print_context, "\n  currencySign: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.currency_sign_string()), seen_objects));
    }
    if (number_format.has_unit()) {
        TRY(js_out(print_context, "\n  unit: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.unit()), seen_objects));
    }
    if (number_format.has_unit_display()) {
        TRY(js_out(print_context, "\n  unitDisplay: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.unit_display_string()), seen_objects));
    }
    TRY(js_out(print_context, "\n  minimumIntegerDigits: "));
    TRY(print_value(print_context, JS::Value(number_format.min_integer_digits()), seen_objects));
    if (number_format.has_min_fraction_digits()) {
        TRY(js_out(print_context, "\n  minimumFractionDigits: "));
        TRY(print_value(print_context, JS::Value(number_format.min_fraction_digits()), seen_objects));
    }
    if (number_format.has_max_fraction_digits()) {
        TRY(js_out(print_context, "\n  maximumFractionDigits: "));
        TRY(print_value(print_context, JS::Value(number_format.max_fraction_digits()), seen_objects));
    }
    if (number_format.has_min_significant_digits()) {
        TRY(js_out(print_context, "\n  minimumSignificantDigits: "));
        TRY(print_value(print_context, JS::Value(number_format.min_significant_digits()), seen_objects));
    }
    if (number_format.has_max_significant_digits()) {
        TRY(js_out(print_context, "\n  maximumSignificantDigits: "));
        TRY(print_value(print_context, JS::Value(number_format.max_significant_digits()), seen_objects));
    }
    TRY(js_out(print_context, "\n  useGrouping: "));
    TRY(print_value(print_context, number_format.use_grouping_to_value(number_format.vm()), seen_objects));
    TRY(js_out(print_context, "\n  roundingType: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.rounding_type_string()), seen_objects));
    TRY(js_out(print_context, "\n  roundingMode: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.rounding_mode_string()), seen_objects));
    TRY(js_out(print_context, "\n  roundingIncrement: "));
    TRY(print_value(print_context, JS::Value(number_format.rounding_increment()), seen_objects));
    TRY(js_out(print_context, "\n  notation: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.notation_string()), seen_objects));
    if (number_format.has_compact_display()) {
        TRY(js_out(print_context, "\n  compactDisplay: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.compact_display_string()), seen_objects));
    }
    TRY(js_out(print_context, "\n  signDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.sign_display_string()), seen_objects));
    TRY(js_out(print_context, "\n  trailingZeroDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(number_format.vm(), number_format.trailing_zero_display_string()), seen_objects));
    return {};
}

ErrorOr<void> print_intl_date_time_format(JS::PrintContext& print_context, JS::Intl::DateTimeFormat& date_time_format, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Intl.DateTimeFormat"sv));
    TRY(js_out(print_context, "\n  locale: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), date_time_format.locale()), seen_objects));
    TRY(js_out(print_context, "\n  calendar: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), date_time_format.calendar()), seen_objects));
    TRY(js_out(print_context, "\n  numberingSystem: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), date_time_format.numbering_system()), seen_objects));
    if (auto const& hour_cycle = date_time_format.date_time_format().hour_cycle; hour_cycle.has_value()) {
        TRY(js_out(print_context, "\n  hourCycle: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), Unicode::hour_cycle_to_string(*hour_cycle)), seen_objects));
    }
    TRY(js_out(print_context, "\n  timeZone: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), date_time_format.time_zone()), seen_objects));
    if (date_time_format.has_date_style()) {
        TRY(js_out(print_context, "\n  dateStyle: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), date_time_format.date_style_string()), seen_objects));
    }
    if (date_time_format.has_time_style()) {
        TRY(js_out(print_context, "\n  timeStyle: "));
        TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), date_time_format.time_style_string()), seen_objects));
    }

    auto result = JS::Intl::for_each_calendar_field(date_time_format.vm(), date_time_format.date_time_format(), [&](auto& option, auto const& property, auto const&) -> JS::ThrowCompletionOr<void> {
        using ValueType = typename RemoveReference<decltype(option)>::ValueType;

        if (!option.has_value())
            return {};

        // Note: We can't `TRY()` here as `for_each_calendar_field` expects a ThrowCompletionOr<T> instead of an ErrorOr<T>,
        //       So the quickest way out is to generate a null throw completion (we handle the throw ourselves).
        if (js_out(print_context, "\n  {}: ", property).is_error())
            return JS::throw_completion(JS::js_null());

        if constexpr (IsIntegral<ValueType>) {
            if (print_value(print_context, JS::Value(*option), seen_objects).is_error())
                return JS::throw_completion(JS::js_null());
        } else {
            auto name = Unicode::calendar_pattern_style_to_string(*option);
            if (print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), name), seen_objects).is_error())
                return JS::throw_completion(JS::js_null());
        }

        return {};
    });

    if (result.is_throw_completion() && result.throw_completion().value()->is_null())
        return Error::from_errno(ENOMEM); // probably

    return {};
}

ErrorOr<void> print_intl_relative_time_format(JS::PrintContext& print_context, JS::Intl::RelativeTimeFormat const& date_time_format, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Intl.RelativeTimeFormat"sv));
    TRY(js_out(print_context, "\n  locale: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), date_time_format.locale()), seen_objects));
    TRY(js_out(print_context, "\n  numberingSystem: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), date_time_format.numbering_system()), seen_objects));
    TRY(js_out(print_context, "\n  style: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), date_time_format.style_string()), seen_objects));
    TRY(js_out(print_context, "\n  numeric: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(date_time_format.vm(), date_time_format.numeric_string()), seen_objects));
    return {};
}

ErrorOr<void> print_intl_plural_rules(JS::PrintContext& print_context, JS::Intl::PluralRules const& plural_rules, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Intl.PluralRules"sv));
    TRY(js_out(print_context, "\n  locale: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(plural_rules.vm(), plural_rules.locale()), seen_objects));
    TRY(js_out(print_context, "\n  type: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(plural_rules.vm(), plural_rules.type_string()), seen_objects));
    TRY(js_out(print_context, "\n  minimumIntegerDigits: "));
    TRY(print_value(print_context, JS::Value(plural_rules.min_integer_digits()), seen_objects));
    if (plural_rules.has_min_fraction_digits()) {
        TRY(js_out(print_context, "\n  minimumFractionDigits: "));
        TRY(print_value(print_context, JS::Value(plural_rules.min_fraction_digits()), seen_objects));
    }
    if (plural_rules.has_max_fraction_digits()) {
        TRY(js_out(print_context, "\n  maximumFractionDigits: "));
        TRY(print_value(print_context, JS::Value(plural_rules.max_fraction_digits()), seen_objects));
    }
    if (plural_rules.has_min_significant_digits()) {
        TRY(js_out(print_context, "\n  minimumSignificantDigits: "));
        TRY(print_value(print_context, JS::Value(plural_rules.min_significant_digits()), seen_objects));
    }
    if (plural_rules.has_max_significant_digits()) {
        TRY(js_out(print_context, "\n  maximumSignificantDigits: "));
        TRY(print_value(print_context, JS::Value(plural_rules.max_significant_digits()), seen_objects));
    }
    TRY(js_out(print_context, "\n  roundingType: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(plural_rules.vm(), plural_rules.rounding_type_string()), seen_objects));
    return {};
}

ErrorOr<void> print_intl_collator(JS::PrintContext& print_context, JS::Intl::Collator const& collator, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Intl.Collator"sv));
    TRY(js_out(print_context, "\n  locale: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(collator.vm(), collator.locale()), seen_objects));
    TRY(js_out(print_context, "\n  usage: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(collator.vm(), collator.usage_string()), seen_objects));
    TRY(js_out(print_context, "\n  sensitivity: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(collator.vm(), collator.sensitivity_string()), seen_objects));
    TRY(js_out(print_context, "\n  caseFirst: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(collator.vm(), collator.case_first_string()), seen_objects));
    TRY(js_out(print_context, "\n  collation: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(collator.vm(), collator.collation()), seen_objects));
    TRY(js_out(print_context, "\n  ignorePunctuation: "));
    TRY(print_value(print_context, JS::Value(collator.ignore_punctuation()), seen_objects));
    TRY(js_out(print_context, "\n  numeric: "));
    TRY(print_value(print_context, JS::Value(collator.numeric()), seen_objects));
    return {};
}

ErrorOr<void> print_intl_segmenter(JS::PrintContext& print_context, JS::Intl::Segmenter const& segmenter, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Intl.Segmenter"sv));
    TRY(js_out(print_context, "\n  locale: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(segmenter.vm(), segmenter.locale()), seen_objects));
    TRY(js_out(print_context, "\n  granularity: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(segmenter.vm(), segmenter.segmenter_granularity_string()), seen_objects));
    return {};
}

ErrorOr<void> print_intl_segments(JS::PrintContext& print_context, JS::Intl::Segments const& segments, HashTable<JS::Object*>& seen_objects)
{
    auto segments_string = JS::Utf16String::create(segments.segments_string());

    TRY(print_type(print_context, "Segments"sv));
    TRY(js_out(print_context, "\n  string: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(segments.vm(), move(segments_string)), seen_objects));
    return {};
}

ErrorOr<void> print_intl_duration_format(JS::PrintContext& print_context, JS::Intl::DurationFormat const& duration_format, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Intl.DurationFormat"sv));
    TRY(js_out(print_context, "\n  locale: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.locale()), seen_objects));
    TRY(js_out(print_context, "\n  numberingSystem: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.numbering_system()), seen_objects));
    TRY(js_out(print_context, "\n  style: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.style_string()), seen_objects));
    TRY(js_out(print_context, "\n  years: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.years_style_string()), seen_objects));
    TRY(js_out(print_context, "\n  yearsDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.years_display_string()), seen_objects));
    TRY(js_out(print_context, "\n  months: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.months_style_string()), seen_objects));
    TRY(js_out(print_context, "\n  monthsDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.months_display_string()), seen_objects));
    TRY(js_out(print_context, "\n  weeks: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.weeks_style_string()), seen_objects));
    TRY(js_out(print_context, "\n  weeksDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.weeks_display_string()), seen_objects));
    TRY(js_out(print_context, "\n  days: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.days_style_string()), seen_objects));
    TRY(js_out(print_context, "\n  daysDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.days_display_string()), seen_objects));
    TRY(js_out(print_context, "\n  hours: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.hours_style_string()), seen_objects));
    TRY(js_out(print_context, "\n  hoursDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.hours_display_string()), seen_objects));
    TRY(js_out(print_context, "\n  minutes: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.minutes_style_string()), seen_objects));
    TRY(js_out(print_context, "\n  minutesDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.minutes_display_string()), seen_objects));
    TRY(js_out(print_context, "\n  seconds: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.seconds_style_string()), seen_objects));
    TRY(js_out(print_context, "\n  secondsDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.seconds_display_string()), seen_objects));
    TRY(js_out(print_context, "\n  milliseconds: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.milliseconds_style_string()), seen_objects));
    TRY(js_out(print_context, "\n  millisecondsDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.milliseconds_display_string()), seen_objects));
    TRY(js_out(print_context, "\n  microseconds: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.microseconds_style_string()), seen_objects));
    TRY(js_out(print_context, "\n  microsecondsDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.microseconds_display_string()), seen_objects));
    TRY(js_out(print_context, "\n  nanoseconds: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.nanoseconds_style_string()), seen_objects));
    TRY(js_out(print_context, "\n  nanosecondsDisplay: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(duration_format.vm(), duration_format.nanoseconds_display_string()), seen_objects));
    if (duration_format.has_fractional_digits()) {
        TRY(js_out(print_context, "\n  fractionalDigits: "));
        TRY(print_value(print_context, JS::Value(duration_format.fractional_digits()), seen_objects));
    }
    return {};
}

ErrorOr<void> print_temporal_duration(JS::PrintContext& print_context, JS::Temporal::Duration const& duration, HashTable<JS::Object*>&)
{
    TRY(print_type(print_context, "Temporal.Duration"sv));
    TRY(js_out(print_context, " \033[34;1m{} y, {} M, {} w, {} d, {} h, {} m, {} s, {} ms, {} us, {} ns\033[0m", duration.years(), duration.months(), duration.weeks(), duration.days(), duration.hours(), duration.minutes(), duration.seconds(), duration.milliseconds(), duration.microseconds(), duration.nanoseconds()));
    return {};
}

ErrorOr<void> print_temporal_instant(JS::PrintContext& print_context, JS::Temporal::Instant const& instant, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Temporal.Instant"sv));
    TRY(js_out(print_context, " "));
    TRY(print_value(print_context, instant.epoch_nanoseconds(), seen_objects));
    return {};
}

ErrorOr<void> print_temporal_plain_date(JS::PrintContext& print_context, JS::Temporal::PlainDate const& plain_date, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Temporal.PlainDate"sv));
    TRY(js_out(print_context, " \033[34;1m{:04}-{:02}-{:02}\033[0m", plain_date.iso_date().year, plain_date.iso_date().month, plain_date.iso_date().day));
    TRY(js_out(print_context, "\n  calendar: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(plain_date.vm(), plain_date.calendar()), seen_objects));
    return {};
}

ErrorOr<void> print_temporal_plain_date_time(JS::PrintContext& print_context, JS::Temporal::PlainDateTime const& plain_date_time, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Temporal.PlainDateTime"sv));
    TRY(js_out(print_context, " \033[34;1m{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}{:03}{:03}\033[0m", plain_date_time.iso_date_time().iso_date.year, plain_date_time.iso_date_time().iso_date.month, plain_date_time.iso_date_time().iso_date.day, plain_date_time.iso_date_time().time.hour, plain_date_time.iso_date_time().time.minute, plain_date_time.iso_date_time().time.second, plain_date_time.iso_date_time().time.millisecond, plain_date_time.iso_date_time().time.microsecond, plain_date_time.iso_date_time().time.nanosecond));
    TRY(js_out(print_context, "\n  calendar: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(plain_date_time.vm(), plain_date_time.calendar()), seen_objects));
    return {};
}

ErrorOr<void> print_temporal_plain_month_day(JS::PrintContext& print_context, JS::Temporal::PlainMonthDay const& plain_month_day, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Temporal.PlainMonthDay"sv));
    TRY(js_out(print_context, " \033[34;1m{:02}-{:02}\033[0m", plain_month_day.iso_date().month, plain_month_day.iso_date().day));
    TRY(js_out(print_context, "\n  calendar: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(plain_month_day.vm(), plain_month_day.calendar()), seen_objects));
    return {};
}

ErrorOr<void> print_temporal_plain_time(JS::PrintContext& print_context, JS::Temporal::PlainTime const& plain_time, HashTable<JS::Object*>&)
{
    TRY(print_type(print_context, "Temporal.PlainTime"sv));
    TRY(js_out(print_context, " \033[34;1m{:02}:{:02}:{:02}.{:03}{:03}{:03}\033[0m", plain_time.time().hour, plain_time.time().minute, plain_time.time().second, plain_time.time().millisecond, plain_time.time().microsecond, plain_time.time().nanosecond));
    return {};
}

ErrorOr<void> print_temporal_plain_year_month(JS::PrintContext& print_context, JS::Temporal::PlainYearMonth const& plain_year_month, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Temporal.PlainYearMonth"sv));
    TRY(js_out(print_context, " \033[34;1m{:04}-{:02}\033[0m", plain_year_month.iso_date().year, plain_year_month.iso_date().month));
    TRY(js_out(print_context, "\n  calendar: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(plain_year_month.vm(), plain_year_month.calendar()), seen_objects));
    return {};
}

ErrorOr<void> print_temporal_zoned_date_time(JS::PrintContext& print_context, JS::Temporal::ZonedDateTime const& zoned_date_time, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Temporal.ZonedDateTime"sv));
    TRY(js_out(print_context, "\n  epochNanoseconds: "));
    TRY(print_value(print_context, zoned_date_time.epoch_nanoseconds(), seen_objects));
    TRY(js_out(print_context, "\n  timeZone: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(zoned_date_time.vm(), zoned_date_time.time_zone()), seen_objects));
    TRY(js_out(print_context, "\n  calendar: "));
    TRY(print_value(print_context, JS::PrimitiveString::create(zoned_date_time.vm(), zoned_date_time.calendar()), seen_objects));
    return {};
}

ErrorOr<void> print_boolean_object(JS::PrintContext& print_context, JS::BooleanObject const& boolean_object, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Boolean"sv));
    TRY(js_out(print_context, " "));
    TRY(print_value(print_context, JS::Value(boolean_object.boolean()), seen_objects));
    return {};
}

ErrorOr<void> print_number_object(JS::PrintContext& print_context, JS::NumberObject const& number_object, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "Number"sv));
    TRY(js_out(print_context, " "));
    TRY(print_value(print_context, JS::Value(number_object.number()), seen_objects));
    return {};
}

ErrorOr<void> print_string_object(JS::PrintContext& print_context, JS::StringObject const& string_object, HashTable<JS::Object*>& seen_objects)
{
    TRY(print_type(print_context, "String"sv));
    TRY(js_out(print_context, " "));
    TRY(print_value(print_context, &string_object.primitive_string(), seen_objects));
    return {};
}

ErrorOr<void> print_value(JS::PrintContext& print_context, JS::Value value, HashTable<JS::Object*>& seen_objects)
{
    if (value.is_empty()) {
        TRY(js_out(print_context, "\033[34;1m<empty>\033[0m"));
        return {};
    }

    if (value.is_object()) {
        if (seen_objects.contains(&value.as_object())) {
            // FIXME: Maybe we should only do this for circular references,
            //        not for all reoccurring objects.
            TRY(js_out(print_context, "<already printed Object {}>", &value.as_object()));
            return {};
        }
        seen_objects.set(&value.as_object());
    }

    if (value.is_object()) {
        auto& object = value.as_object();
        if (is<JS::Array>(object))
            return print_array(print_context, static_cast<JS::Array&>(object), seen_objects);
        if (object.is_function())
            return print_function(print_context, static_cast<JS::FunctionObject&>(object), seen_objects);
        if (is<JS::Date>(object))
            return print_date(print_context, static_cast<JS::Date&>(object), seen_objects);
        if (is<JS::Error>(object))
            return print_error(print_context, object, seen_objects);

        auto prototype_or_error = object.internal_get_prototype_of();
        if (prototype_or_error.has_value() && prototype_or_error.value() != nullptr) {
            auto& prototype = *prototype_or_error.value();
            if (&prototype == prototype.shape().realm().intrinsics().error_prototype())
                return print_error(print_context, object, seen_objects);
        }

        if (is<JS::RegExpObject>(object))
            return print_regexp_object(print_context, static_cast<JS::RegExpObject&>(object), seen_objects);
        if (is<JS::Map>(object))
            return print_map(print_context, static_cast<JS::Map&>(object), seen_objects);
        if (is<JS::Set>(object))
            return print_set(print_context, static_cast<JS::Set&>(object), seen_objects);
        if (is<JS::WeakMap>(object))
            return print_weak_map(print_context, static_cast<JS::WeakMap&>(object), seen_objects);
        if (is<JS::WeakSet>(object))
            return print_weak_set(print_context, static_cast<JS::WeakSet&>(object), seen_objects);
        if (is<JS::WeakRef>(object))
            return print_weak_ref(print_context, static_cast<JS::WeakRef&>(object), seen_objects);
        if (is<JS::DataView>(object))
            return print_data_view(print_context, static_cast<JS::DataView&>(object), seen_objects);
        if (is<JS::ProxyObject>(object))
            return print_proxy_object(print_context, static_cast<JS::ProxyObject&>(object), seen_objects);
        if (is<JS::Promise>(object))
            return print_promise(print_context, static_cast<JS::Promise&>(object), seen_objects);
        if (is<JS::ArrayBuffer>(object))
            return print_array_buffer(print_context, static_cast<JS::ArrayBuffer&>(object), seen_objects);
        if (is<JS::ShadowRealm>(object))
            return print_shadow_realm(print_context, static_cast<JS::ShadowRealm&>(object), seen_objects);
        if (is<JS::GeneratorObject>(object))
            return print_generator(print_context, static_cast<JS::GeneratorObject&>(object), seen_objects);
        if (is<JS::AsyncGenerator>(object))
            return print_async_generator(print_context, static_cast<JS::AsyncGenerator&>(object), seen_objects);
        if (object.is_typed_array())
            return print_typed_array(print_context, static_cast<JS::TypedArrayBase&>(object), seen_objects);
        if (is<JS::BooleanObject>(object))
            return print_boolean_object(print_context, static_cast<JS::BooleanObject&>(object), seen_objects);
        if (is<JS::NumberObject>(object))
            return print_number_object(print_context, static_cast<JS::NumberObject&>(object), seen_objects);
        if (is<JS::StringObject>(object))
            return print_string_object(print_context, static_cast<JS::StringObject&>(object), seen_objects);
        if (is<JS::Intl::DisplayNames>(object))
            return print_intl_display_names(print_context, static_cast<JS::Intl::DisplayNames&>(object), seen_objects);
        if (is<JS::Intl::Locale>(object))
            return print_intl_locale(print_context, static_cast<JS::Intl::Locale&>(object), seen_objects);
        if (is<JS::Intl::ListFormat>(object))
            return print_intl_list_format(print_context, static_cast<JS::Intl::ListFormat&>(object), seen_objects);
        if (is<JS::Intl::NumberFormat>(object))
            return print_intl_number_format(print_context, static_cast<JS::Intl::NumberFormat&>(object), seen_objects);
        if (is<JS::Intl::DateTimeFormat>(object))
            return print_intl_date_time_format(print_context, static_cast<JS::Intl::DateTimeFormat&>(object), seen_objects);
        if (is<JS::Intl::RelativeTimeFormat>(object))
            return print_intl_relative_time_format(print_context, static_cast<JS::Intl::RelativeTimeFormat&>(object), seen_objects);
        if (is<JS::Intl::PluralRules>(object))
            return print_intl_plural_rules(print_context, static_cast<JS::Intl::PluralRules&>(object), seen_objects);
        if (is<JS::Intl::Collator>(object))
            return print_intl_collator(print_context, static_cast<JS::Intl::Collator&>(object), seen_objects);
        if (is<JS::Intl::Segmenter>(object))
            return print_intl_segmenter(print_context, static_cast<JS::Intl::Segmenter&>(object), seen_objects);
        if (is<JS::Intl::Segments>(object))
            return print_intl_segments(print_context, static_cast<JS::Intl::Segments&>(object), seen_objects);
        if (is<JS::Intl::DurationFormat>(object))
            return print_intl_duration_format(print_context, static_cast<JS::Intl::DurationFormat&>(object), seen_objects);
        if (is<JS::Temporal::Duration>(object))
            return print_temporal_duration(print_context, static_cast<JS::Temporal::Duration&>(object), seen_objects);
        if (is<JS::Temporal::Instant>(object))
            return print_temporal_instant(print_context, static_cast<JS::Temporal::Instant&>(object), seen_objects);
        if (is<JS::Temporal::PlainDate>(object))
            return print_temporal_plain_date(print_context, static_cast<JS::Temporal::PlainDate&>(object), seen_objects);
        if (is<JS::Temporal::PlainDateTime>(object))
            return print_temporal_plain_date_time(print_context, static_cast<JS::Temporal::PlainDateTime&>(object), seen_objects);
        if (is<JS::Temporal::PlainMonthDay>(object))
            return print_temporal_plain_month_day(print_context, static_cast<JS::Temporal::PlainMonthDay&>(object), seen_objects);
        if (is<JS::Temporal::PlainTime>(object))
            return print_temporal_plain_time(print_context, static_cast<JS::Temporal::PlainTime&>(object), seen_objects);
        if (is<JS::Temporal::PlainYearMonth>(object))
            return print_temporal_plain_year_month(print_context, static_cast<JS::Temporal::PlainYearMonth&>(object), seen_objects);
        if (is<JS::Temporal::ZonedDateTime>(object))
            return print_temporal_zoned_date_time(print_context, static_cast<JS::Temporal::ZonedDateTime&>(object), seen_objects);
        return print_object(print_context, object, seen_objects);
    }

    if (value.is_string())
        TRY(js_out(print_context, "\033[32;1m"));
    else if (value.is_number() || value.is_bigint())
        TRY(js_out(print_context, "\033[35;1m"));
    else if (value.is_boolean())
        TRY(js_out(print_context, "\033[33;1m"));
    else if (value.is_null())
        TRY(js_out(print_context, "\033[33;1m"));
    else if (value.is_undefined())
        TRY(js_out(print_context, "\033[34;1m"));

    if (value.is_string())
        TRY(js_out(print_context, "\""));
    else if (value.is_negative_zero())
        TRY(js_out(print_context, "-"));

    auto contents = value.to_string_without_side_effects();
    if (value.is_string())
        TRY(js_out(print_context, "{}", TRY(escape_for_string_literal(contents))));
    else
        TRY(js_out(print_context, "{}", contents));

    if (value.is_string())
        TRY(js_out(print_context, "\""));
    TRY(js_out(print_context, "\033[0m"));
    return {};
}
}

namespace JS {
ErrorOr<void> print(JS::Value value, PrintContext& print_context)
{
    HashTable<JS::Object*> seen_objects;
    return print_value(print_context, value, seen_objects);
}
}
