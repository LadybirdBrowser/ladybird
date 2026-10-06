/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/ArrayBufferABIConversions.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

ThrowCompletionOr<GC::Ref<ArrayBuffer>> ArrayBuffer::create(Realm& realm, size_t byte_length, DataBlock::Shared is_shared)
{
    return completion_from_abi<GC::Ref<ArrayBuffer>>(js_array_buffer_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), byte_length, is_shared == DataBlock::Shared::Yes));
}

GC::Ref<ArrayBuffer> ArrayBuffer::create(Realm& realm, ByteBuffer buffer, DataBlock::Shared is_shared)
{
    return array_buffer_from_abi(js_array_buffer_create_from_bytes(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), buffer.data(), buffer.size(), is_shared == DataBlock::Shared::Yes));
}

GC::Ref<ArrayBuffer> ArrayBuffer::create_taking_over_storage(Realm& realm, GC::PrimitiveStorageHandle handle, DataBlock::Shared is_shared)
{
    return array_buffer_from_abi(js_array_buffer_create_adopting_storage(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), primitive_storage_handle_to_abi(handle), is_shared == DataBlock::Shared::Yes));
}

static JSExternalPrimitiveStorage external_primitive_storage_to_abi(DataBlock::ExternalPrimitiveStorage const& storage)
{
    auto fixed_byte_length = storage.size.get_pointer<size_t>();
    return {
        .owner = storage.owner.ptr(),
        .handle = primitive_storage_handle_to_abi(storage.handle),
        .fixed_byte_length = fixed_byte_length ? *fixed_byte_length : 0,
        .has_fixed_byte_length = fixed_byte_length != nullptr,
    };
}

GC::Ref<ArrayBuffer> ArrayBuffer::create_over_external_storage(Realm& realm, DataBlock::ExternalPrimitiveStorage const& storage, DataBlock::Shared is_shared)
{
    auto storage_for_the_runtime = external_primitive_storage_to_abi(storage);
    return array_buffer_from_abi(js_array_buffer_create_with_external_storage(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), &storage_for_the_runtime, is_shared == DataBlock::Shared::Yes));
}

GC::Ptr<ArrayBuffer> ArrayBuffer::create_over_shared_memory(Realm& realm, int shared_memory_fd, size_t byte_length, u64 shared_object_id)
{
    if (shared_memory_fd < 0)
        return nullptr;
    return cell_from_abi<ArrayBuffer>(js_array_buffer_create_from_shared_memory(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), shared_memory_fd, byte_length, shared_object_id));
}

GC::Ref<ArrayBuffer> ArrayBuffer::create_detached(Realm& realm)
{
    auto& vm = realm.vm();
    auto buffer = MUST(create(realm, 0));
    MUST(detach_array_buffer(vm, buffer));
    return buffer;
}

void ArrayBuffer::set_external_storage(DataBlock::ExternalPrimitiveStorage const& storage, DataBlock::Shared is_shared)
{
    auto storage_for_the_runtime = external_primitive_storage_to_abi(storage);
    js_array_buffer_set_external_storage(vm_to_abi(vm()), array_buffer_to_abi(*this), &storage_for_the_runtime, is_shared == DataBlock::Shared::Yes);
}

size_t ArrayBuffer::byte_length() const
{
    return js_array_buffer_byte_length(array_buffer_to_abi(*this));
}

Bytes ArrayBuffer::bytes_from(size_t byte_index) const
{
    size_t byte_length = 0;
    auto* data = js_array_buffer_data(array_buffer_to_abi(*this), &byte_length);
    VERIFY(byte_index <= byte_length);
    if (!data)
        return {};
    return { data + byte_index, byte_length - byte_index };
}

void ArrayBuffer::copy_to(size_t offset, Bytes destination) const
{
    auto source = bytes_from(offset);
    VERIFY(destination.size() <= source.size());
    if (!destination.is_empty())
        __builtin_memcpy(destination.data(), source.data(), destination.size());
}

