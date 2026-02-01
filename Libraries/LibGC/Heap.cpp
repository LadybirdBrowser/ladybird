/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/Debug.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Platform.h>
#include <AK/QuickSort.h>
#include <AK/StackInfo.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Timer.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakInlines.h>
#include <setjmp.h>

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#endif

namespace GC {

static constexpr bool DUMP_GC_STATS_ON_EXIT = false;

static Heap* s_the;

struct GCPause {
    i64 mark_us { 0 };
    i64 finalize_us { 0 };
    i64 weak_us { 0 };
    i64 total_us { 0 };
};

static Vector<GCPause> s_gc_pauses;

[[maybe_unused]] static void dump_gc_stats()
{
    if (s_gc_pauses.is_empty())
        return;

    dbgln("=== GC Statistics ({} collections) ===", s_gc_pauses.size());

    i64 total_mark = 0;
    i64 total_finalize = 0;
    i64 total_weak = 0;
    i64 total_total = 0;
    for (auto& p : s_gc_pauses) {
        total_mark += p.mark_us;
        total_finalize += p.finalize_us;
        total_weak += p.weak_us;
        total_total += p.total_us;
    }

    dbgln("  Total time in GC: {}.{:03} ms", total_total / 1000, total_total % 1000);
    dbgln("    Mark:     {}.{:03} ms", total_mark / 1000, total_mark % 1000);
    dbgln("    Finalize: {}.{:03} ms", total_finalize / 1000, total_finalize % 1000);
    dbgln("    Weak:     {}.{:03} ms", total_weak / 1000, total_weak % 1000);

    auto avg = total_total / static_cast<i64>(s_gc_pauses.size());
    dbgln("  Average pause: {}.{:03} ms", avg / 1000, avg % 1000);

    Vector<GCPause> sorted = s_gc_pauses;
    quick_sort(sorted, [](auto& a, auto& b) { return a.total_us > b.total_us; });

    auto top = min(sorted.size(), static_cast<size_t>(10));
    dbgln("  Top {} worst pauses:", top);
    for (size_t i = 0; i < top; ++i) {
        auto& p = sorted[i];
        dbgln("    {}.{:03} ms (mark: {}.{:03}, finalize: {}.{:03}, weak: {}.{:03})",
            p.total_us / 1000, p.total_us % 1000,
            p.mark_us / 1000, p.mark_us % 1000,
            p.finalize_us / 1000, p.finalize_us % 1000,
            p.weak_us / 1000, p.weak_us % 1000);
    }
    dbgln("==========================================");
}

Heap& Heap::the()
{
    return *s_the;
}

Heap::Heap(AK::Function<void(HashMap<Cell*, GC::HeapRoot>&)> gather_embedder_roots)
    : m_gather_embedder_roots(move(gather_embedder_roots))
{
    s_the = this;
    if constexpr (DUMP_GC_STATS_ON_EXIT)
        atexit(dump_gc_stats);
    static_assert(HeapBlock::min_possible_cell_size <= 32, "Heap Cell tracking uses too much data!");
    m_size_based_cell_allocators.append(make<CellAllocator>(64));
    m_size_based_cell_allocators.append(make<CellAllocator>(96));
    m_size_based_cell_allocators.append(make<CellAllocator>(128));
    m_size_based_cell_allocators.append(make<CellAllocator>(256));
    m_size_based_cell_allocators.append(make<CellAllocator>(512));
    m_size_based_cell_allocators.append(make<CellAllocator>(1024));
    m_size_based_cell_allocators.append(make<CellAllocator>(3072));
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
                auto location = it.value.root_origin->location;
                switch (type) {
                case HeapRoot::Type::ConservativeVector:
                    node.set("root"sv, "ConservativeVector"sv);
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
                case HeapRoot::Type::RegisterPointer:
                    node.set("root"sv, "RegisterPointer"sv);
                    break;
                case HeapRoot::Type::StackPointer:
                    node.set("root"sv, "StackPointer"sv);
                    break;
                case HeapRoot::Type::VM:
                    node.set("root"sv, "VM"sv);
                    break;
                default:
                    VERIFY_NOT_REACHED();
                }
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
    HashMap<Cell*, HeapRoot> roots;
    gather_roots(roots);
    GraphConstructorVisitor visitor(*this, roots);
    visitor.visit_all_cells();
    return visitor.dump();
}

void Heap::collect_garbage(CollectionType collection_type, bool print_report)
{
    VERIFY(!m_collecting_garbage);

    // If an incremental sweep is still in progress, finish it first.
    if (m_incremental_sweep_active && !is_gc_deferred()) {
        dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] New GC triggered, finishing current sweep...");
        while (m_incremental_sweep_active)
            sweep_next_block();
    }

