/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/BinarySearch.h>
#include <AK/Checked.h>
#include <AK/Debug.h>
#include <AK/Diagnostics.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/NumberFormat.h>
#include <AK/Platform.h>
#include <AK/QuickSort.h>
#include <AK/SaturatingMath.h>
#include <AK/ScopeGuard.h>
#include <AK/StackInfo.h>
#include <AK/StackUnwinder.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/StatsLog.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibCore/Timer.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/BlockDescriptor.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/CompressedPointer.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapAccess.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <setjmp.h>

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#endif

#ifdef LIBGC_HAS_CPPTRACE
#    include <cpptrace/cpptrace.hpp>
#endif

namespace GC {

static constexpr size_t GC_MIN_BYTES_THRESHOLD { 32 * 1024 * 1024 };

// The share of running time we want to spend collecting. A cycle costs its mark and sweep time and lasts as long as it
// takes to allocate its budget, so meeting this share takes a budget of allocation_rate * cost * (1 - u) / u.
static constexpr double GC_TARGET_TIME_FRACTION { 0.1 };

// A heap that allocates steadily is collected from the idle timer once its garbage reaches this share of its live
// size (or GC_MIN_BYTES_THRESHOLD, whichever is more), well before the allocation threshold. Nobody waits for those
// collections, but they are not free either, so marking for them may take at most GC_STEADY_TIME_FRACTION of the
// running time. See idle_gc_on_timer().
static constexpr double GC_STEADY_GROWTH_FACTOR { 0.25 };
static constexpr double GC_STEADY_TIME_FRACTION { 0.02 };

// Growth factors relative to live cell bytes. The minimum lets the heap triple between collections, which is where a
// page that allocates slowly ends up. A lower minimum would save more memory, but marking pauses the page for as long
// as it takes to mark everything alive, and it would do so more often while the page is busy.
static constexpr double GC_MIN_GROWTH_FACTOR { 2 };
static constexpr double GC_MAX_GROWTH_FACTOR { 8 };

// The maximum factor goes down to the minimum as the live heap, counting the memory its cells own outside it, grows
// from the first size to the second. Letting a large heap grow by the maximum factor would add gigabytes of garbage.
static constexpr size_t GC_LARGE_HEAP_START_BYTES { 128 * 1024 * 1024 };
static constexpr size_t GC_LARGE_HEAP_END_BYTES { 512 * 1024 * 1024 };

// OPTIMIZATION: A program that allocates fast and keeps little alive gains from a budget far above the maximum factor,
//               so the maximum never limits a budget below this.
static constexpr size_t GC_MAX_BUDGET_FLOOR_BYTES { 256 * 1024 * 1024 };

// NB: Memory that cells own outside the heap (array buffer contents, decoded images) is mostly large, long-lived data,
//     and allocating more cells does not grow it. Letting it grow the threshold by the growth factor makes a page with
//     1 GiB of such memory allocate gigabytes of cells between collections, so it only grows the threshold by a tenth.
static constexpr size_t GC_EXTERNAL_GROWTH_FACTOR_NUMERATOR { 1 };
static constexpr size_t GC_EXTERNAL_GROWTH_FACTOR_DENOMINATOR { 10 };

// The share of the threshold that may be allocated before the sweep that follows a collection has to be done. Until a
// block is swept, its dead cells keep their slots and whatever memory they own outside the heap, and a program that
// keeps the event loop busy gives the sweep timer no chance to run, so allocation pays for the sweep as it goes.
static constexpr size_t GC_SWEEP_ALLOCATION_SHARE_DENOMINATOR { 4 };

static constexpr int GC_INCREMENTAL_SWEEP_INTERVAL_MS = 16;
static constexpr int GC_INCREMENTAL_SWEEP_SLICE_MS = 5;

// The idle GC timer ticks at this interval while the mutator is allocating; IdleCollectionPolicy decides on each tick
// whether to proactively collect. See idle_gc_on_timer().
static constexpr int GC_IDLE_GC_INTERVAL_MS = 4000;

// The shortest time from hearing of an abandoned object graph to collecting it. Whatever replaces the graph, like the
// next page, gets this long to settle, and a burst of abandoned graphs, like closing a window of tabs, shares one
// collection.
static constexpr int GC_ABANDONED_OBJECT_GRAPH_MIN_DELAY_MS = 1000;

// How long after a collection the thread gives the memory that its allocator holds back to the system, unless another
// collection comes first. The allocator only looks for memory to give back when the thread allocates, so what an idle
// thread freed, or what other threads freed of its memory, would stay. The delay keeps this away from a thread that is
// busy, and leaves time for the work that a collection starts on other threads, like compressing source text.
static constexpr int GC_ALLOCATOR_MEMORY_RELEASE_DELAY_MS = 5000;

// Compaction moves at most this many bytes of cells per collection. Moving cells out of nearly empty blocks costs about
// 3 ms per MiB on a desktop machine, half of it to copy the cells and take their new slots, and half to rewrite what
// points at them. Only a heap that went quiet compacts, so nobody waits for the pause, and as the emptiest blocks go
// first, a budget like this one frees most of what can be freed in one collection.
static constexpr size_t GC_COMPACTION_BUDGET_BYTES { 8 * MiB };

// A heap that went quiet compacts in collections of their own, one after another, as long as each frees at least this
// much and at least this much is left to recover.
// NB: While a page is busy, the holes that compaction would close are the room that the page fills again before the
//     next collection, which comes after GC_MIN_BYTES_THRESHOLD of allocation at least. Compacting then frees blocks
//     that the page needs again a few seconds later. Once the page is quiet, the heap would keep its holes for good.
static constexpr size_t GC_COMPACTION_MIN_RECOVERABLE_BYTES { 1 * MiB };

// Every thread that runs a VM has a heap of its own, and all but the main thread may come and go.
AK_TLS_INITIAL_EXEC static thread_local constinit Heap* s_the = nullptr;

namespace {

// LIBGC_LOG_LEVEL controls how much detail collect_garbage() prints:
//   0 (default) - silent.
//   1           - per-GC report with totals and per-phase timing breakdown.
//   2+          - everything in level 1, plus a full block allocator dump.
i32 read_libgc_log_level()
{
    char const* env = getenv("LIBGC_LOG_LEVEL");
    if (!env || !*env)
        return 0;
    return atoi(env);
}

i32 libgc_log_level()
{
    static i32 const level = read_libgc_log_level();
    return level;
}

// LIBGC_MOVE_EVERYTHING=1 has every collection that may move cells (one that does not scan the stack) evacuate every
// block of cells defined outside of C++, moving each cell that can move to another slot. This tests that moving cells
// is safe, and its report tells what keeps cells from moving.
bool libgc_move_everything()
{
    static bool const value = [] {
        char const* env = getenv("LIBGC_MOVE_EVERYTHING");
        return env && *env && atoi(env) != 0;
    }();
    return value;
}

// LIBGC_COMPACT=0 turns compaction off, so that collections never move cells unless LIBGC_MOVE_EVERYTHING asks them to.
bool libgc_compact()
{
    static bool const value = [] {
        char const* env = getenv("LIBGC_COMPACT");
        return !env || !*env || atoi(env) != 0;
    }();
    return value;
}

}

// What a collection that moves cells did, for its report.
struct CompactionStats {
    AK_ALLOC_WITH_KMALLOC;

    enum PinReason : u8 {
        ByValueEdge,
        ByValueRoot,
        NonMovableAllocator,
        PinReasonCount,
    };

    struct Counts {
        size_t cells { 0 };
        size_t bytes { 0 };

        void add(size_t cell_size)
        {
            ++cells;
            bytes += cell_size;
        }
    };

    bool active { false };
    size_t evacuated_blocks { 0 };
    // The evacuated blocks that no cell stays in, which the sweep that follows lets go of.
    size_t freed_blocks { 0 };
    // What selection expected to recover and move, see rust_gc_compaction_select().
    size_t recoverable_bytes { 0 };
    size_t bytes_to_move { 0 };
    // The blocks that compaction could have freed as well, but that would have gone over the budget.
    size_t blocks_left_by_budget { 0 };
    Counts moved;
    Array<Counts, PinReasonCount> pinned;
    size_t recorded_slots { 0 };
    size_t rewritten_slots { 0 };
    i64 select_us { 0 };
    i64 copy_us { 0 };
    i64 fix_up_us { 0 };
    i64 release_us { 0 };
    HashMap<ByteString, Array<Counts, PinReasonCount>> pinned_by_type;
};

namespace {

// Per-phase timings recorded during a single collect_garbage() call. We keep
// these per thread (instead of threading more parameters through the GC's
// internal helpers) since a heap only collects on its own thread, guarded by
// m_collecting_garbage.
struct PhaseTimings {
    // Top-level phases.
    i64 gather_roots_us { 0 };
    i64 mark_live_cells_us { 0 };
    i64 finalize_unmarked_cells_us { 0 };
    i64 sweep_weak_blocks_us { 0 };
    i64 prune_weak_containers_us { 0 };
    i64 sweep_callbacks_us { 0 };
    i64 sweep_dead_cells_us { 0 };

    // gather_roots() subphases.
    i64 gather_embedder_roots_us { 0 };
    i64 gather_conservative_roots_us { 0 };
    i64 gather_explicit_roots_us { 0 };

    // gather_conservative_roots() subphases.
    i64 conservative_register_scan_us { 0 };
    i64 conservative_stack_scan_us { 0 };
    i64 conservative_cell_lookup_us { 0 };

    // mark_live_cells() subphases.
    i64 mark_initial_visit_us { 0 };
    i64 mark_bfs_us { 0 };
    i64 mark_clear_uprooted_us { 0 };
    size_t mark_threads { 0 };

