/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/GenericLexer.h>
#include <AK/HashFunctions.h>
#include <AK/OwnPtr.h>
#include <AK/StringBuilder.h>
#include <AK/StringConversions.h>
#include <AK/TypeCasts.h>
#include <AK/UnicodeUtils.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Utf16View.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/RootVector.h>
#include <LibGC/WeakHashSet.h>
#include <LibJS/Bytecode/Executable.h>
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
#include <LibJS/Runtime/ProxyObject.h>
#include <LibJS/Runtime/RawJSONObject.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/ValueInlines.h>

#include <simdjson.h>

namespace JS {

GC_DEFINE_ALLOCATOR(JSONObject);

class StringifyObjectStack {
public:
    bool contains(Object& object) const
    {
        if (m_lookup.is_empty())
            return m_stack.contains_slow(GC::Ref { object });
        return m_lookup.contains(object);
    }

    void push(Object& object)
    {
        if (m_stack.size() == linear_lookup_limit) {
            for (auto seen_object : m_stack)
                m_lookup.set(*seen_object);
        }
        if (!m_lookup.is_empty())
            m_lookup.set(object);
        m_stack.append(GC::Ref { object });
    }

    void pop()
    {
        auto object = m_stack.take_last();
        if (m_lookup.is_empty())
            return;
        m_lookup.remove(*object);
        if (m_stack.size() <= linear_lookup_limit)
            m_lookup.clear();
    }

private:
    static constexpr size_t linear_lookup_limit = 32;

    GC::RootVector<GC::Ref<Object>> m_stack;
    GC::WeakHashSet<Object> m_lookup;
};

struct StringifyCachedProperty {
    PropertyKey property;
    u32 offset;
    Utf16String serialized_key;
};

class StringifyShapeCache {
public:
    struct Entry {
        GC::Ptr<Shape> shape;
        GC::ConservativeVector<StringifyCachedProperty> properties;
    };

    Entry* get(Shape& shape)
    {
        if (!m_lookup.is_empty()) {
            auto it = m_lookup.find(&shape);
            return it == m_lookup.end() ? nullptr : it->value;
        }
        for (auto const& entry : m_entries) {
            if (entry->shape.ptr() == &shape)
                return entry.ptr();
        }
        return nullptr;
    }

    Entry* add(GC::Ref<Shape> shape)
    {
        if (m_entries.size() >= maximum_entries)
            return nullptr;

        auto entry = make<Entry>();
        entry->shape = shape.ptr();
        auto* entry_pointer = entry.ptr();

        if (m_entries.size() == linear_lookup_limit) {
            for (auto const& existing_entry : m_entries)
                m_lookup.set(existing_entry->shape, existing_entry.ptr());
        }
        if (!m_lookup.is_empty())
            m_lookup.set(shape.ptr(), entry_pointer);

        m_shape_roots.append(shape);
        m_entries.append(move(entry));
        return entry_pointer;
    }

private:
    static constexpr size_t linear_lookup_limit = 4;
    static constexpr size_t maximum_entries = 64;

    GC::RootVector<GC::Ref<Shape>> m_shape_roots;
    Vector<NonnullOwnPtr<Entry>> m_entries;
    HashMap<GC::Ptr<Shape>, Entry*> m_lookup;
};

struct JSONObject::StringifyState {
    GC::Ptr<FunctionObject> replacer_function;
    StringifyObjectStack object_stack;
    StringifyShapeCache shape_cache;
    size_t indent_depth { 0 };
    Utf16String gap;
    Optional<Vector<Utf16String>> property_list;
    Utf16StringBuilder builder;
};

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
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "JSON"_utf16_fly_string), Attribute::Configurable);
}

// 25.5.2 JSON.stringify ( value [ , replacer [ , space ] ] ), https://tc39.es/ecma262/#sec-json.stringify
ThrowCompletionOr<Optional<Utf16String>> JSONObject::stringify_impl(VM& vm, Value value, Value replacer, Value space)
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

    Optional<Utf16String> space_string;
    if (space.is_string()) {
        space_string = space.as_string().utf16_string();
    } else if (space.is_object()) {
        auto& space_object = space.as_object();
        if (is<NumberObject>(space_object))
            space = TRY(space.to_number(vm));
        else if (is<StringObject>(space_object))
            space_string = TRY(space.to_utf16_string(vm));
    }

    if (space.is_number()) {
        auto space_mv = MUST(space.to_integer_or_infinity(vm));
        space_mv = min(10, space_mv);
        state.gap = space_mv < 1 ? Utf16String {} : Utf16String::repeated(' ', space_mv);
    } else if (space_string.has_value()) {
        if (space_string->length_in_code_units() <= 10)
            state.gap = move(*space_string);
        else
            state.gap = Utf16String::from_utf16(space_string->substring_view(0, 10));
    } else {
        state.gap = Utf16String {};
    }

    auto wrapper = Object::create(realm, realm.intrinsics().object_prototype());
    MUST(wrapper->create_data_property_or_throw(Utf16String {}, value));

    bool wrote_value = TRY(serialize_json_property(vm, state, Utf16String {}, wrapper));
    if (!wrote_value)
        return Optional<Utf16String> {};

    return state.builder.to_string();
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
ThrowCompletionOr<bool> JSONObject::serialize_json_property(VM& vm, StringifyState& state, PropertyKey const& key, GC::Ref<Object> holder)
{
    // 1. Let value be ? Get(holder, key).
    auto value = TRY(holder->get(key));

    return serialize_json_value(vm, state, key, holder, value);
}