    {
        TemporaryChange change(m_collecting_garbage, true);

        Core::ElapsedTimer collection_measurement_timer;
        if (print_report)
            collection_measurement_timer.start();

        auto gc_start = MonotonicTime::now();

        if (collection_type == CollectionType::CollectGarbage) {
            if (m_gc_deferrals) {
                m_should_gc_when_deferral_ends = true;
                return;
            }
            HashMap<Cell*, HeapRoot> roots;
            gather_roots(roots);
            mark_live_cells(roots);
        }

        auto after_mark = MonotonicTime::now();

        // Phase 2: Finalization (stop-the-world)
        finalize_unmarked_cells();

        auto after_finalize = MonotonicTime::now();

        // Phase 3: Weak refs (stop-the-world)
        sweep_weak_blocks();

        for (auto& weak_container : m_weak_containers)
            weak_container.remove_dead_cells({});

        auto after_weak = MonotonicTime::now();

        // Phase 4: Sweeping
        // Note: For CollectEverything, we use monolithic sweep to ensure
        // all cells are collected before the Heap destructor completes.
        if (collection_type == CollectionType::CollectEverything) {
            sweep_dead_cells(print_report, collection_measurement_timer);
            if (print_report)
                dump_allocators();
        }

        auto after_sweep = MonotonicTime::now();

        if constexpr (DUMP_GC_STATS_ON_EXIT) {
            GCPause pause;
            pause.mark_us = (after_mark - gc_start).to_nanoseconds() / 1000;
            pause.finalize_us = (after_finalize - after_mark).to_nanoseconds() / 1000;
            pause.weak_us = (after_weak - after_finalize).to_nanoseconds() / 1000;
            pause.total_us = (after_sweep - gc_start).to_nanoseconds() / 1000;
            s_gc_pauses.append(pause);
        }
    }

    run_post_gc_tasks();

    if (collection_type != CollectionType::CollectEverything)
        start_incremental_sweep();
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
        if (allocator.class_name().is_null())
            builder.appendff("generic ({}b)", allocator.cell_size());
        else
            builder.appendff("{} ({}b)", allocator.class_name(), allocator.cell_size());

        builder.appendff(" x {}", total_live_cells);

