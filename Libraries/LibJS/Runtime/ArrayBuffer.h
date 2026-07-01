/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/IntrusiveList.h>
#include <AK/Variant.h>
#include <LibGC/PrimitiveStorage.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class TypedArrayBase;

class CachedTypedArrayView {
protected:
    void remove_from_cached_view_list()
    {
        if (m_cached_view_list_node.is_in_list())
            m_cached_view_list_node.remove();
    }

private:
    friend class ArrayBuffer;

    IntrusiveListNode<CachedTypedArrayView> m_cached_view_list_node;
};

struct ClampedU8 {
};

// 25.1.1 Notation (read-modify-write modification function), https://tc39.es/ecma262/#sec-arraybuffer-notation
using ReadWriteModifyFunction = Function<ByteBuffer(ByteBuffer, ByteBuffer)>;

enum class PreserveResizability {
    FixedLength,
    PreserveResizability
};

// 6.2.9 Data Blocks, https://tc39.es/ecma262/#sec-data-blocks
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
            if (m_handle.is_valid())
                GC::PrimitiveStorage::the().free(m_handle);
        }

        OwnedBackingStore(OwnedBackingStore&& other)
        {
            move_from(move(other));
        }

        OwnedBackingStore& operator=(OwnedBackingStore&& other)
        {
            if (this != &other) {
                if (m_handle.is_valid())
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

        void set_size(size_t new_size, ZeroFillNewBytes zero_fill_new_bytes = ZeroFillNewBytes::No)
        {
            VERIFY(new_size <= capacity());
            MUST(try_resize(new_size, zero_fill_new_bytes));
        }

        ErrorOr<void> try_resize(size_t new_size, ZeroFillNewBytes zero_fill_new_bytes = ZeroFillNewBytes::No)
        {
            auto primitive_zero_fill = zero_fill_new_bytes == ZeroFillNewBytes::Yes
                ? GC::PrimitiveStorage::ZeroFillNewBytes::Yes
                : GC::PrimitiveStorage::ZeroFillNewBytes::No;

            if (!m_handle.is_valid()) {
                if (new_size == 0)
                    return {};
                m_handle = TRY(GC::PrimitiveStorage::the().try_allocate(new_size, primitive_zero_fill));
                return {};
            }

            return GC::PrimitiveStorage::the().try_resize(m_handle, new_size, primitive_zero_fill);
        }

        ErrorOr<void> try_ensure_capacity(size_t new_capacity)
        {
            if (new_capacity <= capacity())
                return {};
            if (!m_handle.is_valid()) {
                m_handle = TRY(GC::PrimitiveStorage::the().try_reserve(0, new_capacity, GC::PrimitiveStorage::ZeroFillNewBytes::No));
                return {};
            }
            return GC::PrimitiveStorage::the().try_reserve(m_handle, new_capacity);
        }

    private:
        void move_from(OwnedBackingStore&& other)
        {
            m_handle = exchange(other.m_handle, {});
        }

        GC::PrimitiveStorageHandle m_handle;
    };

    struct UnownedFixedLengthByteBuffer {
        explicit UnownedFixedLengthByteBuffer(ByteBuffer* buffer)
            : buffer(buffer)
            , size(buffer ? buffer->size() : 0)
        {
        }

        ByteBuffer* buffer = nullptr;
        size_t size = 0;
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

private:
    u8* data()
    {
        return byte_buffer.visit(
            [](Empty) -> u8* { VERIFY_NOT_REACHED(); },
            [](OwnedBackingStore& value) -> u8* { return value.data(); },
            [](UnownedFixedLengthByteBuffer& value) -> u8* { return value.buffer->data(); },
            [](ExternalPrimitiveStorage& value) -> u8* { return value.data(); });
    }
    u8 const* data() const { return const_cast<DataBlock*>(this)->data(); }

public:
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
            [byte_offset](UnownedFixedLengthByteBuffer& value) -> u8* { return value.buffer->data() + byte_offset; },
            [byte_offset](ExternalPrimitiveStorage& value) -> u8* { return GC::PrimitiveStorage::the().data(value.handle, byte_offset); });
    }
    u8 const* data_at(size_t byte_offset) const { return const_cast<DataBlock*>(this)->data_at(byte_offset); }

    size_t contiguous_bytes_from(size_t byte_offset, size_t count) const
    {
        if (!is_caged() || count == 0)
            return count;

        auto data_offset = offset();
        if (data_offset == GC::PrimitiveStorage::invalid_offset)
            return count;

        auto caged_offset = GC::PrimitiveStorage::mask_offset(data_offset + byte_offset);
        return min(count, GC::PrimitiveStorage::default_cage_size - caged_offset);
    }

    size_t contiguous_bytes_before(size_t byte_offset, size_t count) const
    {
        if (!is_caged() || count == 0)
            return count;

        VERIFY(byte_offset > 0);

        auto data_offset = offset();
        if (data_offset == GC::PrimitiveStorage::invalid_offset)
            return count;

        auto caged_offset = GC::PrimitiveStorage::mask_offset(data_offset + byte_offset - 1);
        return min(count, caged_offset + 1);
    }

    void copy_to(size_t offset, Bytes destination) const
    {
        VERIFY(offset <= size());
        VERIFY(destination.size() <= size() - offset);
        size_t copied = 0;
        while (copied < destination.size()) {
            auto chunk_size = contiguous_bytes_from(offset + copied, destination.size() - copied);
            __builtin_memcpy(destination.data() + copied, data_at(offset + copied), chunk_size);
            copied += chunk_size;
        }
    }

    void copy_to(DataBlock& destination, size_t source_offset, size_t destination_offset, size_t count) const
    {
        VERIFY(source_offset <= size());
        VERIFY(count <= size() - source_offset);
        VERIFY(destination_offset <= destination.size());
        VERIFY(count <= destination.size() - destination_offset);

        size_t copied = 0;
        while (copied < count) {
            auto source_chunk_size = contiguous_bytes_from(source_offset + copied, count - copied);
            auto destination_chunk_size = destination.contiguous_bytes_from(destination_offset + copied, count - copied);
            auto chunk_size = min(source_chunk_size, destination_chunk_size);
            __builtin_memcpy(destination.data_at(destination_offset + copied), data_at(source_offset + copied), chunk_size);
            copied += chunk_size;
        }
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

        if (contiguous_bytes_from(offset, count) == count)
            return callback({ data_at(offset), count });

        auto storage = MUST(copy_to_byte_buffer(offset, count));
        return callback(storage.bytes());
    }

    void overwrite(size_t offset, void const* source, size_t count)
    {
        VERIFY(offset <= size());
        VERIFY(count <= size() - offset);
        auto const* source_bytes = static_cast<u8 const*>(source);
        size_t copied = 0;
        while (copied < count) {
            auto chunk_size = contiguous_bytes_from(offset + copied, count - copied);
            __builtin_memcpy(data_at(offset + copied), source_bytes + copied, chunk_size);
            copied += chunk_size;
        }
    }

    void move_data(size_t destination_offset, size_t source_offset, size_t count)
    {
        VERIFY(destination_offset <= size());
        VERIFY(count <= size() - destination_offset);
        VERIFY(source_offset <= size());
        VERIFY(count <= size() - source_offset);

        if (count == 0 || destination_offset == source_offset)
            return;

        if (!is_caged()) {
            __builtin_memmove(data_at(destination_offset), data_at(source_offset), count);
            return;
        }

        if (source_offset < destination_offset && destination_offset < source_offset + count) {
            size_t remaining = count;
            while (remaining > 0) {
                auto source_chunk_size = contiguous_bytes_before(source_offset + remaining, remaining);
                auto destination_chunk_size = contiguous_bytes_before(destination_offset + remaining, remaining);
                auto chunk_size = min(source_chunk_size, destination_chunk_size);
                remaining -= chunk_size;
                __builtin_memmove(data_at(destination_offset + remaining), data_at(source_offset + remaining), chunk_size);
            }
            return;
        }

        size_t copied = 0;
        while (copied < count) {
            auto source_chunk_size = contiguous_bytes_from(source_offset + copied, count - copied);
            auto destination_chunk_size = contiguous_bytes_from(destination_offset + copied, count - copied);
            auto chunk_size = min(source_chunk_size, destination_chunk_size);
            __builtin_memmove(data_at(destination_offset + copied), data_at(source_offset + copied), chunk_size);
            copied += chunk_size;
        }
    }

    void set_size(size_t new_size, ZeroFillNewBytes zero_fill_new_bytes = ZeroFillNewBytes::No)
    {
        auto byte_buffer_zero_fill = zero_fill_new_bytes == ZeroFillNewBytes::Yes
            ? ByteBuffer::ZeroFillNewElements::Yes
            : ByteBuffer::ZeroFillNewElements::No;
        byte_buffer.visit(
            [&](Empty) { VERIFY_NOT_REACHED(); },
            [&](OwnedBackingStore& value) { value.set_size(new_size, zero_fill_new_bytes); },
            [&](UnownedFixedLengthByteBuffer& value) { value.buffer->set_size(new_size, byte_buffer_zero_fill); },
            [&](ExternalPrimitiveStorage&) { VERIFY_NOT_REACHED(); });
    }

    ErrorOr<void> try_resize(size_t new_size, ZeroFillNewBytes zero_fill_new_bytes = ZeroFillNewBytes::No)
    {
        auto byte_buffer_zero_fill = zero_fill_new_bytes == ZeroFillNewBytes::Yes
            ? ByteBuffer::ZeroFillNewElements::Yes
            : ByteBuffer::ZeroFillNewElements::No;
        return byte_buffer.visit(
            [&](Empty) -> ErrorOr<void> { VERIFY_NOT_REACHED(); },
            [&](OwnedBackingStore& value) { return value.try_resize(new_size, zero_fill_new_bytes); },
            [&](UnownedFixedLengthByteBuffer& value) { return value.buffer->try_resize(new_size, byte_buffer_zero_fill); },
            [&](ExternalPrimitiveStorage&) -> ErrorOr<void> { VERIFY_NOT_REACHED(); });
    }

    ErrorOr<void> try_ensure_capacity(size_t new_capacity)
    {
        return byte_buffer.visit(
            [&](Empty) -> ErrorOr<void> { VERIFY_NOT_REACHED(); },
            [&](OwnedBackingStore& value) { return value.try_ensure_capacity(new_capacity); },
            [&](UnownedFixedLengthByteBuffer& value) { return value.buffer->try_ensure_capacity(new_capacity); },
            [&](ExternalPrimitiveStorage&) -> ErrorOr<void> { VERIFY_NOT_REACHED(); });
    }

    size_t size() const
    {
        return byte_buffer.visit(
            [](Empty) -> size_t { return 0u; },
            [](OwnedBackingStore const& buffer) { return buffer.size(); },
            [](UnownedFixedLengthByteBuffer const& value) { return value.size; },
            [](ExternalPrimitiveStorage const& value) { return value.byte_length(); });
    }

    size_t capacity() const
    {
        return byte_buffer.visit(
            [](Empty) -> size_t { return 0; },
            [](OwnedBackingStore const& buffer) { return buffer.capacity(); },
            [](UnownedFixedLengthByteBuffer const& value) { return value.size; },
            [](ExternalPrimitiveStorage const& value) { return value.capacity(); });
    }

    size_t offset() const
    {
        return byte_buffer.visit(
            [](Empty) -> size_t { return GC::PrimitiveStorage::invalid_offset; },
            [](OwnedBackingStore const& buffer) { return buffer.offset(); },
            [](UnownedFixedLengthByteBuffer const&) { return GC::PrimitiveStorage::invalid_offset; },
            [](ExternalPrimitiveStorage const& value) { return value.offset(); });
    }

    bool is_caged() const
    {
        return byte_buffer.visit(
            [](Empty) { return false; },
            [](OwnedBackingStore const& buffer) { return buffer.handle().is_valid() || buffer.size() == 0; },
            [](UnownedFixedLengthByteBuffer const&) { return false; },
            [](ExternalPrimitiveStorage const& value) { return value.handle.is_valid(); });
    }

    size_t external_memory_size() const
    {
        return byte_buffer.visit(
            [](Empty) -> size_t { return 0; },
            [](OwnedBackingStore const& buffer) { return buffer.capacity(); },
            [](UnownedFixedLengthByteBuffer const&) -> size_t { return 0; },
            [](ExternalPrimitiveStorage const&) -> size_t { return 0; });
    }

    bool is_external() const { return byte_buffer.has<ExternalPrimitiveStorage>(); }

    bool shares_storage_with(DataBlock const& other) const
    {
        if (byte_buffer.has<Empty>() || other.byte_buffer.has<Empty>())
            return false;
        return data() == other.data();
    }

    Variant<Empty, OwnedBackingStore, UnownedFixedLengthByteBuffer, ExternalPrimitiveStorage> byte_buffer;
    Shared is_shared = { Shared::No };
};