static bool can_use_direct_property_access(Object const& object)
{
    // OPTIMIZATION: Ordinary non-dictionary objects can snapshot their shape and read data
    //               properties directly by offset while that shape remains unchanged.
    return object.eligible_for_own_property_enumeration_fast_path()
        && !object.has_intrinsic_accessors()
        && !object.has_parameter_map()
        && !object.is_platform_object()
        && !object.shape().is_dictionary()
        && object.indexed_real_size() == 0;
}

static void append_json_number(Utf16StringBuilder& builder, Value value)
{
    if (!value.is_int32()) {
        builder.append(number_to_utf16_string(value.as_double()));
        return;
    }

    auto number = value.as_i32();
    AK::Array<char, 11> buffer;
    auto* end = buffer.data() + buffer.size();
    auto* cursor = end;
    auto magnitude = number < 0 ? static_cast<u32>(-static_cast<i64>(number)) : static_cast<u32>(number);
    do {
        *--cursor = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (number < 0)
        *--cursor = '-';
    builder.append_ascii_without_validation({ cursor, static_cast<size_t>(end - cursor) });
}

ThrowCompletionOr<bool> JSONObject::serialize_json_value(VM& vm, StringifyState& state, PropertyKey const& key, GC::Ref<Object> holder, Value value)
{
    auto& builder = state.builder;

    // OPTIMIZATION: These primitive values do not perform a toJSON lookup. Without a replacer,
    //               they can be emitted before entering the observable part of the algorithm.
    if (!state.replacer_function) {
        if (value.is_null()) {
            builder.append_ascii_without_validation("null"sv.bytes());
            return true;
        }
        if (value.is_boolean()) {
            auto boolean = value.as_bool() ? "true"sv : "false"sv;
            builder.append_ascii_without_validation(boolean.bytes());
            return true;
        }
        if (value.is_string()) {
            builder.append_quoted_escaped_for_json(value.as_string().utf16_string_view());
            return true;
        }
        if (value.is_finite_number()) {
            append_json_number(builder, value);
            return true;
        }
    }

    // 2. If Type(value) is Object or BigInt, then
    if (value.is_object() || value.is_bigint()) {
        // a. Let toJSON be ? GetV(value, "toJSON").
        static auto& to_json_cache = *new Bytecode::StaticPropertyLookupCache;
        auto to_json = TRY(value.get(vm, vm.names.toJSON, to_json_cache));

        // b. If IsCallable(toJSON) is true, then
        if (to_json.is_function()) {
            // i. Set value to ? Call(toJSON, value, « key »).
            value = TRY(call(vm, to_json.as_function(), value, PrimitiveString::create(vm, key.to_utf16_string())));
        }
    }

    // 3. If state.[[ReplacerFunction]] is not undefined, then
    if (state.replacer_function) {
        // a. Set value to ? Call(state.[[ReplacerFunction]], holder, « key, value »).
        value = TRY(call(vm, *state.replacer_function, holder, PrimitiveString::create(vm, key.to_utf16_string()), value));
    }

    // 4. If Type(value) is Object, then
    if (value.is_object()) {
        auto& value_object = value.as_object();

        // a. If value has an [[IsRawJSON]] internal slot, then
        if (is<RawJSONObject>(value_object)) {
            // i. Return ! Get(value, "rawJSON").
            builder.append(MUST(value_object.get(vm.names.rawJSON)).as_string().utf16_string_view());
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
            value = PrimitiveString::create(vm, TRY(value.to_utf16_string(vm)));
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
        builder.append_ascii("null"sv);
        return true;
    }

    // 6. If value is true, return "true".
    // 7. If value is false, return "false".
    if (value.is_boolean()) {
        if (value.as_bool())
            builder.append_ascii("true"sv);
        else
            builder.append_ascii("false"sv);
        return true;
    }

    // 8. If Type(value) is String, return QuoteJSONString(value).
    if (value.is_string()) {
        builder.append_quoted_escaped_for_json(value.as_string().utf16_string_view());
        return true;
    }

    // 9. If Type(value) is Number, then
    if (value.is_number()) {
        // a. If value is finite, return ! ToString(value).
        if (value.is_finite_number()) {
            append_json_number(builder, value);
            return true;
        }

        // b. Return "null".
        builder.append_ascii("null"sv);
        return true;
    }

    // 10. If Type(value) is BigInt, throw a TypeError exception.
    if (value.is_bigint())
        return vm.throw_completion<TypeError>(ErrorType::JsonBigInt);

    // 11. If Type(value) is Object and IsCallable(value) is false, then
    if (value.is_object() && !value.is_function()) {
        auto& value_object = value.as_object();

        if (is<Array>(value_object)) {
            TRY(serialize_json_array(vm, state, value_object));
            return true;
        }

        // a. Let isArray be ? IsArray(value).
        auto is_array = is<ProxyObject>(value_object) && TRY(value.is_array(vm));

        // b. If isArray is true, return ? SerializeJSONArray(state, value).
        if (is_array) {
            TRY(serialize_json_array(vm, state, value_object));
            return true;
        }

        // c. Return ? SerializeJSONObject(state, value).
        TRY(serialize_json_object(vm, state, value_object));
        return true;
    }

    // 12. Return undefined.
    return false;
}

static void write_indent(Utf16StringBuilder& builder, Utf16String const& gap, size_t depth)
{
    for (size_t i = 0; i < depth; ++i)
        builder.append(gap.utf16_view());
}

// 25.5.2.4 SerializeJSONObject ( state, value ), https://tc39.es/ecma262/#sec-serializejsonobject
ThrowCompletionOr<void> JSONObject::serialize_json_object(VM& vm, StringifyState& state, Object& object)
{
    if (vm.did_reach_stack_space_limit())
        return vm.throw_completion<InternalError>(ErrorType::CallStackSizeExceeded);

    if (state.object_stack.contains(object))
        return vm.throw_completion<TypeError>(ErrorType::JsonCircular);

    state.object_stack.push(object);
    ++state.indent_depth;

    auto& builder = state.builder;
    builder.append_ascii('{');
    size_t position_after_open_brace = builder.length_in_code_units();
    bool first = true;

    auto process_property = [&](PropertyKey const& key) -> ThrowCompletionOr<void> {
        if (key.is_symbol())
            return {};

        // Mark position before writing anything for this property
        size_t mark = builder.length_in_code_units();

        // Write separator (comma and possibly newline/indent)
        if (!first) {
            builder.append_ascii(',');
            if (!state.gap.is_empty()) {
                builder.append_ascii('\n');
                write_indent(builder, state.gap, state.indent_depth);
            }
        } else if (!state.gap.is_empty()) {
            builder.append_ascii('\n');
            write_indent(builder, state.gap, state.indent_depth);
        }

        // Write key and colon
        builder.append_quoted_escaped_for_json(key.to_utf16_string());
        builder.append_ascii(':');
        if (!state.gap.is_empty())
            builder.append_ascii(' ');

        // Serialize value
        bool wrote_value = TRY(serialize_json_property(vm, state, key, object));

        if (wrote_value) {
            first = false;
        } else {
            // Rollback - value was undefined, remove everything we wrote for this property
            builder.trim(builder.length_in_code_units() - mark);
        }
        return {};
    };

    auto process_own_properties = [&]() -> ThrowCompletionOr<void> {
        GC::ConservativeVector<PropertyKey> property_list;
        property_list.ensure_capacity(object.own_properties_count());
        TRY(object.for_each_own_property_with_enumerability([&](PropertyKey const& property_key, bool enumerable) -> ThrowCompletionOr<void> {
            if (enumerable)
                property_list.append(property_key);
            return {};
        }));
        for (auto& property : property_list)
            TRY(process_property(property));
        return {};
    };

    if (state.property_list.has_value()) {
        auto const& property_list = state.property_list.value();
        for (auto& property : property_list)
            TRY(process_property(property));
    } else if (!state.replacer_function && state.gap.is_empty() && can_use_direct_property_access(object)) {
        GC::Ref initial_shape = object.shape();
        auto* shape_cache = state.shape_cache.get(initial_shape);
        if (!shape_cache) {
            shape_cache = state.shape_cache.add(initial_shape);
            if (shape_cache) {
                shape_cache->properties.ensure_capacity(initial_shape->property_count());
                initial_shape->for_each_property_in_insertion_order([&](auto const& property_key, auto const& metadata) {
                    if (property_key.is_symbol() || !metadata.attributes.is_enumerable())
                        return;
                    VERIFY(property_key.is_string());

                    Utf16StringBuilder key_builder;
                    key_builder.append_quoted_escaped_for_json(property_key.as_string().view());
                    key_builder.append_ascii(':');
                    auto serialized_key = key_builder.to_string();

                    shape_cache->properties.unchecked_append({ property_key, metadata.offset, move(serialized_key) });
                });
            }
        }

        if (!shape_cache) {
            TRY(process_own_properties());
        } else {
            for (auto const& property : shape_cache->properties) {
                if (&object.shape() == initial_shape.ptr() && !object.get_direct(property.offset).is_accessor()) {
                    auto value = object.get_direct(property.offset);

                    size_t mark = builder.length_in_code_units();
                    if (!first)
                        builder.append_ascii(',');
                    builder.append(property.serialized_key.utf16_view());

                    bool wrote_value = TRY(serialize_json_value(vm, state, property.property, object, value));
                    if (wrote_value)
                        first = false;
                    else
                        builder.trim(builder.length_in_code_units() - mark);
                } else {
                    TRY(process_property(property.property));
                }
            }
        }
    } else {
        TRY(process_own_properties());
    }

    // Close the object
    --state.indent_depth;
    if (builder.length_in_code_units() > position_after_open_brace && !state.gap.is_empty()) {
        builder.append_ascii('\n');
        write_indent(builder, state.gap, state.indent_depth);
    }
    builder.append_ascii('}');

    state.object_stack.pop();
    return {};
}

// 25.5.2.5 SerializeJSONArray ( state, value ), https://tc39.es/ecma262/#sec-serializejsonarray
ThrowCompletionOr<void> JSONObject::serialize_json_array(VM& vm, StringifyState& state, Object& object)
{
    if (vm.did_reach_stack_space_limit())
        return vm.throw_completion<InternalError>(ErrorType::CallStackSizeExceeded);

    if (state.object_stack.contains(object))
        return vm.throw_completion<TypeError>(ErrorType::JsonCircular);

    state.object_stack.push(object);
    ++state.indent_depth;

    auto& builder = state.builder;
    // OPTIMIZATION: Packed arrays have no holes or indexed accessors, so their length and
    //               elements can be read directly while they remain packed.
    auto* simple_array = as_if<Array>(object);
    if (simple_array && !simple_array->is_simple_packed_array())
        simple_array = nullptr;

    auto length = simple_array ? simple_array->indexed_array_like_size() : TRY(length_of_array_like(vm, object));

    builder.append_ascii('[');

    for (size_t i = 0; i < length; ++i) {
        // Write separator
        if (i > 0) {
            builder.append_ascii(',');
            if (!state.gap.is_empty()) {
                builder.append_ascii('\n');
                write_indent(builder, state.gap, state.indent_depth);
            }
        } else if (!state.gap.is_empty()) {
            builder.append_ascii('\n');
            write_indent(builder, state.gap, state.indent_depth);
        }

        // Serialize value (undefined becomes null for arrays)
        bool wrote_value;
        if (simple_array && simple_array->is_simple_packed_array() && i < simple_array->indexed_array_like_size()) {
            auto value = simple_array->indexed_get(i)->value;
            wrote_value = TRY(serialize_json_value(vm, state, i, object, value));
        } else {
            wrote_value = TRY(serialize_json_property(vm, state, i, object));
        }
        if (!wrote_value)
            builder.append_ascii("null"sv);
    }

    // Close the array
    --state.indent_depth;
    if (length > 0 && !state.gap.is_empty()) {
        builder.append_ascii('\n');
        write_indent(builder, state.gap, state.indent_depth);
    }
    builder.append_ascii(']');

    state.object_stack.pop();
    return {};
}

// 25.5.1 JSON.parse ( text [ , reviver ] ), https://tc39.es/ecma262/#sec-json.parse
JS_DEFINE_NATIVE_FUNCTION(JSONObject::parse)
{
    auto& realm = *vm.current_realm();

    auto text = vm.argument(0);
    auto reviver = vm.argument(1);

    // 1. Let jsonString be ? ToString(text).
    auto json_string = TRY(text.to_utf16_string(vm));

    // 2. Let parseResult be ? ParseJSON(jsonString).
    // 3. Let unfiltered be parseResult.[[Value]].
    // NB: We build the JSON Parse Record snapshot inline during parsing, but only
    //     when a reviver is present and may observe the matched source text.
    JSONParseRecord root_record;
    auto unfiltered = TRY(parse_json(vm, json_string, reviver.is_function() ? &root_record : nullptr));

    // 4. If IsCallable(reviver) is false, return unfiltered.
    if (!reviver.is_function())
        return unfiltered;

    // 5. Let root be OrdinaryObjectCreate(%Object.prototype%).
    auto root = Object::create(realm, realm.intrinsics().object_prototype());

    // 6. Let rootName be the empty String.
    Utf16String root_name;

    // 7. Perform ! CreateDataPropertyOrThrow(root, rootName, unfiltered).
    MUST(root->create_data_property_or_throw(root_name, unfiltered));

    // 8. Let snapshot be CreateJSONParseRecord(parseResult.[[ParseNode]], rootName, unfiltered).
    // NB: Keep every value referenced by the parse record snapshot rooted: the records
    //     live in heap-allocated storage the GC does not scan, and the reviver may
    //     detach the original values from the object graph while still running.
    GC::RootVector<Value> kept_alive;
    Function<void(JSONParseRecord const&)> keep_alive = [&](JSONParseRecord const& record) {
        kept_alive.append(record.value);
        for (auto const& element : record.elements)
            keep_alive(element);
        for (auto const& entry : record.entries)
            keep_alive(entry);
    };
    keep_alive(root_record);

    // 9. Return ? InternalizeJSONProperty(root, rootName, reviver, snapshot).
    return internalize_json_property(vm, root, root_name, reviver.as_function(), &root_record);
}

// Unescape a JSON string, properly handling \uXXXX escape sequences including lone surrogates.
// simdjson validates UTF-8 strictly and rejects lone surrogates, but JSON allows them.
// Returns {} on malformed escape sequences.
static Optional<Utf16String> unescape_json_string(StringView raw)
{
    Utf16StringBuilder builder(raw.length());

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

    return builder.to_string();
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

struct JSONTextBytes {
    Utf16View text;
    StringBuilder utf8;
    Vector<size_t> byte_to_code_unit_offsets;

    StringView bytes() const { return utf8.string_view().substring_view(0, utf8.length() - simdjson::SIMDJSON_PADDING); }
    simdjson::padded_string_view padded_view() const { return simdjson::padded_string_view { bytes().characters_without_null_termination(), bytes().length(), utf8.length() }; }

    size_t byte_offset_to_code_unit_offset(size_t byte_offset) const
    {
        if (text.has_ascii_storage())
            return byte_offset;
        VERIFY(byte_offset < byte_to_code_unit_offsets.size());
        return byte_to_code_unit_offsets[byte_offset];
    }
};

struct JSONPropertyKeyCache {
    struct Entry {
        StringView raw_key;
        Optional<PropertyKey> key;
    };

    AK::Array<Entry, 128> entries;

    Entry& entry_for(StringView raw_key)
    {
        u32 ends = 0;
        if (!raw_key.is_empty())
            ends = (static_cast<u32>(static_cast<u8>(raw_key[0])) << 8) | static_cast<u8>(raw_key[raw_key.length() - 1]);
        return entries[pair_int_hash(raw_key.length(), ends) % entries.size()];
    }
};

struct JSONParseState {
    JSONTextBytes text;
    JSONPropertyKeyCache property_keys;
};

enum class TrackCodeUnitOffsets {
    No,
    Yes,
};

static void append_code_unit_offsets(Vector<size_t>& offsets, size_t byte_count, size_t code_unit_count, size_t& code_unit_offset)
{
    for (size_t trailing = 1; trailing < byte_count; ++trailing)
        offsets.unchecked_append(code_unit_offset);
    offsets.unchecked_append(code_unit_offset + code_unit_count);
    code_unit_offset += code_unit_count;
}

static void append_code_unit_offsets(Vector<size_t>& offsets, StringView bytes, size_t& code_unit_offset)
{
    for (size_t i = 0; i < bytes.length();) {
        auto lead = static_cast<u8>(bytes[i]);
        size_t byte_count = 1;
        if (lead >= 0xF0)
            byte_count = 4;
        else if (lead >= 0xE0)
            byte_count = 3;
        else if (lead >= 0xC0)
            byte_count = 2;
        append_code_unit_offsets(offsets, byte_count, byte_count == 4 ? 2 : 1, code_unit_offset);
        i += byte_count;
    }
}

static ErrorOr<JSONTextBytes> json_text_bytes(Utf16View text, TrackCodeUnitOffsets track_code_unit_offsets)
{
    auto maximum_length = text.has_ascii_storage() ? text.length_in_code_units() : 3 * text.length_in_code_units();
    JSONTextBytes text_bytes { text, StringBuilder { maximum_length + simdjson::SIMDJSON_PADDING }, {} };
    auto& utf8 = text_bytes.utf8;

    if (text.has_ascii_storage()) {
        TRY(utf8.try_append(StringView { text.bytes() }));
    } else {
        auto& offsets = text_bytes.byte_to_code_unit_offsets;
        bool track_offsets = track_code_unit_offsets == TrackCodeUnitOffsets::Yes;
        size_t code_unit_offset = 0;
        if (track_offsets)
            TRY(offsets.try_append(0));

        auto remaining = text;
        while (!remaining.is_empty()) {
            size_t valid_code_units = 0;
            bool all_valid = remaining.validate(valid_code_units);

            auto length_before = utf8.length();
            TRY(utf8.try_append(remaining.substring_view(0, valid_code_units)));
            if (track_offsets) {
                auto converted = utf8.string_view().substring_view(length_before);
                TRY(offsets.try_ensure_capacity(offsets.size() + converted.length()));
                append_code_unit_offsets(offsets, converted, code_unit_offset);
            }
            if (all_valid)
                break;

            TRY(utf8.try_appendff("\\u{:04X}", remaining.code_unit_at(valid_code_units)));
            if (track_offsets) {
                TRY(offsets.try_ensure_capacity(offsets.size() + 6));
                append_code_unit_offsets(offsets, 6, 1, code_unit_offset);
            }
            remaining = remaining.substring_view(valid_code_units + 1);
        }
    }

    TRY(utf8.try_append_repeated('\0', simdjson::SIMDJSON_PADDING));
    return text_bytes;
}

static ThrowCompletionOr<Value> parse_simdjson_value(VM&, JSONParseState&, simdjson::ondemand::value, JSONParseRecord* record = nullptr);

// The source text matched by a primitive parse node, used by JSON.parse revivers.
static Utf16String json_token_source(JSONTextBytes const& json_text, std::string_view raw)
{
    StringView source { raw.data(), raw.size() };
    size_t start = 0;
    size_t end = source.length();
    while (start < end && is_ascii_space(source[start]))
        ++start;
    while (end > start && is_ascii_space(source[end - 1]))
        --end;

    auto bytes = json_text.bytes();
    auto* bytes_data = bytes.characters_without_null_termination();
    auto* token_start = source.characters_without_null_termination() + start;
    auto* token_end = source.characters_without_null_termination() + end;
    VERIFY(token_start >= bytes_data);
    VERIFY(token_end >= token_start);
    VERIFY(static_cast<size_t>(token_end - bytes_data) <= bytes.length());

    auto code_unit_start = json_text.byte_offset_to_code_unit_offset(token_start - bytes_data);
    auto code_unit_end = json_text.byte_offset_to_code_unit_offset(token_end - bytes_data);
    return Utf16String::from_utf16(json_text.text.substring_view(code_unit_start, code_unit_end - code_unit_start));
}

template<typename T>
static ThrowCompletionOr<Value> parse_simdjson_number(VM& vm, T& value, StringView raw_sv)
{
    double double_value;
    auto error = value.get_double().get(double_value);
    if (!error) {
        TRY(ensure_simdjson_fully_parsed(vm, value));
        return Value(double_value);
    }

    if (error != simdjson::NUMBER_ERROR)
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    // Validate JSON number format (parse_first_number is more lenient than spec)
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

    // Handle overflow to infinity (e.g., 1e309)
    // simdjson returns NUMBER_ERROR for numbers that overflow double
    // Use parse_first_number as fallback - it handles overflow correctly
    auto result = parse_first_number<double>(raw_sv, TrimWhitespace::No);
    if (result.has_value() && result->characters_parsed == raw_sv.length())
        return Value(result->value);

    return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
}

template<typename T>
static ThrowCompletionOr<Value> parse_simdjson_string(VM& vm, JSONTextBytes const& json_text, T& value)
{
    // Use get_raw_json_string() to get the raw JSON string content (without quotes, with escapes),
    // then unescape ourselves to properly handle lone surrogates like \uD800 which simdjson rejects.
    simdjson::ondemand::raw_json_string raw_string;
    if (value.get_raw_json_string().get(raw_string))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
    char const* raw = raw_string.raw();
    auto bytes = json_text.bytes();
    StringView remaining { raw, static_cast<size_t>(bytes.characters_without_null_termination() + bytes.length() - raw) };

    // Find the length by looking for the closing quote (simdjson validated the structure)
    auto quote_offset = remaining.find('"');
    VERIFY(quote_offset.has_value());
    auto candidate = remaining.substring_view(0, *quote_offset);

    auto backslash_offset = candidate.find('\\');
    if (!backslash_offset.has_value()) {
        if (json_text.text.has_ascii_storage())
            return PrimitiveString::create(vm, Utf16String::from_ascii_without_validation(candidate.bytes()));
        return PrimitiveString::create(vm, Utf16String::from_utf8_without_validation(candidate));
    }

    size_t length = *backslash_offset;
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

static ThrowCompletionOr<PropertyKey> json_property_key(VM& vm, JSONParseState& state, StringView raw_key)
{
    auto& entry = state.property_keys.entry_for(raw_key);
    if (entry.key.has_value() && entry.raw_key == raw_key)
        return *entry.key;

    Optional<PropertyKey> key;
    if (raw_key.find('\\').has_value()) {
        auto unescaped_key = unescape_json_string(raw_key);
        if (!unescaped_key.has_value())
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        key = PropertyKey { unescaped_key.release_value() };
    } else if (state.text.text.has_ascii_storage()) {
        key = PropertyKey { Utf16FlyString::from_ascii_without_validation(raw_key) };
    } else {
        key = PropertyKey { Utf16FlyString::from_utf8_without_validation(raw_key) };
    }
    entry = { raw_key, key };
    return *key;
}

template<typename T>
static ThrowCompletionOr<Value> parse_simdjson_array(VM& vm, JSONParseState& state, T& value, JSONParseRecord* record = nullptr)
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
        JSONParseRecord element_record;
        auto parsed = TRY(parse_simdjson_value(vm, state, element_value, record ? &element_record : nullptr));
        array->define_direct_property(index++, parsed, default_attributes);
        if (record)
            record->elements.append(move(element_record));
    }

    TRY(ensure_simdjson_fully_parsed(vm, value));
    if (record)
        record->value = array;
    return array;
}

template<typename T>
static ThrowCompletionOr<Value> parse_simdjson_object(VM& vm, JSONParseState& state, T& value, JSONParseRecord* record = nullptr)
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
        auto key = TRY(json_property_key(vm, state, { raw_key.data(), raw_key.size() }));
        simdjson::ondemand::value field_value;
        if (field.value().get(field_value))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        JSONParseRecord entry_record;
        auto parsed = TRY(parse_simdjson_value(vm, state, field_value, record ? &entry_record : nullptr));
        object->define_direct_property(key, parsed, default_attributes);
        if (record) {
            entry_record.key = key.to_utf16_string();
            // Duplicate keys keep the last value, matching object property semantics.
            if (auto existing = record->entries.find_if([&](auto& entry) { return entry.key == entry_record.key; }); existing != record->entries.end())
                *existing = move(entry_record);
            else
                record->entries.append(move(entry_record));
        }
    }

    TRY(ensure_simdjson_fully_parsed(vm, value));
    if (record)
        record->value = object;
    return object;
}

