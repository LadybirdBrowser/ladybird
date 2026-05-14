/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NumericLimits.h>
#include <AK/Vector.h>
#include <LibJS/Export.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/PropertyAttributes.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS {

struct PropertyMetadata {
    u32 offset { 0 };
    PropertyAttributes attributes { 0 };
};

class JS_API DescriptorArray final : public Cell {
    GC_CELL(DescriptorArray, Cell);
    GC_DECLARE_ALLOCATOR(DescriptorArray);

public:
    struct Entry {
        PropertyKey property_key;
        u32 offset { 0 };
        u16 attributes { 0 };
        u16 enum_index { 0 };

        PropertyMetadata metadata() const
        {
            return { offset, static_cast<u8>(attributes) };
        }
    };

    static constexpr u32 max_descriptor_count = NumericLimits<u16>::max() + 1;

    DescriptorArray() = default;
    explicit DescriptorArray(DescriptorArray const&, u32 descriptor_count);
    virtual ~DescriptorArray() override = default;

    [[nodiscard]] u32 size() const { return m_entries.size(); }
    [[nodiscard]] Optional<PropertyMetadata> lookup(PropertyKey const&, u32 descriptor_count) const;

    void set(PropertyKey const&, PropertyMetadata, u32 enum_index);
    void set_attributes(PropertyKey const&, PropertyAttributes, u32 descriptor_count);
    void remove(PropertyKey const&, u32 descriptor_count);

    void for_each_in_insertion_order(Function<void(PropertyKey const&, PropertyMetadata const&)> const&, u32 descriptor_count) const;

private:
    virtual void visit_edges(Visitor&) override;
    virtual size_t external_memory_size() const override;

    [[nodiscard]] Optional<size_t> find(PropertyKey const&, u32 descriptor_count) const;
    [[nodiscard]] size_t find_insertion_index(PropertyKey const&) const;

    Vector<Entry> m_entries;
    Vector<u32> m_entry_indices_by_enum_index;
};

static_assert(sizeof(DescriptorArray::Entry) == 16);

}
