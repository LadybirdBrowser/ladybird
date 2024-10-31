/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/BigIntObject.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/Geometry/DOMMatrixReadOnly.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/Geometry/DOMPointReadOnly.h>
#include <LibWeb/Geometry/DOMQuad.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectReadOnly.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// Binary format:
// A list of adjacent shallow values, which may contain references to other
// values (noted by their position in the list, one value following another).
// This list represents the "memory" in the StructuredSerialize algorithm.
// The first item in the list is the root, i.e., the value of everything.
// The format is generally u32-aligned (hence this leaking out into the type)
// Each value has a length based on its type, as defined below.
//
// (Should more redundancy be added, e.g., for lengths/positions of values?)

enum ValueTag {
    // Unused, for ease of catching bugs.
    Empty,

    // UndefinedPrimitive is serialized indicating that the Type is Undefined, no value is serialized.
    UndefinedPrimitive,

    // NullPrimitive is serialized indicating that the Type is Null, no value is serialized.
    NullPrimitive,

    // Following u32 is the boolean value.
    BooleanPrimitive,

    // Following two u32s are the double value.
    NumberPrimitive,

    // The BigIntPrimitive is serialized as a string in base 10 representation.
    // Following two u32s representing the length of the string, then the following u32s, equal to size, is the string representation.
    BigIntPrimitive,

    // Following two u32s representing the length of the string, then the following u32s, equal to size, is the string representation.
    StringPrimitive,

    BooleanObject,

    NumberObject,

    BigIntObject,

    StringObject,

    DateObject,

    RegExpObject,

    GrowableSharedArrayBuffer,

    SharedArrayBuffer,

    ResizeableArrayBuffer,

    ArrayBuffer,

    ArrayBufferView,

    MapObject,

    SetObject,

    ErrorObject,

    ArrayObject,

    Object,

    ObjectReference,

    SerializableObject,

    // TODO: Define many more types

    // This tag or higher are understood to be errors
    ValueTagMax,
};

enum ErrorType {
    Error,
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    ClassName,
    JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
};

