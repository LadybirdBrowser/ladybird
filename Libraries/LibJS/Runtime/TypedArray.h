/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/ByteLength.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/TypedArrayConstructor.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

class TypedArrayBase : public Object {
    JS_OBJECT(TypedArrayBase, Object);

public:
    enum class ContentType {
        BigInt,
        Number,
    };

    enum class Kind {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    ClassName,
        JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
    };

    using IntrinsicConstructor = GC::Ref<TypedArrayConstructor> (Intrinsics::*)();

    ByteLength const& array_length() const { return m_array_length; }
    ByteLength const& byte_length() const { return m_byte_length; }
    u32 byte_offset() const { return m_byte_offset; }
    ContentType content_type() const { return m_content_type; }
    ArrayBuffer* viewed_array_buffer() const { return m_viewed_array_buffer; }
    IntrinsicConstructor intrinsic_constructor() const { return m_intrinsic_constructor; }

    void set_array_length(ByteLength length) { m_array_length = move(length); }
    void set_byte_length(ByteLength length) { m_byte_length = move(length); }
    void set_byte_offset(u32 offset) { m_byte_offset = offset; }
    void set_viewed_array_buffer(ArrayBuffer* array_buffer) { m_viewed_array_buffer = array_buffer; }

    [[nodiscard]] Kind kind() const { return m_kind; }

    u32 element_size() const { return m_element_size; }
    virtual FlyString const& element_name() const = 0;

    // 25.1.3.11 IsUnclampedIntegerElementType ( type ), https://tc39.es/ecma262/#sec-isunclampedintegerelementtype
    virtual bool is_unclamped_integer_element_type() const = 0;
    // 25.1.3.12 IsBigIntElementType ( type ), https://tc39.es/ecma262/#sec-isbigintelementtype
    virtual bool is_bigint_element_type() const = 0;
    // 25.1.3.16 GetValueFromBuffer ( arrayBuffer, byteIndex, type, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-getvaluefrombuffer
    virtual Value get_value_from_buffer(size_t byte_index, ArrayBuffer::Order, bool is_little_endian = true) const = 0;
    // 25.1.3.18 SetValueInBuffer ( arrayBuffer, byteIndex, type, value, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-setvalueinbuffer
    virtual void set_value_in_buffer(size_t byte_index, Value, ArrayBuffer::Order, bool is_little_endian = true) = 0;
    // 25.1.3.19 GetModifySetValueInBuffer ( arrayBuffer, byteIndex, type, value, op ), https://tc39.es/ecma262/#sec-getmodifysetvalueinbuffer
    virtual Value get_modify_set_value_in_buffer(size_t byte_index, Value value, ReadWriteModifyFunction operation, bool is_little_endian = true) = 0;

protected:
    TypedArrayBase(Object& prototype, IntrinsicConstructor intrinsic_constructor, Kind kind, u32 element_size)
        : Object(ConstructWithPrototypeTag::Tag, prototype, MayInterfereWithIndexedPropertyAccess::Yes)
        , m_element_size(element_size)
        , m_kind(kind)
        , m_intrinsic_constructor(intrinsic_constructor)
    {
        set_is_typed_array();
    }

    u32 m_element_size { 0 };
    ByteLength m_array_length { 0 };
    ByteLength m_byte_length { 0 };
    u32 m_byte_offset { 0 };
    ContentType m_content_type { ContentType::Number };
    Kind m_kind {};
    GC::Ptr<ArrayBuffer> m_viewed_array_buffer;
    IntrinsicConstructor m_intrinsic_constructor { nullptr };

private:
    virtual void visit_edges(Visitor&) override;
};

// 10.4.5.9 TypedArray With Buffer Witness Records, https://tc39.es/ecma262/#sec-typedarray-with-buffer-witness-records
struct TypedArrayWithBufferWitness {
    GC::Ref<TypedArrayBase const> object; // [[Object]]
    ByteLength cached_buffer_byte_length; // [[CachedBufferByteLength]]
};

