/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibCore/ElapsedTimer.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapGroup.h>

namespace GC {

HeapGroup::~HeapGroup()
{
    for (auto* heap : m_heaps)
        heap->m_group = nullptr;
}

void HeapGroup::add(Heap& heap)
{
    VERIFY(!heap.m_group);
    heap.m_group = this;
    m_heaps.append(&heap);
}

void HeapGroup::remove(Heap& heap)
{
    VERIFY(heap.m_group == this);
    heap.m_group = nullptr;
    m_heaps.remove_first_matching([&](auto* entry) { return entry == &heap; });
}

void HeapGroup::collect_garbage(bool print_report)
{
    // Defer all member heaps' collections until the last one, so that cross-heap edges are visible to the mark phase.
    for (auto* heap : m_heaps) {
        VERIFY(!heap->m_collecting_garbage);
        if (heap->m_gc_deferrals) {
            heap->m_should_gc_when_deferral_ends = true;
            return;
        }
    }

    for (auto* heap : m_heaps) {
        heap->finish_pending_incremental_sweep();
        heap->m_collecting_garbage = true;
    }
    ScopeGuard unset_collecting = [&] {
        for (auto* heap : m_heaps)
            heap->m_collecting_garbage = false;
    };

    HashMap<Cell*, HeapRoot> roots;
    for (auto* heap : m_heaps)
        heap->gather_roots(roots, nullptr, Heap::IncludeIncomingCrossHeapMembers::No);

    Heap::mark_live_cells_across(m_heaps, roots);

    for (auto* heap : m_heaps)
        heap->run_post_mark_phases(print_report);

    for (auto* heap : m_heaps) {
        Core::ElapsedTimer measurement_timer { Core::TimerType::Precise };
        measurement_timer.start();
        heap->sweep_dead_cells(print_report, measurement_timer);
    }
}

}