        size_t cost = blocks.size() * HeapBlock::BLOCK_SIZE / KiB;
        size_t reserved = allocator.block_allocator().blocks().size() * HeapBlock::BLOCK_SIZE / KiB;
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

void Heap::gather_roots(HashMap<Cell*, HeapRoot>& roots)
{
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

    m_gather_embedder_roots(roots);
    gather_conservative_roots(roots);

    for (auto& root : m_roots)
        roots.set(root.cell(), HeapRoot { .type = HeapRoot::Type::Root, .location = &root.source_location() });

    for (auto& vector : m_root_vectors)
        vector.gather_roots(roots);

    for (auto& hash_map : m_root_hash_maps)
        hash_map.gather_roots(roots);

    if constexpr (HEAP_DEBUG) {
        dbgln("gather_roots:");
        for (auto* root : roots.keys())
            dbgln("  + {}", root);
    }
}

#ifdef HAS_ADDRESS_SANITIZER
NO_SANITIZE_ADDRESS void Heap::gather_asan_fake_stack_roots(HashMap<FlatPtr, HeapRoot>& possible_pointers, FlatPtr addr, FlatPtr min_block_address, FlatPtr max_block_address)
{
    void* begin = nullptr;
    void* end = nullptr;
    void* real_stack = __asan_addr_is_in_fake_stack(__asan_get_current_fake_stack(), reinterpret_cast<void*>(addr), &begin, &end);

    if (real_stack != nullptr) {
        for (auto* real_stack_addr = reinterpret_cast<void const* const*>(begin); real_stack_addr < end; ++real_stack_addr) {
            void const* real_address = *real_stack_addr;
            if (real_address == nullptr)
                continue;
            add_possible_value(possible_pointers, reinterpret_cast<FlatPtr>(real_address), HeapRoot { .type = HeapRoot::Type::StackPointer }, min_block_address, max_block_address);
        }
    }
}
#else
void Heap::gather_asan_fake_stack_roots(HashMap<FlatPtr, HeapRoot>&, FlatPtr, FlatPtr, FlatPtr)
{
}
#endif

NO_SANITIZE_ADDRESS void Heap::gather_conservative_roots(HashMap<Cell*, HeapRoot>& roots)
{
    FlatPtr dummy;

    dbgln_if(HEAP_DEBUG, "gather_conservative_roots:");

    jmp_buf buf;
    setjmp(buf);

    HashMap<FlatPtr, HeapRoot> possible_pointers;

    auto* raw_jmp_buf = reinterpret_cast<FlatPtr const*>(buf);

    FlatPtr min_block_address, max_block_address;
    find_min_and_max_block_addresses(min_block_address, max_block_address);

    for (size_t i = 0; i < ((size_t)sizeof(buf)) / sizeof(FlatPtr); ++i)
        add_possible_value(possible_pointers, raw_jmp_buf[i], HeapRoot { .type = HeapRoot::Type::RegisterPointer }, min_block_address, max_block_address);

    auto stack_reference = bit_cast<FlatPtr>(&dummy);

    for (FlatPtr stack_address = stack_reference; stack_address < m_stack_info.top(); stack_address += sizeof(FlatPtr)) {
        auto data = *reinterpret_cast<FlatPtr*>(stack_address);
        add_possible_value(possible_pointers, data, HeapRoot { .type = HeapRoot::Type::StackPointer }, min_block_address, max_block_address);
        gather_asan_fake_stack_roots(possible_pointers, data, min_block_address, max_block_address);
    }

    for (auto& vector : m_conservative_vectors) {
        for (auto possible_value : vector.possible_values()) {
            add_possible_value(possible_pointers, possible_value, HeapRoot { .type = HeapRoot::Type::ConservativeVector }, min_block_address, max_block_address);
        }
    }

    for_each_cell_among_possible_pointers(m_live_heap_blocks, possible_pointers, [&](Cell* cell, FlatPtr possible_pointer) {
        if (cell->state() == Cell::State::Live) {
            dbgln_if(HEAP_DEBUG, "  ?-> {}", (void const*)cell);
            roots.set(cell, *possible_pointers.get(possible_pointer));
        } else {
            dbgln_if(HEAP_DEBUG, "  #-> {}", (void const*)cell);
        }
    });
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
        auto* block = HeapBlock::from_cell(&cell);
        auto index = block->cell_index(&cell);
        if (block->is_marked(index))
            return;
        dbgln_if(HEAP_DEBUG, "  ! {}", &cell);

        block->set_marked(index);
        m_work_queue.append(cell);
    }