static ErrorType error_name_to_type(String const& name)
{
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    if (name == #ClassName##sv)                                                          \
        return ErrorType::ClassName;
    JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
    return Error;
}

// Serializing and deserializing are each two passes:
// 1. Fill up the memory with all the values, but without translating references
// 2. Translate all the references into the appropriate form

class Serializer {
public:
    Serializer(JS::VM& vm, SerializationMemory& memory, bool for_storage)
        : m_vm(vm)
        , m_memory(memory)
        , m_for_storage(for_storage)
    {
    }

    // https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
    WebIDL::ExceptionOr<SerializationRecord> serialize(JS::Value value)
    {
        // 2. If memory[value] exists, then return memory[value].
        if (m_memory.contains(value)) {
            auto index = m_memory.get(value).value();
            return Vector<u32> { ValueTag::ObjectReference, index };
        }

        // 3. Let deep be false.
        auto deep = false;

        bool return_primitive_type = true;
        // 4. If value is undefined, null, a Boolean, a Number, a BigInt, or a String, then return { [[Type]]: "primitive", [[Value]]: value }.
        if (value.is_undefined()) {
            serialize_enum(m_serialized, ValueTag::UndefinedPrimitive);
        } else if (value.is_null()) {
            serialize_enum(m_serialized, ValueTag::NullPrimitive);
        } else if (value.is_boolean()) {
            serialize_enum(m_serialized, ValueTag::BooleanPrimitive);
            serialize_boolean_primitive(m_serialized, value);
        } else if (value.is_number()) {
            serialize_enum(m_serialized, ValueTag::NumberPrimitive);
            serialize_number_primitive(m_serialized, value);
        } else if (value.is_bigint()) {
            serialize_enum(m_serialized, ValueTag::BigIntPrimitive);
            TRY(serialize_big_int_primitive(m_vm, m_serialized, value));
        } else if (value.is_string()) {
            serialize_enum(m_serialized, ValueTag::StringPrimitive);
            TRY(serialize_string_primitive(m_vm, m_serialized, value));
        } else {
            return_primitive_type = false;
        }

        if (return_primitive_type)
            return m_serialized;

        // 5. If value is a Symbol, then throw a "DataCloneError" DOMException.
        if (value.is_symbol())
            return WebIDL::DataCloneError::create(*m_vm.current_realm(), "Cannot serialize Symbol"_string);

        // 6. Let serialized be an uninitialized value.

        // 7. If value has a [[BooleanData]] internal slot, then set serialized to { [[Type]]: "Boolean", [[BooleanData]]: value.[[BooleanData]] }.
        if (value.is_object() && is<JS::BooleanObject>(value.as_object())) {
            serialize_enum(m_serialized, ValueTag::BooleanObject);
            serialize_boolean_object(m_serialized, value);
        }

        // 8. Otherwise, if value has a [[NumberData]] internal slot, then set serialized to { [[Type]]: "Number", [[NumberData]]: value.[[NumberData]] }.
        else if (value.is_object() && is<JS::NumberObject>(value.as_object())) {
            serialize_enum(m_serialized, ValueTag::NumberObject);
            serialize_number_object(m_serialized, value);
        }

        // 9. Otherwise, if value has a [[BigIntData]] internal slot, then set serialized to { [[Type]]: "BigInt", [[BigIntData]]: value.[[BigIntData]] }.
        else if (value.is_object() && is<JS::BigIntObject>(value.as_object())) {
            serialize_enum(m_serialized, ValueTag::BigIntObject);
            TRY(serialize_big_int_object(m_vm, m_serialized, value));
        }

        // 10. Otherwise, if value has a [[StringData]] internal slot, then set serialized to { [[Type]]: "String", [[StringData]]: value.[[StringData]] }.
        else if (value.is_object() && is<JS::StringObject>(value.as_object())) {
            serialize_enum(m_serialized, ValueTag::StringObject);
            TRY(serialize_string_object(m_vm, m_serialized, value));
        }

        // 11. Otherwise, if value has a [[DateValue]] internal slot, then set serialized to { [[Type]]: "Date", [[DateValue]]: value.[[DateValue]] }.
        else if (value.is_object() && is<JS::Date>(value.as_object())) {
            serialize_enum(m_serialized, ValueTag::DateObject);
            serialize_date_object(m_serialized, value);
        }

        // 12. Otherwise, if value has a [[RegExpMatcher]] internal slot, then set serialized to
        //     { [[Type]]: "RegExp", [[RegExpMatcher]]: value.[[RegExpMatcher]], [[OriginalSource]]: value.[[OriginalSource]],
        //       [[OriginalFlags]]: value.[[OriginalFlags]] }.
        else if (value.is_object() && is<JS::RegExpObject>(value.as_object())) {
            serialize_enum(m_serialized, ValueTag::RegExpObject);
            TRY(serialize_reg_exp_object(m_vm, m_serialized, value));
        }

        // 13. Otherwise, if value has an [[ArrayBufferData]] internal slot, then:
        else if (value.is_object() && is<JS::ArrayBuffer>(value.as_object())) {
            TRY(serialize_array_buffer(m_vm, m_serialized, static_cast<JS::ArrayBuffer&>(value.as_object()), m_for_storage));
        }

        // 14. Otherwise, if value has a [[ViewedArrayBuffer]] internal slot, then:
        else if (value.is_object() && is<JS::TypedArrayBase>(value.as_object())) {
            TRY(serialize_viewed_array_buffer(m_vm, m_serialized, static_cast<JS::TypedArrayBase&>(value.as_object()), m_for_storage, m_memory));
        } else if (value.is_object() && is<JS::DataView>(value.as_object())) {
            TRY(serialize_viewed_array_buffer(m_vm, m_serialized, static_cast<JS::DataView&>(value.as_object()), m_for_storage, m_memory));
        }

        // 15. Otherwise, if value has [[MapData]] internal slot, then:
        else if (value.is_object() && is<JS::Map>(value.as_object())) {
            // 1. Set serialized to { [[Type]]: "Map", [[MapData]]: a new empty List }.
            serialize_enum(m_serialized, ValueTag::MapObject);
            // 2. Set deep to true.
            deep = true;
        }

        // 16. Otherwise, if value has [[SetData]] internal slot, then:
        else if (value.is_object() && is<JS::Set>(value.as_object())) {
            // 1. Set serialized to { [[Type]]: "Set", [[SetData]]: a new empty List }.
            serialize_enum(m_serialized, ValueTag::SetObject);
            // 2. Set deep to true.
            deep = true;
        }

        // 17. Otherwise, if value has an [[ErrorData]] internal slot and value is not a platform object, then:
        else if (value.is_object() && is<JS::Error>(value.as_object()) && !is<Bindings::PlatformObject>(value.as_object())) {
            // 1. Let name be ? Get(value, "name").
            auto name_property = TRY(value.as_object().get(m_vm.names.name));

            // FIXME: Spec bug - https://github.com/whatwg/html/issues/9923
            // MISSING STEP: Set name to ? ToString(name).
            auto name = TRY(name_property.to_string(m_vm));

            // 2. If name is not one of "Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", or "URIError", then set name to "Error".
            auto type = error_name_to_type(name);

            // 3. Let valueMessageDesc be ? value.[[GetOwnProperty]]("message").
            auto value_message_descriptor = TRY(value.as_object().internal_get_own_property(m_vm.names.message));

            // 4. Let message be undefined if IsDataDescriptor(valueMessageDesc) is false, and ? ToString(valueMessageDesc.[[Value]]) otherwise.
            Optional<String> message;
            if (value_message_descriptor.has_value() && value_message_descriptor->is_data_descriptor())
                message = TRY(value_message_descriptor->value->to_string(m_vm));

            // 5. Set serialized to { [[Type]]: "Error", [[Name]]: name, [[Message]]: message }.
            // FIXME: 6. User agents should attach a serialized representation of any interesting accompanying data which are not yet specified, notably the stack property, to serialized.
            serialize_enum(m_serialized, ValueTag::ErrorObject);
            serialize_enum(m_serialized, type);
            serialize_primitive_type(m_serialized, message.has_value());
            if (message.has_value())
                TRY(serialize_string(m_vm, m_serialized, *message));
        }

        // 18. Otherwise, if value is an Array exotic object, then:
        else if (value.is_object() && is<JS::Array>(value.as_object())) {
            // 1. Let valueLenDescriptor be ? OrdinaryGetOwnProperty(value, "length").
            // 2. Let valueLen be valueLenDescriptor.[[Value]].
            // NON-STANDARD: Array objects in LibJS do not have a real length property, so it must be accessed the usual way
            u64 length = MUST(JS::length_of_array_like(m_vm, value.as_object()));

            // 3. Set serialized to { [[Type]]: "Array", [[Length]]: valueLen, [[Properties]]: a new empty List }.
            serialize_enum(m_serialized, ValueTag::ArrayObject);
            serialize_primitive_type(m_serialized, length);

            // 4. Set deep to true.
            deep = true;
        }

        // 19. Otherwise, if value is a platform object that is a serializable object:
        else if (value.is_object() && is<Bindings::Serializable>(value.as_object())) {
            auto& serializable = dynamic_cast<Bindings::Serializable&>(value.as_object());

            // FIXME: 1. If value has a [[Detached]] internal slot whose value is true, then throw a "DataCloneError" DOMException.

            // 2. Let typeString be the identifier of the primary interface of value.
            // 3. Set serialized to { [[Type]]: typeString }.
            serialize_enum(m_serialized, ValueTag::SerializableObject);
            TRY(serialize_string(m_vm, m_serialized, serializable.interface_name()));

            // 4. Set deep to true
            deep = true;
        }

        // 20. Otherwise, if value is a platform object, then throw a "DataCloneError" DOMException.
        else if (value.is_object() && is<Bindings::PlatformObject>(value.as_object())) {
            return throw_completion(WebIDL::DataCloneError::create(*m_vm.current_realm(), "Cannot serialize platform objects"_string));
        }

        // 21. Otherwise, if IsCallable(value) is true, then throw a "DataCloneError" DOMException.
        else if (value.is_function()) {
            return throw_completion(WebIDL::DataCloneError::create(*m_vm.current_realm(), "Cannot serialize functions"_string));
        }

        // FIXME: 22. Otherwise, if value has any internal slot other than [[Prototype]] or [[Extensible]], then throw a "DataCloneError" DOMException.

        // FIXME: 23. Otherwise, if value is an exotic object and value is not the %Object.prototype% intrinsic object associated with any realm, then throw a "DataCloneError" DOMException.

        // 24. Otherwise:
        else {
            // 1. Set serialized to { [[Type]]: "Object", [[Properties]]: a new empty List }.
            serialize_enum(m_serialized, ValueTag::Object);

            // 2. Set deep to true.
            deep = true;
        }

        // 25. Set memory[value] to serialized.
        m_memory.set(make_handle(value), m_next_id++);

        // 26. If deep is true, then:
        if (deep) {
            // 1. If value has a [[MapData]] internal slot, then:
            if (value.is_object() && is<JS::Map>(value.as_object())) {
                auto const& map = static_cast<JS::Map const&>(value.as_object());
                // 1. Let copiedList be a new empty List.
                Vector<JS::Value> copied_list;
                copied_list.ensure_capacity(map.map_size() * 2);
                // 2. For each Record { [[Key]], [[Value]] } entry of value.[[MapData]]:
                for (auto const& entry : static_cast<JS::Map const&>(value.as_object())) {
                    // 1. Let copiedEntry be a new Record { [[Key]]: entry.[[Key]], [[Value]]: entry.[[Value]] }.
                    // 2. If copiedEntry.[[Key]] is not the special value empty, append copiedEntry to copiedList.
                    copied_list.append(entry.key);
                    copied_list.append(entry.value);
                }
                u64 size = map.map_size();
                m_serialized.append(bit_cast<u32*>(&size), 2);
                // 3. For each Record { [[Key]], [[Value]] } entry of copiedList:
                for (auto copied_value : copied_list) {
                    // 1. Let serializedKey be ? StructuredSerializeInternal(entry.[[Key]], forStorage, memory).
                    // 2. Let serializedValue be ? StructuredSerializeInternal(entry.[[Value]], forStorage, memory).
                    auto serialized_value = TRY(structured_serialize_internal(m_vm, copied_value, m_for_storage, m_memory));

                    // 3. Append { [[Key]]: serializedKey, [[Value]]: serializedValue } to serialized.[[MapData]].
                    m_serialized.extend(serialized_value);
                }
            }

            // 2. Otherwise, if value has a [[SetData]] internal slot, then:
            else if (value.is_object() && is<JS::Set>(value.as_object())) {
                auto const& set = static_cast<JS::Set const&>(value.as_object());
                // 1. Let copiedList be a new empty List.
                Vector<JS::Value> copied_list;
                copied_list.ensure_capacity(set.set_size());
                // 2. For each entry of value.[[SetData]]:
                for (auto const& entry : static_cast<JS::Set const&>(value.as_object())) {
                    // 1. If entry is not the special value empty, append entry to copiedList.
                    copied_list.append(entry.key);
                }
                serialize_primitive_type(m_serialized, set.set_size());
                // 3. For each entry of copiedList:
                for (auto copied_value : copied_list) {
                    // 1. Let serializedEntry be ? StructuredSerializeInternal(entry, forStorage, memory).
                    auto serialized_value = TRY(structured_serialize_internal(m_vm, copied_value, m_for_storage, m_memory));

                    // 2. Append serializedEntry to serialized.[[SetData]].
                    m_serialized.extend(serialized_value);
                }
            }

            // 3. Otherwise, if value is a platform object that is a serializable object, then perform the serialization steps for value's primary interface, given value, serialized, and forStorage.
            else if (value.is_object() && is<Bindings::Serializable>(value.as_object())) {
                auto& serializable = dynamic_cast<Bindings::Serializable&>(value.as_object());
                TRY(serializable.serialization_steps(m_serialized, m_for_storage, m_memory));
            }

            // 4. Otherwise, for each key in ! EnumerableOwnProperties(value, key):
            else {
                u64 property_count = 0;
                auto count_offset = m_serialized.size();
                serialize_primitive_type(m_serialized, property_count);
                for (auto key : MUST(value.as_object().enumerable_own_property_names(JS::Object::PropertyKind::Key))) {
                    auto property_key = MUST(JS::PropertyKey::from_value(m_vm, key));

                    // 1. If ! HasOwnProperty(value, key) is true, then:
                    if (MUST(value.as_object().has_own_property(property_key))) {
                        // 1. Let inputValue be ? value.[[Get]](key, value).
                        auto input_value = TRY(value.as_object().internal_get(property_key, value));

                        // 2. Let outputValue be ? StructuredSerializeInternal(inputValue, forStorage, memory).
                        auto output_value = TRY(structured_serialize_internal(m_vm, input_value, m_for_storage, m_memory));

                        // 3. Append { [[Key]]: key, [[Value]]: outputValue } to serialized.[[Properties]].
                        TRY(serialize_string(m_vm, m_serialized, key.as_string()));
                        m_serialized.extend(output_value);

                        property_count++;
                    }
                }
                memcpy(m_serialized.data() + count_offset, &property_count, sizeof(property_count));
            }
        }

        // 27. Return serialized.
        return m_serialized;
    }

private:
    JS::VM& m_vm;
    SerializationMemory& m_memory; // JS value -> index
    u32 m_next_id { 0 };
    SerializationRecord m_serialized;
    bool m_for_storage { false };
};

void serialize_boolean_primitive(SerializationRecord& serialized, JS::Value& value)
{
    VERIFY(value.is_boolean());
    serialize_primitive_type(serialized, value.as_bool());
}

void serialize_number_primitive(SerializationRecord& serialized, JS::Value& value)
{
    VERIFY(value.is_number());
    serialize_primitive_type(serialized, value.as_double());
}

WebIDL::ExceptionOr<void> serialize_big_int_primitive(JS::VM& vm, SerializationRecord& serialized, JS::Value& value)
{
    VERIFY(value.is_bigint());
    auto& val = value.as_bigint();
    TRY(serialize_string(vm, serialized, TRY_OR_THROW_OOM(vm, val.to_string())));
    return {};
}

WebIDL::ExceptionOr<void> serialize_string_primitive(JS::VM& vm, SerializationRecord& serialized, JS::Value& value)
{
    VERIFY(value.is_string());
    TRY(serialize_string(vm, serialized, value.as_string()));
    return {};
}

void serialize_boolean_object(SerializationRecord& serialized, JS::Value& value)
{
    VERIFY(value.is_object() && is<JS::BooleanObject>(value.as_object()));
    auto& boolean_object = static_cast<JS::BooleanObject&>(value.as_object());
    serialize_primitive_type(serialized, boolean_object.boolean());
}

void serialize_number_object(SerializationRecord& serialized, JS::Value& value)
{
    VERIFY(value.is_object() && is<JS::NumberObject>(value.as_object()));
    auto& number_object = static_cast<JS::NumberObject&>(value.as_object());
    serialize_primitive_type(serialized, number_object.number());
}

WebIDL::ExceptionOr<void> serialize_big_int_object(JS::VM& vm, SerializationRecord& serialized, JS::Value& value)
{
    VERIFY(value.is_object() && is<JS::BigIntObject>(value.as_object()));
    auto& bigint_object = static_cast<JS::BigIntObject&>(value.as_object());
    TRY(serialize_string(vm, serialized, TRY_OR_THROW_OOM(vm, bigint_object.bigint().to_string())));
    return {};
}

WebIDL::ExceptionOr<void> serialize_string_object(JS::VM& vm, SerializationRecord& serialized, JS::Value& value)
{
    VERIFY(value.is_object() && is<JS::StringObject>(value.as_object()));
    auto& string_object = static_cast<JS::StringObject&>(value.as_object());
    TRY(serialize_string(vm, serialized, string_object.primitive_string()));
    return {};
}

void serialize_date_object(SerializationRecord& serialized, JS::Value& value)
{
    VERIFY(value.is_object() && is<JS::Date>(value.as_object()));
    auto& date_object = static_cast<JS::Date&>(value.as_object());
    serialize_primitive_type(serialized, date_object.date_value());
}

WebIDL::ExceptionOr<void> serialize_reg_exp_object(JS::VM& vm, SerializationRecord& serialized, JS::Value& value)
{
    VERIFY(value.is_object() && is<JS::RegExpObject>(value.as_object()));
    auto& regexp_object = static_cast<JS::RegExpObject&>(value.as_object());
    // Note: A Regex<ECMA262> object is perfectly happy to be reconstructed with just the source+flags
    //       In the future, we could optimize the work being done on the deserialize step by serializing
    //       more of the internal state (the [[RegExpMatcher]] internal slot)
    TRY(serialize_string(vm, serialized, TRY_OR_THROW_OOM(vm, String::from_byte_string(regexp_object.pattern()))));
    TRY(serialize_string(vm, serialized, TRY_OR_THROW_OOM(vm, String::from_byte_string(regexp_object.flags()))));
    return {};
}

WebIDL::ExceptionOr<void> serialize_bytes(JS::VM& vm, Vector<u32>& vector, ReadonlyBytes bytes)
{
    // Append size of the buffer to the serialized structure.
    u64 const size = bytes.size();
    serialize_primitive_type(vector, size);
    // Append the bytes of the buffer to the serialized structure.
    u64 byte_position = 0;
    while (byte_position < size) {
        u32 combined_value = 0;
        for (u8 i = 0; i < 4; ++i) {
            u8 const byte = bytes[byte_position];
            combined_value |= byte << (i * 8);
            byte_position++;
            if (byte_position == size)
                break;
        }
        TRY_OR_THROW_OOM(vm, vector.try_append(combined_value));
    }
    return {};
}

WebIDL::ExceptionOr<void> serialize_string(JS::VM& vm, Vector<u32>& vector, DeprecatedFlyString const& string)
{
    return serialize_bytes(vm, vector, string.view().bytes());
}

WebIDL::ExceptionOr<void> serialize_string(JS::VM& vm, Vector<u32>& vector, String const& string)
{
    return serialize_bytes(vm, vector, { string.code_points().bytes(), string.code_points().byte_length() });
}

WebIDL::ExceptionOr<void> serialize_string(JS::VM& vm, Vector<u32>& vector, JS::PrimitiveString const& primitive_string)
{
    auto string = primitive_string.utf8_string();
    TRY(serialize_string(vm, vector, string));
    return {};
}

WebIDL::ExceptionOr<void> serialize_array_buffer(JS::VM& vm, Vector<u32>& vector, JS::ArrayBuffer const& array_buffer, bool for_storage)
{
    // 13. Otherwise, if value has an [[ArrayBufferData]] internal slot, then:

    // FIXME: 1.  If IsSharedArrayBuffer(value) is true, then:
    if (false) {
        // 1. If the current settings object's cross-origin isolated capability is false, then throw a "DataCloneError" DOMException.
        // NOTE: This check is only needed when serializing (and not when deserializing) as the cross-origin isolated capability cannot change
        //       over time and a SharedArrayBuffer cannot leave an agent cluster.
        if (current_settings_object().cross_origin_isolated_capability() == CanUseCrossOriginIsolatedAPIs::No)
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot serialize SharedArrayBuffer when cross-origin isolated"_string);

        // 2. If forStorage is true, then throw a "DataCloneError" DOMException.
        if (for_storage)
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot serialize SharedArrayBuffer for storage"_string);

        // FIXME: 3. If value has an [[ArrayBufferMaxByteLength]] internal slot, then set serialized to { [[Type]]: "GrowableSharedArrayBuffer",
        //           [[ArrayBufferData]]: value.[[ArrayBufferData]], [[ArrayBufferByteLengthData]]: value.[[ArrayBufferByteLengthData]],
        //           [[ArrayBufferMaxByteLength]]: value.[[ArrayBufferMaxByteLength]], [[AgentCluster]]: the surrounding agent's agent cluster }.
        // FIXME: 4. Otherwise, set serialized to { [[Type]]: "SharedArrayBuffer", [[ArrayBufferData]]: value.[[ArrayBufferData]],
        //           [[ArrayBufferByteLength]]: value.[[ArrayBufferByteLength]], [[AgentCluster]]: the surrounding agent's agent cluster }.
    }
    // 2. Otherwise:
    else {
        // 1. If IsDetachedBuffer(value) is true, then throw a "DataCloneError" DOMException.
        if (array_buffer.is_detached())
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot serialize detached ArrayBuffer"_string);

        // 2. Let size be value.[[ArrayBufferByteLength]].
        auto size = array_buffer.byte_length();

        // 3. Let dataCopy be ? CreateByteDataBlock(size).
        //    NOTE: This can throw a RangeError exception upon allocation failure.
        auto data_copy = TRY(JS::create_byte_data_block(vm, size));

        // 4. Perform CopyDataBlockBytes(dataCopy, 0, value.[[ArrayBufferData]], 0, size).
        JS::copy_data_block_bytes(data_copy.buffer(), 0, array_buffer.buffer(), 0, size);

        // FIXME: 5. If value has an [[ArrayBufferMaxByteLength]] internal slot, then set serialized to { [[Type]]: "ResizableArrayBuffer",
        //    [[ArrayBufferData]]: dataCopy, [[ArrayBufferByteLength]]: size, [[ArrayBufferMaxByteLength]]: value.[[ArrayBufferMaxByteLength]] }.
        if (false) {
        }
        // 6. Otherwise, set serialized to { [[Type]]: "ArrayBuffer", [[ArrayBufferData]]: dataCopy, [[ArrayBufferByteLength]]: size }.
        else {
            serialize_enum(vector, ValueTag::ArrayBuffer);
            TRY(serialize_bytes(vm, vector, data_copy.buffer().bytes()));
        }
    }
    return {};
}

template<OneOf<JS::TypedArrayBase, JS::DataView> ViewType>
WebIDL::ExceptionOr<void> serialize_viewed_array_buffer(JS::VM& vm, Vector<u32>& vector, ViewType const& view, bool for_storage, SerializationMemory& memory)
{
    // 14. Otherwise, if value has a [[ViewedArrayBuffer]] internal slot, then:

    auto view_record = [&]() {
        if constexpr (IsSame<ViewType, JS::DataView>) {
            return JS::make_data_view_with_buffer_witness_record(view, JS::ArrayBuffer::Order::SeqCst);
        } else {
            return JS::make_typed_array_with_buffer_witness_record(view, JS::ArrayBuffer::Order::SeqCst);
        }
    }();

    // 1. If IsArrayBufferViewOutOfBounds(value) is true, then throw a "DataCloneError" DOMException.
    if constexpr (IsSame<ViewType, JS::DataView>) {
        if (JS::is_view_out_of_bounds(view_record))
            return WebIDL::DataCloneError::create(*vm.current_realm(), MUST(String::formatted(JS::ErrorType::BufferOutOfBounds.message(), "DataView"sv)));
    } else {
        if (JS::is_typed_array_out_of_bounds(view_record))
            return WebIDL::DataCloneError::create(*vm.current_realm(), MUST(String::formatted(JS::ErrorType::BufferOutOfBounds.message(), "TypedArray"sv)));
    }

    // 2. Let buffer be the value of value's [[ViewedArrayBuffer]] internal slot.
    auto* buffer = view.viewed_array_buffer();

    // 3. Let bufferSerialized be ? StructuredSerializeInternal(buffer, forStorage, memory).
    auto buffer_serialized = TRY(structured_serialize_internal(vm, JS::Value(buffer), for_storage, memory));

    // 4. Assert: bufferSerialized.[[Type]] is "ArrayBuffer", "ResizableArrayBuffer", "SharedArrayBuffer", or "GrowableSharedArrayBuffer".
    // NOTE: We currently only implement this for ArrayBuffer
    VERIFY(buffer_serialized[0] == ValueTag::ArrayBuffer);

    // 5. If value has a [[DataView]] internal slot, then set serialized to { [[Type]]: "ArrayBufferView", [[Constructor]]: "DataView",
    //    [[ArrayBufferSerialized]]: bufferSerialized, [[ByteLength]]: value.[[ByteLength]], [[ByteOffset]]: value.[[ByteOffset]] }.
    if constexpr (IsSame<ViewType, JS::DataView>) {
        serialize_enum(vector, ValueTag::ArrayBufferView);
        vector.extend(move(buffer_serialized));               // [[ArrayBufferSerialized]]
        TRY(serialize_string(vm, vector, "DataView"_string)); // [[Constructor]]
        serialize_primitive_type(vector, JS::get_view_byte_length(view_record));
        serialize_primitive_type(vector, view.byte_offset());
    }

    // 6. Otherwise:
    else {
        // 1. Assert: value has a [[TypedArrayName]] internal slot.
        //    NOTE: Handled by constexpr check and template constraints
        // 2. Set serialized to { [[Type]]: "ArrayBufferView", [[Constructor]]: value.[[TypedArrayName]],
        //    [[ArrayBufferSerialized]]: bufferSerialized, [[ByteLength]]: value.[[ByteLength]],
        //    [[ByteOffset]]: value.[[ByteOffset]], [[ArrayLength]]: value.[[ArrayLength]] }.
        serialize_enum(vector, ValueTag::ArrayBufferView);
        vector.extend(move(buffer_serialized));                 // [[ArrayBufferSerialized]]
        TRY(serialize_string(vm, vector, view.element_name())); // [[Constructor]]
        serialize_primitive_type(vector, JS::typed_array_byte_length(view_record));
        serialize_primitive_type(vector, view.byte_offset());
        serialize_primitive_type(vector, JS::typed_array_length(view_record));
    }
    return {};
}
template WebIDL::ExceptionOr<void> serialize_viewed_array_buffer(JS::VM& vm, Vector<u32>& vector, JS::TypedArrayBase const& view, bool for_storage, SerializationMemory& memory);
template WebIDL::ExceptionOr<void> serialize_viewed_array_buffer(JS::VM& vm, Vector<u32>& vector, JS::DataView const& view, bool for_storage, SerializationMemory& memory);

class Deserializer {
public:
    Deserializer(JS::VM& vm, JS::Realm& target_realm, ReadonlySpan<u32> serialized, DeserializationMemory& memory, Optional<size_t> position = {})
        : m_vm(vm)
        , m_serialized(serialized)
        , m_memory(memory)
        , m_position(position.value_or(0))
    {
        VERIFY(vm.current_realm() == &target_realm);
    }

    size_t position() const { return m_position; }

    // https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
    WebIDL::ExceptionOr<JS::Value> deserialize()
    {
        auto tag = deserialize_primitive_type<ValueTag>(m_serialized, m_position);

        // 2. If memory[serialized] exists, then return memory[serialized].
        if (tag == ValueTag::ObjectReference) {
            auto index = m_serialized[m_position++];
            if (index == NumericLimits<u32>::max()) {
                return JS::Object::create(*m_vm.current_realm(), nullptr);
            }
            return m_memory[index];
        }

        // 3. Let deep be false.
        auto deep = false;

        // 4. Let value be an uninitialized value.
        JS::Value value;

        auto is_primitive = false;
        switch (tag) {
        // 5. If serialized.[[Type]] is "primitive", then set value to serialized.[[Value]].
        case ValueTag::UndefinedPrimitive: {
            value = JS::js_undefined();
            is_primitive = true;
            break;
        }
        case ValueTag::NullPrimitive: {
            value = JS::js_null();
            is_primitive = true;
            break;
        }
        case ValueTag::BooleanPrimitive: {
            value = JS::Value { deserialize_boolean_primitive(m_serialized, m_position) };
            is_primitive = true;
            break;
        }
        case ValueTag::NumberPrimitive: {
            value = JS::Value { deserialize_number_primitive(m_serialized, m_position) };
            is_primitive = true;
            break;
        }
        case ValueTag::BigIntPrimitive: {
            auto big_int = TRY(deserialize_big_int_primitive(m_vm, m_serialized, m_position));
            value = JS::Value { big_int };
            is_primitive = true;
            break;
        }
        case ValueTag::StringPrimitive: {
            auto string = TRY(deserialize_string_primitive(m_vm, m_serialized, m_position));
            value = JS::Value { string };
            is_primitive = true;
            break;
        }
        // 6. Otherwise, if serialized.[[Type]] is "Boolean", then set value to a new Boolean object in targetRealm whose [[BooleanData]] internal slot value is serialized.[[BooleanData]].
        case BooleanObject: {
            value = deserialize_boolean_object(*m_vm.current_realm(), m_serialized, m_position);
            break;
        }
        // 7. Otherwise, if serialized.[[Type]] is "Number", then set value to a new Number object in targetRealm whose [[NumberData]] internal slot value is serialized.[[NumberData]].
        case ValueTag::NumberObject: {
            value = deserialize_number_object(*m_vm.current_realm(), m_serialized, m_position);
            break;
        }
        // 8. Otherwise, if serialized.[[Type]] is "BigInt", then set value to a new BigInt object in targetRealm whose [[BigIntData]] internal slot value is serialized.[[BigIntData]].
        case ValueTag::BigIntObject: {
            value = TRY(deserialize_big_int_object(*m_vm.current_realm(), m_serialized, m_position));
            break;
        }
        // 9. Otherwise, if serialized.[[Type]] is "String", then set value to a new String object in targetRealm whose [[StringData]] internal slot value is serialized.[[StringData]].
        case ValueTag::StringObject: {
            value = TRY(deserialize_string_object(*m_vm.current_realm(), m_serialized, m_position));
            break;
        }
        // 10. Otherwise, if serialized.[[Type]] is "Date", then set value to a new Date object in targetRealm whose [[DateValue]] internal slot value is serialized.[[DateValue]].
        case ValueTag::DateObject: {
            value = deserialize_date_object(*m_vm.current_realm(), m_serialized, m_position);
            break;
        }
        // 11. Otherwise, if serialized.[[Type]] is "RegExp", then set value to a new RegExp object in targetRealm whose [[RegExpMatcher]] internal slot value is serialized.[[RegExpMatcher]],
        //     whose [[OriginalSource]] internal slot value is serialized.[[OriginalSource]], and whose [[OriginalFlags]] internal slot value is serialized.[[OriginalFlags]].
        case ValueTag::RegExpObject: {
            value = TRY(deserialize_reg_exp_object(*m_vm.current_realm(), m_serialized, m_position));
            break;
        }
        // FIXME: 12. Otherwise, if serialized.[[Type]] is "SharedArrayBuffer", then:
        // FIXME: 13. Otherwise, if serialized.[[Type]] is "GrowableSharedArrayBuffer", then:
        // 14. Otherwise, if serialized.[[Type]] is "ArrayBuffer", then set value to a new ArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]], and whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]].
        case ValueTag::ArrayBuffer: {
            auto* realm = m_vm.current_realm();
            // If this throws an exception, catch it, and then throw a "DataCloneError" DOMException.
            auto bytes_or_error = deserialize_bytes(m_vm, m_serialized, m_position);
            if (bytes_or_error.is_error())
                return WebIDL::DataCloneError::create(*m_vm.current_realm(), "out of memory"_string);
            value = JS::ArrayBuffer::create(*realm, bytes_or_error.release_value());
            break;
        }
        // FIXME: 15. Otherwise, if serialized.[[Type]] is "ResizableArrayBuffer", then set value to a new ArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]], whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]], and whose [[ArrayBufferMaxByteLength]] internal slot value is a serialized.[[ArrayBufferMaxByteLength]].
        // 16. Otherwise, if serialized.[[Type]] is "ArrayBufferView", then:
        case ValueTag::ArrayBufferView: {
            auto* realm = m_vm.current_realm();
            auto array_buffer_value = TRY(deserialize());
            auto& array_buffer = verify_cast<JS::ArrayBuffer>(array_buffer_value.as_object());
            auto constructor_name = TRY(deserialize_string(m_vm, m_serialized, m_position));
            u32 byte_length = deserialize_primitive_type<u32>(m_serialized, m_position);
            u32 byte_offset = deserialize_primitive_type<u32>(m_serialized, m_position);

            if (constructor_name == "DataView"sv) {
                value = JS::DataView::create(*realm, &array_buffer, byte_length, byte_offset);
            } else {
                u32 array_length = deserialize_primitive_type<u32>(m_serialized, m_position);
                JS::GCPtr<JS::TypedArrayBase> typed_array_ptr;
#define CREATE_TYPED_ARRAY(ClassName)       \
    if (constructor_name == #ClassName##sv) \
        typed_array_ptr = JS::ClassName::create(*realm, array_length, array_buffer);
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    CREATE_TYPED_ARRAY(ClassName)
                JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
#undef CREATE_TYPED_ARRAY
                VERIFY(typed_array_ptr != nullptr); // FIXME: Handle errors better here? Can a fuzzer put weird stuff in the buffer?
                typed_array_ptr->set_byte_length(byte_length);
                typed_array_ptr->set_byte_offset(byte_offset);
                value = typed_array_ptr;
            }
            break;
        }
        // 17. Otherwise, if serialized.[[Type]] is "Map", then:
        case ValueTag::MapObject: {
            auto& realm = *m_vm.current_realm();
            // 1. Set value to a new Map object in targetRealm whose [[MapData]] internal slot value is a new empty List.
            value = JS::Map::create(realm);
            // 2. Set deep to true.
            deep = true;
            break;
        }
        // 18. Otherwise, if serialized.[[Type]] is "Set", then:
        case ValueTag::SetObject: {
            auto& realm = *m_vm.current_realm();
            // 1. Set value to a new Set object in targetRealm whose [[SetData]] internal slot value is a new empty List.
            value = JS::Set::create(realm);
            // 2. Set deep to true.
            deep = true;
            break;
        }
        // 19. Otherwise, if serialized.[[Type]] is "Array", then:
        case ValueTag::ArrayObject: {
            auto& realm = *m_vm.current_realm();
            // 1. Let outputProto be targetRealm.[[Intrinsics]].[[%Array.prototype%]].
            // 2. Set value to ! ArrayCreate(serialized.[[Length]], outputProto).
            auto length = deserialize_primitive_type<u64>(m_serialized, m_position);
            value = MUST(JS::Array::create(realm, length));
            // 3. Set deep to true.
            deep = true;
            break;
        }
        // 20. Otherwise, if serialized.[[Type]] is "Object", then:
        case ValueTag::Object: {
            auto& realm = *m_vm.current_realm();
            // 1. Set value to a new Object in targetRealm.
            value = JS::Object::create(realm, realm.intrinsics().object_prototype());
            // 2. Set deep to true.
            deep = true;
            break;
        }
        // 21. Otherwise, if serialized.[[Type]] is "Error", then:
        case ValueTag::ErrorObject: {
            auto& realm = *m_vm.current_realm();
            auto type = deserialize_primitive_type<ErrorType>(m_serialized, m_position);
            auto has_message = deserialize_primitive_type<bool>(m_serialized, m_position);
            if (has_message) {
                auto message = TRY(deserialize_string(m_vm, m_serialized, m_position));
                switch (type) {
                case ErrorType::Error:
                    value = JS::Error::create(realm, message);
                    break;
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    case ErrorType::ClassName:                                                           \
        value = JS::ClassName::create(realm, message);                                   \
        break;
                    JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
                }
            } else {
                switch (type) {
                case ErrorType::Error:
                    value = JS::Error::create(realm);
                    break;
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    case ErrorType::ClassName:                                                           \
        value = JS::ClassName::create(realm);                                            \
        break;
                    JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
                }
            }
            break;
        }
        // 22. Otherwise:
        default:
            VERIFY(tag == ValueTag::SerializableObject);

            auto& realm = *m_vm.current_realm();
            // 1. Let interfaceName be serialized.[[Type]].
            auto interface_name = TRY(deserialize_string(m_vm, m_serialized, m_position));
            // 2. If the interface identified by interfaceName is not exposed in targetRealm, then throw a "DataCloneError" DOMException.
            if (!is_interface_exposed_on_target_realm(interface_name, realm))
                return WebIDL::DataCloneError::create(realm, "Unsupported type"_string);

            // 3. Set value to a new instance of the interface identified by interfaceName, created in targetRealm.
            value = TRY(create_serialized_type(interface_name, realm));

            // 4. Set deep to true.
            deep = true;
        }

        // 23. Set memory[serialized] to value.
        // IMPLEMENTATION DEFINED: We don't add primitive values to the memory to match the serialization indices (which also doesn't add them)
        if (!is_primitive)
            m_memory.append(value);

        // 24. If deep is true, then:
        if (deep) {
            // 1. If serialized.[[Type]] is "Map", then:
            if (tag == ValueTag::MapObject) {
                auto& map = static_cast<JS::Map&>(value.as_object());
                auto length = deserialize_primitive_type<u64>(m_serialized, m_position);
                // 1. For each Record { [[Key]], [[Value]] } entry of serialized.[[MapData]]:
                for (u64 i = 0u; i < length; ++i) {
                    // 1. Let deserializedKey be ? StructuredDeserialize(entry.[[Key]], targetRealm, memory).
                    auto deserialized_key = TRY(deserialize());

                    // 2. Let deserializedValue be ? StructuredDeserialize(entry.[[Value]], targetRealm, memory).
                    auto deserialized_value = TRY(deserialize());

                    // 3. Append { [[Key]]: deserializedKey, [[Value]]: deserializedValue } to value.[[MapData]].
                    map.map_set(deserialized_key, deserialized_value);
                }
            }

            // 2. Otherwise, if serialized.[[Type]] is "Set", then:
            else if (tag == ValueTag::SetObject) {
                auto& set = static_cast<JS::Set&>(value.as_object());
                auto length = deserialize_primitive_type<u64>(m_serialized, m_position);
                // 1. For each entry of serialized.[[SetData]]:
                for (u64 i = 0u; i < length; ++i) {
                    // 1. Let deserializedEntry be ? StructuredDeserialize(entry, targetRealm, memory).
                    auto deserialized_entry = TRY(deserialize());

                    // 2. Append deserializedEntry to value.[[SetData]].
                    set.set_add(deserialized_entry);
                }
            }

            // 3. Otherwise, if serialized.[[Type]] is "Array" or "Object", then:
            else if (tag == ValueTag::ArrayObject || tag == ValueTag::Object) {
                auto& object = value.as_object();
                auto length = deserialize_primitive_type<u64>(m_serialized, m_position);
                // 1. For each Record { [[Key]], [[Value]] } entry of serialized.[[Properties]]:
                for (u64 i = 0u; i < length; ++i) {
                    auto key = TRY(deserialize_string(m_vm, m_serialized, m_position));

                    // 1. Let deserializedValue be ? StructuredDeserialize(entry.[[Value]], targetRealm, memory).
                    auto deserialized_value = TRY(deserialize());

                    // 2. Let result be ! CreateDataProperty(value, entry.[[Key]], deserializedValue).
                    auto result = MUST(object.create_data_property(key.to_byte_string(), deserialized_value));

                    // 3. Assert: result is true.
                    VERIFY(result);
                }
            }

            // 4. Otherwise:
            else {
                // 1. Perform the appropriate deserialization steps for the interface identified by serialized.[[Type]], given serialized, value, and targetRealm.
                auto& serializable = dynamic_cast<Bindings::Serializable&>(value.as_object());
                TRY(serializable.deserialization_steps(m_serialized, m_position, m_memory));
            }
        }

        // 25. Return value.
        return value;
    }

private:
    JS::VM& m_vm;
    ReadonlySpan<u32> m_serialized;
    JS::MarkedVector<JS::Value> m_memory; // Index -> JS value
    size_t m_position { 0 };

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Bindings::PlatformObject>> create_serialized_type(StringView interface_name, JS::Realm& realm)
    {
        if (interface_name == "Blob"sv)
            return FileAPI::Blob::create(realm);
        if (interface_name == "File"sv)
            return FileAPI::File::create(realm);
        if (interface_name == "FileList"sv)
            return FileAPI::FileList::create(realm);
        if (interface_name == "DOMMatrixReadOnly"sv)
            return Geometry::DOMMatrixReadOnly::create(realm);
        if (interface_name == "DOMMatrix"sv)
            return Geometry::DOMMatrix::create(realm);
        if (interface_name == "DOMPointReadOnly"sv)
            return Geometry::DOMPointReadOnly::create(realm);
        if (interface_name == "DOMPoint"sv)
            return Geometry::DOMPoint::create(realm);
        if (interface_name == "DOMRectReadOnly"sv)
            return Geometry::DOMRectReadOnly::create(realm);
        if (interface_name == "DOMRect"sv)
            return Geometry::DOMRect::create(realm);
        if (interface_name == "CryptoKey"sv)
            return Crypto::CryptoKey::create(realm);
        if (interface_name == "DOMQuad"sv)
            return Geometry::DOMQuad::create(realm);

        VERIFY_NOT_REACHED();
    }

    // FIXME: Consolidate this function with the similar is_interface_exposed_on_target_realm() used when transferring objects.
    //        Also, the name parameter would be better off being the interface name (as a string) so that we don't need a switch statement.
    static bool is_interface_exposed_on_target_realm(StringView interface_name, JS::Realm& realm)
    {
        auto const& intrinsics = Bindings::host_defined_intrinsics(realm);
        return intrinsics.is_exposed(interface_name);
    }
};

bool deserialize_boolean_primitive(ReadonlySpan<u32> const& serialized, size_t& position)
{
    return deserialize_primitive_type<bool>(serialized, position);
}

double deserialize_number_primitive(ReadonlySpan<u32> const& serialized, size_t& position)
{
    return deserialize_primitive_type<double>(serialized, position);
}

JS::NonnullGCPtr<JS::BooleanObject> deserialize_boolean_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position)
{
    auto boolean_primitive = deserialize_boolean_primitive(serialized, position);
    return JS::BooleanObject::create(realm, boolean_primitive);
}

JS::NonnullGCPtr<JS::NumberObject> deserialize_number_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position)
{
    auto number_primitive = deserialize_number_primitive(serialized, position);
    return JS::NumberObject::create(realm, number_primitive);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::BigIntObject>> deserialize_big_int_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position)
{
    auto big_int_primitive = TRY(deserialize_big_int_primitive(realm.vm(), serialized, position));
    return JS::BigIntObject::create(realm, big_int_primitive);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::StringObject>> deserialize_string_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position)
{
    auto string_primitive = TRY(deserialize_string_primitive(realm.vm(), serialized, position));
    return JS::StringObject::create(realm, string_primitive, realm.intrinsics().string_prototype());
}

