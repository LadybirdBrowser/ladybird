/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGC/Export.h>

namespace GC {

struct PrimitiveStorageHandle {
    static constexpr u32 invalid_index = NumericLimits<u32>::max();

    u32 index { invalid_index };
    u32 generation { 0 };

    bool is_valid() const { return index != invalid_index && generation != 0; }
};

class GC_API PrimitiveStorage {
    AK_MAKE_NONCOPYABLE(PrimitiveStorage);
    AK_MAKE_NONMOVABLE(PrimitiveStorage);

public:
    enum class ZeroFillNewBytes {
        No,
        Yes,
    };

    static constexpr size_t default_cage_size = 64ull * GiB;
    static_assert(is_power_of_two(default_cage_size));
    static constexpr size_t cage_offset_mask = default_cage_size - 1;
    static constexpr size_t invalid_offset = NumericLimits<size_t>::max();

    static PrimitiveStorage& the();
    static constexpr size_t mask_offset(size_t offset) { return offset & cage_offset_mask; }

    ErrorOr<PrimitiveStorageHandle> try_allocate(size_t size, ZeroFillNewBytes = ZeroFillNewBytes::Yes);
    ErrorOr<PrimitiveStorageHandle> try_reserve(size_t size, size_t capacity, ZeroFillNewBytes = ZeroFillNewBytes::Yes, size_t guard_size = 0);
    ErrorOr<void> ensure_cage();

    bool is_valid(PrimitiveStorageHandle) const;
    bool contains(void const* address, size_t size = 1) const;
    bool contains(PrimitiveStorageHandle, void const* address, size_t size = 1) const;

    size_t offset(PrimitiveStorageHandle) const;
    size_t size(PrimitiveStorageHandle) const;
    size_t capacity(PrimitiveStorageHandle) const;
    size_t committed_size(PrimitiveStorageHandle) const;

    u8* data(PrimitiveStorageHandle);
    u8 const* data(PrimitiveStorageHandle) const;
    u8* data(PrimitiveStorageHandle, size_t byte_offset);
    u8 const* data(PrimitiveStorageHandle, size_t byte_offset) const;

    ErrorOr<void> try_resize(PrimitiveStorageHandle, size_t new_size, ZeroFillNewBytes = ZeroFillNewBytes::No);
    ErrorOr<void> try_reserve(PrimitiveStorageHandle, size_t new_capacity);
    ErrorOr<void> try_resize_and_reserve(PrimitiveStorageHandle, size_t new_size, size_t new_capacity, ZeroFillNewBytes = ZeroFillNewBytes::No);
    void free(PrimitiveStorageHandle);

    u8* cage_base() { return m_allocator.cage_base(); }
    u8 const* cage_base() const { return m_allocator.cage_base(); }
    size_t cage_size() const { return m_allocator.cage_size(); }

    PrimitiveStorage() = default;

private:
    class Allocator {
    public:
        struct SmallAllocation {
            u16 size_class_index { 0 };
            u16 slab_index { 0 };
            u32 slot_index { 0 };
        };

        struct Allocation {
            size_t offset { invalid_offset };
            size_t capacity { 0 };
            size_t reservation_size { 0 };
            size_t committed_size { 0 };
            Optional<SmallAllocation> small_allocation;
        };

        ErrorOr<Allocation> allocate(size_t size, size_t capacity, ZeroFillNewBytes, size_t guard_size, bool force_large);
        ErrorOr<void> resize(Allocation&, size_t old_size, size_t new_size, ZeroFillNewBytes);
        ErrorOr<Allocation> reallocate(Allocation const&, size_t old_size, size_t new_size, size_t new_capacity, ZeroFillNewBytes, bool force_large);
        void deallocate(Allocation&);
        ErrorOr<void> ensure_cage();

        bool contains(void const* address, size_t size = 1) const;
        bool contains(Allocation const&, void const* address, size_t size = 1) const;
        u8* data(Allocation const&, size_t byte_offset = 0) const;

        u8* cage_base() { return m_cage_base; }
        u8 const* cage_base() const { return m_cage_base; }
        size_t cage_size() const { return m_cage_size; }

    private:
        struct FreeRange {
            size_t offset { 0 };
            size_t size { 0 };
        };

        struct Slab {
            size_t offset { 0 };
            size_t slot_size { 0 };
            u32 slot_count { 0 };
            u32 used_slot_count { 0 };
            Vector<u32> free_slots;
        };

        ErrorOr<Allocation> allocate_small_storage(size_t size, ZeroFillNewBytes);
        ErrorOr<Allocation> allocate_large_storage(size_t size, size_t capacity, ZeroFillNewBytes, size_t guard_size);
        ErrorOr<Allocation> allocate_from_new_slab(u16 size_class_index, size_t slot_size, ZeroFillNewBytes, size_t requested_size);
        ErrorOr<size_t> allocate_cage_range(size_t reservation_size);
        ErrorOr<void> commit_large_storage(Allocation&, size_t new_size);
        void decommit_large_storage(Allocation const&);
        void release_cage_range(size_t offset, size_t size);
        void free_cage_range(size_t offset, size_t size);
        void reclaim_free_ranges_at_top();

        static size_t page_size();

        u8* m_cage_base { nullptr };
        size_t m_cage_size { 0 };
        size_t m_next_offset { 0 };
        Vector<FreeRange> m_free_ranges;
        Vector<Slab> m_small_slabs[13];
    };

    struct Entry {
        u32 generation { 1 };
        bool allocated { false };
        size_t size { 0 };
        Allocator::Allocation allocation;
    };

    PrimitiveStorageHandle install_allocation(Allocator::Allocation, size_t size);
    Entry* entry_for(PrimitiveStorageHandle);
    Entry const* entry_for(PrimitiveStorageHandle) const;
    u8* data_unchecked(Entry const&) const;
    ErrorOr<void> reallocate_entry(Entry&, size_t new_size, size_t new_capacity, ZeroFillNewBytes, bool force_large);

    Allocator m_allocator;
    Vector<Entry> m_entries;
    Vector<u32> m_free_entry_indices;
};

}

extern "C" GC_API FlatPtr js_primitive_storage_cage_base;
