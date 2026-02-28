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
#include <LibIPC/File.h>
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
#include <LibWeb/Bindings/DOMExceptionPrototype.h>
#include <LibWeb/Bindings/DOMMatrixPrototype.h>
#include <LibWeb/Bindings/DOMMatrixReadOnlyPrototype.h>
#include <LibWeb/Bindings/DOMPointPrototype.h>
#include <LibWeb/Bindings/DOMPointReadOnlyPrototype.h>
#include <LibWeb/Bindings/DOMQuadPrototype.h>
#include <LibWeb/Bindings/DOMRectPrototype.h>
#include <LibWeb/Bindings/DOMRectReadOnlyPrototype.h>
#include <LibWeb/Bindings/FileListPrototype.h>
#include <LibWeb/Bindings/FilePrototype.h>
#include <LibWeb/Bindings/ImageBitmapPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MessagePortPrototype.h>
#include <LibWeb/Bindings/QuotaExceededErrorPrototype.h>
#include <LibWeb/Bindings/ReadableStreamPrototype.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/Bindings/TransformStreamPrototype.h>
#include <LibWeb/Bindings/WritableStreamPrototype.h>
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
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::HTML {

enum class ValueTag : u8 {
    Empty, // Unused, for ease of catching bugs.

    UndefinedPrimitive,
    NullPrimitive,
    BooleanPrimitive,
    NumberPrimitive,
    StringPrimitive,
    BigIntPrimitive,

    BooleanObject,
    NumberObject,
    StringObject,
    BigIntObject,
    DateObject,
    RegExpObject,
    MapObject,
    SetObject,
    ArrayObject,
    ErrorObject,
    Object,
    ObjectReference,

    GrowableSharedArrayBuffer,
    SharedArrayBuffer,
    ResizeableArrayBuffer,
    ArrayBuffer,
    ArrayBufferView,

    SerializableObject,

    // TODO: Define many more types
};

enum ErrorType {
    Error,
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    ClassName,
    JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
};

static ErrorType error_name_to_type(StringView name)
{
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    if (name == #ClassName##sv)                                                          \
        return ErrorType::ClassName;
    JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
    return Error;
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
static WebIDL::ExceptionOr<void> serialize_array_buffer(JS::VM& vm, TransferDataEncoder& data_holder, JS::ArrayBuffer const& array_buffer, bool for_storage)
{
    // 13. Otherwise, if value has an [[ArrayBufferData]] internal slot, then:

    // 1. If IsSharedArrayBuffer(value) is true, then:
    if (array_buffer.is_shared_array_buffer()) {
        // 1. If the current principal settings object's cross-origin isolated capability is false, then throw a "DataCloneError" DOMException.
        // NOTE: This check is only needed when serializing (and not when deserializing) as the cross-origin isolated capability cannot change
        //       over time and a SharedArrayBuffer cannot leave an agent cluster.
        if (current_principal_settings_object().cross_origin_isolated_capability() == CanUseCrossOriginIsolatedAPIs::No)
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot serialize SharedArrayBuffer when cross-origin isolated"_utf16);

        // 2. If forStorage is true, then throw a "DataCloneError" DOMException.
        if (for_storage)
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot serialize SharedArrayBuffer for storage"_utf16);

        if (!array_buffer.is_fixed_length()) {
            // 3. If value has an [[ArrayBufferMaxByteLength]] internal slot, then set serialized to { [[Type]]: "GrowableSharedArrayBuffer",
            //           [[ArrayBufferData]]: value.[[ArrayBufferData]], [[ArrayBufferByteLengthData]]: value.[[ArrayBufferByteLengthData]],
            //           [[ArrayBufferMaxByteLength]]: value.[[ArrayBufferMaxByteLength]],
            //           FIXME: [[AgentCluster]]: the surrounding agent's agent cluster }.
            data_holder.encode(ValueTag::GrowableSharedArrayBuffer);
            data_holder.encode(array_buffer.buffer());
            data_holder.encode(array_buffer.max_byte_length());
        } else {
            // 4. Otherwise, set serialized to { [[Type]]: "SharedArrayBuffer", [[ArrayBufferData]]: value.[[ArrayBufferData]],
            //           [[ArrayBufferByteLength]]: value.[[ArrayBufferByteLength]],
            //           FIXME: [[AgentCluster]]: the surrounding agent's agent cluster }.
            data_holder.encode(ValueTag::SharedArrayBuffer);
            data_holder.encode(array_buffer.buffer());
        }
    }
    // 2. Otherwise:
    else {
        // 1. If IsDetachedBuffer(value) is true, then throw a "DataCloneError" DOMException.
        if (array_buffer.is_detached())
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot serialize detached ArrayBuffer"_utf16);

        // 2. Let size be value.[[ArrayBufferByteLength]].
        auto size = array_buffer.byte_length();

        // 3. Let dataCopy be ? CreateByteDataBlock(size).
        //    NOTE: This can throw a RangeError exception upon allocation failure.
        auto data_copy = TRY(JS::create_byte_data_block(vm, size));

        // 4. Perform CopyDataBlockBytes(dataCopy, 0, value.[[ArrayBufferData]], 0, size).
        JS::copy_data_block_bytes(data_copy.buffer(), 0, array_buffer.buffer(), 0, size);

        // 5. If value has an [[ArrayBufferMaxByteLength]] internal slot, then set serialized to { [[Type]]: "ResizableArrayBuffer",
        //    [[ArrayBufferData]]: dataCopy, [[ArrayBufferByteLength]]: size, [[ArrayBufferMaxByteLength]]: value.[[ArrayBufferMaxByteLength]] }.
        if (!array_buffer.is_fixed_length()) {
            data_holder.encode(ValueTag::ResizeableArrayBuffer);
            data_holder.encode(data_copy.buffer());
            data_holder.encode(array_buffer.max_byte_length());
        }
        // 6. Otherwise, set serialized to { [[Type]]: "ArrayBuffer", [[ArrayBufferData]]: dataCopy, [[ArrayBufferByteLength]]: size }.
        else {
            data_holder.encode(ValueTag::ArrayBuffer);
            data_holder.encode(data_copy.buffer());
        }
    }
    return {};
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
template<OneOf<JS::TypedArrayBase, JS::DataView> ViewType>
static WebIDL::ExceptionOr<void> serialize_viewed_array_buffer(JS::VM& vm, TransferDataEncoder& data_holder, ViewType const& view, bool for_storage, SerializationMemory& memory)
{
    // 14. Otherwise, if value has a [[ViewedArrayBuffer]] internal slot, then:

    // 1. If IsArrayBufferViewOutOfBounds(value) is true, then throw a "DataCloneError" DOMException.
    if constexpr (IsSame<ViewType, JS::DataView>) {
        auto view_record = JS::make_data_view_with_buffer_witness_record(view, JS::ArrayBuffer::Order::SeqCst);
        if (JS::is_view_out_of_bounds(view_record))
            return WebIDL::DataCloneError::create(*vm.current_realm(), Utf16String::formatted(JS::ErrorType::BufferOutOfBounds.format(), "DataView"sv));
    } else {
        auto typed_array_record = JS::make_typed_array_with_buffer_witness_record(view, JS::ArrayBuffer::Order::SeqCst);
        if (JS::is_typed_array_out_of_bounds(typed_array_record))
            return WebIDL::DataCloneError::create(*vm.current_realm(), Utf16String::formatted(JS::ErrorType::BufferOutOfBounds.format(), "TypedArray"sv));
    }

    // 2. Let buffer be the value of value's [[ViewedArrayBuffer]] internal slot.
    JS::Value buffer = view.viewed_array_buffer();

    // 3. Let bufferSerialized be ? StructuredSerializeInternal(buffer, forStorage, memory).
    auto buffer_serialized = TRY(structured_serialize_internal(vm, buffer, for_storage, memory));

    // 4. Assert: bufferSerialized.[[Type]] is "ArrayBuffer", "ResizableArrayBuffer", "SharedArrayBuffer", or "GrowableSharedArrayBuffer".
    // NOTE: Object reference + memory check is required when ArrayBuffer is transferred.
    auto tag = TransferDataDecoder { buffer_serialized }.decode<ValueTag>();

    VERIFY(first_is_one_of(tag, ValueTag::ArrayBuffer, ValueTag::ResizeableArrayBuffer, ValueTag::SharedArrayBuffer, ValueTag::GrowableSharedArrayBuffer)
        || (tag == ValueTag::ObjectReference && memory.contains(buffer)));

    auto serialize_byte_length = [&](JS::ByteLength byte_length) {
        VERIFY(!byte_length.is_detached());

        data_holder.encode(byte_length.is_auto());
        if (!byte_length.is_auto())
            data_holder.encode(byte_length.length());
    };

    // 5. If value has a [[DataView]] internal slot, then set serialized to { [[Type]]: "ArrayBufferView", [[Constructor]]: "DataView",
    //    [[ArrayBufferSerialized]]: bufferSerialized, [[ByteLength]]: value.[[ByteLength]], [[ByteOffset]]: value.[[ByteOffset]] }.
    if constexpr (IsSame<ViewType, JS::DataView>) {
        data_holder.encode(ValueTag::ArrayBufferView);
        data_holder.append(move(buffer_serialized)); // [[ArrayBufferSerialized]]
        data_holder.encode("DataView"_utf16);        // [[Constructor]]
        serialize_byte_length(view.byte_length());
        data_holder.encode(view.byte_offset());
    }
    // 6. Otherwise:
    else {
        // 1. Assert: value has a [[TypedArrayName]] internal slot.
        //    NOTE: Handled by constexpr check and template constraints
        // 2. Set serialized to { [[Type]]: "ArrayBufferView", [[Constructor]]: value.[[TypedArrayName]],
        //    [[ArrayBufferSerialized]]: bufferSerialized, [[ByteLength]]: value.[[ByteLength]],
        //    [[ByteOffset]]: value.[[ByteOffset]], [[ArrayLength]]: value.[[ArrayLength]] }.
        data_holder.encode(ValueTag::ArrayBufferView);
        data_holder.append(move(buffer_serialized));               // [[ArrayBufferSerialized]]
        data_holder.encode(view.element_name().to_utf16_string()); // [[Constructor]]
        serialize_byte_length(view.byte_length());
        data_holder.encode(view.byte_offset());
        serialize_byte_length(view.array_length());
    }

    return {};
}

// Serializing and deserializing are each two passes:
// 1. Fill up the memory with all the values, but without translating references
// 2. Translate all the references into the appropriate form
class Serializer {
public:
    Serializer(JS::VM& vm, SerializationMemory& memory, bool for_storage)
        : m_vm(vm)
        , m_memory(memory)
        , m_next_id(memory.size())
        , m_for_storage(for_storage)
    {
    }

    // https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
    // https://whatpr.org/html/9893/structured-data.html#structuredserializeinternal
    WebIDL::ExceptionOr<SerializationRecord> serialize(JS::Value value)
    {
        TransferDataEncoder serialized;

        // 2. If memory[value] exists, then return memory[value].
        if (m_memory.contains(value)) {
            serialized.encode(ValueTag::ObjectReference);
            serialized.encode(m_memory.get(value).value());
            return serialized.take_buffer().take_data();
        }

        // 3. Let deep be false.
        auto deep = false;

        // 4. If value is undefined, null, a Boolean, a Number, a BigInt, or a String, then return { [[Type]]: "primitive", [[Value]]: value }.
        bool return_primitive_type = true;

        if (value.is_undefined()) {
            serialized.encode(ValueTag::UndefinedPrimitive);
        } else if (value.is_null()) {
            serialized.encode(ValueTag::NullPrimitive);
        } else if (value.is_boolean()) {
            serialized.encode(ValueTag::BooleanPrimitive);
            serialized.encode(value.as_bool());
        } else if (value.is_number()) {
            serialized.encode(ValueTag::NumberPrimitive);
            serialized.encode(value.as_double());
        } else if (value.is_bigint()) {
            serialized.encode(ValueTag::BigIntPrimitive);
            serialized.encode(MUST(value.as_bigint().big_integer().to_base(10)));
        } else if (value.is_string()) {
            serialized.encode(ValueTag::StringPrimitive);
            serialized.encode(value.as_string().utf8_string());
        } else {
            return_primitive_type = false;
        }

        if (return_primitive_type)
            return serialized.take_buffer().take_data();

        // 5. If value is a Symbol, then throw a "DataCloneError" DOMException.
        if (value.is_symbol())
            return WebIDL::DataCloneError::create(*m_vm.current_realm(), "Cannot serialize Symbol"_utf16);

        // 6. Let serialized be an uninitialized value.
        // NOTE: We created the serialized value above.
        if (auto object = value.as_if<JS::Object>()) {
            // 7. If value has a [[BooleanData]] internal slot, then set serialized to { [[Type]]: "Boolean", [[BooleanData]]: value.[[BooleanData]] }.
            if (auto const* boolean_object = as_if<JS::BooleanObject>(*object)) {
                serialized.encode(ValueTag::BooleanObject);
                serialized.encode(boolean_object->boolean());
            }

            // 8. Otherwise, if value has a [[NumberData]] internal slot, then set serialized to { [[Type]]: "Number", [[NumberData]]: value.[[NumberData]] }.
            else if (auto const* number_object = as_if<JS::NumberObject>(*object)) {
                serialized.encode(ValueTag::NumberObject);
                serialized.encode(number_object->number());
            }

            // 9. Otherwise, if value has a [[BigIntData]] internal slot, then set serialized to { [[Type]]: "BigInt", [[BigIntData]]: value.[[BigIntData]] }.
            else if (auto const* big_int_object = as_if<JS::BigIntObject>(*object)) {
                serialized.encode(ValueTag::BigIntObject);
                serialized.encode(MUST(big_int_object->bigint().big_integer().to_base(10)));
            }

            // 10. Otherwise, if value has a [[StringData]] internal slot, then set serialized to { [[Type]]: "String", [[StringData]]: value.[[StringData]] }.
            else if (auto const* string_object = as_if<JS::StringObject>(*object)) {
                serialized.encode(ValueTag::StringObject);
                serialized.encode(string_object->primitive_string().utf8_string());
            }

            // 11. Otherwise, if value has a [[DateValue]] internal slot, then set serialized to { [[Type]]: "Date", [[DateValue]]: value.[[DateValue]] }.
            else if (auto const* date = as_if<JS::Date>(*object)) {
                serialized.encode(ValueTag::DateObject);
                serialized.encode(date->date_value());
            }

            // 12. Otherwise, if value has a [[RegExpMatcher]] internal slot, then set serialized to
            //     { [[Type]]: "RegExp", [[RegExpMatcher]]: value.[[RegExpMatcher]], [[OriginalSource]]: value.[[OriginalSource]],
            //       [[OriginalFlags]]: value.[[OriginalFlags]] }.
            else if (auto const* reg_exp_object = as_if<JS::RegExpObject>(*object)) {
                // NOTE: A Regex<ECMA262> object is perfectly happy to be reconstructed with just the source+flags.
                //       In the future, we could optimize the work being done on the deserialize step by serializing
                //       more of the internal state (the [[RegExpMatcher]] internal slot).
                serialized.encode(ValueTag::RegExpObject);
                serialized.encode(reg_exp_object->pattern());
                serialized.encode(reg_exp_object->flags());
            }

            // 13. Otherwise, if value has an [[ArrayBufferData]] internal slot, then:
            else if (auto const* array_buffer = as_if<JS::ArrayBuffer>(*object)) {
                TRY(serialize_array_buffer(m_vm, serialized, *array_buffer, m_for_storage));
            }

            // 14. Otherwise, if value has a [[ViewedArrayBuffer]] internal slot, then:
            else if (auto const* typed_array_base = as_if<JS::TypedArrayBase>(*object)) {
                TRY(serialize_viewed_array_buffer(m_vm, serialized, *typed_array_base, m_for_storage, m_memory));
            } else if (auto const* data_view = as_if<JS::DataView>(*object)) {
                TRY(serialize_viewed_array_buffer(m_vm, serialized, *data_view, m_for_storage, m_memory));
            }

            // 15. Otherwise, if value has a [[MapData]] internal slot, then:
            else if (is<JS::Map>(*object)) {
                // 1. Set serialized to { [[Type]]: "Map", [[MapData]]: a new empty List }.
                serialized.encode(ValueTag::MapObject);

                // 2. Set deep to true.
                deep = true;
            }

            // 16. Otherwise, if value has a [[SetData]] internal slot, then:
            else if (is<JS::Set>(*object)) {
                // 1. Set serialized to { [[Type]]: "Set", [[SetData]]: a new empty List }.
                serialized.encode(ValueTag::SetObject);

                // 2. Set deep to true.
                deep = true;
            }

            // 17. Otherwise, if value has an [[ErrorData]] internal slot and value is not a platform object, then:
            else if (is<JS::Error>(*object) && !is<Bindings::PlatformObject>(*object)) {
                // 1. Let name be ? Get(value, "name").
                auto name = TRY(object->get(m_vm.names.name));

                // 2. If name is not one of "Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", or "URIError", then set name to "Error".
                auto type = ErrorType::Error;
                if (name.is_string())
                    type = error_name_to_type(name.as_string().utf8_string_view());

                // 3. Let valueMessageDesc be ? value.[[GetOwnProperty]]("message").
                auto value_message_descriptor = TRY(object->internal_get_own_property(m_vm.names.message));

                // 4. Let message be undefined if IsDataDescriptor(valueMessageDesc) is false, and ? ToString(valueMessageDesc.[[Value]]) otherwise.
                Optional<Utf16String> message;
                if (value_message_descriptor.has_value() && value_message_descriptor->is_data_descriptor())
                    message = TRY(value_message_descriptor->value->to_utf16_string(m_vm));

                // FIXME: Spec bug - https://github.com/whatwg/html/issues/11321
                // MISSING STEP: Let valueCauseDesc be ? value.[[GetOwnProperty]]("cause").
                auto value_cause_descriptor = TRY(object->internal_get_own_property(m_vm.names.cause));

                // MISSING STEP: Let cause be undefined if IsDataDescriptor(valueCauseDesc) is false, and ? ToString(valueCauseDesc.[[Value]]) otherwise.
                Optional<Utf16String> cause;
                if (value_cause_descriptor.has_value() && value_cause_descriptor->is_data_descriptor())
                    cause = TRY(value_cause_descriptor->value->to_utf16_string(m_vm));

                // 5. Set serialized to { [[Type]]: "Error", [[Name]]: name, [[Message]]: message }.
                // FIXME: 6. User agents should attach a serialized representation of any interesting accompanying data which are not yet specified, notably the stack property, to serialized.
                serialized.encode(ValueTag::ErrorObject);
                serialized.encode(type);
                serialized.encode(message);
                serialized.encode(cause);
            }

            // 18. Otherwise, if value is an Array exotic object, then:
            else if (is<JS::Array>(*object)) {
                // 1. Let valueLenDescriptor be ? OrdinaryGetOwnProperty(value, "length").
                // 2. Let valueLen be valueLenDescriptor.[[Value]].
                // NON-STANDARD: Array objects in LibJS do not have a real length property, so it must be accessed the usual way
                u64 length = MUST(JS::length_of_array_like(m_vm, *object));

                // 3. Set serialized to { [[Type]]: "Array", [[Length]]: valueLen, [[Properties]]: a new empty List }.
                serialized.encode(ValueTag::ArrayObject);
                serialized.encode(length);

                // 4. Set deep to true.
                deep = true;
            }

            // 19. Otherwise, if value is a platform object that is a serializable object:
            else if (auto const* serializable = as_if<Bindings::Serializable>(*object)) {
                // FIXME: 1. If value has a [[Detached]] internal slot whose value is true, then throw a "DataCloneError" DOMException.

                // 2. Let typeString be the identifier of the primary interface of value.
                // 3. Set serialized to { [[Type]]: typeString }.
                serialized.encode(ValueTag::SerializableObject);
                serialized.encode(as<Bindings::PlatformObject>(serializable)->interface_name());

                // 4. Set deep to true
                deep = true;
            }

            // 20. Otherwise, if value is a platform object, then throw a "DataCloneError" DOMException.
            else if (is<Bindings::PlatformObject>(*object)) {
                return throw_completion(WebIDL::DataCloneError::create(*m_vm.current_realm(), "Cannot serialize platform objects"_utf16));
            }

            // 21. Otherwise, if IsCallable(value) is true, then throw a "DataCloneError" DOMException.
            else if (value.is_function()) {
                return throw_completion(WebIDL::DataCloneError::create(*m_vm.current_realm(), "Cannot serialize functions"_utf16));
            }

            // FIXME: 22. Otherwise, if value has any internal slot other than [[Prototype]] or [[Extensible]], then throw a "DataCloneError" DOMException.

            // FIXME: 23. Otherwise, if value is an exotic object and value is not the %Object.prototype% intrinsic object associated with any realm, then throw a "DataCloneError" DOMException.

            // 24. Otherwise:
            else {
                // 1. Set serialized to { [[Type]]: "Object", [[Properties]]: a new empty List }.
                serialized.encode(ValueTag::Object);

                // 2. Set deep to true.
                deep = true;
            }
        }

        // 25. Set memory[value] to serialized.
        m_memory.set(make_root(value), m_next_id++);

        // 26. If deep is true, then:
        if (deep) {
            auto& object = value.as_object();

            // 1. If value has a [[MapData]] internal slot, then:
            if (auto const* map = as_if<JS::Map>(object)) {
                // 1. Let copiedList be a new empty List.
                Vector<JS::Value> copied_list;
                copied_list.ensure_capacity(map->map_size() * 2);

                // 2. For each Record { [[Key]], [[Value]] } entry of value.[[MapData]]:
                for (auto const& entry : *map) {
                    // 1. Let copiedEntry be a new Record { [[Key]]: entry.[[Key]], [[Value]]: entry.[[Value]] }.
                    // 2. If copiedEntry.[[Key]] is not the special value empty, append copiedEntry to copiedList.
                    copied_list.append(entry.key);
                    copied_list.append(entry.value);
                }

                serialized.encode(map->map_size());

                // 3. For each Record { [[Key]], [[Value]] } entry of copiedList:
                for (auto copied_value : copied_list) {
                    // 1. Let serializedKey be ? StructuredSerializeInternal(entry.[[Key]], forStorage, memory).
                    // 2. Let serializedValue be ? StructuredSerializeInternal(entry.[[Value]], forStorage, memory).
                    auto serialized_value = TRY(structured_serialize_internal(m_vm, copied_value, m_for_storage, m_memory));

                    // 3. Append { [[Key]]: serializedKey, [[Value]]: serializedValue } to serialized.[[MapData]].
                    serialized.append(move(serialized_value));
                }
            }

            // 2. Otherwise, if value has a [[SetData]] internal slot, then:
            else if (auto const* set = as_if<JS::Set>(object)) {
                // 1. Let copiedList be a new empty List.
                Vector<JS::Value> copied_list;
                copied_list.ensure_capacity(set->set_size());

                // 2. For each entry of value.[[SetData]]:
                for (auto const& entry : *set) {
                    // 1. If entry is not the special value empty, append entry to copiedList.
                    copied_list.append(entry.key);
                }

                serialized.encode(set->set_size());

                // 3. For each entry of copiedList:
                for (auto copied_value : copied_list) {
                    // 1. Let serializedEntry be ? StructuredSerializeInternal(entry, forStorage, memory).
                    auto serialized_value = TRY(structured_serialize_internal(m_vm, copied_value, m_for_storage, m_memory));

                    // 2. Append serializedEntry to serialized.[[SetData]].
                    serialized.append(move(serialized_value));
                }
            }

            // 3. Otherwise, if value is a platform object that is a serializable object, then perform the serialization steps for value's primary interface, given value, serialized, and forStorage.
            else if (auto* serializable = as_if<Bindings::Serializable>(object)) {
                TRY(serializable->serialization_steps(serialized, m_for_storage, m_memory));
            }

            // 4. Otherwise, for each key in ! EnumerableOwnProperties(value, key):
            else {
                u64 property_count = 0;
                auto count_offset = serialized.buffer().data().size();
                serialized.encode(property_count);

                for (auto key : MUST(object.enumerable_own_property_names(JS::Object::PropertyKind::Key))) {
                    auto property_key = MUST(JS::PropertyKey::from_value(m_vm, key));

                    // 1. If ! HasOwnProperty(value, key) is true, then:
                    if (MUST(object.has_own_property(property_key))) {
                        // 1. Let inputValue be ? value.[[Get]](key, value).
                        auto input_value = TRY(object.internal_get(property_key, value));

                        // 2. Let outputValue be ? StructuredSerializeInternal(inputValue, forStorage, memory).
                        auto output_value = TRY(structured_serialize_internal(m_vm, input_value, m_for_storage, m_memory));

                        // 3. Append { [[Key]]: key, [[Value]]: outputValue } to serialized.[[Properties]].
                        serialized.encode(key.as_string().utf16_string());
                        serialized.append(move(output_value));

                        ++property_count;
                    }
                }

                if (property_count) {
                    auto* data = const_cast<u8*>(serialized.buffer().data().data());
                    memcpy(data + count_offset, &property_count, sizeof(property_count));
                }
            }
        }

        // 27. Return serialized.
        return serialized.take_buffer().take_data();
    }

private:
    JS::VM& m_vm;
    SerializationMemory& m_memory; // JS value -> index
    u32 m_next_id { 0 };
    bool m_for_storage { false };
};

class Deserializer {
public:
    Deserializer(JS::VM& vm, TransferDataDecoder& serialized, JS::Realm& target_realm, DeserializationMemory& memory)
        : m_vm(vm)
        , m_serialized(serialized)
        , m_memory(memory)
    {
        VERIFY(vm.current_realm() == &target_realm);
    }

    // https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
    WebIDL::ExceptionOr<JS::Value> deserialize()
    {
        auto& realm = *m_vm.current_realm();

        auto tag = m_serialized.decode<ValueTag>();

        // 2. If memory[serialized] exists, then return memory[serialized].
        if (tag == ValueTag::ObjectReference) {
            auto index = m_serialized.decode<u32>();
            if (index == NumericLimits<u32>::max())
                return JS::Object::create(*m_vm.current_realm(), nullptr);
            return m_memory[index];
        }

        // 3. Let deep be false.
        auto deep = false;

        // 4. Let value be an uninitialized value.
        JS::Value value;

        auto is_primitive = false;

        auto decode_string = [&]() {
            auto string = m_serialized.decode<String>();
            return JS::PrimitiveString::create(m_vm, string);
        };

        auto decode_utf16_string = [&]() {
            auto string = m_serialized.decode<Utf16String>();
            return JS::PrimitiveString::create(m_vm, string);
        };

        auto decode_big_int = [&]() {
            auto string = m_serialized.decode<String>();
            return JS::BigInt::create(m_vm, MUST(::Crypto::SignedBigInteger::from_base(10, string)));
        };

        switch (tag) {
        // 5. If serialized.[[Type]] is "primitive", then set value to serialized.[[Value]].
        case ValueTag::UndefinedPrimitive:
            value = JS::js_undefined();
            is_primitive = true;
            break;
        case ValueTag::NullPrimitive:
            value = JS::js_null();
            is_primitive = true;
            break;
        case ValueTag::BooleanPrimitive:
            value = JS::Value { m_serialized.decode<bool>() };
            is_primitive = true;
            break;
        case ValueTag::NumberPrimitive:
            value = JS::Value { m_serialized.decode<double>() };
            is_primitive = true;
            break;
        case ValueTag::BigIntPrimitive:
            value = decode_big_int();
            is_primitive = true;
            break;
        case ValueTag::StringPrimitive:
            value = decode_string();
            is_primitive = true;
            break;

        // 6. Otherwise, if serialized.[[Type]] is "Boolean", then set value to a new Boolean object in targetRealm whose [[BooleanData]] internal slot value is serialized.[[BooleanData]].
        case ValueTag::BooleanObject:
            value = JS::BooleanObject::create(realm, m_serialized.decode<bool>());
            break;

        // 7. Otherwise, if serialized.[[Type]] is "Number", then set value to a new Number object in targetRealm whose [[NumberData]] internal slot value is serialized.[[NumberData]].
        case ValueTag::NumberObject:
            value = JS::NumberObject::create(realm, m_serialized.decode<double>());
            break;

        // 8. Otherwise, if serialized.[[Type]] is "BigInt", then set value to a new BigInt object in targetRealm whose [[BigIntData]] internal slot value is serialized.[[BigIntData]].
        case ValueTag::BigIntObject:
            value = JS::BigIntObject::create(realm, decode_big_int());
            break;

        // 9. Otherwise, if serialized.[[Type]] is "String", then set value to a new String object in targetRealm whose [[StringData]] internal slot value is serialized.[[StringData]].
        case ValueTag::StringObject:
            value = JS::StringObject::create(realm, decode_string(), realm.intrinsics().string_prototype());
            break;

        // 10. Otherwise, if serialized.[[Type]] is "Date", then set value to a new Date object in targetRealm whose [[DateValue]] internal slot value is serialized.[[DateValue]].
        case ValueTag::DateObject:
            value = JS::Date::create(realm, m_serialized.decode<double>());
            break;

        // 11. Otherwise, if serialized.[[Type]] is "RegExp", then set value to a new RegExp object in targetRealm whose [[RegExpMatcher]] internal slot value is serialized.[[RegExpMatcher]],
        //     whose [[OriginalSource]] internal slot value is serialized.[[OriginalSource]], and whose [[OriginalFlags]] internal slot value is serialized.[[OriginalFlags]].
        case ValueTag::RegExpObject: {
            auto pattern = decode_utf16_string();
            auto flags = decode_utf16_string();

            value = MUST(JS::regexp_create(m_vm, pattern, flags));
            break;
        }

        // 12. Otherwise, if serialized.[[Type]] is "SharedArrayBuffer", then:
        case ValueTag::SharedArrayBuffer: {
            // FIXME: 1. If targetRealm's corresponding agent cluster is not serialized.[[AgentCluster]], then throw a "DataCloneError" DOMException.

            // 2. Otherwise, set value to a new SharedArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]]
            //    and whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]].
            auto buffer = TRY(m_serialized.decode_buffer(realm));
            value = JS::ArrayBuffer::create(realm, move(buffer), JS::DataBlock::Shared::Yes);
            break;
        }

        // 13. Otherwise, if serialized.[[Type]] is "GrowableSharedArrayBuffer", then:
        case ValueTag::GrowableSharedArrayBuffer: {
            // FIXME: 1. If targetRealm's corresponding agent cluster is not serialized.[[AgentCluster]], then throw a "DataCloneError" DOMException.

            // 2. Otherwise, set value to a new SharedArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]],
            //    whose [[ArrayBufferByteLengthData]] internal slot value is serialized.[[ArrayBufferByteLengthData]],
            //    and whose [[ArrayBufferMaxByteLength]] internal slot value is serialized.[[ArrayBufferMaxByteLength]].
            auto buffer = TRY(m_serialized.decode_buffer(realm));
            auto max_byte_length = m_serialized.decode<size_t>();

            auto data = JS::ArrayBuffer::create(realm, move(buffer), JS::DataBlock::Shared::Yes);
            data->set_max_byte_length(max_byte_length);

            value = data;
            break;
        }

        // 14. Otherwise, if serialized.[[Type]] is "ArrayBuffer", then set value to a new ArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]], and whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]].
        case ValueTag::ArrayBuffer: {
            auto buffer = TRY(m_serialized.decode_buffer(realm));
            value = JS::ArrayBuffer::create(realm, move(buffer));
            break;
        }

        // 15. Otherwise, if serialized.[[Type]] is "ResizableArrayBuffer", then set value to a new ArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]], whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]], and whose [[ArrayBufferMaxByteLength]] internal slot value is a serialized.[[ArrayBufferMaxByteLength]].
        case ValueTag::ResizeableArrayBuffer: {
            auto buffer = TRY(m_serialized.decode_buffer(realm));
            auto max_byte_length = m_serialized.decode<size_t>();

            auto data = JS::ArrayBuffer::create(realm, move(buffer));
            data->set_max_byte_length(max_byte_length);

            value = data;
            break;
        }

        // 16. Otherwise, if serialized.[[Type]] is "ArrayBufferView", then:
        case ValueTag::ArrayBufferView: {
            auto array_buffer_value = TRY(deserialize());
            auto& array_buffer = as<JS::ArrayBuffer>(array_buffer_value.as_object());

            auto deserialize_byte_length = [&]() -> JS::ByteLength {
                auto is_auto = m_serialized.decode<bool>();
                if (is_auto)
                    return JS::ByteLength::auto_();

                auto length = m_serialized.decode<u32>();
                return length;
            };

            auto constructor_name = m_serialized.decode<Utf16String>();
            auto byte_length = deserialize_byte_length();
            auto byte_offset = m_serialized.decode<u32>();

            if (constructor_name == "DataView"sv) {
                value = JS::DataView::create(realm, &array_buffer, byte_length, byte_offset);
            } else {
                auto array_length = deserialize_byte_length();

                GC::Ptr<JS::TypedArrayBase> typed_array;
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (constructor_name == #ClassName##sv)                                         \
        typed_array = JS::ClassName::create(realm, 0, array_buffer);
                JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
#undef CREATE_TYPED_ARRAY

                VERIFY(typed_array); // FIXME: Handle errors better here? Can a fuzzer put weird stuff in the buffer?

                typed_array->set_array_length(array_length);
                typed_array->set_byte_length(byte_length);
                typed_array->set_byte_offset(byte_offset);
                value = typed_array;
            }
            break;
        }

        // 17. Otherwise, if serialized.[[Type]] is "Map", then:
        case ValueTag::MapObject: {
            // 1. Set value to a new Map object in targetRealm whose [[MapData]] internal slot value is a new empty List.
            value = JS::Map::create(realm);

            // 2. Set deep to true.
            deep = true;
            break;
        }

        // 18. Otherwise, if serialized.[[Type]] is "Set", then:
        case ValueTag::SetObject: {
            // 1. Set value to a new Set object in targetRealm whose [[SetData]] internal slot value is a new empty List.
            value = JS::Set::create(realm);

            // 2. Set deep to true.
            deep = true;
            break;
        }

        // 19. Otherwise, if serialized.[[Type]] is "Array", then:
        case ValueTag::ArrayObject: {
            // 1. Let outputProto be targetRealm.[[Intrinsics]].[[%Array.prototype%]].
            // 2. Set value to ! ArrayCreate(serialized.[[Length]], outputProto).
            value = MUST(JS::Array::create(realm, m_serialized.decode<u64>(), realm.intrinsics().array_prototype()));

            // 3. Set deep to true.
            deep = true;
            break;
        }

        // 20. Otherwise, if serialized.[[Type]] is "Object", then:
        case ValueTag::Object: {
            // 1. Set value to a new Object in targetRealm.
            value = JS::Object::create(realm, realm.intrinsics().object_prototype());

            // 2. Set deep to true.
            deep = true;
            break;
        }

        // 21. Otherwise, if serialized.[[Type]] is "Error", then:
        case ValueTag::ErrorObject: {
            auto type = m_serialized.decode<ErrorType>();
            auto message = m_serialized.decode<Optional<Utf16String>>();
            auto cause = m_serialized.decode<Optional<Utf16String>>();

            GC::Ptr<JS::Error> error;

            switch (type) {
            case ErrorType::Error:
                error = JS::Error::create(realm);
                break;
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    case ErrorType::ClassName:                                                           \
        error = JS::ClassName::create(realm);                                            \
        break;
                JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
            }

            VERIFY(error);

            if (message.has_value())
                error->set_message(message.release_value());

            if (cause.has_value())
                error->create_non_enumerable_data_property_or_throw(m_vm.names.cause, JS::PrimitiveString::create(m_vm, cause.release_value()));

            value = error;
            break;
        }

        // 22. Otherwise:
        default:
            VERIFY(tag == ValueTag::SerializableObject);

            // 1. Let interfaceName be serialized.[[Type]].
            auto interface_name = m_serialized.decode<Bindings::InterfaceName>();

            // 2. If the interface identified by interfaceName is not exposed in targetRealm, then throw a "DataCloneError" DOMException.
            if (!is_exposed(interface_name, realm))
                return WebIDL::DataCloneError::create(realm, "Unsupported type"_utf16);

            // 3. Set value to a new instance of the interface identified by interfaceName, created in targetRealm.
            value = create_serialized_type(interface_name, realm);

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
                auto& map = as<JS::Map>(value.as_object());
                auto length = m_serialized.decode<u64>();

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
                auto& set = as<JS::Set>(value.as_object());
                auto length = m_serialized.decode<u64>();

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
                auto length = m_serialized.decode<u64>();

                // 1. For each Record { [[Key]], [[Value]] } entry of serialized.[[Properties]]:
                for (u64 i = 0u; i < length; ++i) {
                    auto key = m_serialized.decode<Utf16String>();

                    // 1. Let deserializedValue be ? StructuredDeserialize(entry.[[Value]], targetRealm, memory).
                    auto deserialized_value = TRY(deserialize());

                    // 2. Let result be ! CreateDataProperty(value, entry.[[Key]], deserializedValue).
                    auto result = MUST(object.create_data_property(key, deserialized_value));

                    // 3. Assert: result is true.
                    VERIFY(result);
                }
            }

            // 4. Otherwise:
            else {
                // 1. Perform the appropriate deserialization steps for the interface identified by serialized.[[Type]], given serialized, value, and targetRealm.
                auto& serializable = as<Bindings::Serializable>(value.as_object());
                TRY(serializable.deserialization_steps(m_serialized, m_memory));
            }
        }

        // 25. Return value.
        return value;
    }

private:
    static GC::Ref<Bindings::PlatformObject> create_serialized_type(Bindings::InterfaceName serialize_type, JS::Realm& realm)
    {
        switch (serialize_type) {
        case Bindings::InterfaceName::Blob:
            return FileAPI::Blob::create(realm);
        case Bindings::InterfaceName::File:
            return FileAPI::File::create(realm);
        case Bindings::InterfaceName::FileList:
            return FileAPI::FileList::create(realm);
        case Bindings::InterfaceName::DOMException:
            return WebIDL::DOMException::create(realm);
        case Bindings::InterfaceName::DOMMatrixReadOnly:
            return Geometry::DOMMatrixReadOnly::create(realm);
        case Bindings::InterfaceName::DOMMatrix:
            return Geometry::DOMMatrix::create(realm);
        case Bindings::InterfaceName::DOMPointReadOnly:
            return Geometry::DOMPointReadOnly::create(realm);
        case Bindings::InterfaceName::DOMPoint:
            return Geometry::DOMPoint::create(realm);
        case Bindings::InterfaceName::DOMRectReadOnly:
            return Geometry::DOMRectReadOnly::create(realm);
        case Bindings::InterfaceName::DOMRect:
            return Geometry::DOMRect::create(realm);
        case Bindings::InterfaceName::CryptoKey:
            return Crypto::CryptoKey::create(realm);
        case Bindings::InterfaceName::DOMQuad:
            return Geometry::DOMQuad::create(realm);
        case Bindings::InterfaceName::ImageData:
            return ImageData::create(realm);
        case Bindings::InterfaceName::ImageBitmap:
            return ImageBitmap::create(realm);
        case Bindings::InterfaceName::QuotaExceededError:
            return WebIDL::QuotaExceededError::create(realm);
        case Bindings::InterfaceName::Unknown:
        default:
            VERIFY_NOT_REACHED();
        }
    }

    JS::VM& m_vm;
    TransferDataDecoder& m_serialized;
    GC::RootVector<JS::Value> m_memory;
};

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializewithtransfer
WebIDL::ExceptionOr<SerializedTransferRecord> structured_serialize_with_transfer(JS::VM& vm, JS::Value value, Vector<GC::Root<JS::Object>> const& transfer_list)
{
    // 1. Let memory be an empty map.
    SerializationMemory memory = {};

    // 2. For each transferable of transferList:
    for (auto const& transferable : transfer_list) {
        auto const* as_array_buffer = as_if<JS::ArrayBuffer>(*transferable);

        // 1. If transferable has neither an [[ArrayBufferData]] internal slot nor a [[Detached]] internal slot, then throw a "DataCloneError" DOMException.
        // FIXME: Handle transferring objects with [[Detached]] internal slot.
        if (!as_array_buffer && !is<Bindings::Transferable>(*transferable))
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot transfer type"_utf16);

        // 2. If transferable has an [[ArrayBufferData]] internal slot and IsSharedArrayBuffer(transferable) is true, then throw a "DataCloneError" DOMException.
        if (as_array_buffer && as_array_buffer->is_shared_array_buffer())
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot transfer shared array buffer"_utf16);

        JS::Value transferable_value { transferable };

        // 3. If memory[transferable] exists, then throw a "DataCloneError" DOMException.
        if (memory.contains(transferable_value))
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot transfer value twice"_utf16);

        // 4. Set memory[transferable] to { [[Type]]: an uninitialized value }.
        memory.set(GC::make_root(transferable_value), memory.size());
    }

    // 3. Let serialized be ? StructuredSerializeInternal(value, false, memory).
    auto serialized = TRY(structured_serialize_internal(vm, value, false, memory));

    // 4. Let transferDataHolders be a new empty List.
    Vector<TransferDataEncoder> transfer_data_holders;
    transfer_data_holders.ensure_capacity(transfer_list.size());

    // 5. For each transferable of transferList:
    for (auto const& transferable : transfer_list) {
        auto* array_buffer = as_if<JS::ArrayBuffer>(*transferable);
        auto is_detached = array_buffer && array_buffer->is_detached();

        // 1. If transferable has an [[ArrayBufferData]] internal slot and IsDetachedBuffer(transferable) is true, then throw a "DataCloneError" DOMException.
        if (is_detached)
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot transfer detached buffer"_utf16);

        // 2. If transferable has a [[Detached]] internal slot and transferable.[[Detached]] is true, then throw a "DataCloneError" DOMException.
        if (auto* transferable_object = as_if<Bindings::Transferable>(*transferable)) {
            if (transferable_object->is_detached())
                return WebIDL::DataCloneError::create(*vm.current_realm(), "Value already transferred"_utf16);
        }

        // 3. Let dataHolder be memory[transferable].
        // IMPLEMENTATION DEFINED: We just create a data holder here, our memory holds indices into the SerializationRecord
        TransferDataEncoder data_holder;

        // 4. If transferable has an [[ArrayBufferData]] internal slot, then:
        if (array_buffer) {
            // 1. If transferable has an [[ArrayBufferMaxByteLength]] internal slot, then:
            if (!array_buffer->is_fixed_length()) {
                // 1. Set dataHolder.[[Type]] to "ResizableArrayBuffer".
                data_holder.encode(TransferType::ResizableArrayBuffer);

                // 2. Set dataHolder.[[ArrayBufferData]] to transferable.[[ArrayBufferData]].
                // 3. Set dataHolder.[[ArrayBufferByteLength]] to transferable.[[ArrayBufferByteLength]].
                data_holder.encode(array_buffer->buffer());

                // 4. Set dataHolder.[[ArrayBufferMaxByteLength]] to transferable.[[ArrayBufferMaxByteLength]].
                data_holder.encode(array_buffer->max_byte_length());
            }
            // 2. Otherwise:
            else {
                // 1. Set dataHolder.[[Type]] to "ArrayBuffer".
                data_holder.encode(TransferType::ArrayBuffer);

                // 2. Set dataHolder.[[ArrayBufferData]] to transferable.[[ArrayBufferData]].
                // 3. Set dataHolder.[[ArrayBufferByteLength]] to transferable.[[ArrayBufferByteLength]].
                data_holder.encode(array_buffer->buffer());
            }

            // 3. Perform ? DetachArrayBuffer(transferable).
            // NOTE: Specifications can use the [[ArrayBufferDetachKey]] internal slot to prevent ArrayBuffers from being detached. This is used in WebAssembly JavaScript Interface, for example. See: https://html.spec.whatwg.org/multipage/references.html#refsWASMJS
            TRY(JS::detach_array_buffer(vm, *array_buffer));
        }
        // 5. Otherwise:
        else {
            // 1. Assert: transferable is a platform object that is a transferable object.
            auto& transferable_object = as<Bindings::Transferable>(*transferable);
            VERIFY(is<Bindings::PlatformObject>(*transferable));

            // 2. Let interfaceName be the identifier of the primary interface of transferable.
            auto interface_name = transferable_object.primary_interface();

            // 3. Set dataHolder.[[Type]] to interfaceName.
            data_holder.encode(interface_name);

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

static bool is_transferable_interface_exposed_on_target_realm(TransferType name, JS::Realm& realm)
{
    switch (name) {
    case TransferType::MessagePort:
        return is_exposed(Bindings::InterfaceName::MessagePort, realm);
    case TransferType::ReadableStream:
        return is_exposed(Bindings::InterfaceName::ReadableStream, realm);
    case TransferType::WritableStream:
        return is_exposed(Bindings::InterfaceName::WritableStream, realm);
    case TransferType::TransformStream:
        return is_exposed(Bindings::InterfaceName::TransformStream, realm);
    case TransferType::ImageBitmap:
        return is_exposed(Bindings::InterfaceName::ImageBitmap, realm);
    case TransferType::Unknown:
        dbgln("Unknown interface type for transfer: {}", to_underlying(name));
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    return false;
}

static WebIDL::ExceptionOr<GC::Ref<Bindings::PlatformObject>> create_transferred_value(TransferType name, JS::Realm& target_realm, TransferDataDecoder& decoder)
{
    switch (name) {
    case TransferType::MessagePort: {
        auto message_port = HTML::MessagePort::create(target_realm);
        TRY(message_port->transfer_receiving_steps(decoder));
        return message_port;
    }
    case TransferType::ReadableStream: {
        auto readable_stream = target_realm.create<Streams::ReadableStream>(target_realm);
        TRY(readable_stream->transfer_receiving_steps(decoder));
        return readable_stream;
    }
    case TransferType::WritableStream: {
        auto writable_stream = target_realm.create<Streams::WritableStream>(target_realm);
        TRY(writable_stream->transfer_receiving_steps(decoder));
        return writable_stream;
    }
    case TransferType::TransformStream: {
        auto transform_stream = target_realm.create<Streams::TransformStream>(target_realm);
        TRY(transform_stream->transfer_receiving_steps(decoder));
        return transform_stream;
    }
    case TransferType::ImageBitmap: {
        auto image_bitmap = target_realm.create<ImageBitmap>(target_realm);
        TRY(image_bitmap->transfer_receiving_steps(decoder));
        return image_bitmap;
    }
    case TransferType::ArrayBuffer:
    case TransferType::ResizableArrayBuffer:
    case TransferType::Unknown:
        break;
    }
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserializewithtransfer
WebIDL::ExceptionOr<DeserializedTransferRecord> structured_deserialize_with_transfer(SerializedTransferRecord& serialize_with_transfer_result, JS::Realm& target_realm)
{
    auto& vm = target_realm.vm();

    // 1. Let memory be an empty map.
    auto memory = DeserializationMemory(vm.heap());

    // 2. Let transferredValues be a new empty List.
    Vector<GC::Root<JS::Object>> transferred_values;

    // 3. For each transferDataHolder of serializeWithTransferResult.[[TransferDataHolders]]:
    for (auto& transfer_data_holder : serialize_with_transfer_result.transfer_data_holders) {
        if (transfer_data_holder.buffer().data().is_empty())
            continue;

        TransferDataDecoder decoder { move(transfer_data_holder) };

        // 1. Let value be an uninitialized value.
        auto value = TRY(structured_deserialize_with_transfer_internal(decoder, target_realm));

        // 5. Set memory[transferDataHolder] to value.
        memory.append(value);

        // 6. Append value to transferredValues.
        transferred_values.append(GC::make_root(value.as_object()));
    }

    // 4. Let deserialized be ? StructuredDeserialize(serializeWithTransferResult.[[Serialized]], targetRealm, memory).
    auto deserialized = TRY(structured_deserialize(vm, serialize_with_transfer_result.serialized, target_realm, memory));

    // 5. Return { [[Deserialized]]: deserialized, [[TransferredValues]]: transferredValues }.
    return DeserializedTransferRecord { .deserialized = deserialized, .transferred_values = move(transferred_values) };
}

// AD-HOC: This non-standard overload is meant to extract just one transferrable value from a serialized transfer record.
//         It's primarily useful for an object's transfer receiving steps to deserialize a nested value.
WebIDL::ExceptionOr<JS::Value> structured_deserialize_with_transfer_internal(TransferDataDecoder& decoder, JS::Realm& target_realm)
{
    auto type = decoder.decode<TransferType>();

    // 1. Let value be an uninitialized value.
    JS::Value value;

    // 2. If transferDataHolder.[[Type]] is "ArrayBuffer", then set value to a new ArrayBuffer object in targetRealm
    //    whose [[ArrayBufferData]] internal slot value is transferDataHolder.[[ArrayBufferData]], and
    //    whose [[ArrayBufferByteLength]] internal slot value is transferDataHolder.[[ArrayBufferByteLength]].
    // NOTE: In cases where the original memory occupied by [[ArrayBufferData]] is accessible during the deserialization,
    //       this step is unlikely to throw an exception, as no new memory needs to be allocated: the memory occupied by
    //       [[ArrayBufferData]] is instead just getting transferred into the new ArrayBuffer. This could be true, for example,
    //       when both the source and target realms are in the same process.
    if (type == TransferType::ArrayBuffer) {
        auto buffer = TRY(decoder.decode_buffer(target_realm));
        value = JS::ArrayBuffer::create(target_realm, move(buffer));
    }

    // 3. Otherwise, if transferDataHolder.[[Type]] is "ResizableArrayBuffer", then set value to a new ArrayBuffer object
    //     in targetRealm whose [[ArrayBufferData]] internal slot value is transferDataHolder.[[ArrayBufferData]], whose
    //     [[ArrayBufferByteLength]] internal slot value is transferDataHolder.[[ArrayBufferByteLength]], and whose
    //     [[ArrayBufferMaxByteLength]] internal slot value is transferDataHolder.[[ArrayBufferMaxByteLength]].
    // NOTE: For the same reason as the previous step, this step is also unlikely to throw an exception.
    else if (type == TransferType::ResizableArrayBuffer) {
        auto buffer = TRY(decoder.decode_buffer(target_realm));
        auto max_byte_length = decoder.decode<size_t>();

        auto data = JS::ArrayBuffer::create(target_realm, move(buffer));
        data->set_max_byte_length(max_byte_length);

        value = data;
    }

    // 4. Otherwise:
    else {
        // 1. Let interfaceName be transferDataHolder.[[Type]].
        // 2. If the interface identified by interfaceName is not exposed in targetRealm, then throw a "DataCloneError" DOMException.
        if (!is_transferable_interface_exposed_on_target_realm(type, target_realm))
            return WebIDL::DataCloneError::create(target_realm, "Unknown type transferred"_utf16);

        // 3. Set value to a new instance of the interface identified by interfaceName, created in targetRealm.
        // 4. Perform the appropriate transfer-receiving steps for the interface identified by interfaceName given transferDataHolder and value.
        value = TRY(create_transferred_value(type, target_realm, decoder));
    }

    return value;
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
    TemporaryExecutionContext execution_context { target_realm };

    if (!memory.has_value())
        memory = DeserializationMemory { vm.heap() };

    TransferDataDecoder decoder { serialized };
    return structured_deserialize_internal(vm, decoder, target_realm, *memory);
}

WebIDL::ExceptionOr<JS::Value> structured_deserialize_internal(JS::VM& vm, TransferDataDecoder& serialized, JS::Realm& target_realm, DeserializationMemory& memory)
{
    Deserializer deserializer(vm, serialized, target_realm, memory);
    return deserializer.deserialize();
}

TransferDataEncoder::TransferDataEncoder()
    : m_encoder(m_buffer)
{
}

TransferDataEncoder::TransferDataEncoder(IPC::MessageBuffer&& buffer)
    : m_buffer(move(buffer))
    , m_encoder(m_buffer)
{
}

void TransferDataEncoder::append(SerializationRecord&& record)
{
    MUST(m_buffer.append_data(record.data(), record.size()));
}

void TransferDataEncoder::extend(Vector<TransferDataEncoder> data_holders)
{
    for (auto& data_holder : data_holders)
        MUST(m_buffer.extend(move(data_holder.m_buffer)));
}

TransferDataDecoder::TransferDataDecoder(SerializationRecord const& record)
    : m_stream(record.span())
    , m_decoder(m_stream, m_files)
{
}

TransferDataDecoder::TransferDataDecoder(TransferDataEncoder&& data_holder)
    : m_buffer(data_holder.take_buffer())
    , m_stream(m_buffer.data().span())
    , m_decoder(m_stream, m_files)
{
    // FIXME: The churn between IPC::File and IPC::AutoCloseFileDescriptor is pretty awkward, we should find a way to
    //        consolidate the way we use these type.
    for (auto& auto_fd : m_buffer.take_fds())
        m_files.enqueue(IPC::File::adopt_fd(auto_fd->take_fd()));
}

WebIDL::ExceptionOr<ByteBuffer> TransferDataDecoder::decode_buffer(JS::Realm& realm)
{
    auto buffer = m_decoder.decode<ByteBuffer>();

    if (buffer.is_error()) {
        VERIFY(buffer.error().code() == ENOMEM);
        return WebIDL::DataCloneError::create(realm, "Unable to allocate memory for transferred buffer"_utf16);
    }

    return buffer.release_value();
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::TransferDataEncoder const& data_holder)
{
    // FIXME: The churn between IPC::File and IPC::AutoCloseFileDescriptor is pretty awkward, we should find a way to
    //        consolidate the way we use these type.
    Vector<IPC::File> files;
    files.ensure_capacity(data_holder.buffer().fds().size());

    for (auto const& auto_fd : data_holder.buffer().fds()) {
        auto fd = const_cast<AutoCloseFileDescriptor&>(*auto_fd).take_fd();
        files.unchecked_append(IPC::File::adopt_fd(fd));
    }

    TRY(encoder.encode(data_holder.buffer().data()));
    TRY(encoder.encode(files));
    return {};
}

template<>
ErrorOr<Web::HTML::TransferDataEncoder> decode(Decoder& decoder)
{
    auto data = TRY(decoder.decode<Web::HTML::SerializationRecord>());
    auto files = TRY(decoder.decode<Vector<IPC::File>>());

    // FIXME: The churn between IPC::File and IPC::AutoCloseFileDescriptor is pretty awkward, we should find a way to
    //        consolidate the way we use these type.
    MessageFileType auto_files;
    auto_files.ensure_capacity(files.size());

    for (auto& fd : files) {
        auto auto_fd = adopt_ref(*new AutoCloseFileDescriptor(fd.take_fd()));
        auto_files.unchecked_append(move(auto_fd));
    }

    IPC::MessageBuffer buffer { move(data), move(auto_files) };
    return Web::HTML::TransferDataEncoder { move(buffer) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::SerializedTransferRecord const& record)
{
    TRY(encoder.encode(record.serialized));
    TRY(encoder.encode(record.transfer_data_holders));
    return {};
}

template<>
ErrorOr<Web::HTML::SerializedTransferRecord> decode(Decoder& decoder)
{
    auto serialized = TRY(decoder.decode<Web::HTML::SerializationRecord>());
    auto transfer_data_holders = TRY(decoder.decode<Vector<Web::HTML::TransferDataEncoder>>());

    return Web::HTML::SerializedTransferRecord { move(serialized), move(transfer_data_holders) };
}

}
