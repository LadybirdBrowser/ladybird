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
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/NumberFormat.h>
#include <AK/Platform.h>
#include <AK/ScopeGuard.h>
#include <AK/StackInfo.h>
#include <AK/StackUnwinder.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
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

static constexpr size_t GC_MIN_BYTES_THRESHOLD { 8 * 1024 * 1024 };
static constexpr size_t GC_HEAP_GROWTH_FACTOR_NUMERATOR { 7 };
static constexpr size_t GC_HEAP_GROWTH_FACTOR_DENOMINATOR { 4 };

static constexpr int GC_INCREMENTAL_SWEEP_INTERVAL_MS = 16;
static constexpr int GC_INCREMENTAL_SWEEP_SLICE_MS = 5;

static Heap* s_the;

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

// Per-phase timings recorded during a single collect_garbage() call. We keep
// these at file scope (instead of threading more parameters through the GC's
// internal helpers) since GC is single-threaded, guarded by m_collecting_garbage.
struct PhaseTimings {
    // Top-level phases.
    i64 gather_roots_us { 0 };
    i64 mark_live_cells_us { 0 };
    i64 finalize_unmarked_cells_us { 0 };
    i64 sweep_weak_blocks_us { 0 };
    i64 sweep_dead_cells_us { 0 };

    // gather_roots() subphases.
    i64 gather_must_survive_roots_us { 0 };
    i64 gather_embedder_roots_us { 0 };
    i64 gather_conservative_roots_us { 0 };
    i64 gather_explicit_roots_us { 0 };

    // gather_conservative_roots() subphases.
    i64 conservative_register_scan_us { 0 };
    i64 conservative_stack_scan_us { 0 };
    i64 conservative_vector_scan_us { 0 };
    i64 conservative_cell_lookup_us { 0 };

    // mark_live_cells() subphases.
    i64 mark_initial_visit_us { 0 };
    i64 mark_bfs_us { 0 };
    i64 mark_clear_uprooted_us { 0 };

    // sweep_dead_cells() subphases.
    i64 sweep_block_iteration_us { 0 };
    i64 sweep_weak_containers_us { 0 };
    i64 sweep_callbacks_us { 0 };
    i64 sweep_block_reclassify_us { 0 };
    i64 sweep_update_threshold_us { 0 };
};
PhaseTimings g_phase_timings;

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
SweepStats g_sweep_stats;

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
IncrementalSweepStats g_incremental_sweep_stats;
bool g_next_incremental_sweep_should_report { false };

// Set by collect_garbage() while a reported collection is in flight. Used by
// the GC's helpers to decide whether they should record subphase timings.
bool g_recording_phase_timings { false };