class JS_API ArrayBuffer final : public Object {
    JS_OBJECT(ArrayBuffer, Object);
    GC_DECLARE_ALLOCATOR(ArrayBuffer);

public:
    static ThrowCompletionOr<GC::Ref<ArrayBuffer>> create(Realm&, size_t, DataBlock::Shared = DataBlock::Shared::No);
    static GC::Ref<ArrayBuffer> create(Realm&, ByteBuffer, DataBlock::Shared = DataBlock::Shared::No);
    static GC::Ref<ArrayBuffer> create(Realm&, ByteBuffer*, DataBlock::Shared = DataBlock::Shared::No);
    static GC::Ref<ArrayBuffer> create(Realm&, DataBlock);

    virtual ~ArrayBuffer() override = default;

    size_t byte_length() const { return m_data_block.size(); }
    virtual size_t external_memory_size() const override { return m_data_block.external_memory_size(); }

    // [[ArrayBufferData]]
    u8* data_at(size_t byte_index) { return m_data_block.data_at(byte_index); }
    u8 const* data_at(size_t byte_index) const { return m_data_block.data_at(byte_index); }
    void copy_to(size_t offset, Bytes destination) const { m_data_block.copy_to(offset, destination); }
    ErrorOr<ByteBuffer> copy_to_byte_buffer(size_t offset, size_t count) const { return m_data_block.copy_to_byte_buffer(offset, count); }
    ErrorOr<ByteBuffer> copy_to_byte_buffer() const { return m_data_block.copy_to_byte_buffer(); }
    template<typename Callback>
    decltype(auto) with_readonly_bytes(size_t offset, size_t count, Callback callback) const { return m_data_block.with_readonly_bytes(offset, count, move(callback)); }
    void copy_data_to(ArrayBuffer& destination, size_t source_offset, size_t destination_offset, size_t count) const { m_data_block.copy_to(destination.m_data_block, source_offset, destination_offset, count); }
    void copy_data_to(DataBlock& destination, size_t source_offset, size_t destination_offset, size_t count) const { m_data_block.copy_to(destination, source_offset, destination_offset, count); }
    void overwrite(size_t offset, void const* source, size_t count) { m_data_block.overwrite(offset, source, count); }
    void move_data(size_t destination_offset, size_t source_offset, size_t count) { m_data_block.move_data(destination_offset, source_offset, count); }
    bool is_external() const { return m_data_block.is_external(); }
    bool is_caged() const { return m_data_block.is_caged(); }
    bool shares_storage_with(ArrayBuffer const& other) const { return m_data_block.shares_storage_with(other.m_data_block); }
    size_t data_offset() const { return m_data_block.offset(); }

