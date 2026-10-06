/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Variant.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGC/PrimitiveStorage.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

struct JSArrayBufferStorage;

namespace JS {

struct ClampedU8 {
};

// 6.2.9 Data Blocks, https://tc39.es/ecma262/#sec-data-blocks
// The bytes of an ArrayBuffer live in the runtime's data block, so a DataBlock of LibJS's users describes storage: the
// storage of LibGC's primitive storage that it owns, storage that a GC cell owns, or a shared memory object, as
// ArrayBuffer::data_block() describes the storage of a buffer and ArrayBuffer::create(Realm&, DataBlock) hands it to
// a new one.
struct DataBlock {
    enum class Shared {
        No,
        Yes,
    };

    enum class ZeroFillNewBytes {
        No,
        Yes,
    };

    class OwnedBackingStore {
    public:
        OwnedBackingStore() = default;
        ~OwnedBackingStore()
        {
            if (m_owns_storage && m_handle.is_valid())
                GC::PrimitiveStorage::the().free(m_handle);
        }

        OwnedBackingStore(OwnedBackingStore&& other)
        {
            move_from(move(other));
        }

        OwnedBackingStore& operator=(OwnedBackingStore&& other)
        {
            if (this != &other) {
                if (m_owns_storage && m_handle.is_valid())
                    GC::PrimitiveStorage::the().free(m_handle);
                move_from(move(other));
            }
            return *this;
        }

        OwnedBackingStore(OwnedBackingStore const&) = delete;
        OwnedBackingStore& operator=(OwnedBackingStore const&) = delete;

        static ErrorOr<OwnedBackingStore> create_zeroed(size_t size)
        {
            OwnedBackingStore buffer;
            if (size > 0)
                buffer.m_handle = TRY(GC::PrimitiveStorage::the().try_allocate(size, GC::PrimitiveStorage::ZeroFillNewBytes::Yes));
            return buffer;
        }

        static ErrorOr<OwnedBackingStore> create_uninitialized(size_t size)
        {
            OwnedBackingStore buffer;
            if (size > 0)
                buffer.m_handle = TRY(GC::PrimitiveStorage::the().try_allocate(size, GC::PrimitiveStorage::ZeroFillNewBytes::No));
            return buffer;
        }

        static ErrorOr<OwnedBackingStore> create_zeroed_with_capacity(size_t size, size_t capacity)
        {
            OwnedBackingStore buffer;
            if (capacity > 0)
                buffer.m_handle = TRY(GC::PrimitiveStorage::the().try_reserve(size, capacity, GC::PrimitiveStorage::ZeroFillNewBytes::Yes));
            return buffer;
        }

        u8* data() { return GC::PrimitiveStorage::the().data(m_handle); }
        u8 const* data() const { return GC::PrimitiveStorage::the().data(m_handle); }
        size_t size() const { return GC::PrimitiveStorage::the().size(m_handle); }
        size_t capacity() const { return GC::PrimitiveStorage::the().capacity(m_handle); }
        size_t offset() const { return GC::PrimitiveStorage::the().offset(m_handle); }
        GC::PrimitiveStorageHandle handle() const { return m_handle; }

    private:
        friend class ArrayBuffer;

        // The storage of a buffer, which ArrayBuffer::data_block() describes without taking it over.
        static OwnedBackingStore describing_storage_of_an_array_buffer(GC::PrimitiveStorageHandle handle)
        {
            OwnedBackingStore buffer;
            buffer.m_handle = handle;
            buffer.m_owns_storage = false;
            return buffer;
        }

        static OwnedBackingStore taking_over(GC::PrimitiveStorageHandle handle)
        {
            OwnedBackingStore buffer;
            buffer.m_handle = handle;
            return buffer;
        }

        GC::PrimitiveStorageHandle give_up_storage()
        {
            VERIFY(m_owns_storage);
            return exchange(m_handle, {});
        }