static ThrowCompletionOr<Value> parse_simdjson_value(VM& vm, JSONParseState& state, simdjson::ondemand::value value, JSONParseRecord* record)
{
    simdjson::ondemand::json_type type;
    if (value.type().get(type))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    // NB: raw_json_token() must be captured before the value is consumed by the get_* calls below.
    switch (type) {
    case simdjson::ondemand::json_type::null:
        if (record) {
            record->value = js_null();
            record->source = json_token_source(state.text, value.raw_json_token());
        }
        return js_null();
    case simdjson::ondemand::json_type::boolean: {
        auto token = value.raw_json_token();
        bool boolean_value;
        if (value.get_bool().get(boolean_value))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        if (record) {
            record->value = Value(boolean_value);
            record->source = json_token_source(state.text, token);
        }
        return Value(boolean_value);
    }
    case simdjson::ondemand::json_type::number: {
        auto raw = value.raw_json_token();
        StringView raw_sv { raw.data(), raw.size() };
        auto parsed = TRY(parse_simdjson_number(vm, value, raw_sv));
        if (record) {
            record->value = parsed;
            record->source = json_token_source(state.text, raw);
        }
        return parsed;
    }
    case simdjson::ondemand::json_type::string: {
        auto token = value.raw_json_token();
        auto parsed = TRY(parse_simdjson_string(vm, state.text, value));
        if (record) {
            record->value = parsed;
            record->source = json_token_source(state.text, token);
        }
        return parsed;
    }
    case simdjson::ondemand::json_type::array:
        return parse_simdjson_array(vm, state, value, record);
    case simdjson::ondemand::json_type::object:
        return parse_simdjson_object(vm, state, value, record);
    case simdjson::ondemand::json_type::unknown:
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
    }

    VERIFY_NOT_REACHED();
}