    // Detaches this ArrayBuffer and returns its underlying DataBlock for use in a TransferArrayBuffer-like operation.
    // If detach fails, the underlying storage is left untouched.
    ThrowCompletionOr<DataBlock> detach_and_take_data_block(VM&);

    // [[ArrayBufferMaxByteLength]]
    size_t max_byte_length() const { return m_max_byte_length.value(); }
    void set_max_byte_length(size_t max_byte_length) { m_max_byte_length = max_byte_length; }

    // Used by allocate_array_buffer() to attach the data block after construction
    void set_data_block(DataBlock);
    void did_change_data_block_capacity(size_t old_external_memory_size);
    ErrorOr<void> try_resize(size_t, DataBlock::ZeroFillNewBytes = DataBlock::ZeroFillNewBytes::No);
    ErrorOr<void> try_ensure_capacity(size_t);

    Value detach_key() const { return m_detach_key; }
    void set_detach_key(Value detach_key) { m_detach_key = detach_key; }

    void detach_buffer();
    void register_cached_typed_array_view(TypedArrayBase&);

    // 25.1.3.4 IsDetachedBuffer ( arrayBuffer ), https://tc39.es/ecma262/#sec-isdetachedbuffer
    bool is_detached() const
    {
        // 1. If arrayBuffer.[[ArrayBufferData]] is null, return true.
        if (m_data_block.byte_buffer.has<Empty>())
            return true;
        // 2. Return false.
        return false;
    }