        void move_from(OwnedBackingStore&& other)
        {
            m_handle = exchange(other.m_handle, {});
            m_owns_storage = exchange(other.m_owns_storage, true);
        }

        GC::PrimitiveStorageHandle m_handle;
        bool m_owns_storage { true };
    };

    struct DynamicPrimitiveStorageSize {
    };

    // AD-HOC: ECMA-262 models ArrayBuffer backing storage as a Data Block. We additionally allow
    //         host code to provide an external caged primitive store, so engine-independent
    //         consumers like LibWeb can project spec-defined host objects onto ArrayBuffer without
    //         teaching LibJS about those hosts.
    struct ExternalPrimitiveStorage {
        explicit ExternalPrimitiveStorage(GC::Ref<GC::Cell> owner, GC::PrimitiveStorageHandle handle)
            : handle(handle)
            , size(DynamicPrimitiveStorageSize {})
            , owner(owner)
        {
        }

        explicit ExternalPrimitiveStorage(GC::Ref<GC::Cell> owner, GC::PrimitiveStorageHandle handle, size_t fixed_size)
            : handle(handle)
            , size(fixed_size)
            , owner(owner)
        {
        }

        u8* data() { return GC::PrimitiveStorage::the().data(handle); }
        u8 const* data() const { return GC::PrimitiveStorage::the().data(handle); }
        size_t byte_length() const
        {
            return size.visit(
                [&](DynamicPrimitiveStorageSize) { return GC::PrimitiveStorage::the().size(handle); },
                [](size_t fixed_size) { return fixed_size; });
        }
        size_t capacity() const { return GC::PrimitiveStorage::the().capacity(handle); }
        size_t offset() const { return GC::PrimitiveStorage::the().offset(handle); }

        GC::PrimitiveStorageHandle handle;
        Variant<DynamicPrimitiveStorageSize, size_t> size;
        GC::Ref<GC::Cell> owner;
    };

    // AD-HOC: Backs a shared, fixed-length Data Block with cross-process shared memory, so a SharedArrayBuffer can be
    //         genuinely shared (not copied) across agents in different processes. object_id names the shared object
    //         in every agent that maps it, and is 0 for an object without one. The runtime maps the object into the
    //         primitive storage cage when a buffer is created over it.
    struct SharedBackingStore {
        explicit SharedBackingStore(Core::AnonymousBuffer buffer, u64 object_id = 0)
            : buffer(move(buffer))
            , object_id(object_id)
        {
        }

        SharedBackingStore(SharedBackingStore&& other)
            : buffer(move(other.buffer))
            , object_id(exchange(other.object_id, 0))
        {
        }

        SharedBackingStore& operator=(SharedBackingStore&& other)
        {
            if (this != &other) {
                buffer = move(other.buffer);
                object_id = exchange(other.object_id, 0);
            }
            return *this;
        }

        SharedBackingStore(SharedBackingStore const&) = delete;
        SharedBackingStore& operator=(SharedBackingStore const&) = delete;

        u8* data() { return buffer.data<u8>(); }
        u8 const* data() const { return buffer.data<u8>(); }
        size_t size() const { return buffer.size(); }

        Core::AnonymousBuffer buffer;
        u64 object_id { 0 };
    };

    u8* data_at(size_t byte_offset)
    {
        return byte_buffer.visit(
            [](Empty) -> u8* { VERIFY_NOT_REACHED(); },
            [byte_offset](OwnedBackingStore& value) -> u8* {
                if (!value.handle().is_valid()) {
                    VERIFY(byte_offset == 0);
                    return nullptr;
                }
                return GC::PrimitiveStorage::the().data(value.handle(), byte_offset);
            },
            [byte_offset](ExternalPrimitiveStorage& value) -> u8* { return GC::PrimitiveStorage::the().data(value.handle, byte_offset); },
            [byte_offset](SharedBackingStore& value) -> u8* { return value.data() + byte_offset; });
    }
    u8 const* data_at(size_t byte_offset) const { return const_cast<DataBlock*>(this)->data_at(byte_offset); }