static ThrowCompletionOr<Value> parse_simdjson_document(VM& vm, JSONParseState& state, simdjson::ondemand::document& document, JSONParseRecord* record = nullptr)
{
    simdjson::ondemand::json_type type;
    if (document.type().get(type))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    // NB: raw_json_token() must be captured before the value is consumed by the get_* calls below.
    std::string_view raw_token;
    if ((type == simdjson::ondemand::json_type::boolean || type == simdjson::ondemand::json_type::number)
        && document.raw_json_token().get(raw_token))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    switch (type) {
    case simdjson::ondemand::json_type::null: {
        if (document.is_null().error())
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        if (!document.at_end())
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        if (record) {
            std::string_view null_token;
            if (document.raw_json_token().get(null_token))
                return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
            record->value = js_null();
            record->source = json_token_source(state.text, null_token);
        }
        return js_null();
    }
    case simdjson::ondemand::json_type::boolean: {
        bool boolean_value;
        if (document.get_bool().get(boolean_value))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        if (!document.at_end())
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        if (record) {
            record->value = Value(boolean_value);
            record->source = json_token_source(state.text, raw_token);
        }
        return Value(boolean_value);
    }
    case simdjson::ondemand::json_type::number: {
        StringView raw_sv { raw_token.data(), raw_token.size() };
        auto trimmed = raw_sv.trim_whitespace();
        auto parsed = TRY(parse_simdjson_number(vm, document, trimmed));
        if (record) {
            record->value = parsed;
            record->source = json_token_source(state.text, raw_token);
        }
        return parsed;
    }
    case simdjson::ondemand::json_type::string: {
        std::string_view string_token;
        if (record && document.raw_json_token().get(string_token))
            return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
        auto parsed = TRY(parse_simdjson_string(vm, state.text, document));
        if (record) {
            record->value = parsed;
            record->source = json_token_source(state.text, string_token);
        }
        return parsed;
    }
    case simdjson::ondemand::json_type::array:
        return parse_simdjson_array(vm, state, document, record);
    case simdjson::ondemand::json_type::object:
        return parse_simdjson_object(vm, state, document, record);
    case simdjson::ondemand::json_type::unknown:
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);
    }

    VERIFY_NOT_REACHED();
}