    // 25.1.3.9 IsFixedLengthArrayBuffer ( arrayBuffer ), https://tc39.es/ecma262/#sec-isfixedlengtharraybuffer
    bool is_fixed_length() const
    {
        // 1. If arrayBuffer has an [[ArrayBufferMaxByteLength]] internal slot, return false.
        if (m_max_byte_length.has_value())
            return false;

        // 2. Return true.
        return true;
    }

    bool can_cache_typed_array_view_data_offset() const
    {
        return !is_detached() && is_fixed_length() && m_data_block.is_caged();
    }

    // 25.2.2.2 IsSharedArrayBuffer ( obj ), https://tc39.es/ecma262/#sec-issharedarraybuffer
    bool is_shared_array_buffer() const
    {
        // 1. Let bufferData be obj.[[ArrayBufferData]].
        // 2. If bufferData is null, return false.
        if (m_data_block.byte_buffer.has<Empty>())
            return false;
        // 3. If bufferData is a Data Block, return false.
        if (m_data_block.is_shared == DataBlock::Shared::No)
            return false;
        // 4. Assert: bufferData is a Shared Data Block.
        VERIFY(m_data_block.is_shared == DataBlock::Shared::Yes);
        // 5. Return true.
        return true;
    }

    enum Order {
        SeqCst,
        Unordered
    };
    template<typename type>
    Value get_value(size_t byte_index, bool is_typed_array, Order, bool is_little_endian = true);
    template<typename type>
    void set_value(size_t byte_index, Value value, bool is_typed_array, Order, bool is_little_endian = true);
    template<typename T>
    Value get_modify_set_value(size_t byte_index, Value value, ReadWriteModifyFunction operation, bool is_little_endian = true);

private:
    ArrayBuffer(DataBlock::OwnedBackingStore buffer, DataBlock::Shared, Object& prototype);
    ArrayBuffer(ByteBuffer* buffer, DataBlock::Shared, Object& prototype);