    void copy_to(size_t offset, Bytes destination) const
    {
        VERIFY(offset <= size());
        VERIFY(destination.size() <= size() - offset);
        if (!destination.is_empty())
            __builtin_memcpy(destination.data(), data_at(offset), destination.size());
    }

    ErrorOr<ByteBuffer> copy_to_byte_buffer(size_t offset, size_t count) const
    {
        VERIFY(offset <= size());
        VERIFY(count <= size() - offset);

        auto destination = TRY(ByteBuffer::create_uninitialized(count));
        copy_to(offset, destination);
        return destination;
    }

    ErrorOr<ByteBuffer> copy_to_byte_buffer() const
    {
        return copy_to_byte_buffer(0, size());
    }

    template<typename Callback>
    decltype(auto) with_readonly_bytes(size_t offset, size_t count, Callback callback) const
    {
        VERIFY(offset <= size());
        VERIFY(count <= size() - offset);

        if (count == 0)
            return callback({});

        // AD-HOC: Never hand out a pointer into a Shared Data Block. Another agent may write to those bytes at any
        //         time, so the consumer gets a snapshot of them instead.
        if (is_shared == Shared::No)
            return callback({ data_at(offset), count });

        auto storage = MUST(copy_to_byte_buffer(offset, count));
        return callback(storage.bytes());
    }

    void overwrite(size_t offset, void const* source, size_t count)
    {
        VERIFY(offset <= size());
        VERIFY(count <= size() - offset);
        if (count > 0)
            __builtin_memcpy(data_at(offset), source, count);
    }

    size_t size() const
    {
        return byte_buffer.visit(
            [](Empty) -> size_t { return 0u; },
            [](OwnedBackingStore const& buffer) { return buffer.size(); },
            [](ExternalPrimitiveStorage const& value) { return value.byte_length(); },
            [](SharedBackingStore const& value) { return value.size(); });
    }

    bool is_external() const { return byte_buffer.has<ExternalPrimitiveStorage>(); }
    bool is_cross_process_shared() const { return byte_buffer.has<SharedBackingStore>(); }

    Optional<Core::AnonymousBuffer> shared_anonymous_buffer() const
    {
        if (auto const* shared = byte_buffer.get_pointer<SharedBackingStore>())
            return shared->buffer;
        return {};
    }

    u64 shared_object_id() const
    {
        if (auto const* shared = byte_buffer.get_pointer<SharedBackingStore>())
            return shared->object_id;
        return 0;
    }

    Variant<Empty, OwnedBackingStore, ExternalPrimitiveStorage, SharedBackingStore> byte_buffer;
    Shared is_shared = { Shared::No };
};

// An ArrayBuffer or a SharedArrayBuffer of the Rust runtime, whose data block the runtime keeps.
//
// The members that make or take a DataBlock are defined in this header, over out-of-line members that describe the
// storage in plain values, because a DataBlock can hold a Core::AnonymousBuffer, and LibJS links LibCore privately.
class JS_API ArrayBuffer final : public Object {
public:
    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_ARRAY_BUFFER; }

    static ThrowCompletionOr<GC::Ref<ArrayBuffer>> create(Realm&, size_t, DataBlock::Shared = DataBlock::Shared::No);
    static GC::Ref<ArrayBuffer> create(Realm&, ByteBuffer, DataBlock::Shared = DataBlock::Shared::No);
    static GC::Ref<ArrayBuffer> create(Realm&, DataBlock);
    static GC::Ref<ArrayBuffer> create(Realm&, Core::AnonymousBuffer, u64 shared_object_id);

    size_t byte_length() const;