// 25.5.1.1 ParseJSON ( text ), https://tc39.es/ecma262/#sec-ParseJSON
ThrowCompletionOr<Value> JSONObject::parse_json(VM& vm, Utf16View text, JSONParseRecord* root_record)
{
    // 1. If StringToCodePoints(text) is not a valid JSON text as specified in ECMA-404, throw a SyntaxError exception.
    // NB: Per ECMA-404, the BOM is not valid JSON whitespace. simdjson silently skips it, so we must reject it explicitly.
    if (text.length_in_code_units() >= 1 && text.code_unit_at(0) == 0xFEFF)
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    JSONParseState state { json_text_bytes(text, root_record ? TrackCodeUnitOffsets::Yes : TrackCodeUnitOffsets::No).release_value_but_fixme_should_propagate_errors(), {} };

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (parser.iterate(state.text.padded_view()).get(document))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    // 2. Let scriptString be the string-concatenation of "(", text, and ");".
    // 3. Let script be ParseText(scriptString, Script).
    // 4. NOTE: The early error rules defined in 13.2.5.1 have special handling for the above invocation of ParseText.
    // 5. Assert: script is a Parse Node.
    // 6. Let result be ! Evaluation of script.
    auto result = TRY(parse_simdjson_document(vm, state, document, root_record));

    // 7. NOTE: The PropertyDefinitionEvaluation semantics defined in 13.2.5.5 have special handling for the above evaluation.
    // 8. Assert: result is either a String, a Number, a Boolean, an Object that is defined by either an ArrayLiteral or an ObjectLiteral, or null.

    // 9. Return result.
    return result;
}