ErrorOr<ByteBuffer> ArrayBuffer::copy_to_byte_buffer(size_t offset, size_t count) const
{
    auto source = bytes_from(offset);
    VERIFY(count <= source.size());
    return ByteBuffer::copy(source.trim(count));
}

ErrorOr<ByteBuffer> ArrayBuffer::copy_to_byte_buffer() const
{
    return ByteBuffer::copy(bytes_from(0));
}

u64 ArrayBuffer::shared_object_id() const
{
    JSArrayBufferStorage storage;
    js_array_buffer_storage(array_buffer_to_abi(*this), &storage);
    return storage.shared_memory_object_id;
}

void ArrayBuffer::copy_data_to(ArrayBuffer& destination, size_t source_offset, size_t destination_offset, size_t count) const
{
    auto source_bytes = bytes_from(source_offset);
    auto destination_bytes = destination.bytes_from(destination_offset);
    VERIFY(count <= source_bytes.size());
    VERIFY(count <= destination_bytes.size());
    if (count > 0)
        __builtin_memmove(destination_bytes.data(), source_bytes.data(), count);
}

void ArrayBuffer::copy_data_to(DataBlock& destination, size_t source_offset, size_t destination_offset, size_t count) const
{
    auto source_bytes = bytes_from(source_offset);
    VERIFY(count <= source_bytes.size());
    VERIFY(destination_offset <= destination.size());
    VERIFY(count <= destination.size() - destination_offset);
    if (count > 0)
        __builtin_memcpy(destination.data_at(destination_offset), source_bytes.data(), count);
}

void ArrayBuffer::overwrite(size_t offset, void const* source, size_t count)
{
    auto destination = bytes_from(offset);
    VERIFY(count <= destination.size());
    if (count > 0)
        __builtin_memcpy(destination.data(), source, count);
}