void print_gc_report(i64 total_us, size_t live_block_count)
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
    dbgln("       Live cells: {} ({})", s.live_cells, human_readable_size(s.live_cell_bytes));
    dbgln("    Live external: {}", human_readable_size(s.live_external_bytes));
    dbgln("  Collected cells: {} ({})", s.collected_cells, human_readable_size(s.collected_cell_bytes));
    dbgln("      Live blocks: {} ({})", live_block_count, human_readable_size(live_block_count * HeapBlock::BLOCK_SIZE));
    dbgln("     Freed blocks: {} ({})", s.freed_block_count, human_readable_size(s.freed_block_count * HeapBlock::BLOCK_SIZE));
    dbgln("");
    dbgln("Phase breakdown (us, % of total):");
    dbgln("  gather_roots                  {:>10} us ({:>5.1f}%)", t.gather_roots_us, pct(t.gather_roots_us));
    dbgln("    must-survive scan           {:>10} us ({:>5.1f}%)", t.gather_must_survive_roots_us, pct(t.gather_must_survive_roots_us));
    dbgln("    embedder roots              {:>10} us ({:>5.1f}%)", t.gather_embedder_roots_us, pct(t.gather_embedder_roots_us));
    dbgln("    conservative roots          {:>10} us ({:>5.1f}%)", t.gather_conservative_roots_us, pct(t.gather_conservative_roots_us));
    dbgln("      register scan             {:>10} us ({:>5.1f}%)", t.conservative_register_scan_us, pct(t.conservative_register_scan_us));
    dbgln("      stack scan                {:>10} us ({:>5.1f}%)", t.conservative_stack_scan_us, pct(t.conservative_stack_scan_us));
    dbgln("      conservative-vector scan  {:>10} us ({:>5.1f}%)", t.conservative_vector_scan_us, pct(t.conservative_vector_scan_us));
    dbgln("      cell lookup               {:>10} us ({:>5.1f}%)", t.conservative_cell_lookup_us, pct(t.conservative_cell_lookup_us));
    dbgln("    explicit roots              {:>10} us ({:>5.1f}%)", t.gather_explicit_roots_us, pct(t.gather_explicit_roots_us));
    dbgln("  mark_live_cells               {:>10} us ({:>5.1f}%)", t.mark_live_cells_us, pct(t.mark_live_cells_us));
    dbgln("    initial visit               {:>10} us ({:>5.1f}%)", t.mark_initial_visit_us, pct(t.mark_initial_visit_us));
    dbgln("    BFS marking                 {:>10} us ({:>5.1f}%)", t.mark_bfs_us, pct(t.mark_bfs_us));
    dbgln("    clear uprooted              {:>10} us ({:>5.1f}%)", t.mark_clear_uprooted_us, pct(t.mark_clear_uprooted_us));
    dbgln("  finalize_unmarked_cells       {:>10} us ({:>5.1f}%)", t.finalize_unmarked_cells_us, pct(t.finalize_unmarked_cells_us));
    dbgln("  sweep_weak_blocks             {:>10} us ({:>5.1f}%)", t.sweep_weak_blocks_us, pct(t.sweep_weak_blocks_us));
    dbgln("  sweep_dead_cells              {:>10} us ({:>5.1f}%)", t.sweep_dead_cells_us, pct(t.sweep_dead_cells_us));
    dbgln("    block iteration             {:>10} us ({:>5.1f}%)", t.sweep_block_iteration_us, pct(t.sweep_block_iteration_us));
    dbgln("    weak containers             {:>10} us ({:>5.1f}%)", t.sweep_weak_containers_us, pct(t.sweep_weak_containers_us));
    dbgln("    sweep callbacks             {:>10} us ({:>5.1f}%)", t.sweep_callbacks_us, pct(t.sweep_callbacks_us));
    dbgln("    block reclassify            {:>10} us ({:>5.1f}%)", t.sweep_block_reclassify_us, pct(t.sweep_block_reclassify_us));
    dbgln("    update threshold            {:>10} us ({:>5.1f}%)", t.sweep_update_threshold_us, pct(t.sweep_update_threshold_us));
    dbgln("=================================================================");
}

void record_incremental_sweep_batch(size_t blocks_swept, i64 elapsed_us, bool forced)
{
    if (!g_incremental_sweep_stats.should_report || blocks_swept == 0)
        return;
    g_incremental_sweep_stats.batches.append({
        .blocks_swept = blocks_swept,
        .elapsed_us = elapsed_us,
        .forced = forced,
    });
}