JS::NonnullGCPtr<JS::Date> deserialize_date_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position)
{
    auto double_value = deserialize_primitive_type<double>(serialized, position);
    return JS::Date::create(realm, double_value);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::RegExpObject>> deserialize_reg_exp_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position)
{
    auto pattern = TRY(deserialize_string_primitive(realm.vm(), serialized, position));
    auto flags = TRY(deserialize_string_primitive(realm.vm(), serialized, position));
    return TRY(JS::regexp_create(realm.vm(), move(pattern), move(flags)));
}

WebIDL::ExceptionOr<ByteBuffer> deserialize_bytes(JS::VM& vm, ReadonlySpan<u32> vector, size_t& position)
{
    u64 const size = deserialize_primitive_type<u64>(vector, position);

    auto bytes = TRY_OR_THROW_OOM(vm, ByteBuffer::create_uninitialized(size));
    u64 byte_position = 0;
    while (position < vector.size() && byte_position < size) {
        for (u8 i = 0; i < 4; ++i) {
            bytes[byte_position++] = (vector[position] >> (i * 8) & 0xFF);
            if (byte_position == size)
                break;
        }
        position++;
    }
    return bytes;
}

WebIDL::ExceptionOr<String> deserialize_string(JS::VM& vm, ReadonlySpan<u32> vector, size_t& position)
{
    auto bytes = TRY(deserialize_bytes(vm, vector, position));
    return TRY_OR_THROW_OOM(vm, String::from_utf8(StringView { bytes }));
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::PrimitiveString>> deserialize_string_primitive(JS::VM& vm, ReadonlySpan<u32> vector, size_t& position)
{
    auto bytes = TRY(deserialize_bytes(vm, vector, position));

    return TRY(Bindings::throw_dom_exception_if_needed(vm, [&vm, &bytes]() {
        return JS::PrimitiveString::create(vm, StringView { bytes });
    }));
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::BigInt>> deserialize_big_int_primitive(JS::VM& vm, ReadonlySpan<u32> vector, size_t& position)
{
    auto string = TRY(deserialize_string_primitive(vm, vector, position));
    auto string_view = TRY(Bindings::throw_dom_exception_if_needed(vm, [&string]() {
        return string->utf8_string_view();
    }));
    auto bigint = MUST(::Crypto::SignedBigInteger::from_base(10, string_view.substring_view(0, string_view.length() - 1)));
    return JS::BigInt::create(vm, bigint);
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializewithtransfer
WebIDL::ExceptionOr<SerializedTransferRecord> structured_serialize_with_transfer(JS::VM& vm, JS::Value value, Vector<JS::Handle<JS::Object>> const& transfer_list)
{
    // 1. Let memory be an empty map.
    SerializationMemory memory = {};

    // 2. For each transferable of transferList:
    for (auto const& transferable : transfer_list) {

        // 1. If transferable has neither an [[ArrayBufferData]] internal slot nor a [[Detached]] internal slot, then throw a "DataCloneError" DOMException.
        // FIXME: Handle transferring ArrayBufferData objects
        if (!is<Bindings::Transferable>(*transferable)) {
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot transfer type"_string);
        }

        // FIXME: 2. If transferable has an [[ArrayBufferData]] internal slot and IsSharedArrayBuffer(transferable) is true, then throw a "DataCloneError" DOMException.

        // 3. If memory[transferable] exists, then throw a "DataCloneError" DOMException.
        auto transferable_value = JS::Value(transferable);
        if (memory.contains(transferable_value)) {
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot transfer value twice"_string);
        }

        // 4. Set memory[transferable] to { [[Type]]: an uninitialized value }.
        memory.set(JS::make_handle(transferable_value), NumericLimits<u32>::max());
    }

    // 3. Let serialized be ? StructuredSerializeInternal(value, false, memory).
    auto serialized = TRY(structured_serialize_internal(vm, value, false, memory));

    // 4. Let transferDataHolders be a new empty List.
    Vector<TransferDataHolder> transfer_data_holders;
    transfer_data_holders.ensure_capacity(transfer_list.size());

    // 5. For each transferable of transferList:
    for (auto& transferable : transfer_list) {
        // 1. FIXME: If transferable has an [[ArrayBufferData]] internal slot and IsDetachedBuffer(transferable) is true, then throw a "DataCloneError" DOMException.

        // 2. If transferable has a [[Detached]] internal slot and transferable.[[Detached]] is true, then throw a "DataCloneError" DOMException.
        if (is<Bindings::Transferable>(*transferable)) {
            auto& transferable_object = dynamic_cast<Bindings::Transferable&>(*transferable);
            if (transferable_object.is_detached()) {
                return WebIDL::DataCloneError::create(*vm.current_realm(), "Value already transferred"_string);
            }
        }

        // 3. Let dataHolder be memory[transferable].
        // IMPLEMENTATION DEFINED: We just create a data holder here, our memory holds indices into the SerializationRecord
        TransferDataHolder data_holder;

        // FIXME 4. If transferable has an [[ArrayBufferData]] internal slot, then:
        if (false) {
        }

        // 5. Otherwise:
        else {
            // 1. Assert: transferable is a platform object that is a transferable object.
            auto& transferable_object = dynamic_cast<Bindings::Transferable&>(*transferable);
            VERIFY(is<Bindings::PlatformObject>(*transferable));

            // 2. Let interfaceName be the identifier of the primary interface of transferable.
            auto interface_name = transferable_object.primary_interface();

            // 3. Set dataHolder.[[Type]] to interfaceName.
            data_holder.data.append(to_underlying(interface_name));

            // 4. Perform the appropriate transfer steps for the interface identified by interfaceName, given transferable and dataHolder.
            TRY(transferable_object.transfer_steps(data_holder));

            // 5. Set transferable.[[Detached]] to true.
            transferable_object.set_detached(true);
        }

        // 6. Append dataHolder to transferDataHolders.
        transfer_data_holders.append(move(data_holder));
    }

    // 6. Return { [[Serialized]]: serialized, [[TransferDataHolders]]: transferDataHolders }.
    return SerializedTransferRecord { .serialized = move(serialized), .transfer_data_holders = move(transfer_data_holders) };
}

static bool is_interface_exposed_on_target_realm(u8 name, JS::Realm& realm)
{
    auto const& intrinsics = Bindings::host_defined_intrinsics(realm);
    switch (static_cast<TransferType>(name)) {
    case TransferType::MessagePort:
        return intrinsics.is_exposed("MessagePort"sv);
        break;
    default:
        dbgln("Unknown interface type for transfer: {}", name);
        break;
    }
    return false;
}

static WebIDL::ExceptionOr<JS::NonnullGCPtr<Bindings::PlatformObject>> create_transferred_value(TransferType name, JS::Realm& target_realm, TransferDataHolder& transfer_data_holder)
{
    switch (name) {
    case TransferType::MessagePort: {
        auto message_port = HTML::MessagePort::create(target_realm);
        TRY(message_port->transfer_receiving_steps(transfer_data_holder));
        return message_port;
    }
    }
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserializewithtransfer
WebIDL::ExceptionOr<DeserializedTransferRecord> structured_deserialize_with_transfer(JS::VM& vm, SerializedTransferRecord& serialize_with_transfer_result)
{
    auto& target_realm = *vm.current_realm();

    // 1. Let memory be an empty map.
    auto memory = DeserializationMemory(vm.heap());

    // 2. Let transferredValues be a new empty List.
    Vector<JS::Handle<JS::Object>> transferred_values;

    // 3. For each transferDataHolder of serializeWithTransferResult.[[TransferDataHolders]]:
    for (auto& transfer_data_holder : serialize_with_transfer_result.transfer_data_holders) {
        // 1. Let value be an uninitialized value.
        JS::Value value;

        // FIXME: 2. If transferDataHolder.[[Type]] is "ArrayBuffer", then set value to a new ArrayBuffer object in targetRealm
        //    whose [[ArrayBufferData]] internal slot value is transferDataHolder.[[ArrayBufferData]], and
        //    whose [[ArrayBufferByteLength]] internal slot value is transferDataHolder.[[ArrayBufferByteLength]].
        // NOTE: In cases where the original memory occupied by [[ArrayBufferData]] is accessible during the deserialization,
        //       this step is unlikely to throw an exception, as no new memory needs to be allocated: the memory occupied by
        //       [[ArrayBufferData]] is instead just getting transferred into the new ArrayBuffer. This could be true, for example,
        //       when both the source and target realms are in the same process.
        if (false) {
        }

        // FIXME: 3. Otherwise, if transferDataHolder.[[Type]] is "ResizableArrayBuffer", then set value to a new ArrayBuffer object
        //     in targetRealm whose [[ArrayBufferData]] internal slot value is transferDataHolder.[[ArrayBufferData]], whose
        //     [[ArrayBufferByteLength]] internal slot value is transferDataHolder.[[ArrayBufferByteLength]], and whose
        //     [[ArrayBufferMaxByteLength]] internal slot value is transferDataHolder.[[ArrayBufferMaxByteLength]].
        // NOTE: For the same reason as the previous step, this step is also unlikely to throw an exception.
        else if (false) {
        }

        // 4. Otherwise:
        else {
            // 1. Let interfaceName be transferDataHolder.[[Type]].
            u8 const interface_name = transfer_data_holder.data.take_first();

            // 2. If the interface identified by interfaceName is not exposed in targetRealm, then throw a "DataCloneError" DOMException.
            if (!is_interface_exposed_on_target_realm(interface_name, target_realm))
                return WebIDL::DataCloneError::create(target_realm, "Unknown type transferred"_string);

            // 3. Set value to a new instance of the interface identified by interfaceName, created in targetRealm.
            // 4. Perform the appropriate transfer-receiving steps for the interface identified by interfaceName given transferDataHolder and value.
            value = TRY(create_transferred_value(static_cast<TransferType>(interface_name), target_realm, transfer_data_holder));
        }

        // 5. Set memory[transferDataHolder] to value.
        memory.append(value);

        // 6. Append value to transferredValues.
        transferred_values.append(JS::make_handle(value.as_object()));
    }

    // 4. Let deserialized be ? StructuredDeserialize(serializeWithTransferResult.[[Serialized]], targetRealm, memory).
    auto deserialized = TRY(structured_deserialize(vm, serialize_with_transfer_result.serialized, target_realm, memory));

    // 5. Return { [[Deserialized]]: deserialized, [[TransferredValues]]: transferredValues }.
    return DeserializedTransferRecord { .deserialized = move(deserialized), .transferred_values = move(transferred_values) };
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserialize
WebIDL::ExceptionOr<SerializationRecord> structured_serialize(JS::VM& vm, JS::Value value)
{
    // 1. Return ? StructuredSerializeInternal(value, false).
    SerializationMemory memory = {};
    return structured_serialize_internal(vm, value, false, memory);
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeforstorage
WebIDL::ExceptionOr<SerializationRecord> structured_serialize_for_storage(JS::VM& vm, JS::Value value)
{
    // 1. Return ? StructuredSerializeInternal(value, true).
    SerializationMemory memory = {};
    return structured_serialize_internal(vm, value, true, memory);
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
WebIDL::ExceptionOr<SerializationRecord> structured_serialize_internal(JS::VM& vm, JS::Value value, bool for_storage, SerializationMemory& memory)
{
    // 1. If memory was not supplied, let memory be an empty map.
    // IMPLEMENTATION DEFINED: We move this requirement up to the callers to make recursion easier

    Serializer serializer(vm, memory, for_storage);
    return serializer.serialize(value);
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM& vm, SerializationRecord const& serialized, JS::Realm& target_realm, Optional<DeserializationMemory> memory)
{
    if (!memory.has_value())
        memory = DeserializationMemory { vm.heap() };

    // IMPLEMENTATION DEFINED: We need to make sure there's an execution context for target_realm on the stack before constructing these JS objects
    auto& target_settings = Bindings::host_defined_environment_settings_object(target_realm);
    target_settings.prepare_to_run_script();

    auto result = TRY(structured_deserialize_internal(vm, serialized.span(), target_realm, *memory));

    target_settings.clean_up_after_running_script();
    VERIFY(result.value.has_value());
    return *result.value;
}

WebIDL::ExceptionOr<DeserializedRecord> structured_deserialize_internal(JS::VM& vm, ReadonlySpan<u32> const& serialized, JS::Realm& target_realm, DeserializationMemory& memory, Optional<size_t> position)
{
    Deserializer deserializer(vm, target_realm, serialized, memory, move(position));
    auto value = TRY(deserializer.deserialize());
    auto deserialized_record = DeserializedRecord {
        .value = value,
        .position = deserializer.position(),
    };
    return deserialized_record;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, ::Web::HTML::TransferDataHolder const& data_holder)
{
    TRY(encoder.encode(data_holder.data));
    TRY(encoder.encode(data_holder.fds));
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, ::Web::HTML::SerializedTransferRecord const& record)
{
    TRY(encoder.encode(record.serialized));
    TRY(encoder.encode(record.transfer_data_holders));
    return {};
}

template<>
ErrorOr<::Web::HTML::TransferDataHolder> decode(Decoder& decoder)
{
    auto data = TRY(decoder.decode<Vector<u8>>());
    auto fds = TRY(decoder.decode<Vector<IPC::File>>());
    return ::Web::HTML::TransferDataHolder { move(data), move(fds) };
}

template<>
ErrorOr<::Web::HTML::SerializedTransferRecord> decode(Decoder& decoder)
{
    auto serialized = TRY(decoder.decode<Vector<u32>>());
    auto transfer_data_holders = TRY(decoder.decode<Vector<::Web::HTML::TransferDataHolder>>());
    return ::Web::HTML::SerializedTransferRecord { move(serialized), move(transfer_data_holders) };
}

}