    // sweep_dead_cells() subphases. Only populated for CollectEverything;
    // normal collections defer sweep to the incremental sweeper.
    i64 sweep_block_iteration_us { 0 };
    i64 sweep_block_reclassify_us { 0 };
    i64 sweep_update_threshold_us { 0 };
};
thread_local PhaseTimings g_phase_timings;

// Stats gathered during sweep_dead_cells() and consumed by the report printer
// in collect_garbage().
struct SweepStats {
    size_t collected_cells { 0 };
    size_t live_cells { 0 };
    size_t collected_cell_bytes { 0 };
    size_t live_cell_bytes { 0 };
    size_t live_external_bytes { 0 };
    size_t freed_block_count { 0 };
};
thread_local SweepStats g_sweep_stats;

struct IncrementalSweepBatchStats {
    size_t blocks_swept { 0 };
    i64 elapsed_us { 0 };
    bool forced { false };
};

struct IncrementalSweepStats {
    bool should_report { false };
    size_t total_blocks { 0 };
    Vector<IncrementalSweepBatchStats> batches;
    Core::ElapsedTimer timer { Core::TimerType::Precise };
};

IncrementalSweepStats& incremental_sweep_stats()
{
    // NB: Free each thread's batches when that thread exits.
#ifdef AK_COMPILER_CLANG
    AK_IGNORE_DIAGNOSTIC("-Wexit-time-destructors", static thread_local IncrementalSweepStats stats)
#else
    static thread_local IncrementalSweepStats stats;
#endif
    return stats;
}

thread_local bool g_next_incremental_sweep_should_report { false };

// What started the collection in progress, for its report.
thread_local CollectionTrigger g_collection_trigger { CollectionTrigger::Explicit };

StringView collection_trigger_name(CollectionTrigger trigger)
{
    switch (trigger) {
    case CollectionTrigger::Explicit:
        return "explicit"sv;
    case CollectionTrigger::Allocation:
        return "allocation"sv;
    case CollectionTrigger::IdlePolicy:
        return "idle policy"sv;
    case CollectionTrigger::SteadyBudget:
        return "steady budget"sv;
    case CollectionTrigger::AbandonedObjectGraph:
        return "abandoned object graph"sv;
    case CollectionTrigger::Compaction:
        return "compaction"sv;
    case CollectionTrigger::DeferralEnded:
        return "deferral ended"sv;
    }
    VERIFY_NOT_REACHED();
}

// Set by collect_garbage() while a reported collection is in flight. Used by
// the GC's helpers to decide whether they should record subphase timings.
thread_local bool g_recording_phase_timings { false };

void print_compaction_summary(CompactionStats const& stats)
{
    auto pinned_bytes = [&](CompactionStats::PinReason reason) { return human_readable_size(stats.pinned[reason].bytes); };
    dbgln("Compaction: {} source blocks, {} freed; moved {} of {} expected; pinned {} by edge, {} by root, {} non-movable; {} recoverable before; {} slots recorded, {} rewritten; select {} us, compaction {} us (copy {}, fix up {}, release {})",
        stats.evacuated_blocks, stats.freed_blocks, human_readable_size(stats.moved.bytes), human_readable_size(stats.bytes_to_move),
        pinned_bytes(CompactionStats::ByValueEdge), pinned_bytes(CompactionStats::ByValueRoot),
        pinned_bytes(CompactionStats::NonMovableAllocator),
        human_readable_size(stats.recoverable_bytes), stats.recorded_slots, stats.rewritten_slots, stats.select_us,
        stats.select_us + stats.copy_us + stats.fix_up_us + stats.release_us, stats.copy_us, stats.fix_up_us, stats.release_us);
}

void print_gc_report(i64 total_us, size_t live_block_count, bool scan_stack, size_t marked_cell_bytes, u64 allocated_bytes, CompactionStats const& stats)
{
    auto const& t = g_phase_timings;
    auto const& s = g_sweep_stats;

    auto pct = [&](i64 part_us) -> double {
        if (total_us <= 0)
            return 0.0;
        return 100.0 * static_cast<double>(part_us) / static_cast<double>(total_us);
    };

    dbgln("Garbage collection report");
    dbgln("=================================================================");
    dbgln("Totals:");
    dbgln("       Time spent: {} us", total_us);
    dbgln("     Stack scanned: {}", scan_stack ? "yes"sv : "no"sv);
    dbgln("          Trigger: {}", collection_trigger_name(g_collection_trigger));
    dbgln("     Marked cells: {}", human_readable_size(marked_cell_bytes));
    dbgln("  Allocated since: {}", human_readable_size(allocated_bytes));
    dbgln("       Live cells: {} ({})", s.live_cells, human_readable_size(s.live_cell_bytes));
    dbgln("    Live external: {}", human_readable_size(s.live_external_bytes));
    dbgln("  Collected cells: {} ({})", s.collected_cells, human_readable_size(s.collected_cell_bytes));
    dbgln("      Live blocks: {} ({})", live_block_count, human_readable_size(live_block_count * HeapBlock::BLOCK_SIZE));
    dbgln("     Freed blocks: {} ({})", s.freed_block_count, human_readable_size(s.freed_block_count * HeapBlock::BLOCK_SIZE));
    dbgln("");
    dbgln("Phase breakdown (us, % of total):");
    dbgln("  gather_roots                  {:>10} us ({:>5.1f}%)", t.gather_roots_us, pct(t.gather_roots_us));
    dbgln("    embedder roots              {:>10} us ({:>5.1f}%)", t.gather_embedder_roots_us, pct(t.gather_embedder_roots_us));
    dbgln("    conservative roots          {:>10} us ({:>5.1f}%)", t.gather_conservative_roots_us, pct(t.gather_conservative_roots_us));
    dbgln("      register scan             {:>10} us ({:>5.1f}%)", t.conservative_register_scan_us, pct(t.conservative_register_scan_us));
    dbgln("      stack scan                {:>10} us ({:>5.1f}%)", t.conservative_stack_scan_us, pct(t.conservative_stack_scan_us));
    dbgln("      cell lookup               {:>10} us ({:>5.1f}%)", t.conservative_cell_lookup_us, pct(t.conservative_cell_lookup_us));
    dbgln("    explicit roots              {:>10} us ({:>5.1f}%)", t.gather_explicit_roots_us, pct(t.gather_explicit_roots_us));
    dbgln("  mark_live_cells               {:>10} us ({:>5.1f}%)", t.mark_live_cells_us, pct(t.mark_live_cells_us));
    dbgln("    clear uprooted              {:>10} us ({:>5.1f}%)", t.mark_clear_uprooted_us, pct(t.mark_clear_uprooted_us));
    dbgln("    initial visit               {:>10} us ({:>5.1f}%)", t.mark_initial_visit_us, pct(t.mark_initial_visit_us));
    dbgln("    BFS marking                 {:>10} us ({:>5.1f}%)", t.mark_bfs_us, pct(t.mark_bfs_us));
    dbgln("      marking threads           {:>10}", t.mark_threads);
    dbgln("  finalize_unmarked_cells       {:>10} us ({:>5.1f}%)", t.finalize_unmarked_cells_us, pct(t.finalize_unmarked_cells_us));
    dbgln("  sweep_weak_blocks             {:>10} us ({:>5.1f}%)", t.sweep_weak_blocks_us, pct(t.sweep_weak_blocks_us));
    dbgln("  prune_weak_containers         {:>10} us ({:>5.1f}%)", t.prune_weak_containers_us, pct(t.prune_weak_containers_us));
    dbgln("  sweep_callbacks               {:>10} us ({:>5.1f}%)", t.sweep_callbacks_us, pct(t.sweep_callbacks_us));
    if (t.sweep_dead_cells_us > 0) {
        dbgln("  sweep_dead_cells              {:>10} us ({:>5.1f}%)", t.sweep_dead_cells_us, pct(t.sweep_dead_cells_us));
        dbgln("    block iteration             {:>10} us ({:>5.1f}%)", t.sweep_block_iteration_us, pct(t.sweep_block_iteration_us));
        dbgln("    block reclassify            {:>10} us ({:>5.1f}%)", t.sweep_block_reclassify_us, pct(t.sweep_block_reclassify_us));
        dbgln("    update threshold            {:>10} us ({:>5.1f}%)", t.sweep_update_threshold_us, pct(t.sweep_update_threshold_us));
    }
    if (stats.active) {
        auto print_counts = [](StringView label, CompactionStats::Counts const& counts) {
            dbgln("  {:<27} {:>10} cells ({})", label, counts.cells, human_readable_size(counts.bytes));
        };
        dbgln("");
        dbgln("Compaction:");
        dbgln("  evacuated blocks            {:>10}", stats.evacuated_blocks);
        dbgln("  freed blocks                {:>10}", stats.freed_blocks);
        print_counts("moved"sv, stats.moved);
        print_counts("pinned: by-value edge"sv, stats.pinned[CompactionStats::ByValueEdge]);
        print_counts("pinned: by-value root"sv, stats.pinned[CompactionStats::ByValueRoot]);
        print_counts("pinned: non-movable type"sv, stats.pinned[CompactionStats::NonMovableAllocator]);
        dbgln("  recorded slots              {:>10}", stats.recorded_slots);
        dbgln("  rewritten slots             {:>10}", stats.rewritten_slots);
        dbgln("  select                      {:>10} us", stats.select_us);
        dbgln("  copy                        {:>10} us", stats.copy_us);
        dbgln("  fix up                      {:>10} us", stats.fix_up_us);
        dbgln("  release                     {:>10} us", stats.release_us);
        if (!stats.pinned_by_type.is_empty()) {
            struct Entry {
                ByteString name;
                Array<CompactionStats::Counts, CompactionStats::PinReasonCount> counts;
                size_t bytes;
            };
            Vector<Entry> entries;
            for (auto const& it : stats.pinned_by_type) {
                size_t bytes = 0;
                for (auto const& counts : it.value)
                    bytes += counts.bytes;
                entries.append({ it.key, it.value, bytes });
            }
            quick_sort(entries, [](auto const& a, auto const& b) { return a.bytes > b.bytes; });
            dbgln("  top types by pinned bytes (edge / root / non-movable):");
            for (size_t i = 0; i < min<size_t>(entries.size(), 25); ++i) {
                auto const& entry = entries[i];
                dbgln("    {:<40} {:>10} {:>10} {:>10}", entry.name,
                    human_readable_size(entry.counts[CompactionStats::ByValueEdge].bytes),
                    human_readable_size(entry.counts[CompactionStats::ByValueRoot].bytes),
                    human_readable_size(entry.counts[CompactionStats::NonMovableAllocator].bytes));
            }
        }
    }
    dbgln("=================================================================");
}

void record_incremental_sweep_batch(size_t blocks_swept, i64 elapsed_us, bool forced)
{
    if (!incremental_sweep_stats().should_report || blocks_swept == 0)
        return;
    incremental_sweep_stats().batches.append({
        .blocks_swept = blocks_swept,
        .elapsed_us = elapsed_us,
        .forced = forced,
    });
}

void print_incremental_sweep_report(size_t live_cell_bytes, size_t live_external_bytes, size_t next_gc_bytes_threshold)
{
    if (!incremental_sweep_stats().should_report)
        return;

    size_t swept_blocks = 0;
    i64 batch_time_us = 0;
    i64 shortest_batch_us = NumericLimits<i64>::max();
    i64 longest_batch_us = 0;
    for (auto const& batch : incremental_sweep_stats().batches) {
        swept_blocks += batch.blocks_swept;
        batch_time_us += batch.elapsed_us;
        shortest_batch_us = min(shortest_batch_us, batch.elapsed_us);
        longest_batch_us = max(longest_batch_us, batch.elapsed_us);
    }

    if (incremental_sweep_stats().batches.is_empty())
        shortest_batch_us = 0;

    dbgln("Incremental sweep report");
    dbgln("=================================================================");
    dbgln("Totals:");
    dbgln("        Wall time: {} us", incremental_sweep_stats().timer.elapsed_time().to_microseconds());
    dbgln("       Batch time: {} us", batch_time_us);
    dbgln("          Batches: {}", incremental_sweep_stats().batches.size());
    dbgln("    Swept blocks: {} / {} ({})", swept_blocks, incremental_sweep_stats().total_blocks, human_readable_size(swept_blocks * HeapBlock::BLOCK_SIZE));
    dbgln("     Live cells: {}", human_readable_size(live_cell_bytes));
    dbgln("  Live external: {}", human_readable_size(live_external_bytes));
    dbgln("  Next threshold: {}", human_readable_size(next_gc_bytes_threshold));
    dbgln("");
    dbgln("Batch timings:");
    dbgln("  Shortest batch: {} us", shortest_batch_us);
    dbgln("   Longest batch: {} us", longest_batch_us);
    for (size_t i = 0; i < incremental_sweep_stats().batches.size(); ++i) {
        auto const& batch = incremental_sweep_stats().batches[i];
        dbgln("    #{:>3}: {:>5} blocks in {:>8} us{}", i + 1, batch.blocks_swept, batch.elapsed_us, batch.forced ? " (forced)"sv : ""sv);
    }
    dbgln("=================================================================");
}

class ScopedPhaseTimer {
public:
    ScopedPhaseTimer(bool enabled, i64& out_microseconds)
        : m_out_microseconds(out_microseconds)
        , m_enabled(enabled)
    {
        if (m_enabled)
            m_timer.start();
    }
    ~ScopedPhaseTimer()
    {
        if (m_enabled)
            m_out_microseconds = m_timer.elapsed_time().to_microseconds();
    }

private:
    Core::ElapsedTimer m_timer { Core::TimerType::Precise };
    i64& m_out_microseconds;
    bool m_enabled;
};

}

Heap& Heap::the()
{
    return *s_the;
}

void Heap::set_default_heap_for_testing(Heap& heap)
{
    s_the = &heap;
}

CellAllocator& Heap::create_cell_allocator(CellAllocatorDescriptorBase& descriptor)
{
    auto index = descriptor.index();
    if (index >= m_cell_allocators_by_descriptor_index.size())
        m_cell_allocators_by_descriptor_index.resize(index + 1);
    auto& allocator = m_cell_allocators_by_descriptor_index[index];
    VERIFY(!allocator);
    allocator = make<CellAllocator>(*this, descriptor);
    return *allocator;
}

Heap::Heap(AK::Function<void(HashMap<Cell*, GC::HeapRoot>&)> gather_embedder_roots, BecomeThreadDefault become_thread_default)
    : m_gather_embedder_roots(move(gather_embedder_roots))
    , m_compaction_stats(make<CompactionStats>())
{
    if (become_thread_default == BecomeThreadDefault::Yes)
        s_the = this;
    m_gc_bytes_threshold = GC_MIN_BYTES_THRESHOLD;
    m_last_collection_end_ns = MonotonicTime::now().nanoseconds();
    static_assert(HeapBlock::min_possible_cell_size <= 32, "Heap Cell tracking uses too much data!");
}

Heap::~Heap()
{
    VERIFY(is_on_owner_thread());
    collect_garbage(CollectionType::CollectEverything);

    // A root that outlives its heap would reach into freed memory when it goes.
    VERIFY(m_roots.is_empty());

    FFI::rust_gc_identity_hashes_destroy(m_identity_hashes);
    m_identity_hashes = nullptr;

    free_weak_blocks();

    while (auto* allocator = m_all_cell_allocators.first())
        m_all_cell_allocators.remove(*allocator);

    for (auto* member : m_incoming_cross_heap_members)
        member->detach_from_heap({});
    m_incoming_cross_heap_members.clear();

    if (s_the == this)
        s_the = nullptr;
}

void Heap::free_weak_blocks()
{
    // NB: A weak pointer outside the heap may outlive it, so a block that one still refers to has to stay. Its pointers
    //     are all null by now, and it is leaked once this heap forgets about it.
    auto free_blocks = [](WeakBlock::List& blocks) {
        while (auto* block = blocks.take_first()) {
            if (!block->has_referenced_impls())
                WeakBlock::destroy(block);
        }
    };
    free_blocks(m_usable_weak_blocks);
    free_blocks(m_full_weak_blocks);
}

void Heap::enable_stats()
{
    VERIFY(!m_stats);
    VERIFY(!is_incremental_sweep_active());
    m_stats = make<Stats>();
}

void Heap::add_stats(AK::JsonObject& values) const
{
    VERIFY(m_stats);
    values.set("gc.allocated_bytes_total"sv, m_total_allocated_bytes);
    values.set("gc.allocated_bytes_since_gc"sv, m_allocated_bytes_since_last_gc);
    values.set("gc.heap_blocks"sv, block_count());
    values.set("gc.heap_block_bytes"sv, block_count() * HeapBlock::BLOCK_SIZE);
    values.set("gc.cell_allocators"sv, m_all_cell_allocators.size_slow());
    values.set("gc.threshold_bytes"sv, m_gc_bytes_threshold);
    values.set("gc.live_cell_bytes"sv, m_stats->live_cell_bytes);
    values.set("gc.live_external_bytes"sv, m_stats->live_external_bytes);
    values.set("gc.collections"sv, m_stats->collections);
    values.set("gc.completed_collections"sv, m_stats->completed_collections);
    values.set("gc.total_duration_ns"sv, m_stats->total_duration_ns);
    values.set("gc.last_duration_ns"sv, m_stats->last_duration_ns);
}

void Heap::finish_stats_collection()
{
    VERIFY(m_stats);
    auto& stats = *m_stats;
    stats.last_duration_ns = stats.pause_duration_ns + stats.sweep_duration_ns;
    stats.total_duration_ns += stats.last_duration_ns;
    ++stats.completed_collections;
    if (auto* log = Core::StatsLog::the()) {
        AK::JsonObject data;
        data.set("collection"sv, stats.collections);
        data.set("duration_ns"sv, stats.last_duration_ns);
        data.set("pause_duration_ns"sv, stats.pause_duration_ns);
        data.set("sweep_duration_ns"sv, stats.sweep_duration_ns);
        data.set("bytes_before"sv, stats.bytes_before);
        data.set("bytes_after"sv, block_count() * HeapBlock::BLOCK_SIZE);
        data.set("live_cell_bytes"sv, stats.live_cell_bytes);
        data.set("live_external_bytes"sv, stats.live_external_bytes);
        log->event("gc"sv, move(data));
    }
}

void Heap::will_allocate(size_t size, TriggersCollection triggers_collection)
{
    ASSERT(!heap_access_is_forbidden_on_this_thread());
    ASSERT(is_on_owner_thread());
    // NB: Sweeping a block destroys its dead cells, and a destructor may allocate. A collection started from there
    //     would mark the block's reachable cells and queue the block for the next sweep, the rest of the current sweep
    //     would then clear those marks, and the next sweep of the block would free cells that are still in use. So an
    //     allocation made while a block is swept leaves the collection to the first allocation after the sweep.
    // NB: An allocation that must not collect still counts towards the next collection, which the next allocation that
    //     may collect starts. Otherwise a program that allocates mostly storage (as one making large arrays does) would
    //     keep gigabytes of dead storage before its few other allocations reached the threshold.
    if (m_block_sweeps_in_progress == 0 && triggers_collection == TriggersCollection::Yes) {
        if (should_collect_on_every_allocation() || m_allocated_bytes_since_last_gc + size > m_gc_bytes_threshold) {
            m_allocated_bytes_since_last_gc = 0;
            // NB: A heap that allocates this much is not quiet.
            m_reducing_memory = false;
            m_compacted_since_activity = false;
            m_should_compact = false;
            TemporaryChange trigger { g_collection_trigger, CollectionTrigger::Allocation };
            collect_garbage();
        }
    }

    m_allocated_bytes_since_last_gc += size;
    m_total_allocated_bytes += size;

    if (m_block_sweeps_in_progress == 0 && triggers_collection == TriggersCollection::Yes)
        sweep_in_step_with_allocation();

    // Keep the idle GC timer armed while allocation is happening, so a proactive collection runs once the mutator's
    // allocation rate drops.
    if (!m_idle_gc_timer || !m_idle_gc_timer->is_active())
        start_idle_gc_timer();
}

void Heap::did_allocate_external_memory(size_t size)
{
    will_allocate(size);
}

void Heap::did_free_external_memory(size_t size)
{
    if (size > m_allocated_bytes_since_last_gc) {
        m_allocated_bytes_since_last_gc = 0;
        return;
    }

    m_allocated_bytes_since_last_gc -= size;
}

double Heap::next_growth_factor(size_t live_cell_bytes)
{
    if (live_cell_bytes == 0)
        return GC_MIN_GROWTH_FACTOR;

    auto live = static_cast<double>(live_cell_bytes);
    auto cost_seconds = static_cast<double>(m_mark_duration_ns + m_sweep_duration_ns) / 1e9;
    auto wanted_budget = m_allocation_bytes_per_second * cost_seconds * (1 - GC_TARGET_TIME_FRACTION) / GC_TARGET_TIME_FRACTION;

    // NB: Allocation rates swing from one cycle to the next, so keep an average that the clamp below does not touch.
    m_wanted_growth_factor = m_wanted_growth_factor.has_value()
        ? (wanted_budget / live + m_wanted_growth_factor.value()) / 2
        : wanted_budget / live;

    auto heap_size = static_cast<double>(live_cell_bytes) + static_cast<double>(m_marked_external_bytes);
    auto large_heap_share = clamp((heap_size - GC_LARGE_HEAP_START_BYTES) / (GC_LARGE_HEAP_END_BYTES - GC_LARGE_HEAP_START_BYTES), 0.0, 1.0);
    auto max_growth_factor = GC_MAX_GROWTH_FACTOR - (GC_MAX_GROWTH_FACTOR - GC_MIN_GROWTH_FACTOR) * large_heap_share;
    max_growth_factor = max(max_growth_factor, static_cast<double>(GC_MAX_BUDGET_FLOOR_BYTES) / live);
    return clamp(m_wanted_growth_factor.value(), GC_MIN_GROWTH_FACTOR, max_growth_factor);
}

void Heap::update_gc_bytes_threshold()
{
    // NB: Only marking knows how much the last collection found alive. A sweep also sees the cells allocated into
    //     blocks it has yet to reach, which are marked to keep them alive.
    auto live_cell_bytes = m_marked_cell_bytes;
    auto live_external_bytes = m_marked_external_bytes;
    if (m_stats) {
        m_stats->live_cell_bytes = live_cell_bytes;
        m_stats->live_external_bytes = live_external_bytes;
    }
    Checked<size_t> next_gc_bytes_threshold = static_cast<size_t>(static_cast<double>(live_cell_bytes) * next_growth_factor(live_cell_bytes));
    m_sweep_duration_ns = 0;

    Checked<size_t> external_growth = live_external_bytes;
    external_growth *= GC_EXTERNAL_GROWTH_FACTOR_NUMERATOR;
    external_growth /= GC_EXTERNAL_GROWTH_FACTOR_DENOMINATOR;
    next_gc_bytes_threshold += external_growth;

    if (next_gc_bytes_threshold.has_overflow()) {
        m_gc_bytes_threshold = NumericLimits<size_t>::max();
        return;
    }

    m_gc_bytes_threshold = max(next_gc_bytes_threshold.value(), GC_MIN_BYTES_THRESHOLD);
}

// Calls the callback for each allocated cell of the block, which includes the cells of a run that an allocator has out.
template<typename Callback>
static void for_each_allocated_cell(HeapBlock& block, Callback callback)
{
    auto& descriptor = block_descriptor_for(&block);
    auto first_cell = bit_cast<FlatPtr>(&block) + HeapBlock::cells_offset();
    for (size_t index = descriptor.first_cell_index; index < descriptor.first_cell_index + descriptor.cell_count; ++index) {
        if (is_allocated(descriptor, index))
            callback(bit_cast<Cell*>(first_cell + (index - descriptor.first_cell_index) * descriptor.cell_size));
    }
}

// The part of the heap region that can hold cells, read once per scan.
struct PossiblePointerBounds {
    static PossiblePointerBounds current()
    {
        return { BlockAllocator::heap_region_start(), BlockAllocator::heap_region_committed_end() };
    }