void print_incremental_sweep_report(size_t live_cell_bytes, size_t live_external_bytes, size_t next_gc_bytes_threshold)
{
    if (!g_incremental_sweep_stats.should_report)
        return;

    size_t swept_blocks = 0;
    i64 batch_time_us = 0;
    i64 shortest_batch_us = NumericLimits<i64>::max();
    i64 longest_batch_us = 0;
    for (auto const& batch : g_incremental_sweep_stats.batches) {
        swept_blocks += batch.blocks_swept;
        batch_time_us += batch.elapsed_us;
        shortest_batch_us = min(shortest_batch_us, batch.elapsed_us);
        longest_batch_us = max(longest_batch_us, batch.elapsed_us);
    }

    if (g_incremental_sweep_stats.batches.is_empty())
        shortest_batch_us = 0;

    dbgln("Incremental sweep report");
    dbgln("=================================================================");
    dbgln("Totals:");
    dbgln("        Wall time: {} us", g_incremental_sweep_stats.timer.elapsed_time().to_microseconds());
    dbgln("       Batch time: {} us", batch_time_us);
    dbgln("          Batches: {}", g_incremental_sweep_stats.batches.size());
    dbgln("    Swept blocks: {} / {} ({})", swept_blocks, g_incremental_sweep_stats.total_blocks, human_readable_size(swept_blocks * HeapBlock::BLOCK_SIZE));
    dbgln("     Live cells: {}", human_readable_size(live_cell_bytes));
    dbgln("  Live external: {}", human_readable_size(live_external_bytes));
    dbgln("  Next threshold: {}", human_readable_size(next_gc_bytes_threshold));
    dbgln("");
    dbgln("Batch timings:");
    dbgln("  Shortest batch: {} us", shortest_batch_us);
    dbgln("   Longest batch: {} us", longest_batch_us);
    for (size_t i = 0; i < g_incremental_sweep_stats.batches.size(); ++i) {
        auto const& batch = g_incremental_sweep_stats.batches[i];
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

Heap::Heap(AK::Function<void(HashMap<Cell*, GC::HeapRoot>&)> gather_embedder_roots)
    : m_gather_embedder_roots(move(gather_embedder_roots))
{
    s_the = this;
    m_gc_bytes_threshold = GC_MIN_BYTES_THRESHOLD;
    static_assert(HeapBlock::min_possible_cell_size <= 32, "Heap Cell tracking uses too much data!");
}

Heap::~Heap()
{
    collect_garbage(CollectionType::CollectEverything);
}

void Heap::will_allocate(size_t size)
{
    if (should_collect_on_every_allocation()) {
        m_allocated_bytes_since_last_gc = 0;
        collect_garbage();
    } else if (m_allocated_bytes_since_last_gc + size > m_gc_bytes_threshold) {
        m_allocated_bytes_since_last_gc = 0;
        collect_garbage();
    }

    m_allocated_bytes_since_last_gc += size;
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

void Heap::update_gc_bytes_threshold(size_t live_cell_bytes, size_t live_external_bytes)
{
    Checked<size_t> live_bytes = live_cell_bytes;
    live_bytes += live_external_bytes;

    if (live_bytes.has_overflow()) {
        m_gc_bytes_threshold = NumericLimits<size_t>::max();
        return;
    }

    Checked<size_t> next_gc_bytes_threshold = live_bytes.value();
    next_gc_bytes_threshold *= GC_HEAP_GROWTH_FACTOR_NUMERATOR;
    next_gc_bytes_threshold /= GC_HEAP_GROWTH_FACTOR_DENOMINATOR;

    if (next_gc_bytes_threshold.has_overflow()) {
        m_gc_bytes_threshold = NumericLimits<size_t>::max();
        return;
    }

    m_gc_bytes_threshold = max(next_gc_bytes_threshold.value(), GC_MIN_BYTES_THRESHOLD);
}

static void add_possible_value(HashMap<FlatPtr, HeapRoot>& possible_pointers, FlatPtr data, HeapRoot origin, FlatPtr min_block_address, FlatPtr max_block_address)
{
    if constexpr (sizeof(FlatPtr*) == sizeof(NanBoxedValue)) {
        // Because NanBoxedValue stores pointers in non-canonical form we have to check if the top bytes
        // match any pointer-backed tag, in that case we have to extract the pointer to its
        // canonical form and add that as a possible pointer.
        FlatPtr possible_pointer;
        if ((data & SHIFTED_IS_CELL_PATTERN) == SHIFTED_IS_CELL_PATTERN)
            possible_pointer = NanBoxedValue::extract_pointer_bits(data);
        else
            possible_pointer = data;
        if (possible_pointer < min_block_address || possible_pointer > max_block_address)
            return;
        possible_pointers.set(possible_pointer, move(origin));
    } else {
        static_assert((sizeof(NanBoxedValue) % sizeof(FlatPtr*)) == 0);
        if (data < min_block_address || data > max_block_address)
            return;
        // In the 32-bit case we will look at the top and bottom part of NanBoxedValue separately we just
        // add both the upper and lower bytes as possible pointers.
        possible_pointers.set(data, move(origin));
    }
}

void Heap::find_min_and_max_block_addresses(FlatPtr& min_address, FlatPtr& max_address)
{
    min_address = explode_byte(0xff);
    max_address = 0;
    for (auto& allocator : m_all_cell_allocators) {
        min_address = min(min_address, allocator.min_block_address());
        max_address = max(max_address, allocator.max_block_address() + HeapBlock::BLOCK_SIZE);
    }
}

template<typename Callback>
static void for_each_cell_among_possible_pointers(HashTable<HeapBlock*> const& all_live_heap_blocks, HashMap<FlatPtr, HeapRoot>& possible_pointers, Callback callback)
{
    for (auto possible_pointer : possible_pointers.keys()) {
        if (!possible_pointer)
            continue;
        auto* possible_heap_block = HeapBlock::from_cell(reinterpret_cast<Cell const*>(possible_pointer));
        if (!all_live_heap_blocks.contains(possible_heap_block))
            continue;
        if (auto* cell = possible_heap_block->cell_from_possible_pointer(possible_pointer)) {
            callback(cell, possible_pointer);
        }
    }
}

class GraphConstructorVisitor final : public Cell::Visitor {
public:
    explicit GraphConstructorVisitor(Heap& heap, HashMap<Cell*, HeapRoot> const& roots)
        : m_heap(heap)
    {
        m_heap.find_min_and_max_block_addresses(m_min_block_address, m_max_block_address);
        m_work_queue.ensure_capacity(roots.size());

        for (auto& [root, root_origin] : roots) {
            auto& graph_node = m_graph.ensure(bit_cast<FlatPtr>(root));
            graph_node.class_name = root->class_name();
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

    virtual void visit_possible_values(ReadonlyBytes bytes) override
    {
        HashMap<FlatPtr, HeapRoot> possible_pointers;

        auto* raw_pointer_sized_values = reinterpret_cast<FlatPtr const*>(bytes.data());
        for (size_t i = 0; i < (bytes.size() / sizeof(FlatPtr)); ++i)
            add_possible_value(possible_pointers, raw_pointer_sized_values[i], HeapRoot { .type = HeapRoot::Type::HeapFunctionCapturedPointer }, m_min_block_address, m_max_block_address);

        for_each_cell_among_possible_pointers(m_heap.m_live_heap_blocks, possible_pointers, [&](Cell* cell, FlatPtr) {
            if (cell->state() != Cell::State::Live)
                return;

            if (m_node_being_visited)
                m_node_being_visited->edges.set(reinterpret_cast<FlatPtr>(cell));

            if (m_graph.get(reinterpret_cast<FlatPtr>(cell)).has_value())
                return;
            m_work_queue.append(*cell);
        });
    }

    void visit_all_cells()
    {
        while (!m_work_queue.is_empty()) {
            auto cell = m_work_queue.take_last();
            m_node_being_visited = &m_graph.ensure(bit_cast<FlatPtr>(cell.ptr()));
            m_node_being_visited->class_name = cell->class_name();
            cell->visit_edges(*this);
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
                auto type = it.value.root_origin->type;
                auto const* location = it.value.root_origin->location;
                switch (type) {
                case HeapRoot::Type::ConservativeVector:
                    node.set("root"sv, "ConservativeVector"sv);
                    break;
                case HeapRoot::Type::HeapFunctionCapturedPointer:
                    node.set("root"sv, "HeapFunctionCapturedPointer"sv);
                    break;
                case HeapRoot::Type::MustSurviveGC:
                    node.set("root"sv, "MustSurviveGC"sv);
                    break;
                case HeapRoot::Type::Root:
                    node.set("root"sv, MUST(String::formatted("Root {} {}:{}", location->function_name(), location->filename(), location->line_number())));
                    break;
                case HeapRoot::Type::RootVector:
                    node.set("root"sv, "RootVector"sv);
                    break;
                case HeapRoot::Type::RootHashMap:
                    node.set("root"sv, "RootHashMap"sv);
                    break;
                case HeapRoot::Type::RegisterPointer:
                    node.set("root"sv, "RegisterPointer"sv);
                    if (it.value.root_origin->stack_frame_index.has_value())
                        node.set("stack_frame_index"sv, it.value.root_origin->stack_frame_index.value());
                    break;
                case HeapRoot::Type::StackPointer:
                    node.set("root"sv, "StackPointer"sv);
                    if (it.value.root_origin->stack_frame_index.has_value())
                        node.set("stack_frame_index"sv, it.value.root_origin->stack_frame_index.value());
                    break;
                case HeapRoot::Type::VM:
                    node.set("root"sv, "VM"sv);
                    break;
                }
                VERIFY(node.has("root"sv));
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

    Heap& m_heap;
    FlatPtr m_min_block_address;
    FlatPtr m_max_block_address;
};

AK::JsonObject Heap::dump_graph()
{
    // An in-progress incremental sweep would leave parts of the heap as freelist
    // entries while the conservative scan in gather_roots() can still pick up
    // not-yet-swept (but unreachable) cells whose internal pointers lead to
    // those freelist entries. Drain the sweep so we operate on a stable heap.
    finish_pending_incremental_sweep();

    HashMap<Cell*, HeapRoot> roots;
    Vector<StackFrameInfo> stack_frames;
    gather_roots(roots, &stack_frames);
    GraphConstructorVisitor visitor(*this, roots);
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

void Heap::collect_garbage(CollectionType collection_type, bool print_report)
{
    VERIFY(!m_collecting_garbage);

    finish_pending_incremental_sweep();
    g_next_incremental_sweep_should_report = false;

    {
        TemporaryChange change(m_collecting_garbage, true);

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

        if (collection_type == CollectionType::CollectGarbage) {
            if (m_gc_deferrals) {
                m_should_gc_when_deferral_ends = true;
                return;
            }
            HashMap<Cell*, HeapRoot> roots;
            {
                ScopedPhaseTimer timer { report, g_phase_timings.gather_roots_us };
                gather_roots(roots);
            }
            {
                ScopedPhaseTimer timer { report, g_phase_timings.mark_live_cells_us };
                mark_live_cells(roots);
            }
        }
        {
            ScopedPhaseTimer timer { report, g_phase_timings.finalize_unmarked_cells_us };
            finalize_unmarked_cells();
        }
        {
            ScopedPhaseTimer timer { report, g_phase_timings.sweep_weak_blocks_us };
            sweep_weak_blocks();
        }

        // Prune weak containers while we're still stop-the-world; doing this
        // during incremental sweep risks reading cells that have already been
        // freed and ASAN-poisoned.
        for (auto& weak_container : m_weak_containers) {
            if (!weak_container.owner_cell({}).is_marked())
                continue;
            weak_container.remove_dead_cells({});
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

        // For CollectEverything we must finish sweeping synchronously so that
        // every cell is collected before the Heap destructor returns. All
        // other collection types defer sweeping to incremental work below.
        if (collection_type == CollectionType::CollectEverything) {
            ScopedPhaseTimer timer { report, g_phase_timings.sweep_dead_cells_us };
            sweep_dead_cells(report, collection_measurement_timer);
        }

        if (report) {
            size_t live_block_count = 0;
            for_each_block([&](auto&) {
                ++live_block_count;
                return IterationDecision::Continue;
            });
            print_gc_report(collection_measurement_timer.elapsed_time().to_microseconds(), live_block_count);
            if (dump_allocators_too)
                dump_allocators();
        }

        g_next_incremental_sweep_should_report = report;
    }

    // Arm incremental sweep before running post-GC tasks so any cells those
    // tasks allocate get tagged as allocated-during-sweep and aren't freed
    // by sweep_block before the next mark phase reaches them.
    if (collection_type != CollectionType::CollectEverything)
        start_incremental_sweep();
    else
        g_next_incremental_sweep_should_report = false;

    run_post_gc_tasks();
}

void Heap::run_post_gc_tasks()
{
    auto tasks = move(m_post_gc_tasks);
    for (auto& task : tasks)
        task();
}

void Heap::dump_allocators()
{
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

            heap_block.for_each_cell([&](Cell* cell) {
                if (cell->state() == Cell::State::Live)
                    ++block.live_cells;
                else if (cell->state() == Cell::State::Dead)
                    ++block.dead_cells;
                else
                    VERIFY_NOT_REACHED();
            });
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

void Heap::enqueue_post_gc_task(AK::Function<void()> task)
{
    m_post_gc_tasks.append(move(task));
}

void Heap::register_sweep_callback(AK::Function<void()> callback)
{
    m_sweep_callbacks.append(move(callback));
}

void Heap::gather_roots(HashMap<Cell*, HeapRoot>& roots, Vector<StackFrameInfo>* out_stack_frames)
{
    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.gather_must_survive_roots_us };
        for_each_block([&](auto& block) {
            if (block.overrides_must_survive_garbage_collection()) {
                block.template for_each_cell_in_state<Cell::State::Live>([&](Cell* cell) {
                    if (cell->must_survive_garbage_collection()) {
                        roots.set(cell, HeapRoot { .type = HeapRoot::Type::MustSurviveGC });
                    }
                });
            }

            return IterationDecision::Continue;
        });
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.gather_embedder_roots_us };
        m_gather_embedder_roots(roots);
    }
    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.gather_conservative_roots_us };
        gather_conservative_roots(roots, out_stack_frames);
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

    if constexpr (HEAP_DEBUG) {
        dbgln("gather_roots:");
        for (auto* root : roots.keys())
            dbgln("  + {}", root);
    }
}

#ifdef HAS_ADDRESS_SANITIZER
NO_SANITIZE_ADDRESS void Heap::gather_asan_fake_stack_roots(HashMap<FlatPtr, HeapRoot>& possible_pointers, FlatPtr addr, FlatPtr min_block_address, FlatPtr max_block_address, FlatPtr stack_reference, FlatPtr stack_top)
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
        void const* real_address = *real_stack_addr;
        if (real_address == nullptr)
            continue;
        add_possible_value(possible_pointers, reinterpret_cast<FlatPtr>(real_address), HeapRoot { .type = HeapRoot::Type::StackPointer }, min_block_address, max_block_address);
    }
}
#else
void Heap::gather_asan_fake_stack_roots(HashMap<FlatPtr, HeapRoot>&, FlatPtr, FlatPtr, FlatPtr, FlatPtr, FlatPtr)
{
}
#endif

NO_SANITIZE_ADDRESS void Heap::gather_conservative_roots(HashMap<Cell*, HeapRoot>& roots, Vector<StackFrameInfo>* out_stack_frames)
{
    FlatPtr dummy;

    dbgln_if(HEAP_DEBUG, "gather_conservative_roots:");

    jmp_buf buf;
    setjmp(buf);

    HashMap<FlatPtr, HeapRoot> possible_pointers;

    auto* raw_jmp_buf = reinterpret_cast<FlatPtr const*>(buf);

    FlatPtr min_block_address, max_block_address;
    find_min_and_max_block_addresses(min_block_address, max_block_address);

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.conservative_register_scan_us };
        for (size_t i = 0; i < ((size_t)sizeof(buf)) / sizeof(FlatPtr); ++i)
            add_possible_value(possible_pointers, raw_jmp_buf[i], HeapRoot { .type = HeapRoot::Type::RegisterPointer }, min_block_address, max_block_address);
    }

    auto stack_reference = bit_cast<FlatPtr>(&dummy);
    auto stack_top = m_stack_info.top();

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
            auto data = *reinterpret_cast<FlatPtr*>(stack_address);
            add_possible_value(possible_pointers, data, HeapRoot { .type = HeapRoot::Type::StackPointer, .stack_frame_index = frame_index_for_stack_address(stack_address) }, min_block_address, max_block_address);
            gather_asan_fake_stack_roots(possible_pointers, data, min_block_address, max_block_address, stack_reference, stack_top);
        }
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.conservative_vector_scan_us };
        for (auto& vector : m_conservative_vectors) {
            for (auto possible_value : vector.possible_values()) {
                add_possible_value(possible_pointers, possible_value, HeapRoot { .type = HeapRoot::Type::ConservativeVector }, min_block_address, max_block_address);
            }
        }
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.conservative_cell_lookup_us };
        for_each_cell_among_possible_pointers(m_live_heap_blocks, possible_pointers, [&](Cell* cell, FlatPtr possible_pointer) {
            if (cell->state() == Cell::State::Live) {
                dbgln_if(HEAP_DEBUG, "  ?-> {}", (void const*)cell);
                roots.set(cell, *possible_pointers.get(possible_pointer));
            } else {
                dbgln_if(HEAP_DEBUG, "  #-> {}", (void const*)cell);
            }
        });
    }
}