ArrayBuffer::StorageDescription ArrayBuffer::storage_description_from_abi(JSArrayBufferStorage const& storage)
{
    StorageDescription description;
    switch (storage.kind) {
    case JS_ARRAY_BUFFER_STORAGE_DETACHED:
        description.kind = StorageKind::Detached;
        break;
    case JS_ARRAY_BUFFER_STORAGE_OWNED:
        description.kind = StorageKind::Owned;
        description.handle = primitive_storage_handle_from_abi(storage.handle);
        break;
    case JS_ARRAY_BUFFER_STORAGE_EXTERNAL:
        description.kind = StorageKind::External;
        description.handle = primitive_storage_handle_from_abi(storage.external.handle);
        description.external_owner = static_cast<GC::Cell*>(storage.external.owner);
        if (storage.external.has_fixed_byte_length)
            description.external_fixed_byte_length = storage.external.fixed_byte_length;
        break;
    case JS_ARRAY_BUFFER_STORAGE_SHARED_MEMORY:
        description.kind = StorageKind::SharedMemory;
        description.shared_memory_object_id = storage.shared_memory_object_id;
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    return description;
}

ArrayBuffer::StorageDescription ArrayBuffer::describe_storage() const
{
    auto* buffer = array_buffer_to_abi(*this);
    JSArrayBufferStorage storage;
    js_array_buffer_storage(buffer, &storage);
    auto description = storage_description_from_abi(storage);
    description.is_shared = js_array_buffer_is_shared_array_buffer(buffer) ? DataBlock::Shared::Yes : DataBlock::Shared::No;
    if (description.kind == StorageKind::SharedMemory) {
        description.shared_memory_fd = js_array_buffer_duplicate_shared_memory(buffer, &description.shared_memory_byte_length);
        VERIFY(description.shared_memory_fd >= 0);
    }
    return description;
}

ThrowCompletionOr<ArrayBuffer::StorageDescription> ArrayBuffer::detach_and_take_storage(VM& vm)
{
    JSArrayBufferStorage storage;
    auto completion = js_array_buffer_detach_and_take_storage(vm_to_abi(vm), array_buffer_to_abi(*this), &storage);
    TRY(completion_from_abi<void>(completion));
    return storage_description_from_abi(storage);
}

size_t ArrayBuffer::max_byte_length() const
{
    return js_array_buffer_max_byte_length(array_buffer_to_abi(*this));
}

void ArrayBuffer::set_max_byte_length(size_t max_byte_length)
{
    js_array_buffer_set_max_byte_length(array_buffer_to_abi(*this), max_byte_length);
}

Value ArrayBuffer::detach_key() const
{
    return value_from_abi(js_array_buffer_detach_key(array_buffer_to_abi(*this)));
}

void ArrayBuffer::set_detach_key(Value detach_key)
{
    js_array_buffer_set_detach_key(array_buffer_to_abi(*this), value_to_abi(detach_key));
}

bool ArrayBuffer::is_detached() const
{
    return js_array_buffer_is_detached(array_buffer_to_abi(*this));
}

bool ArrayBuffer::is_fixed_length() const
{
    return js_array_buffer_is_fixed_length(array_buffer_to_abi(*this));
}

bool ArrayBuffer::is_shared_array_buffer() const
{
    return js_array_buffer_is_shared_array_buffer(array_buffer_to_abi(*this));
}

template<typename AtomicType>
static void load_atomically(u8 const* address, Bytes raw_value, ArrayBuffer::Order order)
{
    auto memory_order = order == ArrayBuffer::Order::SeqCst ? AK::memory_order_seq_cst : AK::memory_order_relaxed;
    auto atomic_value = AK::atomic_load(reinterpret_cast<AtomicType volatile*>(const_cast<u8*>(address)), memory_order);
    __builtin_memcpy(raw_value.data(), &atomic_value, sizeof(AtomicType));
}

template<typename T>
static T value_of_raw_bytes(ReadonlyBytes raw_value)
{
    T value;
    __builtin_memcpy(&value, raw_value.data(), sizeof(T));
    return value;
}

// 25.1.3.16 GetValueFromBuffer ( arrayBuffer, byteIndex, type, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-getvaluefrombuffer
Value ArrayBuffer::get_value_of_element_type(ElementType element_type, size_t byte_index, bool is_typed_array, Order order, bool is_little_endian)
{
    static constexpr AK::Array<size_t, 12> element_sizes { 1, 1, 2, 4, 8, 1, 2, 4, 8, 2, 4, 8 };
    auto element_size = element_sizes[to_underlying(element_type)];

    // 1. Assert: IsDetachedBuffer(arrayBuffer) is false.
    VERIFY(!is_detached());

    // 2. Assert: There are sufficient bytes in arrayBuffer starting at byteIndex to represent a value of type.
    auto block = bytes_from(byte_index);
    VERIFY(element_size <= block.size());

    AK::Array<u8, 8> raw_value_storage {};
    auto raw_value = raw_value_storage.span().trim(element_size);

    // 5. If IsSharedArrayBuffer(arrayBuffer) is true, then
    // AD-HOC: The read of a shared block is an atomic load: sequentially consistent for SeqCst order, and relaxed for a
    //         typed-array element read, while a DataView read may tear.
    if (is_shared_array_buffer() && (order == Order::SeqCst || is_typed_array)) {
        switch (element_size) {
        case 1:
            load_atomically<u8>(block.data(), raw_value, order);
            break;
        case 2:
            load_atomically<u16>(block.data(), raw_value, order);
            break;
        case 4:
            load_atomically<u32>(block.data(), raw_value, order);
            break;
        case 8:
            load_atomically<u64>(block.data(), raw_value, order);
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    // 6. Else,
    else {
        // a. Let rawValue be a List whose elements are bytes from block at indices in the interval from byteIndex (inclusive) to byteIndex + elementSize (exclusive).
        __builtin_memcpy(raw_value.data(), block.data(), element_size);
    }

    // 9. Return RawBytesToNumeric(type, rawValue, isLittleEndian).
    if (!is_little_endian) {
        for (size_t i = 0; i < element_size / 2; ++i)
            swap(raw_value[i], raw_value[element_size - 1 - i]);
    }

    switch (element_type) {
    case ElementType::Uint8:
    case ElementType::Uint8Clamped:
        return Value(value_of_raw_bytes<u8>(raw_value));
    case ElementType::Uint16:
        return Value(value_of_raw_bytes<u16>(raw_value));
    case ElementType::Uint32:
        return Value(value_of_raw_bytes<u32>(raw_value));
    case ElementType::Int8:
        return Value(value_of_raw_bytes<i8>(raw_value));
    case ElementType::Int16:
        return Value(value_of_raw_bytes<i16>(raw_value));
    case ElementType::Int32:
        return Value(value_of_raw_bytes<i32>(raw_value));
    case ElementType::BigUint64:
        return BigInt::create(vm(), Crypto::SignedBigInteger { Crypto::UnsignedBigInteger { value_of_raw_bytes<u64>(raw_value) } });
    case ElementType::BigInt64:
        return BigInt::create(vm(), Crypto::SignedBigInteger { value_of_raw_bytes<i64>(raw_value) });
    case ElementType::Float16:
        return Value(static_cast<double>(value_of_raw_bytes<f16>(raw_value)));
    case ElementType::Float32:
        return Value(static_cast<double>(value_of_raw_bytes<float>(raw_value)));
    case ElementType::Float64:
        return Value(value_of_raw_bytes<double>(raw_value));
    }
    VERIFY_NOT_REACHED();
}

namespace Detail {

ThrowCompletionOr<DataBlock::OwnedBackingStore> allocate_zeroed_storage_for_byte_data_block(VM& vm, size_t size, Optional<size_t> capacity)
{
    // 1. If size > 2^53 - 1, throw a RangeError exception.
    if (size > MAX_ARRAY_LIKE_INDEX)
        return vm.throw_completion<RangeError>(ErrorType::InvalidLength, "array buffer"sv);
    if (capacity.has_value() && *capacity > MAX_ARRAY_LIKE_INDEX)
        return vm.throw_completion<RangeError>(ErrorType::InvalidLength, "array buffer"sv);

    // 2. Let db be a new Data Block value consisting of size bytes. If it is impossible to create such a Data Block, throw a RangeError exception.
    // 3. Set all of the bytes of db to 0.
    auto allocate = [&] {
        return capacity.has_value()
            ? DataBlock::OwnedBackingStore::create_zeroed_with_capacity(size, *capacity)
            : DataBlock::OwnedBackingStore::create_zeroed(size);
    };
    // A large allocation that fails may succeed once dead buffers that pin address space are collected, so it is
    // retried once after a collection.
    auto storage = allocate();
    if (storage.is_error()) {
        vm.heap().collect_garbage();
        storage = allocate();
    }
    if (storage.is_error())
        return vm.throw_completion<RangeError>(ErrorType::NotEnoughMemoryToAllocate, capacity.value_or(size));

    // 4. Return db.
    return storage.release_value();
}

}

ThrowCompletionOr<void> detach_array_buffer(VM& vm, ArrayBuffer& array_buffer, Optional<Value> key)
{
    return completion_from_abi<void>(js_array_buffer_detach(vm_to_abi(vm), array_buffer_to_abi(array_buffer), value_to_abi(key.value_or(js_undefined()))));
}

ThrowCompletionOr<ArrayBuffer*> clone_array_buffer(VM& vm, ArrayBuffer& source_buffer, size_t source_byte_offset, size_t source_length)
{
    auto clone = TRY(completion_from_abi<GC::Ref<ArrayBuffer>>(js_array_buffer_clone(vm_to_abi(vm), array_buffer_to_abi(source_buffer), source_byte_offset, source_length)));
    return clone.ptr();
}

}