    // [[ArrayBufferData]]
    u8* data_at(size_t byte_index) { return bytes_from(byte_index).data(); }
    u8 const* data_at(size_t byte_index) const { return bytes_from(byte_index).data(); }
    void copy_to(size_t offset, Bytes destination) const;
    ErrorOr<ByteBuffer> copy_to_byte_buffer(size_t offset, size_t count) const;
    ErrorOr<ByteBuffer> copy_to_byte_buffer() const;
    Optional<Core::AnonymousBuffer> shared_buffer() const;
    u64 shared_object_id() const;
    template<typename Callback>
    decltype(auto) with_readonly_bytes(size_t offset, size_t count, Callback callback) const;
    void copy_data_to(ArrayBuffer& destination, size_t source_offset, size_t destination_offset, size_t count) const;
    void copy_data_to(DataBlock& destination, size_t source_offset, size_t destination_offset, size_t count) const;
    void overwrite(size_t offset, void const* source, size_t count);

    // A description of the storage of the buffer's data block, which stays the buffer's. It is const so that it cannot
    // be moved into another buffer.
    DataBlock const data_block() const;

    // Detaches this ArrayBuffer and returns its underlying DataBlock for use in a TransferArrayBuffer-like operation.
    // If detach fails, the underlying storage is left untouched.
    ThrowCompletionOr<DataBlock> detach_and_take_data_block(VM&);

    // [[ArrayBufferMaxByteLength]]
    size_t max_byte_length() const;
    void set_max_byte_length(size_t max_byte_length);

    // Only to storage that a GC cell owns, as WebAssembly.Memory refreshes its buffers.
    void set_data_block(DataBlock);

    Value detach_key() const;
    void set_detach_key(Value detach_key);

    // 25.1.3.4 IsDetachedBuffer ( arrayBuffer ), https://tc39.es/ecma262/#sec-isdetachedbuffer
    bool is_detached() const;

    // 25.1.3.9 IsFixedLengthArrayBuffer ( arrayBuffer ), https://tc39.es/ecma262/#sec-isfixedlengtharraybuffer
    bool is_fixed_length() const;

    // 25.2.2.2 IsSharedArrayBuffer ( obj ), https://tc39.es/ecma262/#sec-issharedarraybuffer
    bool is_shared_array_buffer() const;

    enum Order {
        SeqCst,
        Unordered
    };

    // 25.1.3.16 GetValueFromBuffer ( arrayBuffer, byteIndex, type, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-getvaluefrombuffer
    template<typename type>
    Value get_value(size_t byte_index, bool is_typed_array, Order order, bool is_little_endian = true)
    {
        return get_value_of_element_type(element_type_of<type>(), byte_index, is_typed_array, order, is_little_endian);
    }

private:
    // The element types of Table 71, in the order of JS_ENUMERATE_TYPED_ARRAYS.
    enum class ElementType : u8 {
        Uint8,
        Uint8Clamped,
        Uint16,
        Uint32,
        BigUint64,
        Int8,
        Int16,
        Int32,
        BigInt64,
        Float16,
        Float32,
        Float64,
    };

    template<typename T>
    static constexpr ElementType element_type_of()
    {
        if constexpr (IsSame<T, ClampedU8>)
            return ElementType::Uint8Clamped;
        else if constexpr (IsSame<T, u8>)
            return ElementType::Uint8;
        else if constexpr (IsSame<T, u16>)
            return ElementType::Uint16;
        else if constexpr (IsSame<T, u32>)
            return ElementType::Uint32;
        else if constexpr (IsSame<T, u64>)
            return ElementType::BigUint64;
        else if constexpr (IsSame<T, i8>)
            return ElementType::Int8;
        else if constexpr (IsSame<T, i16>)
            return ElementType::Int16;
        else if constexpr (IsSame<T, i32>)
            return ElementType::Int32;
        else if constexpr (IsSame<T, i64>)
            return ElementType::BigInt64;
        else if constexpr (IsSame<T, f16>)
            return ElementType::Float16;
        else if constexpr (IsSame<T, float>)
            return ElementType::Float32;
        else if constexpr (IsSame<T, double>)
            return ElementType::Float64;
        else
            static_assert(DependentFalse<T>, "Not an element type of a buffer");
    }