class MarkingVisitor final : public Cell::Visitor {
public:
    explicit MarkingVisitor(Heap& heap, HashMap<Cell*, HeapRoot> const& roots)
        : m_heap(heap)
    {
        m_heap.find_min_and_max_block_addresses(m_min_block_address, m_max_block_address);
        for (auto* root : roots.keys()) {
            visit(root);
        }
    }

    virtual void visit_impl(Cell& cell) override
    {
        if (cell.is_marked())
            return;
        dbgln_if(HEAP_DEBUG, "  ! {}", &cell);

        cell.set_marked(true);
        m_work_queue.append(cell);
    }

    virtual void visit_impl(ReadonlySpan<NanBoxedValue> values) override
    {
        m_work_queue.grow_capacity(m_work_queue.size() + values.size());

        for (auto value : values) {
            if (!value.is_cell())
                continue;
            auto& cell = value.as_cell();
            if (cell.is_marked())
                continue;
            dbgln_if(HEAP_DEBUG, "  ! {}", &cell);

            cell.set_marked(true);
            m_work_queue.unchecked_append(cell);
        }
    }

    virtual void visit_possible_values(ReadonlyBytes bytes) override
    {
        HashMap<FlatPtr, HeapRoot> possible_pointers;

        auto* raw_pointer_sized_values = reinterpret_cast<FlatPtr const*>(bytes.data());
        for (size_t i = 0; i < (bytes.size() / sizeof(FlatPtr)); ++i)
            add_possible_value(possible_pointers, raw_pointer_sized_values[i], HeapRoot { .type = HeapRoot::Type::HeapFunctionCapturedPointer }, m_min_block_address, m_max_block_address);

        for_each_cell_among_possible_pointers(m_heap.m_live_heap_blocks, possible_pointers, [&](Cell* cell, FlatPtr) {
            if (cell->is_marked())
                return;
            if (cell->state() != Cell::State::Live)
                return;
            cell->set_marked(true);
            m_work_queue.append(*cell);
        });
    }