TypedArrayWithBufferWitness make_typed_array_with_buffer_witness_record(TypedArrayBase const&, ArrayBuffer::Order);
u32 typed_array_byte_length(TypedArrayWithBufferWitness const&);
u32 typed_array_length(TypedArrayWithBufferWitness const&);
bool is_typed_array_out_of_bounds(TypedArrayWithBufferWitness const&);
bool is_typed_array_fixed_length(TypedArrayBase const&);
bool is_valid_integer_index_slow_case(TypedArrayBase const&, CanonicalIndex property_index);

// 10.4.5.16 IsValidIntegerIndex ( O, index ), https://tc39.es/ecma262/#sec-isvalidintegerindex
inline bool is_valid_integer_index(TypedArrayBase const& typed_array, CanonicalIndex property_index)
{
    // 1. If IsDetachedBuffer(O.[[ViewedArrayBuffer]]) is true, return false.
    if (typed_array.viewed_array_buffer()->is_detached())
        return false;

    // 2. If IsIntegralNumber(index) is false, return false.
    // 3. If index is -0𝔽, return false.
    if (!property_index.is_index())
        return false;

    // OPTIMIZATION: For TypedArrays with non-resizable ArrayBuffers, we can avoid most of the work performed by
    //               IsValidIntegerIndex. We just need to check whether the array itself is out-of-bounds and if
    //               the provided index is within the array bounds.
    if (auto const& array_length = typed_array.array_length(); !array_length.is_auto()) {
        auto byte_length = array_buffer_byte_length(*typed_array.viewed_array_buffer(), ArrayBuffer::Unordered);
        auto byte_offset_end = typed_array.byte_offset() + array_length.length() * typed_array.element_size();

        return typed_array.byte_offset() <= byte_length
            && byte_offset_end <= byte_length
            && property_index.as_index() < array_length.length();
    }

    return is_valid_integer_index_slow_case(typed_array, property_index);
}

// 10.4.5.17 TypedArrayGetElement ( O, index ), https://tc39.es/ecma262/#sec-typedarraygetelement
template<typename T>
inline Value typed_array_get_element(TypedArrayBase const& typed_array, CanonicalIndex property_index)
{
    // 1. If IsValidIntegerIndex(O, index) is false, return undefined.
    if (!is_valid_integer_index(typed_array, property_index))
        return js_undefined();

    // 2. Let offset be O.[[ByteOffset]].
    auto offset = typed_array.byte_offset();

    // 3. Let elementSize be TypedArrayElementSize(O).
    // 4. Let byteIndexInBuffer be (ℝ(index) × elementSize) + offset.
    Checked<size_t> byte_index_in_buffer = property_index.as_index();
    byte_index_in_buffer *= typed_array.element_size();
    byte_index_in_buffer += offset;
    // FIXME: Not exactly sure what we should do when overflow occurs.
    //        Just return as if it's an invalid index for now.
    if (byte_index_in_buffer.has_overflow()) {
        dbgln("typed_array_get_element(): byte_index_in_buffer overflowed, returning as if it's an invalid index.");
        return js_undefined();
    }

    // 5. Let elementType be TypedArrayElementType(O).
    // 6. Return GetValueFromBuffer(O.[[ViewedArrayBuffer]], byteIndexInBuffer, elementType, true, unordered).
    return typed_array.viewed_array_buffer()->template get_value<T>(byte_index_in_buffer.value(), true, ArrayBuffer::Order::Unordered);
}

