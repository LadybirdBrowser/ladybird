/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/DeferGC.h>
#include <LibGC/ForeignCell.h>
#include <LibGC/Heap.h>

namespace GC {

void* ForeignCell::foreign_data()
{
    // !!!
    auto offset = round_up_to_power_of_two(sizeof(ForeignCell), m_vtable.alignment);
    return static_cast<void*>(reinterpret_cast<u8*>(this) + offset);
}

ForeignCell::ForeignCell(ForeignCell::Vtable vtable)
    : m_vtable(move(vtable))
{
    if (m_vtable.initialize)
        m_vtable.initialize(foreign_data(), m_vtable.class_metadata_pointer, *this);
}

ForeignCell::~ForeignCell()
{
    if (m_vtable.destroy)
        m_vtable.destroy(foreign_data(), m_vtable.class_metadata_pointer);
}

Ref<ForeignCell> ForeignCell::create(Heap& heap, size_t size, ForeignCell::Vtable vtable)
{
    // NOTE: GC must be deferred so that a collection during allocation doesn't get tripped
    //   up looking for the Cell pointer on the stack or in a register when it might only exist in the heap.
    //   We can't guarantee that the ForeignCell will be stashed in a proper ForeignRef/ForeignPtr or similar
    //   foreign type until after all the dust has settled on both sides of the FFI boundary.
    VERIFY(heap.is_gc_deferred());
    VERIFY(is_power_of_two(vtable.alignment));
    auto& allocator = heap.allocator_for_size(sizeof(ForeignCell) + round_up_to_power_of_two(size, vtable.alignment));
    auto* memory = allocator.allocate_cell(heap);
    auto* foreign_cell = new (memory) ForeignCell(move(vtable));
    return *foreign_cell;
}

void ForeignCell::finalize()
{
    Base::finalize();
    if (m_vtable.finalize)
        m_vtable.finalize(foreign_data(), m_vtable.class_metadata_pointer);
}

void ForeignCell::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_vtable.visit_edges)
        m_vtable.visit_edges(foreign_data(), m_vtable.class_metadata_pointer, visitor);
}

}