    Value get_value_of_element_type(ElementType, size_t byte_index, bool is_typed_array, Order, bool is_little_endian);

    // The bytes of the data block from `byte_index` on, with a null pointer for a block without bytes.
    Bytes bytes_from(size_t byte_index) const;

    enum class StorageKind : u8 {
        Detached,
        Owned,
        External,
        SharedMemory,
    };

    // The storage of a data block in plain values. A description of the storage of a buffer comes with a duplicate of
    // the descriptor of its shared memory object, which the receiver owns.
    struct StorageDescription {
        StorageKind kind { StorageKind::Detached };
        DataBlock::Shared is_shared { DataBlock::Shared::No };
        GC::PrimitiveStorageHandle handle;
        GC::Ptr<GC::Cell> external_owner;
        Optional<size_t> external_fixed_byte_length;
        int shared_memory_fd { -1 };
        size_t shared_memory_byte_length { 0 };
        u64 shared_memory_object_id { 0 };
    };

    enum class OwnedStorageGoesToTheBlock {
        No,
        Yes,
    };
    static DataBlock data_block_from(StorageDescription const&, OwnedStorageGoesToTheBlock);

    static StorageDescription storage_description_from_abi(JSArrayBufferStorage const&);
    StorageDescription describe_storage() const;
    ThrowCompletionOr<StorageDescription> detach_and_take_storage(VM&);

    static GC::Ref<ArrayBuffer> create_taking_over_storage(Realm&, GC::PrimitiveStorageHandle, DataBlock::Shared);
    static GC::Ref<ArrayBuffer> create_over_external_storage(Realm&, DataBlock::ExternalPrimitiveStorage const&, DataBlock::Shared);
    static GC::Ptr<ArrayBuffer> create_over_shared_memory(Realm&, int shared_memory_fd, size_t byte_length, u64 shared_object_id);
    static GC::Ref<ArrayBuffer> create_detached(Realm&);
    void set_external_storage(DataBlock::ExternalPrimitiveStorage const&, DataBlock::Shared);
};

inline DataBlock ArrayBuffer::data_block_from(StorageDescription const& storage, OwnedStorageGoesToTheBlock owned_storage_goes_to_the_block)
{
    switch (storage.kind) {
    case StorageKind::Detached:
        return DataBlock { Empty {}, storage.is_shared };
    case StorageKind::Owned:
        if (owned_storage_goes_to_the_block == OwnedStorageGoesToTheBlock::Yes)
            return DataBlock { DataBlock::OwnedBackingStore::taking_over(storage.handle), storage.is_shared };
        return DataBlock { DataBlock::OwnedBackingStore::describing_storage_of_an_array_buffer(storage.handle), storage.is_shared };
    case StorageKind::External:
        if (storage.external_fixed_byte_length.has_value())
            return DataBlock { DataBlock::ExternalPrimitiveStorage { *storage.external_owner, storage.handle, *storage.external_fixed_byte_length }, storage.is_shared };
        return DataBlock { DataBlock::ExternalPrimitiveStorage { *storage.external_owner, storage.handle }, storage.is_shared };
    case StorageKind::SharedMemory: {
        auto shared_memory = MUST(Core::AnonymousBuffer::create_from_anon_fd(storage.shared_memory_fd, storage.shared_memory_byte_length));
        return DataBlock { DataBlock::SharedBackingStore { move(shared_memory), storage.shared_memory_object_id }, DataBlock::Shared::Yes };
    }
    }
    VERIFY_NOT_REACHED();
}

inline GC::Ref<ArrayBuffer> ArrayBuffer::create(Realm& realm, DataBlock block)
{
    return block.byte_buffer.visit(
        [&](Empty) {
            VERIFY(block.is_shared == DataBlock::Shared::No);
            return create_detached(realm);
        },
        [&](DataBlock::OwnedBackingStore& owned) {
            return create_taking_over_storage(realm, owned.give_up_storage(), block.is_shared);
        },
        [&](DataBlock::ExternalPrimitiveStorage const& external) {
            return create_over_external_storage(realm, external, block.is_shared);
        },
        [&](DataBlock::SharedBackingStore const& shared) {
            VERIFY(block.is_shared == DataBlock::Shared::Yes);
            return create(realm, shared.buffer, shared.object_id);
        });
}

