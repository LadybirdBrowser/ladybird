/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/ArrayBufferConstructor.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/TypedArrayConstructor.h>
#include <LibJS/Runtime/Uint8Array.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

ThrowCompletionOr<TypedArrayBase*> typed_array_from(VM& vm, Value typed_array_value)
{
    auto this_object = TRY(typed_array_value.to_object(vm));
    if (!this_object->is_typed_array())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "TypedArray");

    return static_cast<TypedArrayBase*>(this_object.ptr());
}

// 22.2.5.1.3 InitializeTypedArrayFromArrayBuffer, https://tc39.es/ecma262/#sec-initializetypedarrayfromarraybuffer
static ThrowCompletionOr<void> initialize_typed_array_from_array_buffer(VM& vm, TypedArrayBase& typed_array, ArrayBuffer& array_buffer, Value byte_offset, Value length)
{
    // 1. Let elementSize be TypedArrayElementSize(O).
    auto element_size = typed_array.element_size();

    // 2. Let offset be ? ToIndex(byteOffset).
    auto offset = TRY(byte_offset.to_index(vm));

    // 3. If offset modulo elementSize ≠ 0, throw a RangeError exception.
    if (offset % element_size != 0)
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayInvalidByteOffset, typed_array.class_name(), element_size, offset);

    // 4. Let bufferIsFixedLength be IsFixedLengthArrayBuffer(buffer).
    auto buffer_is_fixed_length = array_buffer.is_fixed_length();

    size_t new_length { 0 };
    // 5. If length is not undefined, then
    if (!length.is_undefined()) {
        // a. Let newLength be ? ToIndex(length).
        new_length = TRY(length.to_index(vm));
    }

    // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
    if (array_buffer.is_detached())
        return vm.throw_completion<TypeError>(ErrorType::DetachedArrayBuffer);

    // 7. Let bufferByteLength be ArrayBufferByteLength(buffer, seq-cst).
    auto buffer_byte_length = array_buffer_byte_length(array_buffer, ArrayBuffer::Order::SeqCst);

    // 8. If length is undefined and bufferIsFixedLength is false, then
    if (length.is_undefined() && !buffer_is_fixed_length) {
        // a. If offset > bufferByteLength, throw a RangeError exception.
        if (offset > buffer_byte_length)
            return vm.throw_completion<RangeError>(ErrorType::TypedArrayOutOfRangeByteOffset, offset, buffer_byte_length);

        // b. Set O.[[ByteLength]] to auto.
        typed_array.set_byte_length(ByteLength::auto_());

        // c. Set O.[[ArrayLength]] to auto.
        typed_array.set_array_length(ByteLength::auto_());
    }
    // 9. Else,
    else {
        Checked<u32> new_byte_length;

        // a. If length is undefined, then
        if (length.is_undefined()) {
            // i. If bufferByteLength modulo elementSize ≠ 0, throw a RangeError exception.
            if (modulo(buffer_byte_length, element_size) != 0)
                return vm.throw_completion<RangeError>(ErrorType::TypedArrayInvalidBufferLength, typed_array.class_name(), element_size, buffer_byte_length);

            // ii. Let newByteLength be bufferByteLength - offset.
            new_byte_length = buffer_byte_length;
            new_byte_length -= offset;

            // iii. If newByteLength < 0, throw a RangeError exception.
            if (new_byte_length.has_overflow())
                return vm.throw_completion<RangeError>(ErrorType::TypedArrayOutOfRangeByteOffset, offset, buffer_byte_length);
        }
        // b. Else,
        else {
            // i. Let newByteLength be newLength × elementSize.
            new_byte_length = new_length;
            new_byte_length *= element_size;

            // ii. If offset + newByteLength > bufferByteLength, throw a RangeError exception.
            Checked<u32> new_byte_end = offset;
            new_byte_end += new_byte_length;

            if (new_byte_end.has_overflow())
                return vm.throw_completion<RangeError>(ErrorType::InvalidLength, "typed array");
            if (new_byte_end.value() > buffer_byte_length)
                return vm.throw_completion<RangeError>(ErrorType::TypedArrayOutOfRangeByteOffsetOrLength, offset, new_byte_end.value(), buffer_byte_length);
        }

        // c. Set O.[[ByteLength]] to newByteLength.
        typed_array.set_byte_length(new_byte_length.value());

        // d. Set O.[[ArrayLength]] to newByteLength / elementSize.
        typed_array.set_array_length(new_byte_length.value() / element_size);
    }

    // 10. Set O.[[ViewedArrayBuffer]] to buffer.
    typed_array.set_viewed_array_buffer(&array_buffer);

    // 11. Set O.[[ByteOffset]] to offset.
    typed_array.set_byte_offset(offset);

    // 12. Return unused.
    return {};
}