    virtual bool is_array_buffer() const final { return true; }

    virtual void visit_edges(Visitor&) override;

    void account_external_memory_change(size_t old_external_memory_size, size_t new_external_memory_size);
    void invalidate_cached_typed_array_view_offsets();

    DataBlock m_data_block;
    Optional<size_t> m_max_byte_length;
    IntrusiveList<&CachedTypedArrayView::m_cached_view_list_node> m_cached_views;

    // The various detach related members of ArrayBuffer are not used by any ECMA262 functionality,
    // but are required to be available for the use of various harnesses like the Test262 test runner.
    Value m_detach_key;
};

template<>
inline bool Object::fast_is<ArrayBuffer>() const { return is_array_buffer(); }

JS_API ThrowCompletionOr<DataBlock> create_byte_data_block(VM& vm, size_t size, Optional<size_t> capacity = {});
JS_API void copy_data_block_bytes(Bytes to_block, u64 to_index, ReadonlyBytes from_block, u64 from_index, u64 count);
ThrowCompletionOr<ArrayBuffer*> allocate_array_buffer(VM&, FunctionObject& constructor, size_t byte_length, Optional<size_t> const& max_byte_length = {});
ThrowCompletionOr<ArrayBuffer*> array_buffer_copy_and_detach(VM&, ArrayBuffer& array_buffer, Value new_length, PreserveResizability preserve_resizability);
JS_API ThrowCompletionOr<void> detach_array_buffer(VM&, ArrayBuffer& array_buffer, Optional<Value> key = {});
ThrowCompletionOr<Optional<size_t>> get_array_buffer_max_byte_length_option(VM&, Value options);
JS_API ThrowCompletionOr<ArrayBuffer*> clone_array_buffer(VM&, ArrayBuffer& source_buffer, size_t source_byte_offset, size_t source_length);
JS_API ThrowCompletionOr<GC::Ref<ArrayBuffer>> allocate_shared_array_buffer(VM&, FunctionObject& constructor, size_t byte_length, Optional<size_t> const& max_byte_length = {});

// 25.1.3.2 ArrayBufferByteLength ( arrayBuffer, order ), https://tc39.es/ecma262/#sec-arraybufferbytelength
inline size_t array_buffer_byte_length(ArrayBuffer const& array_buffer, ArrayBuffer::Order)
{
    // FIXME: 1. If IsSharedArrayBuffer(arrayBuffer) is true and arrayBuffer has an [[ArrayBufferByteLengthData]] internal slot, then
    // FIXME:     a. Let bufferByteLengthBlock be arrayBuffer.[[ArrayBufferByteLengthData]].
    // FIXME:     b. Let rawLength be GetRawBytesFromSharedBlock(bufferByteLengthBlock, 0, biguint64, true, order).
    // FIXME:     c. Let isLittleEndian be the value of the [[LittleEndian]] field of the surrounding agent's Agent Record.
    // FIXME:     d. Return ℝ(RawBytesToNumeric(biguint64, rawLength, isLittleEndian)).

    // 2. Assert: IsDetachedBuffer(arrayBuffer) is false.
    VERIFY(!array_buffer.is_detached());

    // 3. Return arrayBuffer.[[ArrayBufferByteLength]].
    return array_buffer.byte_length();
}