    FlatPtr heap_region_start { 0 };
    FlatPtr cells_end { 0 };
};

struct PossiblePointers {
    // Addresses taken from whole words, as a raw pointer or as a NaN-boxed cell value. They may point into a cell.
    HashMap<FlatPtr, HeapRoot> pointers;
    // Addresses decoded from either 32-bit half of a word, as a compressed cell pointer. Compressed pointers only ever
    // point at the start of a cell, so these do too.
    HashMap<FlatPtr, HeapRoot> compressed_pointers;
};

static ALWAYS_INLINE void add_possible_compressed_pointer(PossiblePointers& possible_pointers, u32 compressed, HeapRoot const& origin, PossiblePointerBounds const& bounds)
{
    // OPTIMIZATION: Most halves are small integers, either half of a pointer, or either half of a NaN-boxed value. Those
    //               decode into the low guard or above everything committed, so two compares reject them before any
    //               hashing.
    if (compressed < MIN_COMPRESSED_CELL_POINTER)
        return;
    auto possible_pointer = decompress_nonnull(compressed, bounds.heap_region_start);
    if (possible_pointer >= bounds.cells_end)
        return;
    possible_pointers.compressed_pointers.set(possible_pointer, origin);
}

static void add_possible_value(PossiblePointers& possible_pointers, FlatPtr data, HeapRoot origin, PossiblePointerBounds const& bounds)
{
    // A compiler may keep only a compressed cell pointer alive across a collection, in a register or in a spill slot that
    // shares a word with other data, so either half of a word may be one.
    add_possible_compressed_pointer(possible_pointers, static_cast<u32>(data), origin, bounds);
    add_possible_compressed_pointer(possible_pointers, static_cast<u32>(data >> 32), origin, bounds);

    // Because NanBoxedValue stores heap offsets in the payload, decode
    // pointer-backed values relative to the heap region before checking the
    // resulting address.
    FlatPtr possible_pointer;
    if ((data & SHIFTED_IS_CELL_PATTERN) == SHIFTED_IS_CELL_PATTERN)
        possible_pointer = bounds.heap_region_start + (data & HEAP_REGION_OFFSET_MASK);
    else
        possible_pointer = data;
    if (possible_pointer < bounds.heap_region_start || possible_pointer >= bounds.cells_end)
        return;
    possible_pointers.pointers.set(possible_pointer, move(origin));
}

template<typename Callback>
static void for_each_cell_among_possible_pointers(Heap const& heap, PossiblePointers& possible_pointers, Callback callback)
{
    // NB: No block is in the low guard of the region, whose descriptors are never backed by memory.
    auto first_block = heap_region_base() + HEAP_REGION_LOW_GUARD_SIZE;
    auto visit = [&](HashMap<FlatPtr, HeapRoot>& candidates, bool must_be_cell_start) {
        for (auto possible_pointer : candidates.keys()) {
            if (possible_pointer < first_block)
                continue;
            auto* cell = static_cast<Cell*>(allocated_cell_containing(&heap, possible_pointer, HeapBlock::cells_offset()));
            if (!cell)
                continue;
            if (must_be_cell_start && reinterpret_cast<FlatPtr>(cell) != possible_pointer)
                continue;
            callback(cell, candidates, possible_pointer);
        }
    };
    // NB: The whole-word candidates go last, so a cell that both find is attributed to the whole word.
    visit(possible_pointers.compressed_pointers, true);
    visit(possible_pointers.pointers, false);
}

class GraphConstructorVisitor final : public Cell::Visitor {
public:
    explicit GraphConstructorVisitor(HashMap<Cell*, HeapRoot> const& roots)
    {
        m_work_queue.ensure_capacity(roots.size());

        for (auto& [root, root_origin] : roots) {
            auto& graph_node = m_graph.ensure(bit_cast<FlatPtr>(root));
            graph_node.class_name = class_name_of(*root);
            graph_node.root_origin = root_origin;

            m_work_queue.append(*root);
        }
    }

    virtual void visit_impl(Cell& cell) override
    {
        if (m_node_being_visited)
            m_node_being_visited->edges.set(reinterpret_cast<FlatPtr>(&cell));

        if (m_graph.get(reinterpret_cast<FlatPtr>(&cell)).has_value())
            return;

        m_work_queue.append(cell);
    }

    virtual void visit_impl(ReadonlySpan<NanBoxedValue> values) override
    {
        for (auto const& value : values)
            visit(value);
    }

    void visit_all_cells()
    {
        while (!m_work_queue.is_empty()) {
            auto cell = m_work_queue.take_last();
            m_node_being_visited = &m_graph.ensure(bit_cast<FlatPtr>(cell.ptr()));
            m_node_being_visited->class_name = class_name_of(*cell);
            if (cell->state() == Cell::State::Live)
                cell->type_info().visit_edges(cell.ptr(), this);
            m_node_being_visited = nullptr;
        }
    }

    AK::JsonObject dump()
    {
        auto graph = AK::JsonObject();
        for (auto& it : m_graph) {
            AK::JsonArray edges;
            for (auto const& value : it.value.edges) {
                edges.must_append(MUST(String::formatted("{}", value)));
            }

            auto node = AK::JsonObject();
            if (it.value.root_origin.has_value()) {
                node.set("root"sv, Heap::describe_root(*it.value.root_origin));
                if (it.value.root_origin->stack_frame_index.has_value())
                    node.set("stack_frame_index"sv, it.value.root_origin->stack_frame_index.value());
            }
            node.set("class_name"sv, it.value.class_name);
            node.set("edges"sv, edges);
            graph.set(ByteString::number(it.key), node);
        }

        return graph;
    }

private:
    struct GraphNode {
        Optional<HeapRoot> root_origin;
        StringView class_name;
        HashTable<FlatPtr> edges {};
    };

    GraphNode* m_node_being_visited { nullptr };
    Vector<Ref<Cell>> m_work_queue;
    HashMap<FlatPtr, GraphNode> m_graph;
};

NO_SANITIZE_ADDRESS AK::JsonObject Heap::dump_graph()
{
    jmp_buf registers;
    setjmp(registers);
    ReadonlySpan<FlatPtr> captured_registers { reinterpret_cast<FlatPtr const*>(registers), sizeof(jmp_buf) / (sizeof(FlatPtr)) };
    return build_graph(captured_registers);
}

AK::JsonObject Heap::build_graph(ReadonlySpan<FlatPtr> callee_saved_registers)
{
    ConservativeScanOrigin origin {
        .stack_floor = bit_cast<FlatPtr>(__builtin_frame_address(0)),
        .callee_saved_registers = callee_saved_registers,
    };

    // An in-progress incremental sweep would leave parts of the heap as freelist
    // entries while the conservative scan in gather_roots() can still pick up
    // not-yet-swept (but unreachable) cells whose internal pointers lead to
    // those freelist entries. Drain the sweep so we operate on a stable heap.
    finish_pending_incremental_sweep();

    // NB: While an allocator has a block's never-allocated cells out, the block counts them as cells, and the
    //     conservative scan in gather_roots() would take a possible pointer into one for a live cell. Give them back,
    //     as a collection does, so that they are the block's lazy region again.
    give_back_local_free_lists();

    HashMap<Cell*, HeapRoot> roots;
    Vector<StackFrameInfo> stack_frames;
    gather_roots(origin, roots, &stack_frames);
    GraphConstructorVisitor visitor(roots);
    visitor.visit_all_cells();
    auto graph = visitor.dump();

    if (!stack_frames.is_empty()) {
        AK::JsonArray stack_frames_array;
        for (auto const& frame : stack_frames) {
            AK::JsonObject frame_object;
            frame_object.set("label"sv, frame.label);
            frame_object.set("size"sv, frame.size_bytes);
            stack_frames_array.must_append(move(frame_object));
        }
        graph.set("stack_frames"sv, move(stack_frames_array));
    }

    return graph;
}

String Heap::describe_root(HeapRoot const& root)
{
    switch (root.type) {
    case HeapRoot::Type::CrossHeapMember:
        return "CrossHeapMember"_string;
    case HeapRoot::Type::Root:
        return MUST(String::formatted("Root {} {}:{}", root.location->function_name(), root.location->filename(), root.location->line_number()));
    case HeapRoot::Type::RootVector:
        return "RootVector"_string;
    case HeapRoot::Type::RootHashMap:
        return "RootHashMap"_string;
    case HeapRoot::Type::RootHashTable:
        return "RootHashTable"_string;
    case HeapRoot::Type::RegisterPointer:
        return "RegisterPointer"_string;
    case HeapRoot::Type::StackPointer:
        return "StackPointer"_string;
    case HeapRoot::Type::VM:
        return "VM"_string;
    }
    VERIFY_NOT_REACHED();
}

// Visits every reachable cell breadth first, so the parent it records for each is on a shortest path from a root.
class RetainingPathVisitor final : public Cell::Visitor {
public:
    explicit RetainingPathVisitor(HashMap<Cell*, HeapRoot> const& roots)
    {
        m_queue.ensure_capacity(roots.size());
        for (auto& [root, root_origin] : roots) {
            m_parents.set(root, nullptr);
            m_queue.append(*root);
        }
    }