    virtual void visit_impl(ReadonlySpan<NanBoxedValue> values) override
    {
        m_work_queue.grow_capacity(m_work_queue.size() + values.size());

        for (auto value : values) {
            if (!value.is_cell())
                continue;
            auto& cell = value.as_cell();
            auto* block = HeapBlock::from_cell(&cell);
            auto index = block->cell_index(&cell);
            if (block->is_marked(index))
                continue;
            dbgln_if(HEAP_DEBUG, "  ! {}", &cell);

            block->set_marked(index);
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
            if (cell->state() != Cell::State::Live)
                return;
            auto* block = HeapBlock::from_cell(cell);
            auto index = block->cell_index(cell);
            if (block->is_marked(index))
                return;
            block->set_marked(index);
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

    MarkingVisitor visitor(*this, roots);
    visitor.mark_all_live_cells();

    for (auto& inverse_root : m_uprooted_cells) {
        auto* block = HeapBlock::from_cell(inverse_root);
        block->clear_marked(block->cell_index(inverse_root));
    }

    m_uprooted_cells.clear();
}

void Heap::finalize_unmarked_cells()
{
    for_each_block([&](auto& block) {
        if (!block.overrides_finalize())
            return IterationDecision::Continue;
        block.template for_each_cell_in_state<Cell::State::Live>([&block](Cell* cell) {
            if (!block.is_marked(block.cell_index(cell)))
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

    for_each_block([&](auto& block) {
        bool block_has_live_cells = false;
        bool block_was_full = block.is_full();
        block.template for_each_cell_in_state<Cell::State::Live>([&](Cell* cell) {
            if (!block.is_marked(block.cell_index(cell))) {
                dbgln_if(HEAP_DEBUG, "  ~ {}", cell);
                block.deallocate(cell);
                ++collected_cells;
                collected_cell_bytes += block.cell_size();
            } else {
                block_has_live_cells = true;
                ++live_cells;
                live_cell_bytes += block.cell_size();
            }
        });
        block.clear_all_marks();
        if (!block_has_live_cells)
            empty_blocks.append(&block);
        else if (block_was_full != block.is_full())
            full_blocks_that_became_usable.append(&block);
        return IterationDecision::Continue;
    });

    for (auto* block : empty_blocks) {
        dbgln_if(HEAP_DEBUG, " - HeapBlock empty @ {}: cell_size={}", block, block->cell_size());
        block->cell_allocator().block_did_become_empty({}, *block);
    }

    for (auto* block : full_blocks_that_became_usable) {
        dbgln_if(HEAP_DEBUG, " - HeapBlock usable again @ {}: cell_size={}", block, block->cell_size());
        block->cell_allocator().block_did_become_usable({}, *block);
    }

    if constexpr (HEAP_DEBUG) {
        for_each_block([&](auto& block) {
            dbgln(" > Live HeapBlock @ {}: cell_size={}", &block, block.cell_size());
            return IterationDecision::Continue;
        });
    }

    m_gc_bytes_threshold = live_cell_bytes > GC_MIN_BYTES_THRESHOLD ? live_cell_bytes : GC_MIN_BYTES_THRESHOLD;

    if (print_report) {
        AK::Duration const time_spent = measurement_timer.elapsed_time();
        size_t live_block_count = 0;
        for_each_block([&](auto&) {
            ++live_block_count;
            return IterationDecision::Continue;
        });

        dbgln("Garbage collection report");
        dbgln("=============================================");
        dbgln("     Time spent: {} ms", time_spent.to_milliseconds());
        dbgln("     Live cells: {} ({} bytes)", live_cells, live_cell_bytes);
        dbgln("Collected cells: {} ({} bytes)", collected_cells, collected_cell_bytes);
        dbgln("    Live blocks: {} ({} bytes)", live_block_count, live_block_count * HeapBlock::BLOCK_SIZE);
        dbgln("   Freed blocks: {} ({} bytes)", empty_blocks.size(), empty_blocks.size() * HeapBlock::BLOCK_SIZE);
        dbgln("=============================================");
    }
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
        if (!block.is_marked(block.cell_index(cell))) {
            dbgln_if(HEAP_DEBUG, "  ~ {}", cell);
            block.deallocate(cell);
            ++collected_cells;
        } else {
            block_has_live_cells = true;
            m_sweep_live_cell_bytes += block.cell_size();
            ++live_cells;
        }
    });
    block.clear_all_marks();

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

    // No more blocks to sweep.
    finish_incremental_sweep();
    return true;
}

void Heap::start_incremental_sweep()
{
    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] === Starting incremental sweep ===");

    m_incremental_sweep_active = true;
    m_sweep_live_cell_bytes = 0;

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

    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] {} blocks to sweep", total_blocks);

    start_incremental_sweep_timer();
}

void Heap::finish_incremental_sweep()
{
    m_gc_bytes_threshold = max(m_sweep_live_cell_bytes, GC_MIN_BYTES_THRESHOLD);

    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep] === Sweep complete ===");
    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep]     Live cell bytes: {} ({} KiB)", m_sweep_live_cell_bytes, m_sweep_live_cell_bytes / KiB);
    dbgln_if(INCREMENTAL_SWEEP_DEBUG, "[sweep]     Next GC threshold: {} ({} KiB)", m_gc_bytes_threshold, m_gc_bytes_threshold / KiB);

    // Clear marks on cells allocated during sweep. Sweep already cleared
    // marks on cells it visited, so only these remain marked.
    for (auto cell : m_cells_allocated_during_sweep) {
        auto* block = HeapBlock::from_cell(cell);
        block->clear_marked(block->cell_index(cell));
    }
    m_cells_allocated_during_sweep.clear();

    m_incremental_sweep_active = false;

    stop_incremental_sweep_timer();
}

void Heap::start_incremental_sweep_timer()
{
    if (!m_incremental_sweep_timer) {
        m_incremental_sweep_timer = Core::Timer::create_repeating(16, [this] {
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
    auto start_time = MonotonicTime::now();
    auto deadline = start_time + AK::Duration::from_milliseconds(5);
    while (MonotonicTime::now() < deadline) {
        if (sweep_next_block())
            break;
        ++blocks_swept;
    }

    if (blocks_swept > 0) {
        auto elapsed = MonotonicTime::now() - start_time;
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