// 10.4.5.18 TypedArraySetElement ( O, index, value ), https://tc39.es/ecma262/#sec-typedarraysetelement
// NOTE: In error cases, the function will return as if it succeeded.
template<typename T>
inline ThrowCompletionOr<void> typed_array_set_element(TypedArrayBase& typed_array, CanonicalIndex property_index, Value value)
{
    VERIFY(!value.is_empty());
    auto& vm = typed_array.vm();

    Value num_value;

    // 1. If O.[[ContentType]] is BigInt, let numValue be ? ToBigInt(value).
    if (typed_array.content_type() == TypedArrayBase::ContentType::BigInt)
        num_value = TRY(value.to_bigint(vm));
    // 2. Otherwise, let numValue be ? ToNumber(value).
    else
        num_value = TRY(value.to_number(vm));

    // 3. If IsValidIntegerIndex(O, index) is true, then
    // NOTE: Inverted for flattened logic.
    if (!is_valid_integer_index(typed_array, property_index))
        return {};

    // a. Let offset be O.[[ByteOffset]].
    auto offset = typed_array.byte_offset();

    // b. Let elementSize be TypedArrayElementSize(O).
    // c. Let byteIndexInBuffer be (ℝ(index) × elementSize) + offset.
    Checked<size_t> byte_index_in_buffer = property_index.as_index();
    byte_index_in_buffer *= typed_array.element_size();
    byte_index_in_buffer += offset;
    // FIXME: Not exactly sure what we should do when overflow occurs.
    //        Just return as if it succeeded for now.
    if (byte_index_in_buffer.has_overflow()) {
        dbgln("typed_array_set_element(): byte_index_in_buffer overflowed, returning as if succeeded.");
        return {};
    }

    // d. Let elementType be TypedArrayElementType(O).
    // e. Perform SetValueInBuffer(O.[[ViewedArrayBuffer]], byteIndexInBuffer, elementType, numValue, true, unordered).
    typed_array.viewed_array_buffer()->template set_value<T>(byte_index_in_buffer.value(), num_value, true, ArrayBuffer::Order::Unordered);

    // 4. Return unused.
    return {};
}

template<typename T>
class TypedArray : public TypedArrayBase {
    JS_OBJECT(TypedArray, TypedArrayBase);

    using UnderlyingBufferDataType = Conditional<IsSame<ClampedU8, T>, u8, T>;

public:
    // 10.4.5.1 [[PreventExtensions]] ( ), https://tc39.es/ecma262/#sec-typedarray-preventextensions
    virtual ThrowCompletionOr<bool> internal_prevent_extensions() override
    {
        // 1. NOTE: The extensibility-related invariants specified in 6.1.7.3 do not allow this method to return true
        //    when O can gain (or lose and then regain) properties, which might occur for properties with integer index
        //    names when its underlying buffer is resized.

        // 2. If IsTypedArrayFixedLength(O) is false, return false.
        if (!is_typed_array_fixed_length(*this))
            return false;

        // 3. Return OrdinaryPreventExtensions(O).
        return Object::internal_prevent_extensions();
    }

    // 10.4.5.2 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-getownproperty-p
    virtual ThrowCompletionOr<Optional<PropertyDescriptor>> internal_get_own_property(PropertyKey const& property_key) const override
    {
        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if (property_key.is_string() || property_key.is_number()) {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            auto numeric_index = canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, then
            if (!numeric_index.is_undefined()) {
                // i. Let value be TypedArrayGetElement(O, numericIndex).
                auto value = typed_array_get_element<T>(*this, numeric_index);

                // ii. If value is undefined, return undefined.
                if (value.is_undefined())
                    return Optional<PropertyDescriptor> {};

                // iii. Return the PropertyDescriptor { [[Value]]: value, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true }.
                return PropertyDescriptor {
                    .value = value,
                    .writable = true,
                    .enumerable = true,
                    .configurable = true,
                };
            }
        }

        // 2. Return OrdinaryGetOwnProperty(O, P).
        return Object::internal_get_own_property(property_key);
    }

    // 10.4.5.3 [[HasProperty]] ( P ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-hasproperty-p
    virtual ThrowCompletionOr<bool> internal_has_property(PropertyKey const& property_key) const override
    {
        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if (property_key.is_string() || property_key.is_number()) {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            auto numeric_index = canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, return IsValidIntegerIndex(O, numericIndex).
            if (!numeric_index.is_undefined())
                return is_valid_integer_index(*this, numeric_index);
        }

        // 2. Return ? OrdinaryHasProperty(O, P).
        return Object::internal_has_property(property_key);
    }