    virtual void visit_impl(Cell& cell) override
    {
        reach(cell);
    }

    virtual void visit_impl(ReadonlySpan<NanBoxedValue> values) override
    {
        for (auto const& value : values)
            visit(value);
    }

    void visit_all_cells()
    {
        for (size_t i = 0; i < m_queue.size(); ++i) {
            m_cell_being_visited = m_queue[i].ptr();
            if (m_cell_being_visited->state() == Cell::State::Live)
                m_cell_being_visited->type_info().visit_edges(m_cell_being_visited, this);
        }
        m_cell_being_visited = nullptr;
    }

    ReadonlySpan<Ref<Cell>> cells() const { return m_queue; }
    Cell* parent(Cell& cell) const { return *m_parents.get(&cell); }

private:
    void reach(Cell& cell)
    {
        if (m_parents.contains(&cell))
            return;
        m_parents.set(&cell, m_cell_being_visited);
        m_queue.append(cell);
    }

    HashMap<Cell*, Cell*> m_parents;
    Vector<Ref<Cell>> m_queue;
    Cell* m_cell_being_visited { nullptr };
};

NO_SANITIZE_ADDRESS Vector<Heap::RetainingPath> Heap::find_retaining_paths(AK::Function<bool(Cell&)> const& is_target, size_t max_paths)
{
    jmp_buf registers;
    setjmp(registers);
    ReadonlySpan<FlatPtr> captured_registers { reinterpret_cast<FlatPtr const*>(registers), sizeof(jmp_buf) / (sizeof(FlatPtr)) };
    return build_retaining_paths(captured_registers, is_target, max_paths);
}

Vector<Heap::RetainingPath> Heap::build_retaining_paths(ReadonlySpan<FlatPtr> callee_saved_registers, AK::Function<bool(Cell&)> const& is_target, size_t max_paths)
{
    ConservativeScanOrigin origin {
        .stack_floor = bit_cast<FlatPtr>(__builtin_frame_address(0)),
        .callee_saved_registers = callee_saved_registers,
    };

    // NB: See build_graph(), which needs a stable heap for the same reasons.
    finish_pending_incremental_sweep();
    give_back_local_free_lists();

    HashMap<Cell*, HeapRoot> roots;
    gather_roots(origin, roots);
    RetainingPathVisitor visitor(roots);
    visitor.visit_all_cells();

    Vector<RetainingPath> paths;
    for (auto const& cell : visitor.cells()) {
        if (paths.size() >= max_paths)
            break;
        if (cell->state() != Cell::State::Live || !is_target(*cell))
            continue;

        Vector<Cell*> path_cells;
        for (Cell* step = cell.ptr(); step; step = visitor.parent(*step))
            path_cells.append(step);

        RetainingPath path { .root = *roots.get(path_cells.last()), .steps = {} };
        path.steps.ensure_capacity(path_cells.size());
        for (size_t i = path_cells.size(); i > 0; --i) {
            auto& step = *path_cells[i - 1];
            path.steps.unchecked_append({ &step, class_name_of(step) });
        }
        paths.append(move(path));
    }
    return paths;
}

// This must run before any finalizer, so that no finalizer can reach a dead cell through a weak pointer. Finalizers run
// in no particular order, so such a cell may already have been finalized, and it may belong to another heap.
void Heap::clear_weak_pointers_to_dead_cells(bool report)
{
    ScopedPhaseTimer timer { report, g_phase_timings.sweep_weak_blocks_us };
    sweep_weak_blocks();
    m_weak_pointers_to_dead_cells_are_cleared = true;
}

void Heap::run_post_mark_phases(bool report)
{
    {
        ScopedPhaseTimer timer { report, g_phase_timings.finalize_unmarked_cells_us };
        finalize_unmarked_cells();
    }

    // Prune weak containers while we're still stop-the-world; doing this
    // during incremental sweep risks reading cells that have already been
    // freed and ASAN-poisoned.
    {
        ScopedPhaseTimer timer { report, g_phase_timings.prune_weak_containers_us };
        for (auto& weak_container : m_weak_containers) {
            if (!weak_container.owner_cell({}).is_marked())
                continue;
            weak_container.remove_dead_cells({});
        }
    }

    // Run sweep callbacks at STW so they fire for every collection,
    // not just CollectEverything. Static caches like
    // StaticPropertyLookupCache prune by mark state and must see valid
    // marks before incremental sweep starts freeing cells.
    {
        ScopedPhaseTimer timer { report, g_phase_timings.sweep_callbacks_us };
        for (auto& callback : m_sweep_callbacks)
            callback();
    }
}

static thread_local bool s_heap_access_is_forbidden_on_this_thread = false;

void forbid_heap_access_on_this_thread()
{
    s_heap_access_is_forbidden_on_this_thread = true;
}

bool heap_access_is_forbidden_on_this_thread()
{
    return s_heap_access_is_forbidden_on_this_thread;
}

NO_SANITIZE_ADDRESS void Heap::collect_garbage(CollectionType collection_type, bool print_report)
{
    VERIFY(!heap_access_is_forbidden_on_this_thread());
    VERIFY(is_on_owner_thread());
    jmp_buf registers;
    setjmp(registers);
    ReadonlySpan<FlatPtr> captured_registers { reinterpret_cast<FlatPtr const*>(registers), sizeof(jmp_buf) / (sizeof(FlatPtr)) };
    run_collection(captured_registers, collection_type, print_report);
}

void Heap::collect_garbage_without_scanning_the_stack()
{
    VERIFY(!heap_access_is_forbidden_on_this_thread());
    VERIFY(is_on_owner_thread());
    run_collection({}, CollectionType::CollectGarbage, false, false);
}

void Heap::run_collection(ReadonlySpan<FlatPtr> callee_saved_registers, CollectionType collection_type, bool print_report, bool scan_stack)
{
    ConservativeScanOrigin origin {
        .stack_floor = bit_cast<FlatPtr>(__builtin_frame_address(0)),
        .callee_saved_registers = callee_saved_registers,
        .scan_stack = scan_stack,
    };

    VERIFY(!m_collecting_garbage);

    update_allocation_rate();
    finish_pending_incremental_sweep();
    auto mark_start_ns = MonotonicTime::now().nanoseconds();
    give_back_local_free_lists();
    g_next_incremental_sweep_should_report = false;

    {
        TemporaryChange change(m_collecting_garbage, true);
        TemporaryChange collection_type_change(m_current_collection_type, collection_type);

        // The caller can force level 1 by passing print_report=true; LIBGC_LOG_LEVEL=N
        // raises the floor for every collection.
        auto effective_log_level = max(libgc_log_level(), print_report ? 1 : 0);
        bool report = effective_log_level >= 1;
        bool dump_allocators_too = effective_log_level >= 2;

        Core::ElapsedTimer collection_measurement_timer { Core::TimerType::Precise };
        if (report) {
            collection_measurement_timer.start();
            g_phase_timings = {};
            g_recording_phase_timings = true;
        }
        ScopeGuard stop_recording = [&] { g_recording_phase_timings = false; };

        if (collection_type == CollectionType::CollectGarbage && m_gc_deferrals) {
            m_should_gc_when_deferral_ends = true;
            return;
        }
        if (m_stats) {
            ++m_stats->collections;
            m_stats->bytes_before = block_count() * HeapBlock::BLOCK_SIZE;
            m_stats->sweep_duration_ns = 0;
        }
        m_compaction_stats->active = false;
        if (collection_type == CollectionType::CollectGarbage) {
            HashMap<Cell*, HeapRoot> roots;
            {
                ScopedPhaseTimer timer { report, g_phase_timings.gather_roots_us };
                gather_roots(origin, roots);
            }
            if (should_move_cells(collection_type, scan_stack)) {
                // NB: This collection may compact, which is what a scheduled one would be for.
                if (m_moving_collection_timer)
                    m_moving_collection_timer->stop();
                begin_evacuation();
            }
            {
                ScopedPhaseTimer timer { report, g_phase_timings.mark_live_cells_us };
                mark_live_cells(roots);
            }
        } else {
            m_marked_cell_bytes = 0;
            m_marked_external_bytes = 0;
        }
        clear_weak_pointers_to_dead_cells(report);
        run_post_mark_phases(report);
        m_weak_pointers_to_dead_cells_are_cleared = false;
        m_mark_duration_ns = MonotonicTime::now().nanoseconds() - mark_start_ns;

        // NB: A collection that moves cells moves them before it sweeps, into slots that are free already. The sweep
        //     that follows frees the dead cells and lets go of the blocks that were emptied, as after any collection, so
        //     destructors run once the collection is over, where they may allocate, and the memory that they free is
        //     still there when the slots that marking recorded are rewritten. Only finalize() may reach other cells,
        //     and it runs before any cell moves. A destructor that reaches another cell may only reach one defined in
        //     C++, which never moves.
        if (m_evacuation) {
            evacuate();
            end_evacuation();
            if (libgc_log_level() >= 1)
                print_compaction_summary(*m_compaction_stats);
        }

        // For CollectEverything we must finish sweeping synchronously so that
        // every cell is collected before the Heap destructor returns. All
        // other collection types defer sweeping to incremental work below,
        // unless incremental sweeping is disabled (no event loop to run it).
        if (collection_type == CollectionType::CollectEverything || !m_incremental_sweep_enabled) {
            ScopedPhaseTimer timer { report, g_phase_timings.sweep_dead_cells_us };
            sweep_dead_cells(report, collection_measurement_timer);
        }

        if (report) {
            size_t live_block_count = 0;
            for_each_block([&](auto&) {
                ++live_block_count;
                return IterationDecision::Continue;
            });
            print_gc_report(collection_measurement_timer.elapsed_time().to_microseconds(), live_block_count, scan_stack, m_marked_cell_bytes, m_total_allocated_bytes - m_total_allocated_bytes_at_last_collection, *m_compaction_stats);
            if (dump_allocators_too)
                dump_allocators();
        }

        g_next_incremental_sweep_should_report = report;
    }

    // Arm incremental sweep before running post-GC tasks so any cells those
    // tasks allocate get tagged as allocated-during-sweep and aren't freed
    // by sweep_block before the next mark phase reaches them.
    if (collection_type != CollectionType::CollectEverything && m_incremental_sweep_enabled)
        start_incremental_sweep();
    else
        g_next_incremental_sweep_should_report = false;

    m_last_collection_end_ns = MonotonicTime::now().nanoseconds();
    m_total_allocated_bytes_at_last_collection = m_total_allocated_bytes;
    // NB: Whatever started this collection, what was allocated before it is no longer garbage that the idle timer or
    //     the allocation threshold should count.
    m_allocated_bytes_since_last_gc = 0;

    if (m_stats) {
        m_stats->pause_duration_ns = m_last_collection_end_ns - mark_start_ns;
        if (!m_incremental_sweep_active)
            finish_stats_collection();
    }

    run_post_gc_tasks();

    // A collection just happened: restart the idle policy's episode (peak rate and watchdog tick count) from here, so
    // a threshold-driven GC mid-episode doesn't leave it comparing against stale state.
    m_idle_collection_policy.reset(m_total_allocated_bytes);
    m_idle_collection_postponed = false;

    // Marking is synchronous, so this collection has already seen any graph the timer was waiting to collect.
    if (m_abandoned_object_graph_state == AbandonedObjectGraphState::WaitingToCollect) {
        m_abandoned_object_graph_state = AbandonedObjectGraphState::WaitingToReleaseMemory;
        m_abandoned_object_graph_timer->stop();
    }
    if (m_abandoned_object_graph_state == AbandonedObjectGraphState::WaitingToReleaseMemory && !m_incremental_sweep_active)
        schedule_release_of_abandoned_object_graph_memory();
    if (!m_incremental_sweep_active)
        schedule_release_of_allocator_memory();

    if (scan_stack && m_moves_everything)
        schedule_collection_that_moves_cells();
    else if (collection_type == CollectionType::CollectGarbage && !m_incremental_sweep_active)
        schedule_compaction_if_worthwhile();
}

bool Heap::should_move_cells(CollectionType collection_type, bool scan_stack) const
{
    // NB: A cell that only the stack points to has to stay where it is, so only a collection that does not scan the
    //     stack moves cells.
    if (collection_type != CollectionType::CollectGarbage || scan_stack)
        return false;
    return m_moves_everything || m_should_compact;
}

size_t Heap::block_count() const
{
    size_t count = 0;
    for (auto& allocator : m_all_cell_allocators)
        count += allocator.block_count();
    return count;
}

// The blocks of the movable allocators, as candidates for compaction, see rust_gc_compaction_select().
static Vector<FFI::CompactionCandidate> compaction_candidates(CellAllocator::List& allocators, size_t block_count)
{
    Vector<FFI::CompactionCandidate> candidates;
    candidates.ensure_capacity(block_count);
    u32 allocator_index = 0;
    for (auto& allocator : allocators) {
        if (!allocator.descriptor().is_movable())
            continue;
        auto cell_size = static_cast<u32>(allocator.cell_size());
        allocator.for_each_block([&](HeapBlock& block) {
            auto cell_count = static_cast<u32>(block.cell_count());
            candidates.unchecked_append({
                .block = &block,
                .allocator = allocator_index,
                // NB: The free cells that the allocator took from its local block count as allocated, so the block
                //     counts as full. Every other block knows exactly which of its cells are allocated.
                .live_cells = allocator.is_local_block(block) ? cell_count : block.allocated_cell_count(),
                .cell_count = cell_count,
                .cell_size = cell_size,
                .kept_by_pins = block_descriptor_for(&block).kept_by_pins,
                .selected = false,
            });
            return IterationDecision::Continue;
        });
        ++allocator_index;
    }
    return candidates;
}

void Heap::schedule_compaction_if_worthwhile()
{
    if (!m_reducing_memory)
        return;
    // After a collection that compacted, what is left is what went over its budget. A compaction that freed little
    // means that its blocks hold pinned cells or that the counts were off, and the next one would not do better.
    auto const& last = *m_compaction_stats;
    size_t recoverable_blocks = 0;
    if (last.active) {
        if (last.freed_blocks * HeapBlock::BLOCK_SIZE >= GC_COMPACTION_MIN_RECOVERABLE_BYTES)
            recoverable_blocks = last.blocks_left_by_budget;
    } else {
        auto candidates = compaction_candidates(m_all_cell_allocators, block_count());
        recoverable_blocks = FFI::rust_gc_compaction_select(candidates.data(), candidates.size(), 0).fitting_blocks;
    }
    if (recoverable_blocks * HeapBlock::BLOCK_SIZE < GC_COMPACTION_MIN_RECOVERABLE_BYTES) {
        m_reducing_memory = false;
        m_compacted_since_activity = true;
        return;
    }
    m_should_compact = true;
    if (libgc_log_level() >= 1)
        dbgln("Scheduling a collection that compacts: {} of {} recoverable", human_readable_size(recoverable_blocks * HeapBlock::BLOCK_SIZE), human_readable_size(block_count() * HeapBlock::BLOCK_SIZE));
    schedule_collection_that_moves_cells();
}

bool Heap::moves_everything()
{
    return libgc_move_everything();
}

// Picks the blocks this collection evacuates, before it marks: the blocks that compaction selects, or every block of
// cells defined outside of C++ in the stress mode.
void Heap::begin_evacuation()
{
    VERIFY(!m_evacuation);
    // NB: A pending sweep is finished before a collection, so every block knows how many cells it holds.
    VERIFY(!m_incremental_sweep_active);
    auto& stats = *m_compaction_stats;
    stats = {};

    Vector<HeapBlock*> blocks;
    if (m_moves_everything) {
        for (auto& allocator : m_all_cell_allocators) {
            // NB: Cells defined in C++ never move. Blocks of cells defined elsewhere are evacuated whether or not
            //     their allocator is movable, which tells what keeps cells from moving.
            if (!allocator.foreign_type())
                continue;
            allocator.for_each_block([&](HeapBlock& block) {
                blocks.append(&block);
                return IterationDecision::Continue;
            });
        }
    } else {
        m_should_compact = false;
        auto select_start_ns = MonotonicTime::now().nanoseconds();
        auto candidates = compaction_candidates(m_all_cell_allocators, block_count());
        auto selection = FFI::rust_gc_compaction_select(candidates.data(), candidates.size(), GC_COMPACTION_BUDGET_BYTES);
        for (auto const& candidate : candidates) {
            if (candidate.selected)
                blocks.append(static_cast<HeapBlock*>(candidate.block));
        }
        stats.recoverable_bytes = selection.recoverable_bytes;
        stats.bytes_to_move = selection.bytes_to_move;
        stats.blocks_left_by_budget = selection.fitting_blocks - selection.selected_blocks;
        stats.select_us = (MonotonicTime::now().nanoseconds() - select_start_ns) / 1000;
        // NB: A compacting collection that selects nothing ends the phase in which the heap reduces its memory, which
        //     would otherwise go on to schedule another collection like this one, see
        //     schedule_compaction_if_worthwhile().
        if (blocks.is_empty()) {
            m_reducing_memory = false;
            m_compacted_since_activity = true;
        }
    }
    if (blocks.is_empty())
        return;

    m_evacuated_blocks = move(blocks);
    stats.active = true;
    stats.evacuated_blocks = m_evacuated_blocks.size();
    m_evacuation = FFI::rust_gc_evacuation_create(
        {
            .heap_region_base = heap_region_base(),
            .block_cells_offset = static_cast<u32>(HeapBlock::cells_offset()),
            .value_is_cell_pattern = SHIFTED_IS_CELL_PATTERN,
            .value_offset_mask = HEAP_REGION_OFFSET_MASK,
        },
        this, m_evacuated_blocks.size());
    for (auto* block : m_evacuated_blocks)
        FFI::rust_gc_evacuation_add_block(m_evacuation, block);
}

Cell* Heap::allocate_destination_for_moved_cell(CellAllocator& allocator)
{
    auto* cell = static_cast<Cell*>(FFI::rust_gc_allocator_allocate_destination(allocator.rust_allocator({})));
    ASAN_UNPOISON_MEMORY_REGION(cell, allocator.cell_size());
    return cell;
}

// Moves the live cells of the evacuated blocks that nothing pinned to free slots of other blocks of their allocator:
// takes their new slots, rewrites the slots that marking recorded and the weak pointers, copies the cells, and frees
// the old slots without destroying their cells. Nothing is swept yet, so the slots of dead cells are not free to take,
// and the sweep that follows frees the dead cells of every block, the evacuated ones included.
void Heap::evacuate()
{
    auto& stats = *m_compaction_stats;
    bool count_by_type = libgc_log_level() >= 2;
    stats.recorded_slots = FFI::rust_gc_evacuation_recorded_slot_count(m_evacuation);

    auto count_pinned = [&](Cell* cell, size_t cell_size, CompactionStats::PinReason reason) {
        stats.pinned[reason].add(cell_size);
        if (count_by_type)
            stats.pinned_by_type.ensure(class_name_of(*cell))[reason].add(cell_size);
    };

    // NB: The free cells of a block have to be back in their block before the slots of cells that move are freed.
    //     Nothing has allocated since the collection gave them back, see allocate_cell(), so this only makes sure.
    give_back_local_free_lists();

    struct AllocatorMoves {
        CellAllocator* allocator;
        Vector<FFI::CellMove> moves;
    };
    Vector<AllocatorMoves> all_moves;
    Array<void*, FFI::MAX_CELLS_PER_BLOCK> cells;
    Vector<HeapBlock*> blocks_kept_by_pins;

    auto copy_start_ns = MonotonicTime::now().nanoseconds();
    // NB: The blocks of an allocator come one after another, see begin_evacuation().
    for (size_t first_block_of_allocator = 0; first_block_of_allocator < m_evacuated_blocks.size();) {
        auto& allocator = m_evacuated_blocks[first_block_of_allocator]->cell_allocator();
        auto cell_size = allocator.cell_size();
        auto end = first_block_of_allocator;
        while (end < m_evacuated_blocks.size() && &m_evacuated_blocks[end]->cell_allocator() == &allocator)
            ++end;

        Vector<FFI::CellMove> moves;
        for (size_t i = exchange(first_block_of_allocator, end); i < end; ++i) {
            auto& block = *m_evacuated_blocks[i];
            auto& descriptor = block_descriptor_for(&block);
            // NB: The cells that stay are the marked ones that do not move. The allocated cells that are not marked died,
            //     and the sweep frees them.
            size_t staying_cells = 0;
            if (count_by_type || !allocator.descriptor().is_movable()) {
                for_each_allocated_cell(block, [&](Cell* cell) {
                    if (!is_marked(descriptor, cell))
                        return;
                    if (!allocator.descriptor().is_movable()) {
                        count_pinned(cell, cell_size, CompactionStats::NonMovableAllocator);
                        ++staying_cells;
                    } else if (auto reason = FFI::rust_gc_evacuation_pin_reason(m_evacuation, cell); reason != FFI::PinReason::Unpinned) {
                        stats.pinned_by_type.ensure(class_name_of(*cell))[reason == FFI::PinReason::Root ? CompactionStats::ByValueRoot : CompactionStats::ByValueEdge].add(cell_size);
                    }
                });
            }
            if (allocator.descriptor().is_movable()) {
                FFI::PinnedCells pinned {};
                auto count = FFI::rust_gc_evacuation_cells_to_move(m_evacuation, &block, cells.data(), &pinned);
                moves.grow_capacity(moves.size() + count);
                for (size_t j = 0; j < count; ++j)
                    moves.unchecked_append({ .from = cells[j], .to = nullptr });
                auto& by_edge = stats.pinned[CompactionStats::ByValueEdge];
                by_edge.cells += pinned.by_edge;
                by_edge.bytes += pinned.by_edge * cell_size;
                auto& by_root = stats.pinned[CompactionStats::ByValueRoot];
                by_root.cells += pinned.by_root;
                by_root.bytes += pinned.by_root * cell_size;
                staying_cells = pinned.by_edge + pinned.by_root;
                if (staying_cells > 0)
                    blocks_kept_by_pins.append(&block);
            }
            if (staying_cells == 0)
                ++stats.freed_blocks;
        }
        if (moves.is_empty())
            continue;

        // NB: Every destination is taken before any cell moves, so no cell moves twice, and no old slot is free yet. A
        //     cell that moves is marked where it goes, so that the sweep that follows keeps it.
        for (auto& cell_move : moves) {
            cell_move.to = allocate_destination_for_moved_cell(allocator);
            set_marked(cell_move.to);
            stats.moved.add(cell_size);
        }
        FFI::rust_gc_evacuation_forward_cells(m_evacuation, moves.data(), moves.size(), cell_size);
        all_moves.append({ &allocator, move(moves) });
    }
    auto fix_up_start_ns = MonotonicTime::now().nanoseconds();
    stats.copy_us = (fix_up_start_ns - copy_start_ns) / 1000;

    // NB: The slots are rewritten where they are, before any cell is copied, so a cell that moves takes its rewritten
    //     slots along.
    if (!all_moves.is_empty()) {
        stats.rewritten_slots = FFI::rust_gc_evacuation_fix_up_slots(m_evacuation);
        auto forwarding_address = [this](void* cell) {
            return FFI::rust_gc_evacuation_forwarding_address(m_evacuation, cell);
        };
        for (auto& weak_block : m_usable_weak_blocks)
            weak_block.update_moved_cells(forwarding_address);
        for (auto& weak_block : m_full_weak_blocks)
            weak_block.update_moved_cells(forwarding_address);
    }
    auto copy_start_again_ns = MonotonicTime::now().nanoseconds();
    stats.fix_up_us = (copy_start_again_ns - fix_up_start_ns) / 1000;

    for (auto& [allocator, moves] : all_moves)
        FFI::rust_gc_evacuation_move_cells(m_evacuation, moves.data(), moves.size(), allocator->cell_size());
    auto release_start_ns = MonotonicTime::now().nanoseconds();
    stats.copy_us += (release_start_ns - copy_start_again_ns) / 1000;

    for (auto& [allocator, moves] : all_moves) {
        for (auto& cell_move : moves)
            FFI::rust_gc_allocator_release_moved_cell(allocator->rust_allocator({}), cell_move.from);
    }

    // NB: What is left in these blocks is what pins kept there, and evacuating them again would not free them, see
    //     rust_gc_compaction_select().
    for (auto* block : blocks_kept_by_pins)
        FFI::rust_gc_allocator_keep_cells_pinned(block->cell_allocator().rust_allocator({}), block);
    stats.release_us = (MonotonicTime::now().nanoseconds() - release_start_ns) / 1000;
}

bool Heap::is_live_cell(Cell const* cell) const
{
    VERIFY(m_collecting_garbage);
    if (!cell)
        return false;
    auto address = bit_cast<FlatPtr>(cell);
    if (address < heap_region_base() + HEAP_REGION_LOW_GUARD_SIZE || address >= BlockAllocator::heap_region_committed_end())
        return false;
    // NB: A cell of a run that an allocator took counts as allocated, but it is still a free cell until the allocator
    //     hands it out. Every collection gives the runs back before it marks.
    return allocated_cell_containing(this, address, HeapBlock::cells_offset()) == cell && cell->state() == Cell::State::Live;
}

void Heap::end_evacuation()
{
    FFI::rust_gc_evacuation_destroy(m_evacuation);
    m_evacuation = nullptr;
    m_evacuated_blocks.clear();
}

void Heap::schedule_collection_that_moves_cells()
{
    // NB: The stress mode moves cells as often as it can: after each collection that scans the stack, the next turn of
    //     the event loop collects again without scanning it, if the embedder vouches that its outermost turns hold no
    //     cells.
    if (!m_outermost_event_loop_turn_holds_no_cells)
        return;
    if (!m_moving_collection_timer) {
        m_moving_collection_timer = Core::Timer::create_single_shot(AK::Duration::zero(), [this] {
            if (!m_moves_everything && !m_should_compact)
                return;
            // NB: Only an outermost turn of the event loop that is not deferring GC can collect without scanning the
            //     stack, so try again later rather than spin while a nested loop runs.
            if (is_gc_deferred() || Core::ThreadEventQueue::current().is_processing_nested()) {
                m_moving_collection_timer->start(AK::Duration::from_milliseconds(GC_IDLE_GC_INTERVAL_MS));
                return;
            }
            // NB: Whatever the event loop has queued goes first, for one turn at most, so that a queue that is never
            //     empty does not hold off the collection for good, as in idle_gc_on_timer().
            if (!m_moving_collection_postponed && Core::ThreadEventQueue::current().has_pending_events()) {
                m_moving_collection_postponed = true;
                m_moving_collection_timer->start(AK::Duration::zero());
                return;
            }
            m_moving_collection_postponed = false;
            collect_garbage_from_timer(CollectionTrigger::Compaction);
        });
    }
    // NB: This replaces a retry that waits for a nested event loop or a GC deferral to end.
    m_moving_collection_postponed = false;
    m_moving_collection_timer->restart(AK::Duration::zero());
}

void Heap::update_allocation_rate()
{
    auto now_ns = MonotonicTime::now().nanoseconds();
    if (now_ns > m_last_collection_end_ns)
        m_allocation_bytes_per_second = static_cast<double>(m_total_allocated_bytes - m_total_allocated_bytes_at_last_collection) * 1e9 / static_cast<double>(now_ns - m_last_collection_end_ns);
}

void Heap::set_should_collect_on_every_allocation(bool should_collect)
{
    m_should_collect_on_every_allocation = should_collect;
    // NB: Code that pops local free lists itself would take cells from them without collecting.
    if (should_collect)
        give_back_local_free_lists();
}

void Heap::give_back_local_free_lists()
{
    for (auto& allocator : m_all_cell_allocators)
        allocator.give_back_local_free_list({});
}

void Heap::run_post_gc_tasks()
{
    auto post_move_tasks = move(m_post_move_tasks);
    for (auto& task : post_move_tasks)
        task();
    auto tasks = move(m_post_gc_tasks);
    for (auto& task : tasks)
        task();
}

void Heap::dump_allocators()
{
    // NB: While an allocator has a run of a block's free cells out, they count as allocated. Give them back, so that
    //     they are not counted.
    give_back_local_free_lists();

    size_t total_in_committed_blocks = 0;
    size_t total_waste = 0;
    for (auto& allocator : m_all_cell_allocators) {
        struct BlockStats {
            HeapBlock& block;
            size_t live_cells { 0 };
            size_t dead_cells { 0 };
            size_t total_cells { 0 };
        };
        Vector<BlockStats> blocks;

        size_t total_live_cells = 0;
        size_t total_dead_cells = 0;
        size_t cell_count = (HeapBlock::BLOCK_SIZE - sizeof(HeapBlock)) / allocator.cell_size();

        allocator.for_each_block([&](HeapBlock& heap_block) {
            BlockStats block { heap_block };

            for_each_allocated_cell(heap_block, [&](Cell* cell) {
                VERIFY(cell->state() == Cell::State::Live);
                ++block.live_cells;
            });
            block.dead_cells = heap_block.cell_count() - block.live_cells;
            total_live_cells += block.live_cells;
            total_dead_cells += block.dead_cells;

            blocks.append({ block });
            return IterationDecision::Continue;
        });

        if (blocks.is_empty())
            continue;

        total_in_committed_blocks += blocks.size() * HeapBlock::BLOCK_SIZE;

        StringBuilder builder;
        if (allocator.class_name().has_value())
            builder.appendff("{} ({}b)", allocator.class_name().value(), allocator.cell_size());
        else
            builder.appendff("generic ({}b)", allocator.cell_size());

        builder.appendff(" x {}", total_live_cells);

        size_t cost = blocks.size() * HeapBlock::BLOCK_SIZE / KiB;
        size_t reserved = allocator.block_allocator().block_count() * HeapBlock::BLOCK_SIZE / KiB;
        builder.appendff(", cost: {} KiB, reserved: {} KiB", cost, reserved);

        size_t total_dead_bytes = ((blocks.size() * cell_count) - total_live_cells) * allocator.cell_size();
        if (total_dead_bytes) {
            builder.appendff(", waste: {} KiB", total_dead_bytes / KiB);
            total_waste += total_dead_bytes;
        }

        dbgln("{}", builder.string_view());

        for (auto& block : blocks) {
            dbgln("  block at {:p}: live {} / dead {} / total {} cells", &block.block, block.live_cells, block.dead_cells, block.block.cell_count());
        }
    }
    dbgln("Total allocated: {} KiB", total_in_committed_blocks / KiB);
    dbgln("Total wasted on fragmentation: {} KiB", total_waste / KiB);
}

// Counts the edges to cells that cells visit, in their own fields or in storage they own, without marking anything.
class CensusVisitor final : public Cell::Visitor {
public:
    size_t cell_edges { 0 };

private:
    virtual void visit_impl(Cell&) override { ++cell_edges; }
    virtual void visit_impl(ReadonlySpan<NanBoxedValue> values) override
    {
        for (auto const& value : values) {
            if (value.is_cell())
                ++cell_edges;
        }
    }
};

AK::JsonObject Heap::census()
{
    VERIFY(!m_collecting_garbage);

    // NB: Unswept cells of the last collection are still allocated, and so are the cells of the runs that allocators
    //     have out, so finish the sweep and give those back first.
    finish_pending_incremental_sweep();
    give_back_local_free_lists();

    size_t total_block_bytes = 0;
    size_t total_live_bytes = 0;
    AK::JsonArray allocators;
    for (auto& allocator : m_all_cell_allocators) {
        auto const& type_info = allocator.type_info();

        size_t blocks = 0;
        size_t cell_slots = 0;
        size_t live_cells = 0;
        size_t external_bytes = 0;
        CensusVisitor visitor;
        struct ClassCounts {
            size_t live_cells { 0 };
            size_t cell_edges { 0 };
        };
        HashMap<StringView, ClassCounts> counts_by_class;
        allocator.for_each_block([&](HeapBlock& block) {
            ++blocks;
            cell_slots += block.cell_count();
            for_each_allocated_cell(block, [&](Cell* cell) {
                ++live_cells;
                auto edges_before = visitor.cell_edges;
                type_info.visit_edges(cell, &visitor);
                if (type_info.external_memory_size)
                    external_bytes += type_info.external_memory_size(cell);
                // A C++ allocator holds exactly one class, but cells defined in other languages can share an allocator
                // with their subclasses.
                if (type_info.class_name) {
                    size_t length = 0;
                    auto const* name = type_info.class_name(cell, &length);
                    auto& counts = counts_by_class.ensure(StringView { name, length }, [] { return ClassCounts {}; });
                    ++counts.live_cells;
                    counts.cell_edges += visitor.cell_edges - edges_before;
                }
            });
            return IterationDecision::Continue;
        });
        if (blocks == 0)
            continue;

        auto block_bytes = blocks * HeapBlock::BLOCK_SIZE;
        auto live_bytes = live_cells * allocator.cell_size();
        total_block_bytes += block_bytes;
        total_live_bytes += live_bytes;

        AK::JsonObject entry;
        entry.set("name"sv, allocator.class_name().value_or("generic"sv));
        entry.set("cell_size"sv, allocator.cell_size());
        entry.set("blocks"sv, blocks);
        entry.set("block_bytes"sv, block_bytes);
        entry.set("cell_slots"sv, cell_slots);
        entry.set("live_cells"sv, live_cells);
        entry.set("live_bytes"sv, live_bytes);
        entry.set("free_bytes"sv, block_bytes - live_bytes);
        entry.set("external_bytes"sv, external_bytes);
        entry.set("cell_edges"sv, visitor.cell_edges);
        if (!counts_by_class.is_empty()) {
            AK::JsonObject classes;
            for (auto& it : counts_by_class) {
                AK::JsonObject counts;
                counts.set("live_cells"sv, it.value.live_cells);
                counts.set("cell_edges"sv, it.value.cell_edges);
                classes.set(it.key, move(counts));
            }
            entry.set("classes"sv, move(classes));
        }
        allocators.must_append(move(entry));
    }

    AK::JsonObject census;
    census.set("block_size"sv, HeapBlock::BLOCK_SIZE);
    census.set("block_bytes"sv, total_block_bytes);
    census.set("live_bytes"sv, total_live_bytes);
    census.set("allocators"sv, move(allocators));
    return census;
}

void Heap::enqueue_post_gc_task(AK::Function<void()> task)
{
    m_post_gc_tasks.append(move(task));
}

void Heap::enqueue_post_move_task(AK::Function<void()> task)
{
    m_post_move_tasks.append(move(task));
}

void Heap::register_sweep_callback(AK::Function<void()> callback)
{
    m_sweep_callbacks.append(move(callback));
}

void Heap::gather_roots(ConservativeScanOrigin const& origin, HashMap<Cell*, HeapRoot>& roots, Vector<StackFrameInfo>* out_stack_frames, IncludeIncomingCrossHeapMembers include_incoming_cross_heap_members)
{
    for (auto& allocator : m_all_cell_allocators)
        VERIFY(!FFI::rust_gc_allocator_local_block(allocator.rust_allocator({})));

    // Cross-heap members targeting this heap act as roots for local collections (as the foreign holder is invisible to a local mark).
    if (include_incoming_cross_heap_members == IncludeIncomingCrossHeapMembers::Yes) {
        for (auto* member : m_incoming_cross_heap_members) {
            if (auto* cell = member->cell_base())
                roots.set(cell, HeapRoot { .type = HeapRoot::Type::CrossHeapMember });
        }
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.gather_embedder_roots_us };
        m_gather_embedder_roots(roots);
    }
    if (origin.scan_stack) {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.gather_conservative_roots_us };
        gather_conservative_roots(origin, roots, out_stack_frames);
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.gather_explicit_roots_us };
        for (auto& root : m_roots)
            roots.set(root.cell(), HeapRoot { .type = HeapRoot::Type::Root, .location = &root.source_location() });

        for (auto& vector : m_root_vectors)
            vector.gather_roots(roots);

        for (auto& hash_map : m_root_hash_maps)
            hash_map.gather_roots(roots);
    }

    for (auto& hash_table : m_root_hash_tables)
        hash_table.gather_roots(roots);

    if constexpr (HEAP_DEBUG) {
        dbgln("gather_roots:");
        for (auto* root : roots.keys())
            dbgln("  + {}", root);
    }
}