inline GC::Ref<ArrayBuffer> ArrayBuffer::create(Realm& realm, Core::AnonymousBuffer shared_memory, u64 shared_object_id)
{
    if (auto buffer = create_over_shared_memory(realm, shared_memory.fd(), shared_memory.size(), shared_object_id))
        return *buffer;

    // The runtime cannot map the object into the primitive storage cage, where the bytes of every buffer live, so the
    // buffer gets a copy of its bytes instead. A buffer of no bytes has none to share.
    auto bytes = shared_memory.size() > 0 ? MUST(ByteBuffer::copy(shared_memory.data<u8>(), shared_memory.size())) : ByteBuffer {};
    return create(realm, move(bytes), DataBlock::Shared::Yes);
}

inline Optional<Core::AnonymousBuffer> ArrayBuffer::shared_buffer() const
{
    auto storage = describe_storage();
    if (storage.kind != StorageKind::SharedMemory)
        return {};
    return MUST(Core::AnonymousBuffer::create_from_anon_fd(storage.shared_memory_fd, storage.shared_memory_byte_length));
}

inline DataBlock const ArrayBuffer::data_block() const
{
    return data_block_from(describe_storage(), OwnedStorageGoesToTheBlock::No);
}

inline ThrowCompletionOr<DataBlock> ArrayBuffer::detach_and_take_data_block(VM& vm)
{
    auto storage = TRY(detach_and_take_storage(vm));
    return data_block_from(storage, OwnedStorageGoesToTheBlock::Yes);
}

inline void ArrayBuffer::set_data_block(DataBlock block)
{
    auto const* external = block.byte_buffer.get_pointer<DataBlock::ExternalPrimitiveStorage>();
    VERIFY(external);
    set_external_storage(*external, block.is_shared);
}

template<typename Callback>
decltype(auto) ArrayBuffer::with_readonly_bytes(size_t offset, size_t count, Callback callback) const
{
    auto bytes = bytes_from(0);
    VERIFY(offset <= bytes.size());
    VERIFY(count <= bytes.size() - offset);

    if (count == 0)
        return callback({});

    // AD-HOC: Never hand out a pointer into a Shared Data Block. Another agent may write to those bytes at any time, so
    //         the consumer gets a snapshot of them instead.
    if (!is_shared_array_buffer())
        return callback(ReadonlyBytes { bytes.slice(offset, count) });

    auto storage = MUST(ByteBuffer::copy(bytes.slice(offset, count)));
    return callback(storage.bytes());
}

namespace Detail {

// The zeroed bytes of CreateByteDataBlock, or the RangeError it throws.
JS_API ThrowCompletionOr<DataBlock::OwnedBackingStore> allocate_zeroed_storage_for_byte_data_block(VM&, size_t size, Optional<size_t> capacity);

}

// 6.2.9.1 CreateByteDataBlock ( size ), https://tc39.es/ecma262/#sec-createbytedatablock
inline ThrowCompletionOr<DataBlock> create_byte_data_block(VM& vm, size_t size, Optional<size_t> capacity = {})
{
    auto owned_backing_store = TRY(Detail::allocate_zeroed_storage_for_byte_data_block(vm, size, capacity));
    return DataBlock { move(owned_backing_store), DataBlock::Shared::No };
}

JS_API ThrowCompletionOr<void> detach_array_buffer(VM&, ArrayBuffer& array_buffer, Optional<Value> key = {});
JS_API ThrowCompletionOr<ArrayBuffer*> clone_array_buffer(VM&, ArrayBuffer& source_buffer, size_t source_byte_offset, size_t source_length);

}