// 25.5.1.1 InternalizeJSONProperty ( holder, name, reviver, parseRecord ), https://tc39.es/ecma262/#sec-internalizejsonproperty
ThrowCompletionOr<Value> JSONObject::internalize_json_property(VM& vm, GC::Ref<Object> holder, PropertyKey const& name, FunctionObject& reviver, JSONParseRecord const* parse_record)
{
    auto& realm = *vm.current_realm();

    // 1. Let value be ? Get(holder, name).
    auto value = TRY(holder->get(name));

    // 2. Let context be OrdinaryObjectCreate(%Object.prototype%).
    auto context = Object::create(realm, realm.intrinsics().object_prototype());

    // 3. If parseRecord is a JSON Parse Record and SameValue(parseRecord.[[Value]], value) is true, then
    // NB: An empty List of records is represented as a null pointer here.
    Vector<JSONParseRecord> const* element_records = nullptr;
    Vector<JSONParseRecord> const* entry_records = nullptr;
    if (parse_record && same_value(parse_record->value, value)) {
        // a. If value is not an Object, then
        if (!value.is_object()) {
            // i. Let parseNode be parseRecord.[[ParseNode]].
            // ii. Assert: parseNode is neither an ArrayLiteral Parse Node nor an ObjectLiteral Parse Node.
            // iii. Let sourceText be the source text matched by parseNode.
            // iv. Perform ! CreateDataPropertyOrThrow(context, "source", CodePointsToString(sourceText)).
            // NB: We captured sourceText while parsing rather than from a retained parse node.
            VERIFY(parse_record->source.has_value());
            MUST(context->create_data_property_or_throw(vm.names.source, PrimitiveString::create(vm, *parse_record->source)));
        }
        // b. Let elementRecords be parseRecord.[[Elements]].
        element_records = &parse_record->elements;
        // c. Let entryRecords be parseRecord.[[Entries]].
        entry_records = &parse_record->entries;
    }
    // 4. Else,
    //     a. Let elementRecords be a new empty List.
    //     b. Let entryRecords be a new empty List.

    // 5. If value is an Object, then
    if (value.is_object()) {
        // a. Let isArray be ? IsArray(value).
        auto is_array = TRY(value.is_array(vm));

        auto& value_object = value.as_object();
        auto process_property = [&](PropertyKey const& key, JSONParseRecord const* child_record) -> ThrowCompletionOr<void> {
            // i/ii/iii. Let newElement be ? InternalizeJSONProperty(value, propertyKey, reviver, elementRecord/entryRecord).
            auto new_element = TRY(internalize_json_property(vm, value_object, key, reviver, child_record));
            // If newElement is undefined, perform ? value.[[Delete]](propertyKey).
            if (new_element.is_undefined())
                TRY(value_object.internal_delete(key));
            // Else, perform ? CreateDataProperty(value, propertyKey, newElement).
            else
                TRY(value_object.create_data_property(key, new_element));
            return {};
        };

        // b. If isArray is true, then
        if (is_array) {
            // i. Let elementRecordsLength be the number of elements in elementRecords.
            auto element_records_length = element_records ? element_records->size() : 0;
            // ii. Let length be ? LengthOfArrayLike(value).
            auto length = TRY(length_of_array_like(vm, value_object));
            // iii-iv. Repeat, while index < length,
            for (size_t index = 0; index < length; ++index) {
                // 2. If index < elementRecordsLength, let elementRecord be elementRecords[index]; else let elementRecord be empty.
                auto const* element_record = index < element_records_length ? &element_records->at(index) : nullptr;
                TRY(process_property(index, element_record));
            }
        }
        // c. Else,
        else {
            // i. Let keys be ? EnumerableOwnProperties(value, key).
            auto keys = TRY(value_object.enumerable_own_property_names(Object::PropertyKind::Key));
            // ii. For each String propertyKey of keys, do
            for (auto& property_key : keys) {
                auto key = property_key.as_string().utf16_string();
                // 1. If there exists an element entry of entryRecords such that entry.[[Key]] is propertyKey,
                //    let entryRecord be entry; else let entryRecord be empty.
                JSONParseRecord const* entry_record = nullptr;
                if (entry_records) {
                    if (auto entry = entry_records->find_if([&](auto& entry) { return entry.key == key; }); entry != entry_records->end())
                        entry_record = &*entry;
                }
                TRY(process_property(key, entry_record));
            }
        }
    }

    // 6. Return ? Call(reviver, holder, « name, value, context »).
    return TRY(call(vm, reviver, holder, PrimitiveString::create(vm, name.to_utf16_string()), value, context));
}

