/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/GenericLexer.h>
#include <AK/StringBuilder.h>
#include <AK/StringConversions.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/BigIntObject.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/RawJSONObject.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/ValueInlines.h>

#include <simdjson.h>

namespace JS {

GC_DEFINE_ALLOCATOR(JSONObject);

JSONObject::JSONObject(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void JSONObject::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.stringify, stringify, 3, attr);
    define_native_function(realm, vm.names.parse, parse, 2, attr);
    define_native_function(realm, vm.names.rawJSON, raw_json, 1, attr);
    define_native_function(realm, vm.names.isRawJSON, is_raw_json, 1, attr);

    // 25.5.3 JSON [ @@toStringTag ], https://tc39.es/ecma262/#sec-json-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "JSON"_string), Attribute::Configurable);
}

// 25.5.2 JSON.stringify ( value [ , replacer [ , space ] ] ), https://tc39.es/ecma262/#sec-json.stringify
ThrowCompletionOr<Optional<String>> JSONObject::stringify_impl(VM& vm, Value value, Value replacer, Value space)
{
    auto& realm = *vm.current_realm();

    StringifyState state;

    if (replacer.is_object()) {
        if (replacer.as_object().is_function()) {
            state.replacer_function = &replacer.as_function();
        } else {
            auto is_array = TRY(replacer.is_array(vm));
            if (is_array) {
                auto& replacer_object = replacer.as_object();
                auto replacer_length = TRY(length_of_array_like(vm, replacer_object));
                Vector<Utf16String> list;
                for (size_t i = 0; i < replacer_length; ++i) {
                    auto replacer_value = TRY(replacer_object.get(i));
                    Optional<Utf16String> item;
                    if (replacer_value.is_string()) {
                        item = replacer_value.as_string().utf16_string();
                    } else if (replacer_value.is_number()) {
                        item = MUST(replacer_value.to_utf16_string(vm));
                    } else if (replacer_value.is_object()) {
                        auto& value_object = replacer_value.as_object();
                        if (is<StringObject>(value_object) || is<NumberObject>(value_object))
                            item = TRY(replacer_value.to_utf16_string(vm));
                    }
                    if (item.has_value() && !list.contains_slow(*item)) {
                        list.append(*item);
                    }
                }
                state.property_list = move(list);
            }
        }
    }

    if (space.is_object()) {
        auto& space_object = space.as_object();
        if (is<NumberObject>(space_object))
            space = TRY(space.to_number(vm));
        else if (is<StringObject>(space_object))
            space = TRY(space.to_primitive_string(vm));
    }

    if (space.is_number()) {
        auto space_mv = MUST(space.to_integer_or_infinity(vm));
        space_mv = min(10, space_mv);
        state.gap = space_mv < 1 ? String {} : MUST(String::repeated(' ', space_mv));
    } else if (space.is_string()) {
        auto string = space.as_string().utf8_string();
        if (string.bytes().size() <= 10)
            state.gap = string;
        else
            state.gap = MUST(string.substring_from_byte_offset(0, 10));
    } else {
        state.gap = String {};
    }

    auto wrapper = Object::create(realm, realm.intrinsics().object_prototype());
    MUST(wrapper->create_data_property_or_throw(Utf16String {}, value));

    bool wrote_value = TRY(serialize_json_property(vm, state, Utf16String {}, wrapper));
    if (!wrote_value)
        return Optional<String> {};

    return state.builder.to_string_without_validation();
}

// 25.5.2 JSON.stringify ( value [ , replacer [ , space ] ] ), https://tc39.es/ecma262/#sec-json.stringify
JS_DEFINE_NATIVE_FUNCTION(JSONObject::stringify)
{
    if (!vm.argument_count())
        return js_undefined();

    auto value = vm.argument(0);
    auto replacer = vm.argument(1);
    auto space = vm.argument(2);

    auto maybe_string = TRY(stringify_impl(vm, value, replacer, space));
    if (!maybe_string.has_value())
        return js_undefined();

    return PrimitiveString::create(vm, maybe_string.release_value());
}