#ifdef HAS_ADDRESS_SANITIZER
NO_SANITIZE_ADDRESS static void gather_asan_fake_stack_roots(PossiblePointers& possible_pointers, FlatPtr addr, PossiblePointerBounds const& bounds, FlatPtr stack_reference, FlatPtr stack_top)
{
    void* begin = nullptr;
    void* end = nullptr;
    void* real_stack = __asan_addr_is_in_fake_stack(__asan_get_current_fake_stack(), reinterpret_cast<void*>(addr), &begin, &end);
    if (real_stack == nullptr)
        return;

    // Only consider stack addresses that are inside the real stack's active range. ASan keeps fake frames in a
    // per-thread pool after the owning function returns, and we need to take care not to resurrect dead pointers from
    // below the stack pointer.
    auto real_stack_addr = bit_cast<FlatPtr>(real_stack);
    if (real_stack_addr < stack_reference || real_stack_addr >= stack_top)
        return;

    for (auto* real_stack_addr = reinterpret_cast<void const* const*>(begin); real_stack_addr < end; ++real_stack_addr) {
        // ASan hands out fake frames from a pool without clearing them, so the parts of a frame that its function has not
        // written yet still hold whatever an earlier function left there, often a pointer to a cell. ASan poisons the
        // redzones and every local that is out of scope, so no live value can be in a poisoned byte. Skip those: a word
        // is addressable up to its first poisoned byte, which leaves the low bytes of the value on a little-endian
        // machine.
        auto* first_poisoned_byte = static_cast<u8 const*>(__asan_region_is_poisoned(const_cast<void*>(static_cast<void const*>(real_stack_addr)), sizeof(FlatPtr)));
        auto addressable_bytes = first_poisoned_byte ? static_cast<size_t>(first_poisoned_byte - reinterpret_cast<u8 const*>(real_stack_addr)) : sizeof(FlatPtr);
        if (addressable_bytes == 0)
            continue;
        auto real_address = reinterpret_cast<FlatPtr>(*real_stack_addr);
        if (addressable_bytes < sizeof(FlatPtr))
            real_address &= (static_cast<FlatPtr>(1) << (addressable_bytes * 8)) - 1;
        if (real_address == 0)
            continue;
        add_possible_value(possible_pointers, real_address, HeapRoot { .type = HeapRoot::Type::StackPointer }, bounds);
    }
}
#else
static void gather_asan_fake_stack_roots(PossiblePointers&, FlatPtr, PossiblePointerBounds const&, FlatPtr, FlatPtr)
{
}
#endif