// 1.3 JSON.rawJSON ( text ), https://tc39.es/proposal-json-parse-with-source/#sec-json.rawjson
JS_DEFINE_NATIVE_FUNCTION(JSONObject::raw_json)
{
    auto& realm = *vm.current_realm();

    // 1. Let jsonString be ? ToString(text).
    auto json_string = TRY(vm.argument(0).to_utf16_string(vm));

    // 2. Throw a SyntaxError exception if jsonString is the empty String, or if either the first or last code unit of
    //    jsonString is any of 0x0009 (CHARACTER TABULATION), 0x000A (LINE FEED), 0x000D (CARRIAGE RETURN), or
    //    0x0020 (SPACE).
    auto json_string_view = json_string.utf16_view();
    if (json_string_view.is_empty())
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    static constexpr AK::Array invalid_code_points { 0x09, 0x0A, 0x0D, 0x20 };
    auto first_char = json_string_view.code_unit_at(0);
    auto last_char = json_string_view.code_unit_at(json_string_view.length_in_code_units() - 1);

    if (invalid_code_points.contains_slow(first_char) || invalid_code_points.contains_slow(last_char))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    // 3. Parse StringToCodePoints(jsonString) as a JSON text as specified in ECMA-404. Throw a SyntaxError exception
    //    if it is not a valid JSON text as defined in that specification, or if its outermost value is an object or
    //    array as defined in that specification.
    auto json_text = json_text_bytes(json_string_view, TrackCodeUnitOffsets::No).release_value_but_fixme_should_propagate_errors();

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(json_text.padded_view()).get(doc))
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