    // 10.4.5.4 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-defineownproperty-p-desc
    virtual ThrowCompletionOr<bool> internal_define_own_property(PropertyKey const& property_key, PropertyDescriptor const& property_descriptor, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr) override
    {
        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if (property_key.is_string() || property_key.is_number()) {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            auto numeric_index = canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, then
            if (!numeric_index.is_undefined()) {
                // i. If IsValidIntegerIndex(O, numericIndex) is false, return false.
                if (!is_valid_integer_index(*this, numeric_index))
                    return false;

                // ii. If Desc has a [[Configurable]] field and if Desc.[[Configurable]] is false, return false.
                if (property_descriptor.configurable.has_value() && !*property_descriptor.configurable)
                    return false;

                // iii. If Desc has an [[Enumerable]] field and if Desc.[[Enumerable]] is false, return false.
                if (property_descriptor.enumerable.has_value() && !*property_descriptor.enumerable)
                    return false;

                // iv. If IsAccessorDescriptor(Desc) is true, return false.
                if (property_descriptor.is_accessor_descriptor())
                    return false;

                // v. If Desc has a [[Writable]] field and if Desc.[[Writable]] is false, return false.
                if (property_descriptor.writable.has_value() && !*property_descriptor.writable)
                    return false;

                // vi. If Desc has a [[Value]] field, perform ? TypedArraySetElement(O, numericIndex, Desc.[[Value]]).
                if (property_descriptor.value.has_value())
                    TRY(typed_array_set_element<T>(*this, numeric_index, *property_descriptor.value));

                // vii. Return true.
                return true;
            }
        }

        // 2. Return ! OrdinaryDefineOwnProperty(O, P, Desc).
        return Object::internal_define_own_property(property_key, property_descriptor, precomputed_get_own_property);
    }

    // 10.4.5.5 [[Get]] ( P, Receiver ), https://tc39.es/ecma262/#sec-typedarray-get
    virtual ThrowCompletionOr<Value> internal_get(PropertyKey const& property_key, Value receiver, CacheablePropertyMetadata* cacheable_metadata, PropertyLookupPhase phase) const override
    {
        VERIFY(!receiver.is_empty());

        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if (property_key.is_string() || property_key.is_number()) {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            auto numeric_index = canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, then
            if (!numeric_index.is_undefined()) {
                // i. Return TypedArrayGetElement(O, numericIndex).
                return typed_array_get_element<T>(*this, numeric_index);
            }
        }

        // 2. Return ? OrdinaryGet(O, P, Receiver).
        return Object::internal_get(property_key, receiver, cacheable_metadata, phase);
    }

    // 10.4.5.6 [[Set]] ( P, V, Receiver ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-set-p-v-receiver
    virtual ThrowCompletionOr<bool> internal_set(PropertyKey const& property_key, Value value, Value receiver, CacheablePropertyMetadata*) override
    {
        VERIFY(!value.is_empty());
        VERIFY(!receiver.is_empty());

        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if (property_key.is_string() || property_key.is_number()) {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            auto numeric_index = canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, then
            if (!numeric_index.is_undefined()) {
                // i. If SameValue(O, Receiver) is true, then
                if (same_value(this, receiver)) {
                    // 1. Perform ? TypedArraySetElement(O, numericIndex, V).
                    TRY(typed_array_set_element<T>(*this, numeric_index, value));

                    // 2. Return true.
                    return true;
                }

                // ii. If IsValidIntegerIndex(O, numericIndex) is false, return true.
                if (!is_valid_integer_index(*this, numeric_index))
                    return true;
            }
        }

        // 2. Return ? OrdinarySet(O, P, V, Receiver).
        return Object::internal_set(property_key, value, receiver);
    }