// 23.2.5.1.2 InitializeTypedArrayFromTypedArray ( O, srcArray ), https://tc39.es/ecma262/#sec-initializetypedarrayfromtypedarray
template<typename T>
static ThrowCompletionOr<void> initialize_typed_array_from_typed_array(VM& vm, TypedArray<T>& typed_array, TypedArrayBase& source_array)
{
    auto& realm = *vm.current_realm();

    // 1. Let srcData be srcArray.[[ViewedArrayBuffer]].
    auto* source_data = source_array.viewed_array_buffer();
    VERIFY(source_data);

    // 2. Let elementType be TypedArrayElementType(O).
    auto const& element_type = typed_array.element_name();

    // 3. Let elementSize be TypedArrayElementSize(O).
    auto element_size = typed_array.element_size();

    // 4. Let srcType be TypedArrayElementType(srcArray).
    auto const& source_type = source_array.element_name();

    // 5. Let srcElementSize be TypedArrayElementSize(srcArray).
    auto source_element_size = source_array.element_size();

    // 6. Let srcByteOffset be srcArray.[[ByteOffset]].
    auto source_byte_offset = source_array.byte_offset();

    // 7. Let srcRecord be MakeTypedArrayWithBufferWitnessRecord(srcArray, seq-cst).
    auto source_record = make_typed_array_with_buffer_witness_record(source_array, ArrayBuffer::Order::SeqCst);

    // 8. If IsTypedArrayOutOfBounds(srcRecord) is true, throw a TypeError exception.
    if (is_typed_array_out_of_bounds(source_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 9. Let elementLength be TypedArrayLength(srcRecord).
    auto element_length = typed_array_length(source_record);

    // 10. Let byteLength be elementSize × elementLength.
    Checked<size_t> byte_length = element_size;
    byte_length *= element_length;
    if (byte_length.has_overflow())
        return vm.template throw_completion<RangeError>(ErrorType::InvalidLength, "typed array");

    ArrayBuffer* data = nullptr;

    // 11. If elementType is srcType, then
    if (element_type == source_type) {
        // a. Let data be ? CloneArrayBuffer(srcData, srcByteOffset, byteLength).
        data = TRY(clone_array_buffer(vm, *source_data, source_byte_offset, byte_length.value()));
    }
    // 12. Else,
    else {
        // a. Let data be ? AllocateArrayBuffer(%ArrayBuffer%, byteLength).
        data = TRY(allocate_array_buffer(vm, realm.intrinsics().array_buffer_constructor(), byte_length.value()));

        // b. If srcArray.[[ContentType]] is not O.[[ContentType]], throw a TypeError exception.
        if (source_array.content_type() != typed_array.content_type())
            return vm.template throw_completion<TypeError>(ErrorType::TypedArrayContentTypeMismatch, typed_array.class_name(), source_array.class_name());

        // c. Let srcByteIndex be srcByteOffset.
        u64 source_byte_index = source_byte_offset;

        // d. Let targetByteIndex be 0.
        u64 target_byte_index = 0;

        // e. Let count be elementLength.
        // f. Repeat, while count > 0,
        for (u32 i = 0; i < element_length; ++i) {
            // i. Let value be GetValueFromBuffer(srcData, srcByteIndex, srcType, true, unordered).
            auto value = source_array.get_value_from_buffer(source_byte_index, ArrayBuffer::Order::Unordered);

            // ii. Perform SetValueInBuffer(data, targetByteIndex, elementType, value, true, unordered).
            data->template set_value<T>(target_byte_index, value, true, ArrayBuffer::Order::Unordered);

            // iii. Set srcByteIndex to srcByteIndex + srcElementSize.
            source_byte_index += source_element_size;

            // iv. Set targetByteIndex to targetByteIndex + elementSize.
            target_byte_index += element_size;

            // v. Set count to count - 1.
        }
    }

    // 13. Set O.[[ViewedArrayBuffer]] to data.
    typed_array.set_viewed_array_buffer(data);

    // 14. Set O.[[ByteLength]] to byteLength.
    typed_array.set_byte_length(byte_length.value());

    // 15. Set O.[[ByteOffset]] to 0.
    typed_array.set_byte_offset(0);

    // 16. Set O.[[ArrayLength]] to elementLength.
    typed_array.set_array_length(element_length);

    // 17. Return unused.
    return {};
}

// 23.2.5.1.6 AllocateTypedArrayBuffer ( O, length ), https://tc39.es/ecma262/#sec-allocatetypedarraybuffer
template<typename T>
static ThrowCompletionOr<void> allocate_typed_array_buffer(VM& vm, TypedArray<T>& typed_array, size_t length)
{
    auto& realm = *vm.current_realm();

    // Enforce 2GB "Excessive Length" limit
    if (length > NumericLimits<i32>::max() / sizeof(T))
        return vm.template throw_completion<RangeError>(ErrorType::InvalidLength, "typed array");

    // 1. Assert: O.[[ViewedArrayBuffer]] is undefined.

    // 2. Let elementSize be TypedArrayElementSize(O).
    auto element_size = typed_array.element_size();
    if (Checked<size_t>::multiplication_would_overflow(element_size, length))
        return vm.template throw_completion<RangeError>(ErrorType::InvalidLength, "typed array");

    // 3. Let byteLength be elementSize × length.
    auto byte_length = element_size * length;

    // 4. Let data be ? AllocateArrayBuffer(%ArrayBuffer%, byteLength).
    auto* data = TRY(allocate_array_buffer(vm, realm.intrinsics().array_buffer_constructor(), byte_length));

    // 5. Set O.[[ViewedArrayBuffer]] to data.
    typed_array.set_viewed_array_buffer(data);

    // 6. Set O.[[ByteLength]] to byteLength.
    typed_array.set_byte_length(byte_length);

    // 7. Set O.[[ByteOffset]] to 0.
    typed_array.set_byte_offset(0);

    // 8. Set O.[[ArrayLength]] to length.
    typed_array.set_array_length(length);

    // 9. Return unused.
    return {};
}

// 23.2.5.1.5 InitializeTypedArrayFromArrayLike, https://tc39.es/ecma262/#sec-initializetypedarrayfromarraylike
template<typename T>
static ThrowCompletionOr<void> initialize_typed_array_from_array_like(VM& vm, TypedArray<T>& typed_array, Object const& array_like)
{
    // 1. Let len be ? LengthOfArrayLike(arrayLike).
    auto length = TRY(length_of_array_like(vm, array_like));

    // 2. Perform ? AllocateTypedArrayBuffer(O, len).
    TRY(allocate_typed_array_buffer(vm, typed_array, length));

    // 3. Let k be 0.
    // 4. Repeat, while k < len,
    for (size_t k = 0; k < length; k++) {
        // a. Let Pk be ! ToString(𝔽(k)).
        // b. Let kValue be ? Get(arrayLike, Pk).
        auto k_value = TRY(array_like.get(k));

        // c. Perform ? Set(O, Pk, kValue, true).
        TRY(typed_array.set(k, k_value, Object::ShouldThrowExceptions::Yes));

        // d. Set k to k + 1.
    }

    // 5. Return unused.
    return {};
}

// 23.2.5.1.4 InitializeTypedArrayFromList, https://tc39.es/ecma262/#sec-initializetypedarrayfromlist
template<typename T>
static ThrowCompletionOr<void> initialize_typed_array_from_list(VM& vm, TypedArray<T>& typed_array, GC::RootVector<Value> const& list)
{
    // 1. Let len be the number of elements in values.
    auto length = list.size();

    // 2. Perform ? AllocateTypedArrayBuffer(O, len).
    TRY(allocate_typed_array_buffer(vm, typed_array, length));

    // 3. Let k be 0.
    // 4. Repeat, while k < len,
    for (size_t k = 0; k < length; k++) {
        // a. Let Pk be ! ToString(𝔽(k)).
        // b. Let kValue be the first element of values and remove that element from values.
        auto value = list[k];

        // c. Perform ? Set(O, Pk, kValue, true).
        TRY(typed_array.set(k, value, Object::ShouldThrowExceptions::Yes));

        // d. Set k to k + 1.
    }

    // 5. Assert: values is now an empty List.
    // 6. Return unused.
    return {};
}

// 23.2.4.2 TypedArrayCreate ( constructor, argumentList ), https://tc39.es/ecma262/#typedarray-create
ThrowCompletionOr<TypedArrayBase*> typed_array_create(VM& vm, FunctionObject& constructor, GC::RootVector<Value> arguments)
{
    Optional<double> first_argument;
    if (arguments.size() == 1 && arguments[0].is_number())
        first_argument = arguments[0].as_double();

    // 1. Let newTypedArray be ? Construct(constructor, argumentList).
    auto new_typed_array = TRY(construct(vm, constructor, arguments.span()));

    // 2. Let taRecord be ? ValidateTypedArray(newTypedArray, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *new_typed_array, ArrayBuffer::Order::SeqCst));

    // 3. If the number of elements in argumentList is 1 and argumentList[0] is a Number, then
    if (first_argument.has_value()) {
        // a. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
        if (is_typed_array_out_of_bounds(typed_array_record))
            return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

        // b. Let length be TypedArrayLength(taRecord).
        auto length = typed_array_length(typed_array_record);

        // c. If length < ℝ(argumentList[0]), throw a TypeError exception.
        if (length < *first_argument)
            return vm.throw_completion<TypeError>(ErrorType::InvalidLength, "typed array");
    }

    // 4. Return newTypedArray.
    return static_cast<TypedArrayBase*>(new_typed_array.ptr());
}

// 23.2.4.3 TypedArrayCreateSameType ( exemplar, argumentList ), https://tc39.es/ecma262/#sec-typedarray-create-same-type
ThrowCompletionOr<TypedArrayBase*> typed_array_create_same_type(VM& vm, TypedArrayBase const& exemplar, GC::RootVector<Value> arguments)
{
    auto& realm = *vm.current_realm();

    // 1. Let constructor be the intrinsic object associated with the constructor name exemplar.[[TypedArrayName]] in Table 68.
    auto constructor = (realm.intrinsics().*exemplar.intrinsic_constructor())();

    // 2. Let result be ? TypedArrayCreate(constructor, argumentList).
    auto* result = TRY(typed_array_create(vm, constructor, move(arguments)));

    // 3. Assert: result has [[TypedArrayName]] and [[ContentType]] internal slots.
    // 4. Assert: result.[[ContentType]] is exemplar.[[ContentType]].
    // 5. Return result.
    return result;
}

// 23.2.4.4 ValidateTypedArray ( O ), https://tc39.es/ecma262/#sec-validatetypedarray
ThrowCompletionOr<TypedArrayWithBufferWitness> validate_typed_array(VM& vm, Object const& object, ArrayBuffer::Order order)
{
    // 1. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
    if (!object.is_typed_array())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "TypedArray");

    // 2. Assert: O has a [[ViewedArrayBuffer]] internal slot.
    auto const& typed_array = static_cast<TypedArrayBase const&>(object);

    // 3. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, order).
    auto typed_array_record = make_typed_array_with_buffer_witness_record(typed_array, order);

    // 4. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
    if (is_typed_array_out_of_bounds(typed_array_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 5. Return taRecord.
    return typed_array_record;
}

// 23.2.4.7 CompareTypedArrayElements ( x, y, comparefn ), https://tc39.es/ecma262/#sec-typedarray-create-same-type
ThrowCompletionOr<double> compare_typed_array_elements(VM& vm, Value x, Value y, FunctionObject* comparefn)
{
    // 1. Assert: x is a Number and y is a Number, or x is a BigInt and y is a BigInt.
    VERIFY(((x.is_number() && y.is_number()) || (x.is_bigint() && y.is_bigint())));

    // 2. If comparefn is not undefined, then
    if (comparefn != nullptr) {
        // a. Let v be ? ToNumber(? Call(comparefn, undefined, « x, y »)).
        auto value = TRY(call(vm, comparefn, js_undefined(), x, y));
        auto value_number = TRY(value.to_number(vm));

        // b. If v is NaN, return +0𝔽.
        if (value_number.is_nan())
            return 0;

        // c. Return v.
        return value_number.as_double();
    }

    // 3. If x and y are both NaN, return +0𝔽.
    if (x.is_nan() && y.is_nan())
        return 0;

    // 4. If x is NaN, return 1𝔽.
    if (x.is_nan())
        return 1;

    // 5. If y is NaN, return -1𝔽.
    if (y.is_nan())
        return -1;

    // 6. If x < y, return -1𝔽.
    if (x.is_bigint()
            ? (x.as_bigint().big_integer() < y.as_bigint().big_integer())
            : (x.as_double() < y.as_double()))
        return -1;

    // 7. If x > y, return 1𝔽.
    if (x.is_bigint()
            ? (x.as_bigint().big_integer() > y.as_bigint().big_integer())
            : (x.as_double() > y.as_double()))
        return 1;

    // 8. If x is -0𝔽 and y is +0𝔽, return -1𝔽.
    if (x.is_negative_zero() && y.is_positive_zero())
        return -1;

    // 9. If x is +0𝔽 and y is -0𝔽, return 1𝔽.
    if (x.is_positive_zero() && y.is_negative_zero())
        return 1;

    // 10. Return +0𝔽.
    return 0;
}

void TypedArrayBase::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_viewed_array_buffer);
}

#define JS_DEFINE_TYPED_ARRAY(ClassName, snake_name, PrototypeName, ConstructorName, Type)                                  \
    GC_DEFINE_ALLOCATOR(ClassName);                                                                                         \
    GC_DEFINE_ALLOCATOR(PrototypeName);                                                                                     \
    GC_DEFINE_ALLOCATOR(ConstructorName);                                                                                   \
    ThrowCompletionOr<GC::Ref<ClassName>> ClassName::create(Realm& realm, u32 length, FunctionObject& new_target)           \
    {                                                                                                                       \
        auto* prototype = TRY(get_prototype_from_constructor(realm.vm(), new_target, &Intrinsics::snake_name##_prototype)); \
        auto array_buffer = TRY(ArrayBuffer::create(realm, length * sizeof(UnderlyingBufferDataType)));                     \
        return realm.create<ClassName>(*prototype, length, *array_buffer);                                                  \
    }                                                                                                                       \
                                                                                                                            \
    ThrowCompletionOr<GC::Ref<ClassName>> ClassName::create(Realm& realm, u32 length)                                       \
    {                                                                                                                       \
        auto array_buffer = TRY(ArrayBuffer::create(realm, length * sizeof(UnderlyingBufferDataType)));                     \
        return create(realm, length, *array_buffer);                                                                        \
    }                                                                                                                       \
                                                                                                                            \
    GC::Ref<ClassName> ClassName::create(Realm& realm, u32 length, ArrayBuffer& array_buffer)                               \
    {                                                                                                                       \
        return realm.create<ClassName>(realm.intrinsics().snake_name##_prototype(), length, array_buffer);                  \
    }                                                                                                                       \
                                                                                                                            \
    ClassName::ClassName(Object& prototype, u32 length, ArrayBuffer& array_buffer)                                          \
        : TypedArray(prototype,                                                                                             \
              bit_cast<TypedArrayBase::IntrinsicConstructor>(&Intrinsics::snake_name##_constructor),                        \
              length, array_buffer, Kind::ClassName)                                                                        \
    {                                                                                                                       \
        if constexpr (#ClassName##sv.is_one_of("BigInt64Array", "BigUint64Array"))                                          \
            m_content_type = ContentType::BigInt;                                                                           \
        else                                                                                                                \
            m_content_type = ContentType::Number;                                                                           \
    }                                                                                                                       \
                                                                                                                            \
    ClassName::~ClassName()                                                                                                 \
    {                                                                                                                       \
    }                                                                                                                       \
                                                                                                                            \
    FlyString const& ClassName::element_name() const                                                                        \
    {                                                                                                                       \
        return vm().names.ClassName.as_string();                                                                            \
    }                                                                                                                       \
                                                                                                                            \
    PrototypeName::PrototypeName(Object& prototype)                                                                         \
        : Object(ConstructWithPrototypeTag::Tag, prototype, MayInterfereWithIndexedPropertyAccess::Yes)                     \
    {                                                                                                                       \
    }                                                                                                                       \
                                                                                                                            \
    PrototypeName::~PrototypeName()                                                                                         \
    {                                                                                                                       \
    }                                                                                                                       \
                                                                                                                            \
    void PrototypeName::initialize(Realm& realm)                                                                            \
    {                                                                                                                       \
        auto& vm = this->vm();                                                                                              \
        Base::initialize(realm);                                                                                            \
        define_direct_property(vm.names.BYTES_PER_ELEMENT, Value((i32)sizeof(Type)), 0);                                    \
                                                                                                                            \
        if constexpr (IsSame<PrototypeName, Uint8ArrayPrototype>)                                                           \
            Uint8ArrayPrototypeHelpers::initialize(realm, *this);                                                           \
    }                                                                                                                       \
                                                                                                                            \
    ConstructorName::ConstructorName(Realm& realm, Object& prototype)                                                       \
        : NativeFunction(realm.vm().names.ClassName.as_string(), prototype)                                                 \
    {                                                                                                                       \
    }                                                                                                                       \
                                                                                                                            \
    ConstructorName::~ConstructorName()                                                                                     \
    {                                                                                                                       \
    }                                                                                                                       \
                                                                                                                            \
    void ConstructorName::initialize(Realm& realm)                                                                          \
    {                                                                                                                       \
        auto& vm = this->vm();                                                                                              \
        Base::initialize(realm);                                                                                            \
                                                                                                                            \
        /* 23.2.6.2 TypedArray.prototype, https://tc39.es/ecma262/#sec-typedarray.prototype */                              \
        define_direct_property(vm.names.prototype, realm.intrinsics().snake_name##_prototype(), 0);                         \
                                                                                                                            \
        /* 23.2.6.1 TypedArray.BYTES_PER_ELEMENT, https://tc39.es/ecma262/#sec-typedarray.bytes_per_element */              \
        define_direct_property(vm.names.BYTES_PER_ELEMENT, Value((i32)sizeof(Type)), 0);                                    \
                                                                                                                            \
        define_direct_property(vm.names.length, Value(3), Attribute::Configurable);                                         \
                                                                                                                            \
        if constexpr (IsSame<ConstructorName, Uint8ArrayConstructor>)                                                       \
            Uint8ArrayConstructorHelpers::initialize(realm, *this);                                                         \
    }                                                                                                                       \
                                                                                                                            \
    /* 23.2.5.1 TypedArray ( ...args ), https://tc39.es/ecma262/#sec-typedarray */                                          \
    ThrowCompletionOr<Value> ConstructorName::call()                                                                        \
    {                                                                                                                       \
        auto& vm = this->vm();                                                                                              \
        return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.ClassName);                        \
    }                                                                                                                       \
                                                                                                                            \
    /* 23.2.5.1 TypedArray ( ...args ), https://tc39.es/ecma262/#sec-typedarray */                                          \
    ThrowCompletionOr<GC::Ref<Object>> ConstructorName::construct(FunctionObject& new_target)                               \
    {                                                                                                                       \
        auto& vm = this->vm();                                                                                              \
        auto& realm = *vm.current_realm();                                                                                  \
                                                                                                                            \
        if (vm.argument_count() == 0)                                                                                       \
            return TRY(ClassName::create(realm, 0, new_target));                                                            \
                                                                                                                            \
        auto first_argument = vm.argument(0);                                                                               \
        if (first_argument.is_object()) {                                                                                   \
            auto typed_array = TRY(ClassName::create(realm, 0, new_target));                                                \
            if (first_argument.as_object().is_typed_array()) {                                                              \
                auto& arg_typed_array = static_cast<TypedArrayBase&>(first_argument.as_object());                           \
                TRY(initialize_typed_array_from_typed_array(vm, *typed_array, arg_typed_array));                            \
            } else if (is<ArrayBuffer>(first_argument.as_object())) {                                                       \
                auto& array_buffer = static_cast<ArrayBuffer&>(first_argument.as_object());                                 \
                TRY(initialize_typed_array_from_array_buffer(vm, *typed_array, array_buffer,                                \
                    vm.argument(1), vm.argument(2)));                                                                       \
            } else {                                                                                                        \
                auto iterator = TRY(first_argument.get_method(vm, vm.well_known_symbol_iterator()));                        \
                if (iterator) {                                                                                             \
                    auto values = TRY(iterator_to_list(vm, TRY(get_iterator_from_method(vm, first_argument, *iterator))));  \
                    TRY(initialize_typed_array_from_list(vm, *typed_array, values));                                        \
                } else {                                                                                                    \
                    TRY(initialize_typed_array_from_array_like(vm, *typed_array, first_argument.as_object()));              \
                }                                                                                                           \
            }                                                                                                               \
            return typed_array;                                                                                             \
        }                                                                                                                   \
                                                                                                                            \
        auto array_length_or_error = first_argument.to_index(vm);                                                           \
        if (array_length_or_error.is_error()) {                                                                             \
            auto error = array_length_or_error.release_error();                                                             \
            if (error.value()->is_object() && is<RangeError>(error.value()->as_object())) {                                 \
                /* Re-throw more specific RangeError */                                                                     \
                return vm.throw_completion<RangeError>(ErrorType::InvalidLength, "typed array");                            \
            }                                                                                                               \
            return error;                                                                                                   \
        }                                                                                                                   \
        auto array_length = array_length_or_error.release_value();                                                          \
        if (array_length > NumericLimits<i32>::max() / sizeof(Type))                                                        \
            return vm.throw_completion<RangeError>(ErrorType::InvalidLength, "typed array");                                \
        /* FIXME: What is the best/correct behavior here? */                                                                \
        if (Checked<u32>::multiplication_would_overflow(array_length, sizeof(Type)))                                        \
            return vm.throw_completion<RangeError>(ErrorType::InvalidLength, "typed array");                                \
        return TRY(ClassName::create(realm, array_length, new_target));                                                     \
    }

#undef __JS_ENUMERATE
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    JS_DEFINE_TYPED_ARRAY(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType);
JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

// 10.4.5.10 MakeTypedArrayWithBufferWitnessRecord ( obj, order ), https://tc39.es/ecma262/#sec-maketypedarraywithbufferwitnessrecord
TypedArrayWithBufferWitness make_typed_array_with_buffer_witness_record(TypedArrayBase const& typed_array, ArrayBuffer::Order order)
{
    // 1. Let buffer be obj.[[ViewedArrayBuffer]].
    auto* buffer = typed_array.viewed_array_buffer();

    ByteLength byte_length { 0 };

    // 2. If IsDetachedBuffer(buffer) is true, then
    if (buffer->is_detached()) {
        // a. Let byteLength be detached.
        byte_length = ByteLength::detached();
    }
    // 3. Else,
    else {
        // a. Let byteLength be ArrayBufferByteLength(buffer, order).
        byte_length = array_buffer_byte_length(*buffer, order);
    }

    // 4. Return the TypedArray With Buffer Witness Record { [[Object]]: obj, [[CachedBufferByteLength]]: byteLength }.
    return { .object = typed_array, .cached_buffer_byte_length = move(byte_length) };
}

// 10.4.5.12 TypedArrayByteLength ( taRecord ), https://tc39.es/ecma262/#sec-typedarraybytelength
u32 typed_array_byte_length(TypedArrayWithBufferWitness const& typed_array_record)
{
    // 1. If IsTypedArrayOutOfBounds(taRecord) is true, return 0.
    if (is_typed_array_out_of_bounds(typed_array_record))
        return 0;

    // 2. Let length be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 3. If length = 0, return 0.
    if (length == 0)
        return 0;

    // 4. Let O be taRecord.[[Object]].
    auto object = typed_array_record.object;

    // 5. If O.[[ByteLength]] is not auto, return O.[[ByteLength]].
    if (!object->byte_length().is_auto())
        return object->byte_length().length();

    // 6. Let elementSize be TypedArrayElementSize(O).
    auto element_size = object->element_size();

    // 7. Return length × elementSize.
    return length * element_size;
}

// 10.4.5.13 TypedArrayLength ( taRecord ), https://tc39.es/ecma262/#sec-typedarraylength
u32 typed_array_length(TypedArrayWithBufferWitness const& typed_array_record)
{
    // 1. Assert: IsTypedArrayOutOfBounds(taRecord) is false.
    VERIFY(!is_typed_array_out_of_bounds(typed_array_record));

    // 2. Let O be taRecord.[[Object]].
    auto object = typed_array_record.object;

    // 3. If O.[[ArrayLength]] is not auto, return O.[[ArrayLength]].
    if (!object->array_length().is_auto())
        return object->array_length().length();

    // 4. Assert: IsFixedLengthArrayBuffer(O.[[ViewedArrayBuffer]]) is false.
    VERIFY(!object->viewed_array_buffer()->is_fixed_length());

    // 5. Let byteOffset be O.[[ByteOffset]].
    auto byte_offset = object->byte_offset();

    // 6. Let elementSize be TypedArrayElementSize(O).
    auto element_size = object->element_size();

    // 7. Let byteLength be taRecord.[[CachedBufferByteLength]].
    auto const& byte_length = typed_array_record.cached_buffer_byte_length;

    // 8. Assert: byteLength is not detached.
    VERIFY(!byte_length.is_detached());

    // 9. Return floor((byteLength - byteOffset) / elementSize).
    return (byte_length.length() - byte_offset) / element_size;
}

// 10.4.5.14 IsTypedArrayOutOfBounds ( taRecord ), https://tc39.es/ecma262/#sec-istypedarrayoutofbounds
bool is_typed_array_out_of_bounds(TypedArrayWithBufferWitness const& typed_array_record)
{
    // 1. Let O be taRecord.[[Object]].
    auto object = typed_array_record.object;

    // 2. Let bufferByteLength be taRecord.[[CachedBufferByteLength]].
    auto const& buffer_byte_length = typed_array_record.cached_buffer_byte_length;

    // 3. Assert: IsDetachedBuffer(O.[[ViewedArrayBuffer]]) is true if and only if bufferByteLength is detached.
    VERIFY(object->viewed_array_buffer()->is_detached() == buffer_byte_length.is_detached());

    // 4. If bufferByteLength is detached, return true.
    if (buffer_byte_length.is_detached())
        return true;

    // 5. Let byteOffsetStart be O.[[ByteOffset]].
    auto byte_offset_start = object->byte_offset();
    u32 byte_offset_end = 0;

    // 6. If O.[[ArrayLength]] is auto, then
    if (object->array_length().is_auto()) {
        // a. Let byteOffsetEnd be bufferByteLength.
        byte_offset_end = buffer_byte_length.length();
    }
    // 7. Else,
    else {
        // a. Let elementSize be TypedArrayElementSize(O).
        auto element_size = object->element_size();

        // b. Let byteOffsetEnd be byteOffsetStart + O.[[ArrayLength]] × elementSize.
        byte_offset_end = byte_offset_start + object->array_length().length() * element_size;
    }

    // 8. If byteOffsetStart > bufferByteLength or byteOffsetEnd > bufferByteLength, return true.
    if ((byte_offset_start > buffer_byte_length.length()) || (byte_offset_end > buffer_byte_length.length()))
        return true;

    // 9. NOTE: 0-length TypedArrays are not considered out-of-bounds.
    // 10. Return false.
    return false;
}

// 10.4.5.15 IsTypedArrayFixedLength ( O ), https://tc39.es/ecma262/#sec-istypedarrayfixedlength
bool is_typed_array_fixed_length(TypedArrayBase const& typed_array)
{
    // 1. If O.[[ArrayLength]] is AUTO, return false.
    if (typed_array.array_length().is_auto())
        return false;

    // 2. Let buffer be O.[[ViewedArrayBuffer]].
    auto const* buffer = typed_array.viewed_array_buffer();

    // 3. If IsFixedLengthArrayBuffer(buffer) is false and IsSharedArrayBuffer(buffer) is false, return false.
    if (!buffer->is_fixed_length() && !buffer->is_shared_array_buffer())
        return false;

    // 4. Return true.
    return true;
}

// 10.4.5.16 IsValidIntegerIndex ( O, index ), https://tc39.es/ecma262/#sec-isvalidintegerindex
bool is_valid_integer_index_slow_case(TypedArrayBase const& typed_array, CanonicalIndex property_index)
{
    // 4. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, unordered).
    auto typed_array_record = make_typed_array_with_buffer_witness_record(typed_array, ArrayBuffer::Unordered);

    // 5. NOTE: Bounds checking is not a synchronizing operation when O's backing buffer is a growable SharedArrayBuffer.

    // 6. If IsTypedArrayOutOfBounds(taRecord) is true, return false.
    if (is_typed_array_out_of_bounds(typed_array_record))
        return false;

    // 7. Let length be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 8. If ℝ(index) < 0 or ℝ(index) ≥ length, return false.
    if (property_index.as_index() >= length)
        return false;

    // 9. Return true.
    return true;
}

}