// 25.5.2.1 SerializeJSONProperty ( state, key, holder ), https://tc39.es/ecma262/#sec-serializejsonproperty
// 1.4.1 SerializeJSONProperty ( state, key, holder ), https://tc39.es/proposal-json-parse-with-source/#sec-serializejsonproperty
// Returns true if a value was serialized, false if the value was undefined (should be omitted).
ThrowCompletionOr<bool> JSONObject::serialize_json_property(VM& vm, StringifyState& state, PropertyKey const& key, Object* holder)
{
    auto& builder = state.builder;

    // 1. Let value be ? Get(holder, key).
    auto value = TRY(holder->get(key));

    // 2. If Type(value) is Object or BigInt, then
    if (value.is_object() || value.is_bigint()) {
        // a. Let toJSON be ? GetV(value, "toJSON").
        auto to_json = TRY(value.get(vm, vm.names.toJSON));

        // b. If IsCallable(toJSON) is true, then
        if (to_json.is_function()) {
            // i. Set value to ? Call(toJSON, value, « key »).
            value = TRY(call(vm, to_json.as_function(), value, PrimitiveString::create(vm, key.to_string())));
        }
    }

    // 3. If state.[[ReplacerFunction]] is not undefined, then
    if (state.replacer_function) {
        // a. Set value to ? Call(state.[[ReplacerFunction]], holder, « key, value »).
        value = TRY(call(vm, *state.replacer_function, holder, PrimitiveString::create(vm, key.to_string()), value));
    }

    // 4. If Type(value) is Object, then
    if (value.is_object()) {
        auto& value_object = value.as_object();

        // a. If value has an [[IsRawJSON]] internal slot, then
        if (is<RawJSONObject>(value_object)) {
            // i. Return ! Get(value, "rawJSON").
            builder.append(MUST(value_object.get(vm.names.rawJSON)).as_string().utf8_string());
            return true;
        }
        // b. If value has a [[NumberData]] internal slot, then
        if (is<NumberObject>(value_object)) {
            // i. Set value to ? ToNumber(value).
            value = TRY(value.to_number(vm));
        }
        // c. Else if value has a [[StringData]] internal slot, then
        else if (is<StringObject>(value_object)) {
            // i. Set value to ? ToString(value).
            value = TRY(value.to_primitive_string(vm));
        }
        // d. Else if value has a [[BooleanData]] internal slot, then
        else if (auto const* boolean = as_if<BooleanObject>(value_object)) {
            // i. Set value to value.[[BooleanData]].
            value = Value { boolean->boolean() };
        }
        // e. Else if value has a [[BigIntData]] internal slot, then
        else if (auto const* bigint = as_if<BigIntObject>(value_object)) {
            // i. Set value to value.[[BigIntData]].
            value = Value { &bigint->bigint() };
        }
    }

    // 5. If value is null, return "null".
    if (value.is_null()) {
        builder.append("null"sv);
        return true;
    }

    // 6. If value is true, return "true".
    // 7. If value is false, return "false".
    if (value.is_boolean()) {
        builder.append(value.as_bool() ? "true"sv : "false"sv);
        return true;
    }

    // 8. If Type(value) is String, return QuoteJSONString(value).
    if (value.is_string()) {
        quote_json_string(builder, value.as_string().utf16_string_view());
        return true;
    }

    // 9. If Type(value) is Number, then
    if (value.is_number()) {
        // a. If value is finite, return ! ToString(value).
        if (value.is_finite_number()) {
            number_to_string(builder, value.as_double());
            return true;
        }

        // b. Return "null".
        builder.append("null"sv);
        return true;
    }

    // 10. If Type(value) is BigInt, throw a TypeError exception.
    if (value.is_bigint())
        return vm.throw_completion<TypeError>(ErrorType::JsonBigInt);

    // 11. If Type(value) is Object and IsCallable(value) is false, then
    if (value.is_object() && !value.is_function()) {
        // a. Let isArray be ? IsArray(value).
        auto is_array = TRY(value.is_array(vm));

        // b. If isArray is true, return ? SerializeJSONArray(state, value).
        if (is_array) {
            TRY(serialize_json_array(vm, state, value.as_object()));
            return true;
        }

        // c. Return ? SerializeJSONObject(state, value).
        TRY(serialize_json_object(vm, state, value.as_object()));
        return true;
    }

    // 12. Return undefined.
    return false;
}