// 25.1.3.14 RawBytesToNumeric ( type, rawBytes, isLittleEndian ), https://tc39.es/ecma262/#sec-rawbytestonumeric
template<typename T>
static Value raw_bytes_to_numeric(VM& vm, Bytes raw_value, bool is_little_endian)
{
    // 1. Let elementSize be the Element Size value specified in Table 70 for Element Type type.
    //    NOTE: Used in step 7, but not needed with our implementation of that step.

    // 2. If isLittleEndian is false, reverse the order of the elements of rawBytes.
    if (!is_little_endian) {
        VERIFY(raw_value.size() % 2 == 0);
        for (size_t i = 0; i < raw_value.size() / 2; ++i)
            swap(raw_value[i], raw_value[raw_value.size() - 1 - i]);
    }

    using UnderlyingBufferDataType = Conditional<IsSame<ClampedU8, T>, u8, T>;

    // 3. If type is Float16, then
    if constexpr (IsSame<UnderlyingBufferDataType, f16>) {
        // a. Let value be the byte elements of rawBytes concatenated and interpreted as a little-endian bit string encoding of an IEEE 754-2019 binary16 value.
        f16 value;
        raw_value.copy_to({ &value, sizeof(f16) });

        // b. If value is an IEEE 754-2019 binary16 NaN value, return the NaN Number value.
        if (isnan(static_cast<double>(value)))
            return js_nan();

        // c. Return the Number value that corresponds to value.
        return Value(value);
    }

    // 4. If type is Float32, then
    if constexpr (IsSame<UnderlyingBufferDataType, float>) {
        // a. Let value be the byte elements of rawBytes concatenated and interpreted as a little-endian bit string encoding of an IEEE 754-2019 binary32 value.
        float value;
        raw_value.copy_to({ &value, sizeof(float) });

        // b. If value is an IEEE 754-2019 binary32 NaN value, return the NaN Number value.
        if (isnan(value))
            return js_nan();

        // c. Return the Number value that corresponds to value.
        return Value(value);
    }

    // 5. If type is Float64, then
    if constexpr (IsSame<UnderlyingBufferDataType, double>) {
        // a. Let value be the byte elements of rawBytes concatenated and interpreted as a little-endian bit string encoding of an IEEE 754-2019 binary64 value.
        double value;
        raw_value.copy_to({ &value, sizeof(double) });

        // b. If value is an IEEE 754-2019 binary64 NaN value, return the NaN Number value.
        if (isnan(value))
            return js_nan();

        // c. Return the Number value that corresponds to value.
        return Value(value);
    }

    // NOTE: Not in spec, sanity check for steps below.
    if constexpr (!IsIntegral<UnderlyingBufferDataType>)
        VERIFY_NOT_REACHED();

    // 6. If IsUnsignedElementType(type) is true, then
    //     a. Let intValue be the byte elements of rawBytes concatenated and interpreted as a bit string encoding of an unsigned little-endian binary number.
    // 7. Else,
    //     a. Let intValue be the byte elements of rawBytes concatenated and interpreted as a bit string encoding of a binary little-endian two's complement number of bit length elementSize × 8.
    //
    // NOTE: The signed/unsigned logic above is implemented in step 7 by the IsSigned<> check, and in step 8 by JS::Value constructor overloads.
    UnderlyingBufferDataType int_value = 0;
    raw_value.copy_to({ &int_value, sizeof(UnderlyingBufferDataType) });

    // 8. If IsBigIntElementType(type) is true, return the BigInt value that corresponds to intValue.
    if constexpr (sizeof(UnderlyingBufferDataType) == 8) {
        if constexpr (IsSigned<UnderlyingBufferDataType>) {
            static_assert(IsSame<UnderlyingBufferDataType, i64>);
            return BigInt::create(vm, Crypto::SignedBigInteger { int_value });
        } else {
            static_assert(IsOneOf<UnderlyingBufferDataType, u64, double>);
            return BigInt::create(vm, Crypto::SignedBigInteger { Crypto::UnsignedBigInteger { int_value } });
        }
    }
    // 9. Otherwise, return the Number value that corresponds to intValue.
    else {
        return Value(int_value);
    }
}

