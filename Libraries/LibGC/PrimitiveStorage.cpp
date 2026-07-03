/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NeverDestroyed.h>
#include <AK/ScopeGuard.h>
#include <LibCore/System.h>
#include <LibGC/PrimitiveStorage.h>

extern "C" {
GC_API FlatPtr js_primitive_storage_cage_base = 0;
}

namespace GC {

static constexpr size_t small_slab_size = 64 * KiB;
static constexpr size_t minimum_small_slot_size = 16;
static constexpr size_t maximum_small_slot_size = 64 * KiB;

static size_t round_up_to_page(size_t value)
{
    auto page = static_cast<size_t>(PAGE_SIZE);
    return align_up_to(value, page);
}

static Optional<u16> small_size_class_index(size_t size)
{
    auto slot_size = max(size, minimum_small_slot_size);
    if (slot_size > maximum_small_slot_size)
        return {};

    size_t class_size = minimum_small_slot_size;
    u16 index = 0;
    while (class_size < slot_size) {
        class_size <<= 1;
        ++index;
    }
    return index;
}

static size_t small_slot_size_for_class(u16 index)
{
    return minimum_small_slot_size << index;
}

PrimitiveStorage& PrimitiveStorage::the()
{
    static NeverDestroyed<PrimitiveStorage> storage;
    return *storage;
}

ErrorOr<void> PrimitiveStorage::ensure_cage()
{
    return m_allocator.ensure_cage();
}

size_t PrimitiveStorage::Allocator::page_size()
{
    return static_cast<size_t>(PAGE_SIZE);
}

ErrorOr<void> PrimitiveStorage::Allocator::ensure_cage()
{
    if (m_cage_base)
        return {};

    if constexpr (sizeof(FlatPtr) < sizeof(u64))
        return Error::from_errno(ENOMEM);

    m_cage_size = PrimitiveStorage::default_cage_size;
    Checked<size_t> reservation_size = m_cage_size;
    reservation_size += page_size();
    if (reservation_size.has_overflow())
        return Error::from_errno(ENOMEM);

    // Leave an inaccessible page after the logical cage so a masked fixed-width
    // access at the top edge cannot cross into an unrelated mapping.
    auto mapping = TRY(Core::System::reserve_address_space(reservation_size.value()));
    m_cage_base = static_cast<u8*>(mapping);
    js_primitive_storage_cage_base = bit_cast<FlatPtr>(m_cage_base);
    return {};
}

ErrorOr<PrimitiveStorage::Allocator::Allocation> PrimitiveStorage::Allocator::allocate(size_t size, size_t capacity, ZeroFillNewBytes zero_fill_new_bytes, size_t guard_size, bool force_large)
{
    VERIFY(size <= capacity);
    if (!force_large && guard_size == 0 && size == capacity && small_size_class_index(size).has_value())
        return allocate_small_storage(size, zero_fill_new_bytes);
    return allocate_large_storage(size, capacity, zero_fill_new_bytes, guard_size);
}

ErrorOr<PrimitiveStorage::Allocator::Allocation> PrimitiveStorage::Allocator::allocate_small_storage(size_t size, ZeroFillNewBytes zero_fill_new_bytes)
{
    auto size_class_index = small_size_class_index(size);
    VERIFY(size_class_index.has_value());

    TRY(ensure_cage());

    auto slot_size = small_slot_size_for_class(*size_class_index);
    auto& slabs = m_small_slabs[*size_class_index];

    for (u32 slab_index = 0; slab_index < slabs.size(); ++slab_index) {
        auto& slab = slabs[slab_index];
        if (slab.free_slots.is_empty())
            continue;

        auto slot_index = slab.free_slots.take_last();
        ++slab.used_slot_count;
        auto offset = slab.offset + slot_index * slot_size;
        if (zero_fill_new_bytes == ZeroFillNewBytes::Yes)
            __builtin_memset(m_cage_base + offset, 0, size);
        return Allocation {
            .offset = offset,
            .capacity = slot_size,
            .reservation_size = slot_size,
            .committed_size = slot_size,
            .small_allocation = SmallAllocation { *size_class_index, static_cast<u16>(slab_index), slot_index },
        };
    }

    return allocate_from_new_slab(*size_class_index, slot_size, zero_fill_new_bytes, size);
}

ErrorOr<PrimitiveStorage::Allocator::Allocation> PrimitiveStorage::Allocator::allocate_from_new_slab(u16 size_class_index, size_t slot_size, ZeroFillNewBytes zero_fill_new_bytes, size_t requested_size)
{
    auto& slabs = m_small_slabs[size_class_index];
    if (slabs.size() >= NumericLimits<u16>::max())
        return Error::from_errno(ENOMEM);

    auto slab_offset = TRY(allocate_cage_range(small_slab_size));
    auto commit_result = Core::System::commit_memory(m_cage_base + slab_offset, small_slab_size);
    if (commit_result.is_error()) {
        release_cage_range(slab_offset, small_slab_size);
        return commit_result.release_error();
    }
    auto release_slab_on_error = ArmedScopeGuard([&] {
        release_cage_range(slab_offset, small_slab_size);
    });

    Slab slab;
    slab.offset = slab_offset;
    slab.slot_size = slot_size;
    slab.slot_count = static_cast<u32>(small_slab_size / slot_size);
    slab.used_slot_count = 1;
    slab.free_slots.ensure_capacity(slab.slot_count);
    for (u32 slot = slab.slot_count - 1; slot > 0; --slot)
        slab.free_slots.unchecked_append(slot);

    auto slab_index = static_cast<u16>(slabs.size());
    slabs.append(move(slab));
    release_slab_on_error.disarm();

    if (zero_fill_new_bytes == ZeroFillNewBytes::Yes)
        __builtin_memset(m_cage_base + slab_offset, 0, requested_size);

    return Allocation {
        .offset = slab_offset,
        .capacity = slot_size,
        .reservation_size = slot_size,
        .committed_size = slot_size,
        .small_allocation = SmallAllocation { size_class_index, slab_index, 0 },
    };
}

ErrorOr<PrimitiveStorage::Allocator::Allocation> PrimitiveStorage::Allocator::allocate_large_storage(size_t size, size_t capacity, ZeroFillNewBytes zero_fill_new_bytes, size_t guard_size)
{
    TRY(ensure_cage());

    auto rounded_capacity = round_up_to_page(capacity);
    auto rounded_guard_size = round_up_to_page(guard_size);
    Checked<size_t> reservation_size = rounded_capacity;
    reservation_size += rounded_guard_size;
    if (reservation_size.has_overflow())
        return Error::from_errno(ENOMEM);

    auto offset = TRY(allocate_cage_range(reservation_size.value()));
    auto committed_size = round_up_to_page(size);
    if (committed_size > 0) {
        auto commit_result = Core::System::commit_memory(m_cage_base + offset, committed_size);
        if (commit_result.is_error()) {
            release_cage_range(offset, reservation_size.value());
            return commit_result.release_error();
        }
    }
    if (zero_fill_new_bytes == ZeroFillNewBytes::Yes && size > 0)
        __builtin_memset(m_cage_base + offset, 0, size);

    return Allocation {
        .offset = offset,
        .capacity = capacity,
        .reservation_size = reservation_size.value(),
        .committed_size = committed_size,
        .small_allocation = {},
    };
}

ErrorOr<size_t> PrimitiveStorage::Allocator::allocate_cage_range(size_t reservation_size)
{
    VERIFY(m_cage_base);
    reservation_size = round_up_to_page(reservation_size);
    if (reservation_size == 0)
        return m_next_offset;

    for (size_t i = 0; i < m_free_ranges.size(); ++i) {
        auto range = m_free_ranges[i];
        if (range.size < reservation_size)
            continue;

        auto offset = range.offset;
        m_free_ranges[i].offset += reservation_size;
        m_free_ranges[i].size -= reservation_size;
        if (m_free_ranges[i].size == 0)
            m_free_ranges.remove(i);
        return offset;
    }

    auto next_offset = align_up_to(m_next_offset, page_size());
    Checked<size_t> new_next_offset = next_offset;
    new_next_offset += reservation_size;
    if (new_next_offset.has_overflow() || new_next_offset.value() > m_cage_size)
        return Error::from_errno(ENOMEM);

    m_next_offset = new_next_offset.value();
    return next_offset;
}

ErrorOr<void> PrimitiveStorage::Allocator::resize(Allocation& allocation, size_t old_size, size_t new_size, ZeroFillNewBytes zero_fill_new_bytes)
{
    VERIFY(new_size <= allocation.capacity);

    if (!allocation.small_allocation.has_value())
        TRY(commit_large_storage(allocation, new_size));

    if (zero_fill_new_bytes == ZeroFillNewBytes::Yes && new_size > old_size)
        __builtin_memset(data(allocation, old_size), 0, new_size - old_size);

    return {};
}

ErrorOr<void> PrimitiveStorage::Allocator::commit_large_storage(Allocation& allocation, size_t new_size)
{
    VERIFY(!allocation.small_allocation.has_value());
    VERIFY(new_size <= allocation.capacity);

    auto new_committed_size = round_up_to_page(new_size);
    if (new_committed_size > allocation.committed_size) {
        TRY(Core::System::commit_memory(m_cage_base + allocation.offset + allocation.committed_size, new_committed_size - allocation.committed_size));
        allocation.committed_size = new_committed_size;
    }

    return {};
}

void PrimitiveStorage::Allocator::decommit_large_storage(Allocation const& allocation)
{
    VERIFY(!allocation.small_allocation.has_value());
    if (allocation.committed_size == 0)
        return;
    MUST(Core::System::decommit_memory(m_cage_base + allocation.offset, allocation.committed_size));
}

ErrorOr<PrimitiveStorage::Allocator::Allocation> PrimitiveStorage::Allocator::reallocate(Allocation const& old_allocation, size_t old_size, size_t new_size, size_t new_capacity, ZeroFillNewBytes zero_fill_new_bytes, bool force_large)
{
    VERIFY(new_size <= new_capacity);

    auto allocation = TRY(allocate(new_size, new_capacity, ZeroFillNewBytes::No, 0, force_large));
    auto bytes_to_copy = min(old_size, new_size);
    if (bytes_to_copy > 0)
        __builtin_memcpy(m_cage_base + allocation.offset, m_cage_base + old_allocation.offset, bytes_to_copy);
    if (zero_fill_new_bytes == ZeroFillNewBytes::Yes && new_size > old_size)
        __builtin_memset(m_cage_base + allocation.offset + old_size, 0, new_size - old_size);

    return allocation;
}

void PrimitiveStorage::Allocator::deallocate(Allocation& allocation)
{
    if (auto small_allocation = allocation.small_allocation; small_allocation.has_value()) {
        auto& slabs = m_small_slabs[small_allocation->size_class_index];
        VERIFY(small_allocation->slab_index < slabs.size());
        auto& slab = slabs[small_allocation->slab_index];
        VERIFY(slab.free_slots.size() < slab.slot_count);
        slab.free_slots.unchecked_append(small_allocation->slot_index);
        VERIFY(slab.used_slot_count > 0);
        --slab.used_slot_count;
        allocation = {};
        return;
    }

    if (allocation.reservation_size > 0) {
        decommit_large_storage(allocation);
        release_cage_range(allocation.offset, allocation.reservation_size);
    }
    allocation = {};
}

void PrimitiveStorage::Allocator::release_cage_range(size_t offset, size_t size)
{
    size = round_up_to_page(size);
    if (size == 0)
        return;

    if (offset <= m_next_offset && size == m_next_offset - offset) {
        m_next_offset = offset;
        reclaim_free_ranges_at_top();
        return;
    }

    free_cage_range(offset, size);
}

void PrimitiveStorage::Allocator::free_cage_range(size_t offset, size_t size)
{
    size = round_up_to_page(size);
    if (size == 0)
        return;

    size_t insert_before = 0;
    while (insert_before < m_free_ranges.size() && m_free_ranges[insert_before].offset < offset)
        ++insert_before;

    m_free_ranges.insert(insert_before, FreeRange { offset, size });

    if (insert_before > 0) {
        auto& previous = m_free_ranges[insert_before - 1];
        auto& current = m_free_ranges[insert_before];
        if (previous.offset + previous.size == current.offset) {
            previous.size += current.size;
            m_free_ranges.remove(insert_before);
            --insert_before;
        }
    }

    if (insert_before + 1 < m_free_ranges.size()) {
        auto& current = m_free_ranges[insert_before];
        auto& next = m_free_ranges[insert_before + 1];
        if (current.offset + current.size == next.offset) {
            current.size += next.size;
            m_free_ranges.remove(insert_before + 1);
        }
    }

    reclaim_free_ranges_at_top();
}

void PrimitiveStorage::Allocator::reclaim_free_ranges_at_top()
{
    while (!m_free_ranges.is_empty()) {
        auto range = m_free_ranges.last();
        if (range.offset > m_next_offset || range.size != m_next_offset - range.offset)
            return;

        m_next_offset = range.offset;
        m_free_ranges.remove(m_free_ranges.size() - 1);
    }
}

bool PrimitiveStorage::Allocator::contains(void const* address, size_t size) const
{
    if (!m_cage_base || !address)
        return false;
    auto address_value = bit_cast<FlatPtr>(address);
    auto base = bit_cast<FlatPtr>(m_cage_base);
    if (address_value < base)
        return false;
    auto offset = address_value - base;
    return offset <= m_cage_size && size <= m_cage_size - offset;
}

bool PrimitiveStorage::Allocator::contains(Allocation const& allocation, void const* address, size_t size) const
{
    if (!address || allocation.offset == invalid_offset)
        return false;
    auto address_value = bit_cast<FlatPtr>(address);
    auto base = bit_cast<FlatPtr>(m_cage_base);
    if (!m_cage_base || address_value < base)
        return false;
    auto offset = address_value - base;
    if (offset < allocation.offset)
        return false;
    auto allocation_offset = offset - allocation.offset;
    return allocation_offset <= allocation.reservation_size && size <= allocation.reservation_size - allocation_offset;
}

u8* PrimitiveStorage::Allocator::data(Allocation const& allocation, size_t byte_offset) const
{
    VERIFY(m_cage_base);
    VERIFY(allocation.offset != invalid_offset);
    return m_cage_base + PrimitiveStorage::mask_offset(allocation.offset + byte_offset);
}

ErrorOr<PrimitiveStorageHandle> PrimitiveStorage::try_allocate(size_t size, ZeroFillNewBytes zero_fill_new_bytes)
{
    auto allocation = TRY(m_allocator.allocate(size, size, zero_fill_new_bytes, 0, false));
    return install_allocation(move(allocation), size);
}

ErrorOr<PrimitiveStorageHandle> PrimitiveStorage::try_reserve(size_t size, size_t capacity, ZeroFillNewBytes zero_fill_new_bytes, size_t guard_size)
{
    if (size > capacity)
        return Error::from_errno(ENOMEM);

    auto allocation = TRY(m_allocator.allocate(size, capacity, zero_fill_new_bytes, guard_size, true));
    return install_allocation(move(allocation), size);
}

PrimitiveStorageHandle PrimitiveStorage::install_allocation(Allocator::Allocation allocation, size_t size)
{
    u32 index;
    if (m_free_entry_indices.is_empty()) {
        VERIFY(m_entries.size() < PrimitiveStorageHandle::invalid_index);
        index = static_cast<u32>(m_entries.size());
        m_entries.append({});
    } else {
        index = m_free_entry_indices.take_last();
    }

    auto& entry = m_entries[index];
    VERIFY(!entry.allocated);
    entry.allocated = true;
    entry.size = size;
    entry.allocation = move(allocation);

    return PrimitiveStorageHandle { index, entry.generation };
}

bool PrimitiveStorage::is_valid(PrimitiveStorageHandle handle) const
{
    return entry_for(handle) != nullptr;
}

bool PrimitiveStorage::contains(void const* address, size_t size) const
{
    return m_allocator.contains(address, size);
}

bool PrimitiveStorage::contains(PrimitiveStorageHandle handle, void const* address, size_t size) const
{
    auto const* entry = entry_for(handle);
    return entry && m_allocator.contains(entry->allocation, address, size);
}

size_t PrimitiveStorage::offset(PrimitiveStorageHandle handle) const
{
    auto const* entry = entry_for(handle);
    return entry ? entry->allocation.offset : invalid_offset;
}

size_t PrimitiveStorage::size(PrimitiveStorageHandle handle) const
{
    auto const* entry = entry_for(handle);
    return entry ? entry->size : 0;
}

size_t PrimitiveStorage::capacity(PrimitiveStorageHandle handle) const
{
    auto const* entry = entry_for(handle);
    return entry ? entry->allocation.capacity : 0;
}

size_t PrimitiveStorage::committed_size(PrimitiveStorageHandle handle) const
{
    auto const* entry = entry_for(handle);
    return entry ? entry->allocation.committed_size : 0;
}

u8* PrimitiveStorage::data(PrimitiveStorageHandle handle)
{
    auto* entry = entry_for(handle);
    return entry ? data_unchecked(*entry) : nullptr;
}

u8 const* PrimitiveStorage::data(PrimitiveStorageHandle handle) const
{
    return const_cast<PrimitiveStorage&>(*this).data(handle);
}

u8* PrimitiveStorage::data(PrimitiveStorageHandle handle, size_t byte_offset)
{
    auto* entry = entry_for(handle);
    return entry ? m_allocator.data(entry->allocation, byte_offset) : nullptr;
}

u8 const* PrimitiveStorage::data(PrimitiveStorageHandle handle, size_t byte_offset) const
{
    return const_cast<PrimitiveStorage&>(*this).data(handle, byte_offset);
}

ErrorOr<void> PrimitiveStorage::try_resize(PrimitiveStorageHandle handle, size_t new_size, ZeroFillNewBytes zero_fill_new_bytes)
{
    auto* entry = entry_for(handle);
    if (!entry)
        return Error::from_errno(EINVAL);

    if (new_size <= entry->allocation.capacity) {
        TRY(m_allocator.resize(entry->allocation, entry->size, new_size, zero_fill_new_bytes));
        entry->size = new_size;
        return {};
    }

    return reallocate_entry(*entry, new_size, new_size, zero_fill_new_bytes, false);
}

ErrorOr<void> PrimitiveStorage::try_reserve(PrimitiveStorageHandle handle, size_t new_capacity)
{
    auto* entry = entry_for(handle);
    if (!entry)
        return Error::from_errno(EINVAL);
    if (new_capacity <= entry->allocation.capacity)
        return {};

    return reallocate_entry(*entry, entry->size, new_capacity, ZeroFillNewBytes::No, true);
}

ErrorOr<void> PrimitiveStorage::try_resize_and_reserve(PrimitiveStorageHandle handle, size_t new_size, size_t new_capacity, ZeroFillNewBytes zero_fill_new_bytes)
{
    if (new_size > new_capacity)
        return Error::from_errno(ENOMEM);

    auto* entry = entry_for(handle);
    if (!entry)
        return Error::from_errno(EINVAL);

    if (new_capacity <= entry->allocation.capacity) {
        TRY(m_allocator.resize(entry->allocation, entry->size, new_size, zero_fill_new_bytes));
        entry->size = new_size;
        return {};
    }

    return reallocate_entry(*entry, new_size, new_capacity, zero_fill_new_bytes, true);
}

ErrorOr<void> PrimitiveStorage::reallocate_entry(Entry& entry, size_t new_size, size_t new_capacity, ZeroFillNewBytes zero_fill_new_bytes, bool force_large)
{
    VERIFY(new_size <= new_capacity);
    auto allocation = TRY(m_allocator.reallocate(entry.allocation, entry.size, new_size, new_capacity, zero_fill_new_bytes, force_large));
    m_allocator.deallocate(entry.allocation);
    entry.allocation = move(allocation);
    entry.size = new_size;
    return {};
}

void PrimitiveStorage::free(PrimitiveStorageHandle handle)
{
    auto* entry = entry_for(handle);
    if (!entry)
        return;

    m_allocator.deallocate(entry->allocation);
    entry->allocated = false;
    entry->size = 0;
    ++entry->generation;
    if (entry->generation == 0)
        ++entry->generation;
    m_free_entry_indices.append(handle.index);
}

PrimitiveStorage::Entry* PrimitiveStorage::entry_for(PrimitiveStorageHandle handle)
{
    if (!handle.is_valid() || handle.index >= m_entries.size())
        return nullptr;
    auto& entry = m_entries[handle.index];
    if (!entry.allocated || entry.generation != handle.generation)
        return nullptr;
    return &entry;
}

PrimitiveStorage::Entry const* PrimitiveStorage::entry_for(PrimitiveStorageHandle handle) const
{
    return const_cast<PrimitiveStorage&>(*this).entry_for(handle);
}

u8* PrimitiveStorage::data_unchecked(Entry const& entry) const
{
    return m_allocator.data(entry.allocation);
}

}