static void write_indent(StringBuilder& builder, StringView gap, size_t depth)
{
    for (size_t i = 0; i < depth; ++i)
        builder.append(gap);
}

// 25.5.2.4 SerializeJSONObject ( state, value ), https://tc39.es/ecma262/#sec-serializejsonobject
ThrowCompletionOr<void> JSONObject::serialize_json_object(VM& vm, StringifyState& state, Object& object)
{
    if (state.seen_objects.contains(&object))
        return vm.throw_completion<TypeError>(ErrorType::JsonCircular);

    state.seen_objects.set(&object);
    ++state.indent_depth;

    auto& builder = state.builder;
    builder.append('{');
    size_t position_after_open_brace = builder.length();
    bool first = true;

    auto process_property = [&](PropertyKey const& key) -> ThrowCompletionOr<void> {
        if (key.is_symbol())
            return {};

        // Mark position before writing anything for this property
        size_t mark = builder.length();

        // Write separator (comma and possibly newline/indent)
        if (!first) {
            builder.append(',');
            if (!state.gap.is_empty()) {
                builder.append('\n');
                write_indent(builder, state.gap, state.indent_depth);
            }
        } else if (!state.gap.is_empty()) {
            builder.append('\n');
            write_indent(builder, state.gap, state.indent_depth);
        }

        // Write key and colon
        quote_json_string(builder, key.to_string());
        builder.append(':');
        if (!state.gap.is_empty())
            builder.append(' ');

        // Serialize value
        bool wrote_value = TRY(serialize_json_property(vm, state, key, &object));

        if (wrote_value) {
            first = false;
        } else {
            // Rollback - value was undefined, remove everything we wrote for this property
            builder.trim(builder.length() - mark);
        }
        return {};
    };

    if (state.property_list.has_value()) {
        auto property_list = state.property_list.value();
        for (auto& property : property_list)
            TRY(process_property(property));
    } else {
        auto property_list = TRY(object.enumerable_own_property_names(PropertyKind::Key));
        for (auto& property : property_list)
            TRY(process_property(property.as_string().utf16_string()));
    }

    // Close the object
    --state.indent_depth;
    if (builder.length() > position_after_open_brace && !state.gap.is_empty()) {
        builder.append('\n');
        write_indent(builder, state.gap, state.indent_depth);
    }
    builder.append('}');

    state.seen_objects.remove(&object);
    return {};
}

// 25.5.2.5 SerializeJSONArray ( state, value ), https://tc39.es/ecma262/#sec-serializejsonarray
ThrowCompletionOr<void> JSONObject::serialize_json_array(VM& vm, StringifyState& state, Object& object)
{
    if (state.seen_objects.contains(&object))
        return vm.throw_completion<TypeError>(ErrorType::JsonCircular);

    state.seen_objects.set(&object);
    ++state.indent_depth;

    auto& builder = state.builder;
    auto length = TRY(length_of_array_like(vm, object));

    builder.append('[');

    for (size_t i = 0; i < length; ++i) {
        // Write separator
        if (i > 0) {
            builder.append(',');
            if (!state.gap.is_empty()) {
                builder.append('\n');
                write_indent(builder, state.gap, state.indent_depth);
            }
        } else if (!state.gap.is_empty()) {
            builder.append('\n');
            write_indent(builder, state.gap, state.indent_depth);
        }

        // Serialize value (undefined becomes null for arrays)
        bool wrote_value = TRY(serialize_json_property(vm, state, i, &object));
        if (!wrote_value)
            builder.append("null"sv);
    }

    // Close the array
    --state.indent_depth;
    if (length > 0 && !state.gap.is_empty()) {
        builder.append('\n');
        write_indent(builder, state.gap, state.indent_depth);
    }
    builder.append(']');

    state.seen_objects.remove(&object);
    return {};
}