NO_SANITIZE_ADDRESS void Heap::gather_conservative_roots(ConservativeScanOrigin const& origin, HashMap<Cell*, HeapRoot>& roots, Vector<StackFrameInfo>* out_stack_frames)
{
    dbgln_if(HEAP_DEBUG, "gather_conservative_roots:");

    PossiblePointers possible_pointers;
    auto bounds = PossiblePointerBounds::current();

    // The scan covers the entry point's frame and its callers; the collection's own frames below the floor are left out.
    FlatPtr own_frame;
    auto stack_reference = origin.stack_floor;
    auto stack_top = m_stack_info.top();
    VERIFY(stack_reference > bit_cast<FlatPtr>(&own_frame) && stack_reference < stack_top);

    // The captured registers are a buffer in the entry point's frame, so they fall inside the range scanned below.
    auto captured_registers_start = bit_cast<FlatPtr>(origin.callee_saved_registers.data());
    auto captured_registers_end = captured_registers_start + origin.callee_saved_registers.size() * sizeof(FlatPtr);
    VERIFY(captured_registers_start % sizeof(FlatPtr) == 0);
    VERIFY(captured_registers_start >= stack_reference && captured_registers_end <= stack_top);

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.conservative_register_scan_us };
        for (auto register_value : origin.callee_saved_registers) {
            add_possible_value(possible_pointers, register_value, HeapRoot { .type = HeapRoot::Type::RegisterPointer }, bounds);
            gather_asan_fake_stack_roots(possible_pointers, register_value, bounds, stack_reference, stack_top);
        }
    }

    // Build frame boundary map for annotation if requested.
    // Each entry maps a frame pointer address to the stack frame index in out_stack_frames.
    struct FrameBoundary {
        FlatPtr start;
        u32 frame_index;
    };
    Vector<FrameBoundary> frame_boundaries;

#ifdef LIBGC_HAS_CPPTRACE
    if (out_stack_frames) {
        // Walk the frame pointer chain to collect frame boundaries and return addresses.
        Vector<FlatPtr> frame_starts;
        std::vector<cpptrace::frame_ptr> return_addresses;

        FlatPtr current_fp = bit_cast<FlatPtr>(__builtin_frame_address(0));
        AK::unwind_stack_from_frame_pointer(
            current_fp,
            [&](FlatPtr address) -> Optional<FlatPtr> {
                if (address < stack_reference || address >= stack_top)
                    return {};
                return *reinterpret_cast<FlatPtr*>(address);
            },
            [&](AK::StackFrame frame) -> IterationDecision {
                // Ensure the previous FP is above the current one (stack grows downward).
                if (frame.previous_frame_pointer != 0 && frame.previous_frame_pointer <= current_fp)
                    return IterationDecision::Break;
                frame_starts.append(current_fp);
                return_addresses.push_back(static_cast<cpptrace::frame_ptr>(frame.return_address) - 1);
                current_fp = frame.previous_frame_pointer;
                return IterationDecision::Continue;
            });

        if (!frame_starts.is_empty()) {
            auto resolved = cpptrace::raw_trace { move(return_addresses) }.resolve();

            auto format_frame_label = [](cpptrace::stacktrace_frame const& frame) -> String {
                StringBuilder label;
                if (!frame.symbol.empty()) {
                    label.append(StringView(frame.symbol.c_str(), frame.symbol.length()));
                    if (frame.line.has_value()) {
                        auto filename = StringView { frame.filename.c_str(), frame.filename.length() };
                        auto last_slash = filename.find_last('/');
                        if (last_slash.has_value())
                            filename = filename.substring_view(*last_slash + 1);
                        label.appendff(" {}:{}", filename, frame.line.value());
                    }
                }
                return MUST(label.to_string());
            };

            // resolve() may expand inline frames, so there can be more resolved
            // frames than return addresses. We want the non-inline frame for each
            // return address, since that represents the actual function whose
            // locals occupy the stack range.
            frame_boundaries.ensure_capacity(frame_starts.size());
            size_t raw_frame_index = 0;
            for (size_t i = 0; i < resolved.frames.size() && raw_frame_index < frame_starts.size(); ++i) {
                auto const& frame = resolved.frames[i];
                if (frame.is_inline) {
                    out_stack_frames->append({ .label = format_frame_label(frame) });
                    continue;
                }

                auto frame_label_index = static_cast<u32>(out_stack_frames->size());
                auto frame_start = frame_starts[raw_frame_index];
                auto frame_end = frame_starts.get(raw_frame_index + 1).value_or(stack_top);
                out_stack_frames->append({ .label = format_frame_label(frame), .size_bytes = frame_end - frame_start });
                frame_boundaries.append({ frame_start, frame_label_index });
                ++raw_frame_index;
            }
        }
    }