// 25.1.3.16 GetValueFromBuffer ( arrayBuffer, byteIndex, type, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-getvaluefrombuffer
template<typename T>
Value ArrayBuffer::get_value(size_t byte_index, [[maybe_unused]] bool is_typed_array, Order, bool is_little_endian)
{
    auto& vm = this->vm();
    // 1. Assert: IsDetachedBuffer(arrayBuffer) is false.
    VERIFY(!is_detached());

    // 2. Assert: There are sufficient bytes in arrayBuffer starting at byteIndex to represent a value of type.
    VERIFY(byte_index <= m_data_block.size());
    VERIFY(sizeof(T) <= m_data_block.size() - byte_index);

    // 3. Let block be arrayBuffer.[[ArrayBufferData]].
    // 4. Let elementSize be the Element Size value specified in Table 70 for Element Type type.
    auto element_size = sizeof(T);

    AK::Array<u8, sizeof(T)> raw_value {};

    // FIXME: 5. If IsSharedArrayBuffer(arrayBuffer) is true, then
    if (false) {
        // FIXME: a. Let execution be the [[CandidateExecution]] field of the surrounding agent's Agent Record.
        // FIXME: b. Let eventsRecord be the Agent Events Record of execution.[[EventsRecords]] whose [[AgentSignifier]] is AgentSignifier().
        // FIXME: c. If isTypedArray is true and IsNoTearConfiguration(type, order) is true, let noTear be true; otherwise let noTear be false.
        // FIXME: d. Let rawValue be a List of length elementSize whose elements are nondeterministically chosen byte values.
        // FIXME: e. NOTE: In implementations, rawValue is the result of a non-atomic or atomic read instruction on the underlying hardware. The nondeterminism is a semantic prescription of the memory model to describe observable behaviour of hardware with weak consistency.
        // FIXME: f. Let readEvent be ReadSharedMemory { [[Order]]: order, [[NoTear]]: noTear, [[Block]]: block, [[ByteIndex]]: byteIndex, [[ElementSize]]: elementSize }.
        // FIXME: g. Append readEvent to eventsRecord.[[EventList]].
        // FIXME: h. Append Chosen Value Record { [[Event]]: readEvent, [[ChosenValue]]: rawValue } to execution.[[ChosenValues]].
    }
    // 6. Else,
    else {
        // a. Let rawValue be a List whose elements are bytes from block at indices in the interval from byteIndex (inclusive) to byteIndex + elementSize (exclusive).
        m_data_block.copy_to(byte_index, raw_value.span());
    }

    // 7. Assert: The number of elements in rawValue is elementSize.
    VERIFY(raw_value.size() == element_size);

    // 8. If isLittleEndian is not present, set isLittleEndian to the value of the [[LittleEndian]] field of the surrounding agent's Agent Record.
    //    NOTE: Done by default parameter at declaration of this function.

    // 9. Return RawBytesToNumeric(type, rawValue, isLittleEndian).
    return raw_bytes_to_numeric<T>(vm, raw_value, is_little_endian);
}

// 25.1.3.17 NumericToRawBytes ( type, value, isLittleEndian ), https://tc39.es/ecma262/#sec-numerictorawbytes
template<typename T>
static void numeric_to_raw_bytes(VM& vm, Value value, bool is_little_endian, Bytes raw_bytes)
{
    VERIFY(value.is_number() || value.is_bigint());
    using UnderlyingBufferDataType = Conditional<IsSame<ClampedU8, T>, u8, T>;
    VERIFY(raw_bytes.size() == sizeof(UnderlyingBufferDataType));
    auto flip_if_needed = [&]() {
        if (is_little_endian)
            return;
        VERIFY(sizeof(UnderlyingBufferDataType) % 2 == 0);
        for (size_t i = 0; i < sizeof(UnderlyingBufferDataType) / 2; ++i)
            swap(raw_bytes[i], raw_bytes[sizeof(UnderlyingBufferDataType) - 1 - i]);
    };
    if constexpr (IsSame<UnderlyingBufferDataType, f16>) {
        auto raw_value = static_cast<f16>(MUST(value.to_double(vm)));
        ReadonlyBytes { &raw_value, sizeof(f16) }.copy_to(raw_bytes);
        flip_if_needed();
        return;
    }
    if constexpr (IsSame<UnderlyingBufferDataType, float>) {
        float raw_value = MUST(value.to_double(vm));
        ReadonlyBytes { &raw_value, sizeof(float) }.copy_to(raw_bytes);
        flip_if_needed();
        return;
    }
    if constexpr (IsSame<UnderlyingBufferDataType, double>) {
        double raw_value = MUST(value.to_double(vm));
        ReadonlyBytes { &raw_value, sizeof(double) }.copy_to(raw_bytes);
        flip_if_needed();
        return;
    }
    if constexpr (!IsIntegral<UnderlyingBufferDataType>)
        VERIFY_NOT_REACHED();
    if constexpr (sizeof(UnderlyingBufferDataType) == 8) {
        UnderlyingBufferDataType int_value;

        if constexpr (IsSigned<UnderlyingBufferDataType>)
            int_value = MUST(value.to_bigint_int64(vm));
        else
            int_value = MUST(value.to_bigint_uint64(vm));

        ReadonlyBytes { &int_value, sizeof(UnderlyingBufferDataType) }.copy_to(raw_bytes);
        flip_if_needed();
        return;
    } else {
        UnderlyingBufferDataType int_value;
        if constexpr (IsSigned<UnderlyingBufferDataType>) {
            if constexpr (sizeof(UnderlyingBufferDataType) == 4)
                int_value = MUST(value.to_i32(vm));
            else if constexpr (sizeof(UnderlyingBufferDataType) == 2)
                int_value = MUST(value.to_i16(vm));
            else
                int_value = MUST(value.to_i8(vm));
        } else {
            if constexpr (sizeof(UnderlyingBufferDataType) == 4)
                int_value = MUST(value.to_u32(vm));
            else if constexpr (sizeof(UnderlyingBufferDataType) == 2)
                int_value = MUST(value.to_u16(vm));
            else if constexpr (!IsSame<T, ClampedU8>)
                int_value = MUST(value.to_u8(vm));
            else
                int_value = MUST(value.to_u8_clamp(vm));
        }
        ReadonlyBytes { &int_value, sizeof(UnderlyingBufferDataType) }.copy_to(raw_bytes);
        if constexpr (sizeof(UnderlyingBufferDataType) % 2 == 0)
            flip_if_needed();
        return;
    }
}

