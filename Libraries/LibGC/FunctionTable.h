/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGC/Cell.h>
#include <LibGC/Export.h>
#include <LibGC/ExternalEntityTable.h>

namespace GC {

using FunctionTableHandle = ExternalEntityTableHandle;

// What a GC::Function does with the box that holds its captures. Each kind of function has one static instance. The
// signature identifies the function type it was made for, so a handle that names another function's entry is caught
// before its box is invoked as the wrong type.
struct FunctionOperations {
    u64 const* signature { nullptr };
    void (*visit_edges)(void* box, Cell::Visitor&) { nullptr };
    void (*destroy)(void* box) { nullptr };
};

// The parts of every GC::Function that live outside the GC heap: the operations of its callable and the box that
// holds what it captured. A function cell holds only a handle to its entry, so the cell holds no pointer at all.
class GC_API FunctionTable : private ExternalEntityTable {
    AK_MAKE_NONCOPYABLE(FunctionTable);
    AK_MAKE_NONMOVABLE(FunctionTable);

public:
    // A box this small is kept in its entry, which saves allocating one for most functions.
    static constexpr size_t inline_box_capacity = 48;
    static constexpr size_t box_alignment = 16;

    struct Slot {
        FunctionOperations const* operations { nullptr };
        void* box { nullptr };
        alignas(box_alignment) u8 inline_box[inline_box_capacity];
    };
    static_assert(sizeof(Slot) == 64);

    struct Allocation {
        FunctionTableHandle handle;
        void* box { nullptr };
    };

    FunctionTable() = default;

    // Reserves an entry with room for a box of box_size bytes, which the caller then constructs in place. The box keeps
    // its address until the entry is freed, also while the table grows, as a callable may make functions as it runs.
    ALWAYS_INLINE Allocation allocate(FunctionOperations const& operations, size_t box_size)
    {
        auto handle = allocate_entry(tag);
        if (handle.index == m_chunks.size() * slots_per_chunk)
            m_chunks.append(make<Chunk>());
        auto& slot = slot_at(handle.index);
        slot.operations = &operations;
        if (box_size == 0)
            slot.box = nullptr;
        else if (box_size <= inline_box_capacity)
            slot.box = slot.inline_box;
        else
            slot.box = kmalloc(box_size);
        VERIFY(!box_size || slot.box);
        return { handle, slot.box };
    }

    ALWAYS_INLINE Slot const& slot(FunctionTableHandle handle) const
    {
        VERIFY(is_valid(handle, tag));
        return const_cast<FunctionTable&>(*this).slot_at(handle.index);
    }

    // Releases the entry of a function whose box has been destroyed.
    ALWAYS_INLINE void free(FunctionTableHandle handle)
    {
        VERIFY(is_valid(handle, tag));
        auto& slot = slot_at(handle.index);
        if (slot.box && slot.box != slot.inline_box)
            kfree(slot.box);
        slot.operations = nullptr;
        slot.box = nullptr;
        free_entry(handle, tag);
    }

private:
    static constexpr auto tag = balanced_external_entity_table_tag(1);
    static constexpr size_t slots_per_chunk = 256;

    struct Chunk {
        AK_ALLOC_WITH_KMALLOC;
        Slot slots[slots_per_chunk];
    };

    ALWAYS_INLINE Slot& slot_at(u32 index) { return m_chunks[index / slots_per_chunk]->slots[index % slots_per_chunk]; }

    Vector<NonnullOwnPtr<Chunk>> m_chunks;
};

}