#else
    (void)out_stack_frames;
#endif

    // Find the frame index for a given stack address. Frame boundaries are sorted ascending
    // by start address. We want the last boundary whose start is <= the address.
    auto frame_index_for_stack_address = [&](FlatPtr address) -> Optional<u32> {
        if (frame_boundaries.is_empty())
            return {};
        if (address < frame_boundaries[0].start || address >= stack_top)
            return {};
        size_t nearby = 0;
        binary_search(frame_boundaries, address, &nearby, [](FlatPtr addr, FrameBoundary const& boundary) {
            return static_cast<int>(addr - boundary.start);
        });
        return frame_boundaries[nearby].frame_index;
    };

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.conservative_stack_scan_us };
        for (FlatPtr stack_address = stack_reference; stack_address < stack_top; stack_address += sizeof(FlatPtr)) {
            // Stepping over the captured registers keeps roots that live only in a register attributed as RegisterPointer.
            if (stack_address == captured_registers_start) {
                stack_address = captured_registers_end - sizeof(FlatPtr);
                continue;
            }
            auto data = *reinterpret_cast<FlatPtr*>(stack_address);
            add_possible_value(possible_pointers, data, HeapRoot { .type = HeapRoot::Type::StackPointer, .stack_frame_index = frame_index_for_stack_address(stack_address) }, bounds);
            gather_asan_fake_stack_roots(possible_pointers, data, bounds, stack_reference, stack_top);
        }
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.conservative_cell_lookup_us };
        for_each_cell_among_possible_pointers(*this, possible_pointers, [&](Cell* cell, HashMap<FlatPtr, HeapRoot>& candidates, FlatPtr possible_pointer) {
            if (cell->state() == Cell::State::Live) {
                dbgln_if(HEAP_DEBUG, "  ?-> {}", (void const*)cell);
                roots.set(cell, *candidates.get(possible_pointer));
            } else {
                dbgln_if(HEAP_DEBUG, "  #-> {}", (void const*)cell);
            }
        });
    }
}

// The visitor of one marker of a collection, which hands everything it visits straight to the marker.
class MarkingVisitor final : public Cell::Visitor {
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit MarkingVisitor(FFI::Marker* marker)
    {
        m_marker = reinterpret_cast<GCMarker*>(marker);
    }

    virtual void visit_impl(Cell& cell) override
    {
        FFI::rust_gc_marker_visit_cell(marker(), &cell);
    }

    virtual void visit_impl(ReadonlySpan<NanBoxedValue> values) override
    {
        FFI::rust_gc_marker_visit_values(marker(), reinterpret_cast<u64 const*>(values.data()), values.size());
    }

    virtual void visit_ephemeron(Cell& key, NanBoxedValue const& value) override
    {
        if (!value.is_cell())
            return;
        FFI::rust_gc_marker_visit_ephemeron(marker(), &key, &value.as_cell());
    }

private:
    FFI::Marker* marker() const { return reinterpret_cast<FFI::Marker*>(m_marker); }
};

void Heap::mark_live_cells(HashMap<Cell*, HeapRoot>& roots)
{
    Heap* domain[] = { this };
    mark_live_cells_across(domain, roots);
}

// OPTIMIZATION: Marking starts helper threads only once the heaps it marks are this large, and the collecting thread did
//               not finish alone soon.
static constexpr size_t GC_PARALLEL_MARKING_MIN_HEAP_BYTES = 8 * MiB;

void Heap::mark_live_cells_across(ReadonlySpan<Heap* const> heaps, HashMap<Cell*, HeapRoot>& roots)
{
    dbgln_if(HEAP_DEBUG, "mark_live_cells:");

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.mark_clear_uprooted_us };
        for (auto* heap : heaps) {
            for (auto& cell : heap->m_uprooted_cells)
                roots.remove(cell.ptr());

            heap->m_uprooted_cells.clear();
        }
    }

    Vector<void const*> root_cells;
    size_t heap_bytes = 0;
    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.mark_initial_visit_us };
        root_cells.ensure_capacity(roots.size());
        for (auto* root : roots.keys())
            root_cells.unchecked_append(root);
        for (auto* heap : heaps)
            heap_bytes += heap->block_count() * HeapBlock::BLOCK_SIZE;
    }

    Vector<size_t, 1> marked_cell_bytes;
    Vector<size_t, 1> marked_external_bytes;
    marked_cell_bytes.resize(heaps.size());
    marked_external_bytes.resize(heaps.size());

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    FFI::MarkRequest request {
        .layout = {
            .heap_region_base = heap_region_base(),
            .cell_state_offset = static_cast<u32>(offsetof(Cell, m_state)),
            .cell_state_live = to_underlying(Cell::State::Live),
            .type_info_visit_edges_offset = static_cast<u32>(offsetof(CellTypeInfo, visit_edges)),
            .type_info_external_memory_size_offset = static_cast<u32>(offsetof(CellTypeInfo, external_memory_size)),
            .value_is_cell_pattern = SHIFTED_IS_CELL_PATTERN,
            .value_offset_mask = HEAP_REGION_OFFSET_MASK,
        },
        .heaps = reinterpret_cast<void const* const*>(heaps.data()),
        .heap_count = heaps.size(),
        .roots = root_cells.data(),
        .root_count = root_cells.size(),
        .parallel = heap_bytes >= GC_PARALLEL_MARKING_MIN_HEAP_BYTES,
        .context = nullptr,
        .create_visitor = [](void*, FFI::Marker* marker) -> void* {
            return static_cast<Cell::Visitor*>(new MarkingVisitor(marker));
        },
        .destroy_visitor = [](void*, void* visitor) { delete static_cast<MarkingVisitor*>(static_cast<Cell::Visitor*>(visitor)); },
        .marked_cell_bytes = marked_cell_bytes.data(),
        .marked_external_bytes = marked_external_bytes.data(),
        .threads_used = 0,
        // NB: Only a collection of one heap moves cells.
        .evacuation = heaps.size() == 1 ? heaps[0]->m_evacuation : nullptr,
    };
#pragma GCC diagnostic pop

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.mark_bfs_us };
        FFI::rust_gc_marker_mark(&request);
    }
    g_phase_timings.mark_threads = request.threads_used;

    for (size_t i = 0; i < heaps.size(); ++i) {
        heaps[i]->m_marked_cell_bytes = marked_cell_bytes[i];
        heaps[i]->m_marked_external_bytes = marked_external_bytes[i];
    }
}

void Heap::mark_cell_allocated_during_incremental_sweep(Cell& cell)
{
    auto* block = HeapBlock::from_cell(&cell);
    if (block->is_pending_sweep() || block->is_being_swept())
        set_marked(&cell);
}

void Heap::finalize_unmarked_cells()
{
    for_each_block([&](auto& block) {
        auto* finalize = block.type_info().finalize;
        if (!finalize)
            return IterationDecision::Continue;
        auto& descriptor = block_descriptor_for(&block);
        for_each_allocated_cell(block, [&](Cell* cell) {
            if (!is_marked(descriptor, cell))
                finalize(cell);
        });
        return IterationDecision::Continue;
    });
}

void Heap::sweep_weak_blocks()
{
    for (auto& weak_block : m_usable_weak_blocks) {
        weak_block.sweep();
    }
    Vector<WeakBlock&> now_usable_weak_blocks;
    for (auto& weak_block : m_full_weak_blocks) {
        weak_block.sweep();
        if (weak_block.can_allocate())
            now_usable_weak_blocks.append(weak_block);
    }
    for (auto& weak_block : now_usable_weak_blocks) {
        m_usable_weak_blocks.append(weak_block);
    }
}

void Heap::sweep_dead_cells(bool print_report, Core::ElapsedTimer const& measurement_timer)
{
    dbgln_if(HEAP_DEBUG, "sweep_dead_cells:");
    auto sweep_start_ns = MonotonicTime::now().nanoseconds();

    size_t collected_cells = 0;
    size_t live_cells = 0;
    size_t collected_cell_bytes = 0;
    size_t live_cell_bytes = 0;
    size_t freed_blocks = 0;

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.sweep_block_iteration_us };
        for (auto& allocator : m_all_cell_allocators) {
            auto* rust_allocator = allocator.rust_allocator({});
            FFI::SweepStats stats {};
            FFI::rust_gc_allocator_start_sweep(rust_allocator);
            while (FFI::rust_gc_allocator_sweep_next_block(rust_allocator, false, &stats)) { }
            collected_cells += stats.collected_cells;
            live_cells += stats.live_cells;
            collected_cell_bytes += stats.collected_cells * allocator.cell_size();
            live_cell_bytes += stats.live_cells * allocator.cell_size();
            freed_blocks += stats.freed_blocks;
        }
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.sweep_update_threshold_us };
        m_sweep_duration_ns += MonotonicTime::now().nanoseconds() - sweep_start_ns;
        update_gc_bytes_threshold();
    }

    if (print_report) {
        g_sweep_stats = {
            .collected_cells = collected_cells,
            .live_cells = live_cells,
            .collected_cell_bytes = collected_cell_bytes,
            .live_cell_bytes = live_cell_bytes,
            .live_external_bytes = m_marked_external_bytes,
            .freed_block_count = freed_blocks,
        };
    }
    (void)measurement_timer;

    // CollectEverything finishes synchronously, and is currently only used
    // when the heap is going away. Do not spin up background decommit work
    // from process teardown.
}

void Heap::sweep_block(HeapBlock& block)
{
    auto start_ns = MonotonicTime::now().nanoseconds();
    FFI::SweepStats stats {};
    FFI::rust_gc_allocator_sweep_pending_block(block.cell_allocator().rust_allocator({}), &block, &stats);
    did_sweep(stats.swept_blocks, MonotonicTime::now().nanoseconds() - start_ns);
}

void Heap::did_sweep(size_t swept_blocks, i64 duration_ns)
{
    m_blocks_swept += swept_blocks;
    m_sweep_duration_ns += duration_ns;
    if (m_stats)
        m_stats->sweep_duration_ns += duration_ns;
}

void Heap::did_sweep_blocks_for_allocation(Badge<CellAllocator>, size_t swept_blocks, i64 duration_ns)
{
    // NB: An allocation from a destructor that a sweep runs is part of that sweep, whose time already counts.
    did_sweep(swept_blocks, m_block_sweeps_in_progress == 0 ? duration_ns : 0);
}

bool Heap::sweep_next_block()
{
    if (!m_incremental_sweep_active)
        return true;

    if (is_gc_deferred())
        return true;

    // Find the next allocator that has blocks pending sweep.
    while (auto* allocator = m_allocators_to_sweep.first()) {
        auto start_ns = MonotonicTime::now().nanoseconds();
        FFI::SweepStats stats {};
        if (FFI::rust_gc_allocator_sweep_next_block(allocator->rust_allocator({}), true, &stats)) {
            did_sweep(stats.swept_blocks, MonotonicTime::now().nanoseconds() - start_ns);
            if (!allocator->has_blocks_pending_sweep())
                allocator->m_sweep_list_node.remove();
            return false;
        }
        // Allocator was drained by allocation-directed sweeping.
        allocator->m_sweep_list_node.remove();
    }

    return true;
}

void Heap::start_incremental_sweep()
{
    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] === Starting incremental sweep ===");

    m_incremental_sweep_active = true;
    incremental_sweep_stats().should_report = false;
    incremental_sweep_stats().total_blocks = 0;
    incremental_sweep_stats().batches.clear();
    incremental_sweep_stats().should_report = g_next_incremental_sweep_should_report;
    g_next_incremental_sweep_should_report = false;
    if (incremental_sweep_stats().should_report)
        incremental_sweep_stats().timer.start();

    // Populate each allocator's pending sweep list with its current blocks.
    // Blocks allocated during incremental sweep won't be on these lists
    // and don't need sweeping.
    size_t total_blocks = 0;
    for (auto& allocator : m_all_cell_allocators) {
        auto blocks = FFI::rust_gc_allocator_start_sweep(allocator.rust_allocator({}));
        total_blocks += blocks;
        if (blocks)
            m_allocators_to_sweep.append(allocator);
    }
    incremental_sweep_stats().total_blocks = total_blocks;
    m_blocks_to_sweep = total_blocks;
    m_blocks_swept = 0;

    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] {} blocks to sweep", total_blocks);

    start_incremental_sweep_timer();
}