    void mark_all_live_cells()
    {
        while (!m_work_queue.is_empty()) {
            m_work_queue.take_last()->visit_edges(*this);
        }
    }

private:
    Heap& m_heap;
    Vector<Ref<Cell>> m_work_queue;
    FlatPtr m_min_block_address;
    FlatPtr m_max_block_address;
};

void Heap::mark_live_cells(HashMap<Cell*, HeapRoot> const& roots)
{
    dbgln_if(HEAP_DEBUG, "mark_live_cells:");

    Optional<MarkingVisitor> visitor;
    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.mark_initial_visit_us };
        visitor.emplace(*this, roots);
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.mark_bfs_us };
        visitor->mark_all_live_cells();
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.mark_clear_uprooted_us };
        for (auto& inverse_root : m_uprooted_cells)
            inverse_root->set_marked(false);

        m_uprooted_cells.clear();
    }
}

void Heap::finalize_unmarked_cells()
{
    for_each_block([&](auto& block) {
        if (!block.overrides_finalize())
            return IterationDecision::Continue;
        block.template for_each_cell_in_state<Cell::State::Live>([](Cell* cell) {
            if (!cell->is_marked())
                cell->finalize();
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
    Vector<HeapBlock*, 32> empty_blocks;
    Vector<HeapBlock*, 32> full_blocks_that_became_usable;

    size_t collected_cells = 0;
    size_t live_cells = 0;
    size_t collected_cell_bytes = 0;
    size_t live_cell_bytes = 0;
    size_t live_external_bytes = 0;

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.sweep_block_iteration_us };
        for_each_block([&](auto& block) {
            bool block_has_live_cells = false;
            bool block_was_full = block.is_full();
            block.template for_each_cell_in_state<Cell::State::Live>([&](Cell* cell) {
                if (!cell->is_marked()) {
                    dbgln_if(HEAP_DEBUG, "  ~ {}", cell);
                    block.deallocate(cell);
                    ++collected_cells;
                    collected_cell_bytes += block.cell_size();
                } else {
                    cell->set_marked(false);
                    block_has_live_cells = true;
                    ++live_cells;
                    live_cell_bytes += block.cell_size();
                    auto cell_external_memory_size = cell->external_memory_size();
                    live_external_bytes = cell_external_memory_size > NumericLimits<size_t>::max() - live_external_bytes
                        ? NumericLimits<size_t>::max()
                        : live_external_bytes + cell_external_memory_size;
                }
            });
            if (!block_has_live_cells)
                empty_blocks.append(&block);
            else if (block_was_full != block.is_full())
                full_blocks_that_became_usable.append(&block);
            return IterationDecision::Continue;
        });
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.sweep_block_reclassify_us };
        for (auto* block : empty_blocks) {
            dbgln_if(HEAP_DEBUG, " - HeapBlock empty @ {}: cell_size={}", block, block->cell_size());
            block->cell_allocator().block_did_become_empty({}, *block);
        }

        for (auto* block : full_blocks_that_became_usable) {
            dbgln_if(HEAP_DEBUG, " - HeapBlock usable again @ {}: cell_size={}", block, block->cell_size());
            block->cell_allocator().block_did_become_usable({}, *block);
        }
    }

    if constexpr (HEAP_DEBUG) {
        for_each_block([&](auto& block) {
            dbgln(" > Live HeapBlock @ {}: cell_size={}", &block, block.cell_size());
            return IterationDecision::Continue;
        });
    }

    {
        ScopedPhaseTimer timer { g_recording_phase_timings, g_phase_timings.sweep_update_threshold_us };
        update_gc_bytes_threshold(live_cell_bytes, live_external_bytes);
    }

    if (print_report) {
        g_sweep_stats = {
            .collected_cells = collected_cells,
            .live_cells = live_cells,
            .collected_cell_bytes = collected_cell_bytes,
            .live_cell_bytes = live_cell_bytes,
            .live_external_bytes = live_external_bytes,
            .freed_block_count = empty_blocks.size(),
        };
    }
    (void)measurement_timer;

    // Sweep is done; kick the global decommit worker so the slots we just
    // freed get madvise()'d off the GC pause path.
    BlockAllocator::wake_decommit_worker_async();
}

void Heap::sweep_block(HeapBlock& block)
{
    // Remove from the allocator's pending sweep list.
    block.m_sweep_list_node.remove();

    bool block_has_live_cells = false;
    bool block_was_full = block.is_full();
    size_t collected_cells = 0;
    size_t live_cells = 0;

    block.for_each_cell_in_state<Cell::State::Live>([&](Cell* cell) {
        if (!cell->is_marked()) {
            dbgln_if(HEAP_DEBUG, "  ~ {}", cell);
            block.deallocate(cell);
            ++collected_cells;
        } else {
            cell->set_marked(false);
            block_has_live_cells = true;
            m_sweep_live_cell_bytes += block.cell_size();
            auto cell_external_memory_size = cell->external_memory_size();
            m_sweep_live_external_bytes = cell_external_memory_size > NumericLimits<size_t>::max() - m_sweep_live_external_bytes
                ? NumericLimits<size_t>::max()
                : m_sweep_live_external_bytes + cell_external_memory_size;
            ++live_cells;
        }
    });

    if (!block_has_live_cells) {
        dbgln_if(HEAP_DEBUG, " - HeapBlock empty @ {}: cell_size={}", &block, block.cell_size());
        dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] Block @ {} freed ({} cells collected)",
            &block, collected_cells);
        block.cell_allocator().block_did_become_empty({}, block);
    } else if (block_was_full && !block.is_full()) {
        dbgln_if(HEAP_DEBUG, " - HeapBlock usable again @ {}: cell_size={}", &block, block.cell_size());
        dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] Block @ {} now usable (live: {}, collected: {})",
            &block, live_cells, collected_cells);
        block.cell_allocator().block_did_become_usable({}, block);
    } else if constexpr (INCREMENTAL_SWEEP_DEBUG) {
        dbgln("[sweep] Block @ {} swept (live: {}, collected: {})",
            &block, live_cells, collected_cells);
    }
}