    // 10.4.5.7 [[Delete]] ( P ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-delete-p
    virtual ThrowCompletionOr<bool> internal_delete(PropertyKey const& property_key) override
    {
        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if (property_key.is_string() || property_key.is_number()) {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            auto numeric_index = canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, then
            if (!numeric_index.is_undefined()) {
                // i. If IsValidIntegerIndex(O, numericIndex) is false, return true; else return false.
                if (!is_valid_integer_index(*this, numeric_index))
                    return true;
                return false;
            }
        }

        // 2. Return ? OrdinaryDelete(O, P).
        return Object::internal_delete(property_key);
    }

    // 10.4.5.8 [[OwnPropertyKeys]] ( ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-ownpropertykeys
    virtual ThrowCompletionOr<GC::RootVector<Value>> internal_own_property_keys() const override
    {
        auto& vm = this->vm();

        // 1. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
        auto typed_array_record = make_typed_array_with_buffer_witness_record(*this, ArrayBuffer::Order::SeqCst);

        // 2. Let keys be a new empty List.
        auto keys = GC::RootVector<Value> { heap() };

        // 3. If IsTypedArrayOutOfBounds(taRecord) is false, then
        if (!is_typed_array_out_of_bounds(typed_array_record)) {
            // a. Let length be TypedArrayLength(taRecord).
            auto length = typed_array_length(typed_array_record);

            // b. For each integer i such that 0 ≤ i < length, in ascending order, do
            for (size_t i = 0; i < length; ++i) {
                // i. Append ! ToString(𝔽(i)) to keys.
                keys.append(PrimitiveString::create(vm, String::number(i)));
            }
        }

        // 4. For each own property key P of O such that P is a String and P is not an integer index, in ascending chronological order of property creation, do
        for (auto& it : shape().property_table()) {
            if (it.key.is_string()) {
                // a. Append P to keys.
                keys.append(it.key.to_value(vm));
            }
        }

        // 5. For each own property key P of O such that P is a Symbol, in ascending chronological order of property creation, do
        for (auto& it : shape().property_table()) {
            if (it.key.is_symbol()) {
                // a. Append P to keys.
                keys.append(it.key.to_value(vm));
            }
        }

        // 6. Return keys.
        return { move(keys) };
    }

    ReadonlySpan<UnderlyingBufferDataType> data() const
    {
        auto typed_array_record = make_typed_array_with_buffer_witness_record(*this, ArrayBuffer::Order::SeqCst);

        if (is_typed_array_out_of_bounds(typed_array_record)) {
            // FIXME: Propagate this as an error?
            return {};
        }

        auto length = typed_array_length(typed_array_record);
        return { reinterpret_cast<UnderlyingBufferDataType const*>(m_viewed_array_buffer->buffer().data() + m_byte_offset), length };
    }

    Span<UnderlyingBufferDataType> data()
    {
        auto typed_array_record = make_typed_array_with_buffer_witness_record(*this, ArrayBuffer::Order::SeqCst);

        if (is_typed_array_out_of_bounds(typed_array_record)) {
            // FIXME: Propagate this as an error?
            return {};
        }

        auto length = typed_array_length(typed_array_record);
        return { reinterpret_cast<UnderlyingBufferDataType*>(m_viewed_array_buffer->buffer().data() + m_byte_offset), length };
    }

    bool is_unclamped_integer_element_type() const override
    {
        constexpr bool is_unclamped_integer = IsSame<T, i8> || IsSame<T, u8> || IsSame<T, i16> || IsSame<T, u16> || IsSame<T, i32> || IsSame<T, u32>;
        return is_unclamped_integer;
    }

    bool is_bigint_element_type() const override
    {
        constexpr bool is_bigint = IsSame<T, i64> || IsSame<T, u64>;
        return is_bigint;
    }