void Heap::finish_incremental_sweep()
{
    update_gc_bytes_threshold();

    // NB: A graph abandoned during this sweep was alive when marking ran, so the threshold just set counts it.
    if (m_abandoned_object_graph_state == AbandonedObjectGraphState::WaitingToCollect)
        limit_gc_bytes_threshold_for_abandoned_object_graph();

    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] === Sweep complete ===");
    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep]     Live cell bytes: {} ({} KiB)", m_marked_cell_bytes, m_marked_cell_bytes / KiB);
    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep]     Live external bytes: {} ({} KiB)", m_marked_external_bytes, m_marked_external_bytes / KiB);
    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep]     Next GC threshold: {} ({} KiB)", m_gc_bytes_threshold, m_gc_bytes_threshold / KiB);
    print_incremental_sweep_report(m_marked_cell_bytes, m_marked_external_bytes, m_gc_bytes_threshold);

    m_incremental_sweep_active = false;
    if (m_stats)
        finish_stats_collection();

    stop_incremental_sweep_timer();

    // Sweep is done; kick the global decommit worker so the slots we just freed get madvise()'d off the GC pause path.
    BlockAllocator::wake_decommit_worker_async();

    if (m_abandoned_object_graph_state == AbandonedObjectGraphState::WaitingToReleaseMemory)
        schedule_release_of_abandoned_object_graph_memory();
    schedule_release_of_allocator_memory();
    schedule_compaction_if_worthwhile();
}

void Heap::finish_pending_incremental_sweep()
{
    if (!m_incremental_sweep_active || is_gc_deferred())
        return;

    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] Finishing pending sweep...");
    size_t blocks_swept = 0;
    auto start_time = MonotonicTime::now();
    while (m_incremental_sweep_active) {
        if (sweep_next_block()) {
            auto elapsed = MonotonicTime::now() - start_time;
            record_incremental_sweep_batch(blocks_swept, elapsed.to_microseconds(), true);
            finish_incremental_sweep();
            break;
        }
        ++blocks_swept;
    }
}

void Heap::sweep_in_step_with_allocation()
{
    if (!m_incremental_sweep_active || is_gc_deferred())
        return;

    // Keep the share of blocks swept ahead of the share of the allocation allowance used since the collection. The
    // allowance comes from the smallest threshold that the cells this collection found alive can give, since the
    // threshold itself is only updated once the sweep is done. A collection that something asked for does not reset
    // the bytes allocated towards the threshold, so this counts from the end of the collection instead.
    auto smallest_threshold = max(static_cast<size_t>(static_cast<double>(m_marked_cell_bytes) * GC_MIN_GROWTH_FACTOR), GC_MIN_BYTES_THRESHOLD);
    auto allowance = max(smallest_threshold / GC_SWEEP_ALLOCATION_SHARE_DENOMINATOR, static_cast<size_t>(1));
    auto allocated = min(static_cast<size_t>(m_total_allocated_bytes - m_total_allocated_bytes_at_last_collection), allowance);
    auto blocks_due = static_cast<size_t>(static_cast<double>(m_blocks_to_sweep) * static_cast<double>(allocated) / static_cast<double>(allowance));
    // NB: Once every block is due, the sweep goes on until it finds nothing left to sweep, which finishes it.
    auto is_ahead = [&] { return m_blocks_swept >= blocks_due && blocks_due < m_blocks_to_sweep; };
    if (is_ahead())
        return;

    size_t blocks_swept = 0;
    auto start_time = MonotonicTime::now();
    while (!is_ahead()) {
        if (sweep_next_block()) {
            record_incremental_sweep_batch(blocks_swept, (MonotonicTime::now() - start_time).to_microseconds(), false);
            finish_incremental_sweep();
            return;
        }
        ++blocks_swept;
    }
    record_incremental_sweep_batch(blocks_swept, (MonotonicTime::now() - start_time).to_microseconds(), false);
}

void Heap::start_incremental_sweep_timer()
{
    if (!m_incremental_sweep_timer) {
        m_incremental_sweep_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(GC_INCREMENTAL_SWEEP_INTERVAL_MS), [this] {
            sweep_on_timer();
        });
    }
    m_incremental_sweep_timer->start();
}

void Heap::stop_incremental_sweep_timer()
{
    if (m_incremental_sweep_timer)
        m_incremental_sweep_timer->stop();
}

void Heap::sweep_on_timer()
{
    if (!m_incremental_sweep_active)
        return;

    if (is_gc_deferred())
        return;

    size_t blocks_swept = 0;
    bool finished_sweep = false;
    auto start_time = MonotonicTime::now();
    auto deadline = start_time + AK::Duration::from_milliseconds(GC_INCREMENTAL_SWEEP_SLICE_MS);
    while (MonotonicTime::now() < deadline) {
        if (sweep_next_block()) {
            auto elapsed = MonotonicTime::now() - start_time;
            record_incremental_sweep_batch(blocks_swept, elapsed.to_microseconds(), false);
            finish_incremental_sweep();
            finished_sweep = true;
            break;
        }
        ++blocks_swept;
    }

    if (blocks_swept > 0 && !finished_sweep) {
        auto elapsed = MonotonicTime::now() - start_time;
        record_incremental_sweep_batch(blocks_swept, elapsed.to_microseconds(), false);
        dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] Timer slice: {} blocks in {}ms",
            blocks_swept, elapsed.to_milliseconds());
    }
}

void Heap::start_idle_gc_timer()
{
    if (!m_idle_gc_timer) {
        m_idle_gc_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(GC_IDLE_GC_INTERVAL_MS), [this] {
            idle_gc_on_timer();
        });
    }
    m_idle_collection_policy.reset(m_total_allocated_bytes);
    m_idle_gc_timer->start();
}

void Heap::idle_gc_on_timer()
{
    // Leave an in-progress incremental sweep alone; it is already reclaiming memory. A GC deferral means now is not a
    // safe time to collect. In both cases we reconsider on the next tick.
    if (m_incremental_sweep_active || is_gc_deferred())
        return;

    // NB: Only a heap whose outermost event loop turns hold no cells ever runs a collection that moves them, so no other
    //     heap looks for blocks to compact.
    bool may_reduce_memory = libgc_compact() && !m_moves_everything && m_outermost_event_loop_turn_holds_no_cells;
    IdleCollectionPolicy::SteadyBudget steady_budget {
        .bytes = max(static_cast<size_t>(static_cast<double>(m_marked_cell_bytes) * GC_STEADY_GROWTH_FACTOR), GC_MIN_BYTES_THRESHOLD),
        .ns_since_last_collection = MonotonicTime::now().nanoseconds() - m_last_collection_end_ns,
        .min_ns_between_collections = static_cast<i64>(static_cast<double>(m_mark_duration_ns) * (1 - GC_STEADY_TIME_FRACTION) / GC_STEADY_TIME_FRACTION),
    };
    auto decision = m_idle_collection_policy.evaluate(m_total_allocated_bytes, m_allocated_bytes_since_last_gc, m_gc_bytes_threshold, steady_budget);
    switch (decision) {
    case IdleCollectionPolicy::Decision::KeepWaiting:
        // NB: A heap that went quiet with little garbage still has the holes that its last sweep left, which
        //     compacting collections close.
        if (may_reduce_memory && !m_compacted_since_activity && m_idle_collection_policy.last_tick_was_quiet()
            && !Core::ThreadEventQueue::current().has_pending_events()) {
            m_reducing_memory = true;
            schedule_compaction_if_worthwhile();
        }
        return;
    case IdleCollectionPolicy::Decision::Park:
        // Nothing left to collect; the next allocation will re-arm the timer.
        m_idle_gc_timer->stop();
        return;
    case IdleCollectionPolicy::Decision::Collect:
    case IdleCollectionPolicy::Decision::CollectSteadyBudget:
        // NB: Whatever the event loop has queued, like input or a task, goes first, and the next tick looks again.
        //     What has not reached the queue yet, like a message still in its socket, waits for the collection. It
        //     waits for one tick at most, so that a queue that is never empty does not hold off the collection and the
        //     watchdog for good.
        if (!m_idle_collection_postponed && Core::ThreadEventQueue::current().has_pending_events()) {
            m_idle_collection_postponed = true;
            return;
        }
        // A heap that went quiet gives back what it can, compacting after this collection, see
        // schedule_compaction_if_worthwhile().
        m_reducing_memory = may_reduce_memory && m_idle_collection_policy.last_tick_was_quiet();
        m_compacted_since_activity = false;
        m_allocated_bytes_since_last_gc = 0;
        collect_garbage_from_timer(decision == IdleCollectionPolicy::Decision::Collect ? CollectionTrigger::IdlePolicy : CollectionTrigger::SteadyBudget);
        m_idle_gc_timer->stop();
        return;
    }
}

void Heap::limit_gc_bytes_threshold_for_abandoned_object_graph()
{
    // The threshold came from a live size that still counts the abandoned objects. Left alone, whatever comes next
    // could allocate that whole budget on top of them.
    m_gc_bytes_threshold = min(m_gc_bytes_threshold, m_allocated_bytes_since_last_gc + GC_MIN_BYTES_THRESHOLD);
}

void Heap::did_abandon_object_graph()
{
    VERIFY(is_on_owner_thread());
    limit_gc_bytes_threshold_for_abandoned_object_graph();

    if (m_abandoned_object_graph_state == AbandonedObjectGraphState::WaitingToCollect)
        return;
    m_abandoned_object_graph_state = AbandonedObjectGraphState::WaitingToCollect;

    if (!m_abandoned_object_graph_timer) {
        m_abandoned_object_graph_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(GC_ABANDONED_OBJECT_GRAPH_MIN_DELAY_MS), [this] {
            on_abandoned_object_graph_timer();
        });
    }

    // Marking pauses everything, so a heap that is expensive to mark waits longer. After this long, one marking pause
    // makes up the share of running time we want to spend collecting.
    auto marking_cost_budget_ns = static_cast<double>(m_mark_duration_ns) * (1 - GC_TARGET_TIME_FRACTION) / GC_TARGET_TIME_FRACTION;
    m_abandoned_object_graph_timer->restart(max(AK::Duration::from_milliseconds(GC_ABANDONED_OBJECT_GRAPH_MIN_DELAY_MS), AK::Duration::from_nanoseconds(static_cast<i64>(marking_cost_budget_ns))));
}

void Heap::on_abandoned_object_graph_timer()
{
    if (m_abandoned_object_graph_state == AbandonedObjectGraphState::WaitingToReleaseMemory) {
        m_abandoned_object_graph_state = AbandonedObjectGraphState::None;
        ak_kmalloc_collect();
        return;
    }

    // A collection now would have to finish the sweep in one go, and a deferral means now is not a safe time to
    // collect.
    if (m_incremental_sweep_active || is_gc_deferred()) {
        m_abandoned_object_graph_timer->restart();
        return;
    }

    m_allocated_bytes_since_last_gc = 0;
    collect_garbage_from_timer(CollectionTrigger::AbandonedObjectGraph);
}

void Heap::collect_garbage_from_timer(CollectionTrigger trigger)
{
    TemporaryChange trigger_change { g_collection_trigger, trigger };
    // A timer fires from the event loop. In its outermost turn the only frames on the stack are the event loop's and the
    // embedder's, so if the embedder vouches for its own, there is no root to find there. Words that only look like a
    // cell pointer are still there, and some never change, so scanning them could keep a cell alive for good. A nested
    // event loop has its caller's frames further up the stack, and those need the scan.
    if (m_outermost_event_loop_turn_holds_no_cells && !Core::ThreadEventQueue::current().is_processing_nested()) {
        collect_garbage_without_scanning_the_stack();
        return;
    }
    collect_garbage();
}

void Heap::collect_garbage_and_return_memory_to_system()
{
    // NB: A collection that has to wait for a deferral to end still lets go of as much as this one would. What it
    //     frees goes back to the system some seconds later, as after any other collection.
    if (is_gc_deferred()) {
        m_should_gc_when_deferral_ends = true;
        m_should_return_memory_when_deferral_ends = true;
        return;
    }
    {
        TemporaryChange change { m_is_returning_memory_to_system, true };
        collect_garbage();
        finish_pending_incremental_sweep();
    }
    for (auto& allocator : m_all_cell_allocators)
        allocator.block_allocator().return_freed_blocks_to_system();
    ak_kmalloc_collect();
}

void Heap::schedule_release_of_abandoned_object_graph_memory()
{
    // NB: A timer, since a sweep can finish in the middle of any allocation, and the allocator can't be collected from
    //     everywhere.
    m_abandoned_object_graph_timer->restart(AK::Duration::zero());
}

void Heap::schedule_release_of_allocator_memory()
{
    if (!m_allocator_memory_release_timer) {
        m_allocator_memory_release_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(GC_ALLOCATOR_MEMORY_RELEASE_DELAY_MS), [] {
            ak_kmalloc_collect();
            FFI::rust_gc_marker_release_unused_helper_memory();
        });
    }
    m_allocator_memory_release_timer->restart();
}

void Heap::defer_gc()
{
    ++m_gc_deferrals;
}

void Heap::undefer_gc()
{
    VERIFY(m_gc_deferrals > 0);
    --m_gc_deferrals;

    if (m_gc_deferrals || !m_should_gc_when_deferral_ends)
        return;

    // NB: A destructor that a block sweep runs can end a deferral of its own, e.g. the one around constructing a cell
    //     it allocates. For the reason will_allocate() gives, no collection may start there, so the request waits for
    //     a deferral that ends outside the sweep.
    if (m_block_sweeps_in_progress > 0)
        return;

    {
        TemporaryChange change { m_is_returning_memory_to_system, m_should_return_memory_when_deferral_ends };
        TemporaryChange trigger { g_collection_trigger, CollectionTrigger::DeferralEnded };
        collect_garbage();
    }
    m_should_gc_when_deferral_ends = false;
    m_should_return_memory_when_deferral_ends = false;
}

void Heap::uproot_cell(Cell* cell)
{
    m_uprooted_cells.append(cell);
}

WeakImpl* Heap::create_weak_impl(void* ptr)
{
    // Once weak pointers to dead cells have been cleared, one that is created to a dead cell, e.g. by a finalizer of
    // this or another heap, must be null as well, or nothing would clear it before the cell is swept.
    if (m_weak_pointers_to_dead_cells_are_cleared && !static_cast<Cell*>(ptr)->is_marked())
        return &WeakImpl::the_null_weak_impl;

    if (m_usable_weak_blocks.is_empty()) {
        auto* weak_block = WeakBlock::create();
        m_usable_weak_blocks.append(*weak_block);
    }

    auto* weak_block = m_usable_weak_blocks.first();
    auto* new_weak_impl = weak_block->allocate(static_cast<Cell*>(ptr));
    if (!weak_block->can_allocate()) {
        m_full_weak_blocks.append(*weak_block);
    }

    return new_weak_impl;
}

}
