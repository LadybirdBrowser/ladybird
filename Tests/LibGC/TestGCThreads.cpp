/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AssertionFailure.h>
#include <AK/Atomic.h>
#include <AK/Vector.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakInlines.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

#if !defined(AK_OS_WINDOWS)
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace {

Atomic<size_t> s_live_cells { 0 };

class ThreadTestCell final : public GC::Cell {
    GC_CELL(ThreadTestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(ThreadTestCell);

public:
    virtual ~ThreadTestCell() override { s_live_cells.fetch_sub(1); }

    GC::Ptr<ThreadTestCell>& next() { return m_next; }

private:
    ThreadTestCell() { s_live_cells.fetch_add(1); }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_next);
    }

    GC::Ptr<ThreadTestCell> m_next;
};

GC_DEFINE_ALLOCATOR(ThreadTestCell);

template<typename Callback>
void run_on_threads(size_t thread_count, IGNORE_USE_IN_ESCAPING_LAMBDA Callback callback)
{
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t i = 0; i < thread_count; ++i) {
        auto thread = Threading::Thread::construct("GCThreadTest"sv, [&callback] {
            callback();
            return static_cast<intptr_t>(0);
        });
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        MUST(thread->join());
}

void churn_cells()
{
    GC::Heap heap([](auto&) { });
    heap.set_incremental_sweep_enabled(false);
    EXPECT_EQ(&GC::Heap::the(), &heap);

    // More than one block of cells, and enough weak pointers for a weak block.
    auto list = GC::make_root(heap.allocate<ThreadTestCell>());
    Vector<GC::Weak<ThreadTestCell>> weak_pointers;
    for (size_t i = 0; i < 2000; ++i) {
        auto cell = heap.allocate<ThreadTestCell>();
        cell->next() = list.cell();
        list = GC::make_root(cell);
        weak_pointers.append(cell);
    }
    heap.collect_garbage();
    for (auto& weak : weak_pointers)
        EXPECT(weak.ptr());
    list = {};
}

}

TEST_CASE(heaps_on_several_threads_do_not_share_their_allocators)
{
    run_on_threads(8, [] {
        for (size_t i = 0; i < 10; ++i)
            churn_cells();
    });
    EXPECT_EQ(s_live_cells.load(), 0u);
}

TEST_CASE(heap_blocks_come_back_when_a_heap_goes_away)
{
    auto churn = [] {
        run_on_threads(4, [] {
            for (size_t i = 0; i < 10; ++i)
                churn_cells();
        });
    };

    churn();
    auto committed_end = GC::BlockAllocator::heap_region_committed_end();
    for (size_t i = 0; i < 20; ++i)
        churn();
    // NB: The decommit worker may have one chunk prepared ahead of the blocks that are in use.
    EXPECT(GC::BlockAllocator::heap_region_committed_end() <= committed_end + 2 * MiB);
}

TEST_CASE(a_new_heap_can_become_the_default_of_a_thread)
{
    run_on_threads(1, [] {
        {
            GC::Heap heap([](auto&) { });
            EXPECT_EQ(&GC::Heap::the(), &heap);
        }
        GC::Heap heap([](auto&) { });
        EXPECT_EQ(&GC::Heap::the(), &heap);
    });
}

TEST_CASE(null_weak_pointers_can_be_copied_on_several_threads)
{
    run_on_threads(8, [] {
        for (size_t i = 0; i < 1'000'000; ++i) {
            GC::Weak<ThreadTestCell> weak;
            auto copy = weak;
            EXPECT(!copy);
        }
    });
}

TEST_CASE(weak_pointer_can_outlive_its_heap)
{
    GC::Weak<ThreadTestCell> weak;
    {
        GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
        auto cell = heap.allocate<ThreadTestCell>();
        weak = cell;
        EXPECT(weak);
    }
    EXPECT(!weak);
    auto copy = weak;
    EXPECT(!copy);
    weak = nullptr;
    EXPECT(!copy);
}

#if !defined(AK_OS_WINDOWS)
static constexpr int verification_failure_exit_code = 42;

template<typename Callback>
static bool crashes(Callback callback)
{
    auto pid = fork();
    VERIFY(pid >= 0);
    if (pid == 0) {
        // NB: A failed check would print a symbolized backtrace before it crashes, which takes about a second.
        AK::set_assertion_failure_callback([](auto, auto) { _exit(verification_failure_exit_code); });
        callback();
        _exit(0);
    }
    int status = 0;
    VERIFY(waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) && WEXITSTATUS(status) == verification_failure_exit_code;
}

TEST_CASE(a_heap_must_go_away_on_its_own_thread)
{
    EXPECT(crashes([] {
        auto* heap = new GC::Heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
        run_on_threads(1, [heap] { delete heap; });
    }));
}

TEST_CASE(a_heap_must_collect_on_its_own_thread)
{
    EXPECT(crashes([] {
        auto* heap = new GC::Heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
        run_on_threads(1, [heap] { heap->collect_garbage(); });
    }));
}
#endif