// 25.5.2.2 QuoteJSONString ( value ), https://tc39.es/ecma262/#sec-quotejsonstring
void JSONObject::quote_json_string(StringBuilder& builder, Utf16View const& string)
{
    // 1. Let product be the String value consisting solely of the code unit 0x0022 (QUOTATION MARK).
    builder.append('"');

    // 2. For each code point C of StringToCodePoints(value), do
    for (auto code_point : string) {
        // a. If C is listed in the "Code Point" column of Table 70, then
        // i. Set product to the string-concatenation of product and the escape sequence for C as specified in the "Escape Sequence" column of the corresponding row.
        switch (code_point) {
        case '\b':
            builder.append("\\b"sv);
            break;
        case '\t':
            builder.append("\\t"sv);
            break;
        case '\n':
            builder.append("\\n"sv);
            break;
        case '\f':
            builder.append("\\f"sv);
            break;
        case '\r':
            builder.append("\\r"sv);
            break;
        case '"':
            builder.append("\\\""sv);
            break;
        case '\\':
            builder.append("\\\\"sv);
            break;
        default:
            // b. Else if C has a numeric value less than 0x0020 (SPACE), or if C has the same numeric value as a leading surrogate or trailing surrogate, then
            if (code_point < 0x20 || is_unicode_surrogate(code_point)) {
                // i. Let unit be the code unit whose numeric value is that of C.
                // ii. Set product to the string-concatenation of product and UnicodeEscape(unit).
                builder.appendff("\\u{:04x}", code_point);
            }
            // c. Else,
            else {
                // i. Set product to the string-concatenation of product and UTF16EncodeCodePoint(C).
                builder.append_code_point(code_point);
            }
        }
    }

    // 3. Set product to the string-concatenation of product and the code unit 0x0022 (QUOTATION MARK).
    builder.append('"');
}

// 25.5.1 JSON.parse ( text [ , reviver ] ), https://tc39.es/ecma262/#sec-json.parse
JS_DEFINE_NATIVE_FUNCTION(JSONObject::parse)
{
    auto& realm = *vm.current_realm();

    auto text = vm.argument(0);
    auto reviver = vm.argument(1);

    // 1. Let jsonString be ? ToString(text).
    auto json_string = TRY(text.to_string(vm));

    // 2. Let unfiltered be ? ParseJSON(jsonString).
    auto unfiltered = TRY(parse_json(vm, json_string));

    // 3. If IsCallable(reviver) is true, then
    if (reviver.is_function()) {
        // a. Let root be OrdinaryObjectCreate(%Object.prototype%).
        auto root = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Let rootName be the empty String.
        Utf16String root_name;

        // c. Perform ! CreateDataPropertyOrThrow(root, rootName, unfiltered).
        MUST(root->create_data_property_or_throw(root_name, unfiltered));

        // d. Return ? InternalizeJSONProperty(root, rootName, reviver).
        return internalize_json_property(vm, root, root_name, reviver.as_function());
    }
    // 4. Else,
    //     a. Return unfiltered.
    return unfiltered;
}