bool Heap::sweep_next_block()
{
    if (!m_incremental_sweep_active)
        return true;

    if (is_gc_deferred())
        return true;

    // Find the next allocator that has blocks pending sweep.
    while (auto* allocator = m_allocators_to_sweep.first()) {
        if (auto* block = allocator->m_blocks_pending_sweep.first()) {
            sweep_block(*block);
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
    m_sweep_live_cell_bytes = 0;
    m_sweep_live_external_bytes = 0;
    g_incremental_sweep_stats.should_report = false;
    g_incremental_sweep_stats.total_blocks = 0;
    g_incremental_sweep_stats.batches.clear();
    g_incremental_sweep_stats.should_report = g_next_incremental_sweep_should_report;
    g_next_incremental_sweep_should_report = false;
    if (g_incremental_sweep_stats.should_report)
        g_incremental_sweep_stats.timer.start();

    // Populate each allocator's pending sweep list with its current blocks.
    // Blocks allocated during incremental sweep won't be on these lists
    // and don't need sweeping.
    size_t total_blocks = 0;
    for (auto& allocator : m_all_cell_allocators) {
        allocator.for_each_block([&](HeapBlock& block) {
            allocator.m_blocks_pending_sweep.append(block);
            ++total_blocks;
            return IterationDecision::Continue;
        });
        if (allocator.has_blocks_pending_sweep())
            m_allocators_to_sweep.append(allocator);
    }
    g_incremental_sweep_stats.total_blocks = total_blocks;

    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] {} blocks to sweep", total_blocks);

    start_incremental_sweep_timer();
}

void Heap::finish_incremental_sweep()
{
    update_gc_bytes_threshold(m_sweep_live_cell_bytes, m_sweep_live_external_bytes);

    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] === Sweep complete ===");
    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep]     Live cell bytes: {} ({} KiB)", m_sweep_live_cell_bytes, m_sweep_live_cell_bytes / KiB);
    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep]     Live external bytes: {} ({} KiB)", m_sweep_live_external_bytes, m_sweep_live_external_bytes / KiB);
    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep]     Next GC threshold: {} ({} KiB)", m_gc_bytes_threshold, m_gc_bytes_threshold / KiB);
    print_incremental_sweep_report(m_sweep_live_cell_bytes, m_sweep_live_external_bytes, m_gc_bytes_threshold);

    // Clear marks on cells allocated during sweep. Sweep already cleared
    // marks on cells it visited, so only these remain marked.
    for (auto cell : m_cells_allocated_during_sweep)
        cell->set_marked(false);
    m_cells_allocated_during_sweep.clear();

    m_incremental_sweep_active = false;

    stop_incremental_sweep_timer();
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

void Heap::start_incremental_sweep_timer()
{
    if (!m_incremental_sweep_timer) {
        m_incremental_sweep_timer = Core::Timer::create_repeating(GC_INCREMENTAL_SWEEP_INTERVAL_MS, [this] {
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

void Heap::defer_gc()
{
    ++m_gc_deferrals;
}

void Heap::undefer_gc()
{
    VERIFY(m_gc_deferrals > 0);
    --m_gc_deferrals;

    if (!m_gc_deferrals) {
        if (m_should_gc_when_deferral_ends)
            collect_garbage();
        m_should_gc_when_deferral_ends = false;
    }
}

void Heap::uproot_cell(Cell* cell)
{
    m_uprooted_cells.append(cell);
}

WeakImpl* Heap::create_weak_impl(void* ptr)
{
    if (m_usable_weak_blocks.is_empty()) {
        // NOTE: These are leaked on Heap destruction, but that's fine since Heap is tied to process lifetime.
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
