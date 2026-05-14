/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <LibJS/Runtime/DescriptorArray.h>
#include <LibJS/Runtime/ExternalMemory.h>

namespace JS {

GC_DEFINE_ALLOCATOR(DescriptorArray);

static u32 property_key_hash(PropertyKey const& property_key)
{
    return AK::Traits<PropertyKey>::hash(property_key);
}

DescriptorArray::DescriptorArray(DescriptorArray const& other, u32 descriptor_count)
{
    VERIFY(descriptor_count <= max_descriptor_count);
    m_entries.ensure_capacity(descriptor_count);
    other.for_each_in_insertion_order([&](auto const& property_key, auto const& metadata) {
        set(property_key, metadata, m_entries.size());
    },
        descriptor_count);
}

Optional<PropertyMetadata> DescriptorArray::lookup(PropertyKey const& property_key, u32 descriptor_count) const
{
    auto index = find(property_key, descriptor_count);
    if (!index.has_value())
        return {};
    return m_entries[*index].metadata();
}

void DescriptorArray::set(PropertyKey const& property_key, PropertyMetadata metadata, u32 enum_index)
{
    VERIFY(enum_index < max_descriptor_count);
    if (auto existing_index = find(property_key, enum_index + 1); existing_index.has_value()) {
        auto& entry = m_entries[*existing_index];
        entry.offset = metadata.offset;
        entry.attributes = metadata.attributes.bits();
        return;
    }

    auto insertion_index = find_insertion_index(property_key);
    m_entries.insert(insertion_index, { property_key, metadata.offset, metadata.attributes.bits(), static_cast<u16>(enum_index) });
}

void DescriptorArray::set_attributes(PropertyKey const& property_key, PropertyAttributes attributes, u32 descriptor_count)
{
    auto index = find(property_key, descriptor_count);
    VERIFY(index.has_value());
    m_entries[*index].attributes = attributes.bits();
}

void DescriptorArray::remove(PropertyKey const& property_key, u32 descriptor_count)
{
    auto index = find(property_key, descriptor_count);
    VERIFY(index.has_value());

    auto removed_offset = m_entries[*index].offset;
    auto removed_enum_index = m_entries[*index].enum_index;
    m_entries.remove(*index);

    for (auto& entry : m_entries) {
        if (entry.enum_index >= descriptor_count)
            continue;
        if (entry.offset > removed_offset)
            --entry.offset;
        if (entry.enum_index > removed_enum_index)
            --entry.enum_index;
    }
}

void DescriptorArray::for_each_in_insertion_order(Function<void(PropertyKey const&, PropertyMetadata const&)> const& callback, u32 descriptor_count) const
{
    Vector<Entry const*, 32> entries;
    entries.ensure_capacity(descriptor_count);
    for (auto const& entry : m_entries) {
        if (entry.enum_index < descriptor_count)
            entries.unchecked_append(&entry);
    }

    quick_sort(entries, [](auto const* lhs, auto const* rhs) {
        return lhs->enum_index < rhs->enum_index;
    });

    for (auto const* entry : entries) {
        auto metadata = entry->metadata();
        callback(entry->property_key, metadata);
    }
}

void DescriptorArray::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& entry : m_entries)
        entry.property_key.visit_edges(visitor);
}

size_t DescriptorArray::external_memory_size() const
{
    return vector_external_memory_size(m_entries);
}

Optional<size_t> DescriptorArray::find(PropertyKey const& property_key, u32 descriptor_count) const
{
    if (m_entries.size() <= 16) {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            auto const& entry = m_entries[i];
            if (entry.enum_index < descriptor_count && entry.property_key == property_key)
                return i;
        }
        return {};
    }

    auto hash = property_key_hash(property_key);
    size_t low = 0;
    size_t high = m_entries.size();
    while (low < high) {
        auto middle = low + (high - low) / 2;
        if (property_key_hash(m_entries[middle].property_key) < hash)
            low = middle + 1;
        else
            high = middle;
    }

    for (auto i = low; i < m_entries.size() && property_key_hash(m_entries[i].property_key) == hash; ++i) {
        auto const& entry = m_entries[i];
        if (entry.enum_index < descriptor_count && entry.property_key == property_key)
            return i;
    }
    return {};
}

size_t DescriptorArray::find_insertion_index(PropertyKey const& property_key) const
{
    auto hash = property_key_hash(property_key);
    size_t low = 0;
    size_t high = m_entries.size();
    while (low < high) {
        auto middle = low + (high - low) / 2;
        if (property_key_hash(m_entries[middle].property_key) <= hash)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

}