// Unescape a JSON string, properly handling \uXXXX escape sequences including lone surrogates.
// simdjson validates UTF-8 strictly and rejects lone surrogates, but JSON allows them.
// Returns {} on malformed escape sequences.
static Optional<Utf16String> unescape_json_string(StringView raw)
{
    StringBuilder builder(StringBuilder::Mode::UTF16, raw.length());

    GenericLexer lexer { raw };

    auto consume_hex4 = [&]() -> Optional<u16> {
        if (lexer.tell_remaining() < 4)
            return {};
        u16 value = 0;
        for (int i = 0; i < 4; ++i) {
            auto ch = lexer.consume();
            value <<= 4;
            if (ch >= '0' && ch <= '9')
                value |= ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                value |= ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F')
                value |= ch - 'A' + 10;
            else
                return {};
        }
        return value;
    };

    while (!lexer.is_eof()) {
        if (lexer.consume_specific('\\')) {
            if (lexer.is_eof())
                return {};
            auto escaped = lexer.consume();
            switch (escaped) {
            case '"':
                builder.append_code_unit('"');
                break;
            case '\\':
                builder.append_code_unit('\\');
                break;
            case '/':
                builder.append_code_unit('/');
                break;
            case 'b':
                builder.append_code_unit('\b');
                break;
            case 'f':
                builder.append_code_unit('\f');
                break;
            case 'n':
                builder.append_code_unit('\n');
                break;
            case 'r':
                builder.append_code_unit('\r');
                break;
            case 't':
                builder.append_code_unit('\t');
                break;
            case 'u': {
                auto code_unit = consume_hex4();
                if (!code_unit.has_value())
                    return {};
                builder.append_code_unit(*code_unit);
                break;
            }
            default:
                return {};
            }
        } else {
            // Non-escaped character - copy UTF-8 code point to UTF-16
            auto ch = lexer.consume();
            if ((ch & 0x80) == 0) {
                // ASCII
                builder.append_code_unit(ch);
            } else if ((ch & 0xE0) == 0xC0) {
                // 2-byte UTF-8
                if (lexer.is_eof())
                    return {};
                auto ch2 = lexer.consume();
                u32 code_point = ((ch & 0x1F) << 6) | (ch2 & 0x3F);
                builder.append_code_unit(code_point);
            } else if ((ch & 0xF0) == 0xE0) {
                // 3-byte UTF-8
                if (lexer.tell_remaining() < 2)
                    return {};
                auto ch2 = lexer.consume();
                auto ch3 = lexer.consume();
                u32 code_point = ((ch & 0x0F) << 12) | ((ch2 & 0x3F) << 6) | (ch3 & 0x3F);
                builder.append_code_unit(code_point);
            } else if ((ch & 0xF8) == 0xF0) {
                // 4-byte UTF-8 (needs surrogate pair)
                if (lexer.tell_remaining() < 3)
                    return {};
                auto ch2 = lexer.consume();
                auto ch3 = lexer.consume();
                auto ch4 = lexer.consume();
                u32 code_point = ((ch & 0x07) << 18) | ((ch2 & 0x3F) << 12) | ((ch3 & 0x3F) << 6) | (ch4 & 0x3F);
                builder.append_code_point(code_point);
            } else {
                return {};
            }
        }
    }

    return builder.to_utf16_string();
}

template<typename T>
static ALWAYS_INLINE ThrowCompletionOr<void> ensure_simdjson_fully_parsed(VM& vm, T& value)
{
    if constexpr (IsSame<T, simdjson::ondemand::document>) {
        if (!value.at_end())
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
    }
    return {};
}

static ThrowCompletionOr<Value> parse_simdjson_value(VM&, simdjson::ondemand::value);