// 25.1.3.18 SetValueInBuffer ( arrayBuffer, byteIndex, type, value, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-setvalueinbuffer
template<typename T>
void ArrayBuffer::set_value(size_t byte_index, Value value, [[maybe_unused]] bool is_typed_array, Order, bool is_little_endian)
{
    auto& vm = this->vm();

    // 1. Assert: IsDetachedBuffer(arrayBuffer) is false.
    VERIFY(!is_detached());

    // 2. Assert: There are sufficient bytes in arrayBuffer starting at byteIndex to represent a value of type.
    VERIFY(byte_index <= m_data_block.size());
    VERIFY(sizeof(T) <= m_data_block.size() - byte_index);

    // 3. Assert: value is a BigInt if IsBigIntElementType(type) is true; otherwise, value is a Number.
    if constexpr (IsIntegral<T> && sizeof(T) == 8)
        VERIFY(value.is_bigint());
    else
        VERIFY(value.is_number());

    // FIXME: 5. Let elementSize be the Element Size value specified in Table 70 for Element Type type.

    // 6. If isLittleEndian is not present, set isLittleEndian to the value of the [[LittleEndian]] field of the surrounding agent's Agent Record.
    //    NOTE: Done by default parameter at declaration of this function.

    // 7. Let rawBytes be NumericToRawBytes(type, value, isLittleEndian).
    AK::Array<u8, sizeof(T)> raw_bytes;
    numeric_to_raw_bytes<T>(vm, value, is_little_endian, raw_bytes);

    // FIXME: 8. If IsSharedArrayBuffer(arrayBuffer) is true, then
    if (false) {
        // FIXME: a. Let execution be the [[CandidateExecution]] field of the surrounding agent's Agent Record.
        // FIXME: b. Let eventsRecord be the Agent Events Record of execution.[[EventsRecords]] whose [[AgentSignifier]] is AgentSignifier().
        // FIXME: c. If isTypedArray is true and IsNoTearConfiguration(type, order) is true, let noTear be true; otherwise let noTear be false.
        // FIXME: d. Append WriteSharedMemory { [[Order]]: order, [[NoTear]]: noTear, [[Block]]: block, [[ByteIndex]]: byteIndex, [[ElementSize]]: elementSize, [[Payload]]: rawBytes } to eventsRecord.[[EventList]].
    }
    // 9. Else,
    else {
        // a. Store the individual bytes of rawBytes into block, starting at block[byteIndex].
        m_data_block.overwrite(byte_index, raw_bytes.data(), raw_bytes.size());
    }

    // 10. Return unused.
}

// 25.1.3.19 GetModifySetValueInBuffer ( arrayBuffer, byteIndex, type, value, op [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-getmodifysetvalueinbuffer
template<typename T>
Value ArrayBuffer::get_modify_set_value(size_t byte_index, Value value, ReadWriteModifyFunction operation, bool is_little_endian)
{
    auto& vm = this->vm();

    auto raw_bytes = MUST(ByteBuffer::create_uninitialized(sizeof(T)));
    numeric_to_raw_bytes<T>(vm, value, is_little_endian, raw_bytes);

    // FIXME: Check for shared buffer

    auto raw_bytes_read = MUST(ByteBuffer::create_uninitialized(sizeof(T)));
    m_data_block.copy_to(byte_index, raw_bytes_read);
    auto raw_bytes_modified = operation(raw_bytes_read, raw_bytes);
    m_data_block.overwrite(byte_index, raw_bytes_modified.data(), raw_bytes_modified.size());

    return raw_bytes_to_numeric<T>(vm, raw_bytes_read, is_little_endian);
}

}