    Value get_value_from_buffer(size_t byte_index, ArrayBuffer::Order order, bool is_little_endian = true) const override { return viewed_array_buffer()->template get_value<T>(byte_index, true, order, is_little_endian); }
    void set_value_in_buffer(size_t byte_index, Value value, ArrayBuffer::Order order, bool is_little_endian = true) override { return viewed_array_buffer()->template set_value<T>(byte_index, value, true, order, is_little_endian); }
    Value get_modify_set_value_in_buffer(size_t byte_index, Value value, ReadWriteModifyFunction operation, bool is_little_endian = true) override { return viewed_array_buffer()->template get_modify_set_value<T>(byte_index, value, move(operation), is_little_endian); }

protected:
    TypedArray(Object& prototype, IntrinsicConstructor intrinsic_constructor, u32 array_length, ArrayBuffer& array_buffer, Kind kind)
        : TypedArrayBase(prototype, intrinsic_constructor, kind, sizeof(UnderlyingBufferDataType))
    {
        VERIFY(!Checked<u32>::multiplication_would_overflow(array_length, sizeof(UnderlyingBufferDataType)));
        m_viewed_array_buffer = &array_buffer;
        if (array_length)
            VERIFY(!data().is_null());
        m_array_length = array_length;
        m_byte_length = m_viewed_array_buffer->byte_length();
    }
};

ThrowCompletionOr<TypedArrayBase*> typed_array_from(VM&, Value);
ThrowCompletionOr<TypedArrayBase*> typed_array_create(VM&, FunctionObject& constructor, GC::RootVector<Value> arguments);
ThrowCompletionOr<TypedArrayBase*> typed_array_create_same_type(VM&, TypedArrayBase const& exemplar, GC::RootVector<Value> arguments);
ThrowCompletionOr<TypedArrayWithBufferWitness> validate_typed_array(VM&, Object const&, ArrayBuffer::Order);
ThrowCompletionOr<double> compare_typed_array_elements(VM&, Value x, Value y, FunctionObject* comparefn);

#define JS_DECLARE_TYPED_ARRAY(ClassName, snake_name, PrototypeName, ConstructorName, Type)                  \
    class ClassName : public TypedArray<Type> {                                                              \
        JS_OBJECT(ClassName, TypedArray);                                                                    \
        GC_DECLARE_ALLOCATOR(ClassName);                                                                     \
                                                                                                             \
    public:                                                                                                  \
        virtual ~ClassName();                                                                                \
        static ThrowCompletionOr<GC::Ref<ClassName>> create(Realm&, u32 length, FunctionObject& new_target); \
        static ThrowCompletionOr<GC::Ref<ClassName>> create(Realm&, u32 length);                             \
        static GC::Ref<ClassName> create(Realm&, u32 length, ArrayBuffer& buffer);                           \
        virtual FlyString const& element_name() const override;                                              \
                                                                                                             \
    protected:                                                                                               \
        ClassName(Object& prototype, u32 length, ArrayBuffer& array_buffer);                                 \
    };                                                                                                       \
    class PrototypeName final : public Object {                                                              \
        JS_OBJECT(PrototypeName, Object);                                                                    \
        GC_DECLARE_ALLOCATOR(PrototypeName);                                                                 \
                                                                                                             \
    public:                                                                                                  \
        virtual void initialize(Realm&) override;                                                            \
        virtual ~PrototypeName() override;                                                                   \
                                                                                                             \
    private:                                                                                                 \
        PrototypeName(Object& prototype);                                                                    \
    };                                                                                                       \
    class ConstructorName final : public NativeFunction {                                                    \
        JS_OBJECT(ConstructorName, NativeFunction);                                                          \
        GC_DECLARE_ALLOCATOR(ConstructorName);                                                               \
                                                                                                             \
    public:                                                                                                  \
        virtual void initialize(Realm&) override;                                                            \
        virtual ~ConstructorName() override;                                                                 \
                                                                                                             \
        virtual ThrowCompletionOr<Value> call() override;                                                    \
        virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject& new_target) override;           \
                                                                                                             \
    private:                                                                                                 \
        explicit ConstructorName(Realm&, Object& prototype);                                                 \
        virtual bool has_constructor() const override                                                        \
        {                                                                                                    \
            return true;                                                                                     \
        }                                                                                                    \
    };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    JS_DECLARE_TYPED_ARRAY(ClassName, snake_name, PrototypeName, ConstructorName, Type);
JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

}