template<typename T>
static ThrowCompletionOr<Value> parse_simdjson_number(VM& vm, T& value, StringView raw_sv)
{
    // Validate JSON number format (simdjson is more lenient than spec)
    // - No leading zeros (except "0" or "0.xxx")
    // - No trailing decimal point (e.g., "1." is invalid)
    size_t i = 0;
    if (i < raw_sv.length() && raw_sv[i] == '-')
        ++i;
    if (i < raw_sv.length() && raw_sv[i] == '0' && i + 1 < raw_sv.length() && is_ascii_digit(raw_sv[i + 1]))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed); // Leading zero
    while (i < raw_sv.length() && is_ascii_digit(raw_sv[i]))
        ++i;
    if (i < raw_sv.length() && raw_sv[i] == '.') {
        ++i;
        if (i >= raw_sv.length() || !is_ascii_digit(raw_sv[i]))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed); // Trailing decimal
    }

    double double_value;
    auto error = value.get_double().get(double_value);
    if (!error) {
        TRY(ensure_simdjson_fully_parsed(vm, value));
        return Value(double_value);
    }

    // Handle overflow to infinity (e.g., 1e309)
    // simdjson returns NUMBER_ERROR for numbers that overflow double
    // Use parse_first_number as fallback - it handles overflow correctly
    if (error == simdjson::NUMBER_ERROR) {
        auto result = parse_first_number<double>(raw_sv, TrimWhitespace::No);
        if (result.has_value() && result->characters_parsed == raw_sv.length())
            return Value(result->value);
    }

    return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
}

template<typename T>
static ThrowCompletionOr<Value> parse_simdjson_string(VM& vm, T& value)
{
    // Use get_raw_json_string() to get the raw JSON string content (without quotes, with escapes),
    // then unescape ourselves to properly handle lone surrogates like \uD800 which simdjson rejects.
    simdjson::ondemand::raw_json_string raw_string;
    if (value.get_raw_json_string().get(raw_string))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
    char const* raw = raw_string.raw();
    // Find the length by looking for the closing quote (simdjson validated the structure)
    size_t length = 0;
    while (raw[length] != '"') {
        if (raw[length] == '\\')
            ++length; // Skip escaped character
        ++length;
    }
    auto unescaped = unescape_json_string({ raw, length });
    if (!unescaped.has_value())
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
    return PrimitiveString::create(vm, unescaped.release_value());
}

template<typename T>
static ThrowCompletionOr<Value> parse_simdjson_array(VM& vm, T& value)
{
    auto& realm = *vm.current_realm();

    simdjson::ondemand::array simdjson_array;
    if (value.get_array().get(simdjson_array))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    auto array = MUST(Array::create(realm, 0));
    size_t index = 0;

    for (auto element : simdjson_array) {
        simdjson::ondemand::value element_value;
        if (element.get(element_value))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        auto parsed = TRY(parse_simdjson_value(vm, element_value));
        array->define_direct_property(index++, parsed, default_attributes);
    }

    TRY(ensure_simdjson_fully_parsed(vm, value));
    return array;
}

template<typename T>
static ThrowCompletionOr<Value> parse_simdjson_object(VM& vm, T& value)
{
    auto& realm = *vm.current_realm();

    simdjson::ondemand::object simdjson_object;
    if (value.get_object().get(simdjson_object))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    auto object = Object::create(realm, realm.intrinsics().object_prototype());

    for (auto field : simdjson_object) {
        // Use escaped_key() to get the raw JSON key (with escapes), then unescape ourselves
        std::string_view raw_key;
        if (field.escaped_key().get(raw_key))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        auto unescaped_key = unescape_json_string({ raw_key.data(), raw_key.size() });
        if (!unescaped_key.has_value())
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        simdjson::ondemand::value field_value;
        if (field.value().get(field_value))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        auto parsed = TRY(parse_simdjson_value(vm, field_value));
        object->define_direct_property(unescaped_key.release_value(), parsed, default_attributes);
    }

    TRY(ensure_simdjson_fully_parsed(vm, value));
    return object;
}

static ThrowCompletionOr<Value> parse_simdjson_value(VM& vm, simdjson::ondemand::value value)
{
    simdjson::ondemand::json_type type;
    if (value.type().get(type))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    switch (type) {
    case simdjson::ondemand::json_type::null:
        return js_null();
    case simdjson::ondemand::json_type::boolean: {
        bool boolean_value;
        if (value.get_bool().get(boolean_value))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        return Value(boolean_value);
    }
    case simdjson::ondemand::json_type::number: {
        auto raw = value.raw_json_token();
        StringView raw_sv { raw.data(), raw.size() };
        return parse_simdjson_number(vm, value, raw_sv);
    }
    case simdjson::ondemand::json_type::string:
        return parse_simdjson_string(vm, value);
    case simdjson::ondemand::json_type::array:
        return parse_simdjson_array(vm, value);
    case simdjson::ondemand::json_type::object:
        return parse_simdjson_object(vm, value);
    case simdjson::ondemand::json_type::unknown:
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
    }

    VERIFY_NOT_REACHED();
}

static ThrowCompletionOr<Value> parse_simdjson_document(VM& vm, simdjson::ondemand::document& document)
{
    simdjson::ondemand::json_type type;
    if (document.type().get(type))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    switch (type) {
    case simdjson::ondemand::json_type::null: {
        if (document.is_null().error())
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        if (!document.at_end())
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        return js_null();
    }
    case simdjson::ondemand::json_type::boolean: {
        bool boolean_value;
        if (document.get_bool().get(boolean_value))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        if (!document.at_end())
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        return Value(boolean_value);
    }
    case simdjson::ondemand::json_type::number: {
        // Get raw token first in case get_double fails (e.g., overflow)
        std::string_view raw;
        if (document.raw_json_token().get(raw))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        StringView raw_sv { raw.data(), raw.size() };
        auto trimmed = raw_sv.trim_whitespace();
        return parse_simdjson_number(vm, document, trimmed);
    }
    case simdjson::ondemand::json_type::string:
        return parse_simdjson_string(vm, document);
    case simdjson::ondemand::json_type::array:
        return parse_simdjson_array(vm, document);
    case simdjson::ondemand::json_type::object:
        return parse_simdjson_object(vm, document);
    case simdjson::ondemand::json_type::unknown:
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
    }

    VERIFY_NOT_REACHED();
}

// 25.5.1.1 ParseJSON ( text ), https://tc39.es/ecma262/#sec-ParseJSON
ThrowCompletionOr<Value> JSONObject::parse_json(VM& vm, StringView text)
{
    // 1. If StringToCodePoints(text) is not a valid JSON text as specified in ECMA-404, throw a SyntaxError exception.
    // NB: Per ECMA-404, the BOM is not valid JSON whitespace. simdjson silently skips it, so we must reject it explicitly.
    if (text.length() >= 3
        && static_cast<u8>(text[0]) == 0xEF
        && static_cast<u8>(text[1]) == 0xBB
        && static_cast<u8>(text[2]) == 0xBF) {
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(text.characters_without_null_termination(), text.length());

    simdjson::ondemand::document document;
    if (parser.iterate(padded).get(document))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    // 2. Let scriptString be the string-concatenation of "(", text, and ");".
    // 3. Let script be ParseText(scriptString, Script).
    // 4. NOTE: The early error rules defined in 13.2.5.1 have special handling for the above invocation of ParseText.
    // 5. Assert: script is a Parse Node.
    // 6. Let result be ! Evaluation of script.
    auto result = TRY(parse_simdjson_document(vm, document));

    // 7. NOTE: The PropertyDefinitionEvaluation semantics defined in 13.2.5.5 have special handling for the above evaluation.
    // 8. Assert: result is either a String, a Number, a Boolean, an Object that is defined by either an ArrayLiteral or an ObjectLiteral, or null.

    // 9. Return result.
    return result;
}

// 25.5.1.1 InternalizeJSONProperty ( holder, name, reviver ), https://tc39.es/ecma262/#sec-internalizejsonproperty
ThrowCompletionOr<Value> JSONObject::internalize_json_property(VM& vm, Object* holder, PropertyKey const& name, FunctionObject& reviver)
{
    auto value = TRY(holder->get(name));
    if (value.is_object()) {
        auto is_array = TRY(value.is_array(vm));

        auto& value_object = value.as_object();
        auto process_property = [&](PropertyKey const& key) -> ThrowCompletionOr<void> {
            auto element = TRY(internalize_json_property(vm, &value_object, key, reviver));
            if (element.is_undefined())
                TRY(value_object.internal_delete(key));
            else
                TRY(value_object.create_data_property(key, element));
            return {};
        };

        if (is_array) {
            auto length = TRY(length_of_array_like(vm, value_object));
            for (size_t i = 0; i < length; ++i)
                TRY(process_property(i));
        } else {
            auto property_list = TRY(value_object.enumerable_own_property_names(Object::PropertyKind::Key));
            for (auto& property_key : property_list)
                TRY(process_property(property_key.as_string().utf16_string()));
        }
    }

    return TRY(call(vm, reviver, holder, PrimitiveString::create(vm, name.to_string()), value));
}

// 1.3 JSON.rawJSON ( text ), https://tc39.es/proposal-json-parse-with-source/#sec-json.rawjson
JS_DEFINE_NATIVE_FUNCTION(JSONObject::raw_json)
{
    auto& realm = *vm.current_realm();

    // 1. Let jsonString be ? ToString(text).
    auto json_string = TRY(vm.argument(0).to_string(vm));

    // 2. Throw a SyntaxError exception if jsonString is the empty String, or if either the first or last code unit of
    //    jsonString is any of 0x0009 (CHARACTER TABULATION), 0x000A (LINE FEED), 0x000D (CARRIAGE RETURN), or
    //    0x0020 (SPACE).
    auto bytes = json_string.bytes_as_string_view();
    if (bytes.is_empty())
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    static constexpr AK::Array invalid_code_points { 0x09, 0x0A, 0x0D, 0x20 };
    auto first_char = bytes[0];
    auto last_char = bytes[bytes.length() - 1];

    if (invalid_code_points.contains_slow(first_char) || invalid_code_points.contains_slow(last_char))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    // 3. Parse StringToCodePoints(jsonString) as a JSON text as specified in ECMA-404. Throw a SyntaxError exception
    //    if it is not a valid JSON text as defined in that specification, or if its outermost value is an object or
    //    array as defined in that specification.
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(json_string.bytes_as_string_view().characters_without_null_termination(), json_string.bytes_as_string_view().length());

    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    simdjson::ondemand::json_type type;
    if (doc.type().get(type) || type == simdjson::ondemand::json_type::unknown)
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    if (type == simdjson::ondemand::json_type::object || type == simdjson::ondemand::json_type::array)
        return vm.throw_completion<SyntaxError>(ErrorType::JsonRawJSONNonPrimitive);

    // Consume the value to advance past it, then check for trailing content
    switch (type) {
    case simdjson::ondemand::json_type::null:
        (void)doc.is_null();
        break;
    case simdjson::ondemand::json_type::boolean:
        (void)doc.get_bool();
        break;
    case simdjson::ondemand::json_type::number:
        (void)doc.get_double();
        break;
    case simdjson::ondemand::json_type::string:
        (void)doc.get_string();
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    if (!doc.at_end())
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    // 4. Let internalSlotsList be « [[IsRawJSON]] ».
    // 5. Let obj be OrdinaryObjectCreate(null, internalSlotsList).
    auto object = RawJSONObject::create(realm, nullptr);

    // 6. Perform ! CreateDataPropertyOrThrow(obj, "rawJSON", jsonString).
    MUST(object->create_data_property_or_throw(vm.names.rawJSON, PrimitiveString::create(vm, json_string)));

    // 7. Perform ! SetIntegrityLevel(obj, frozen).
    MUST(object->set_integrity_level(Object::IntegrityLevel::Frozen));

    // 8. Return obj.
    return object;
}

// 1.1 JSON.isRawJSON ( O ), https://tc39.es/proposal-json-parse-with-source/#sec-json.israwjson
JS_DEFINE_NATIVE_FUNCTION(JSONObject::is_raw_json)
{
    // 1. If Type(O) is Object and O has an [[IsRawJSON]] internal slot, return true.
    // 2. Return false.
    return vm.argument(0).is<RawJSONObject>();
}

}
