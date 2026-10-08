/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/System.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/CAPI.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/HeapRegion.h>
#include <LibGC/PrimitiveStorage.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>

namespace GC {

static_assert(sizeof(GCCellTypeInfo) == sizeof(CellTypeInfo));
static_assert(alignof(GCCellTypeInfo) == alignof(CellTypeInfo));
static_assert(offsetof(GCCellTypeInfo, cell_size) == offsetof(CellTypeInfo, cell_size));
static_assert(offsetof(GCCellTypeInfo, alignment) == offsetof(CellTypeInfo, alignment));
static_assert(offsetof(GCCellTypeInfo, kind) == offsetof(CellTypeInfo, kind));
static_assert(offsetof(GCCellTypeInfo, visit_edges) == offsetof(CellTypeInfo, visit_edges));
static_assert(offsetof(GCCellTypeInfo, finalize) == offsetof(CellTypeInfo, finalize));
static_assert(offsetof(GCCellTypeInfo, destroy) == offsetof(CellTypeInfo, destroy));
static_assert(offsetof(GCCellTypeInfo, external_memory_size) == offsetof(CellTypeInfo, external_memory_size));
static_assert(offsetof(GCCellTypeInfo, class_name) == offsetof(CellTypeInfo, class_name));
static_assert(sizeof(CellKind) == sizeof(uint8_t));

static_assert(GC_CELL_KIND_OTHER == to_underlying(CellKind::Other));
static_assert(GC_CELL_KIND_OBJECT == to_underlying(CellKind::Object));
static_assert(GC_CELL_KIND_PRIMITIVE_STRING == to_underlying(CellKind::PrimitiveString));
static_assert(GC_CELL_KIND_SYMBOL == to_underlying(CellKind::Symbol));
static_assert(GC_CELL_KIND_BIGINT == to_underlying(CellKind::BigInt));
static_assert(GC_CELL_KIND_ACCESSOR == to_underlying(CellKind::Accessor));

static_assert(GC_CELL_STATE_LIVE == to_underlying(Cell::State::Live));
static_assert(GC_CELL_STATE_DEAD == to_underlying(Cell::State::Dead));

static_assert(sizeof(NanBoxedValue) == sizeof(uint64_t));

static_assert(IsSame<decltype(PrimitiveStorageHandle::index), u32>);
static_assert(IsSame<decltype(PrimitiveStorageHandle::generation), u32>);
static_assert(GC_PRIMITIVE_STORAGE_INVALID_OFFSET == PrimitiveStorage::invalid_offset);

static constexpr GCPrimitiveStorageHandle encode_primitive_storage_handle(PrimitiveStorageHandle handle)
{
    return (static_cast<u64>(handle.generation) << GC_PRIMITIVE_STORAGE_HANDLE_GENERATION_SHIFT) | handle.index;
}

static constexpr PrimitiveStorageHandle decode_primitive_storage_handle(GCPrimitiveStorageHandle handle)
{
    return {
        .index = static_cast<u32>(handle),
        .generation = static_cast<u32>(handle >> GC_PRIMITIVE_STORAGE_HANDLE_GENERATION_SHIFT),
    };
}

static_assert(encode_primitive_storage_handle({ .index = 0x01234567, .generation = 0x89abcdef }) == 0x89abcdef01234567);
static_assert(decode_primitive_storage_handle(0x89abcdef01234567).index == 0x01234567);
static_assert(decode_primitive_storage_handle(0x89abcdef01234567).generation == 0x89abcdef);
static_assert(decode_primitive_storage_handle(GC_PRIMITIVE_STORAGE_NULL_HANDLE).generation == 0);

class CAPICellAllocator final : public CellAllocatorDescriptorBase {
public:
    CAPICellAllocator(CellTypeInfo const& type_info, StringView class_name)
        : CellAllocatorDescriptorBase(type_info, class_name)
    {
    }
};

class RootGatheringVisitor final : public Cell::Visitor {
public:
    explicit RootGatheringVisitor(HashMap<Cell*, HeapRoot>& roots)
        : m_roots(roots)
    {
    }

    virtual void visit_possible_values(ReadonlyBytes) override
    {
        // NB: Roots are gathered precisely; a conservative range belongs in a ConservativeRangeProvider.
        VERIFY_NOT_REACHED();
    }

private:
    virtual void visit_impl(Cell& cell) override
    {
        m_roots.set(&cell, HeapRoot { .type = HeapRoot::Type::VM });
    }

    virtual void visit_impl(ReadonlySpan<NanBoxedValue> values) override
    {
        for (auto const& value : values) {
            if (value.is_cell())
                visit_impl(value.as_cell());
        }
    }

    HashMap<Cell*, HeapRoot>& m_roots;
};

struct CAPI {
    static_assert(offsetof(ForeignCell, m_word_owned_by_the_foreign_implementation) == 0);
    static_assert(offsetof(ForeignCell, m_mark) == offsetof(Cell, m_mark));
    static_assert(offsetof(ForeignCell, m_state) == offsetof(Cell, m_state));
    static_assert(offsetof(ForeignCell, m_cell_kind) == offsetof(Cell, m_cell_kind));

    static void get_layout(GCLayout& layout)
    {
        layout = {
            .cell_mark_offset = offsetof(Cell, m_mark),
            .cell_state_offset = offsetof(Cell, m_state),
            .cell_kind_offset = offsetof(Cell, m_cell_kind),
            .min_cell_size = HeapBlock::min_possible_cell_size,
            .max_cell_size = HeapBlock::BLOCK_SIZE - sizeof(HeapBlock),
            .max_cell_alignment = __BIGGEST_ALIGNMENT__,
            .cell_type_info_size = sizeof(CellTypeInfo),
            .weak_impl_pointer_offset = WeakImpl::value_offset(),
            .free_cell_next_offset = HeapBlock::freelist_next_offset(),
            .free_cell_link_mask = HeapBlock::freelist_link_mask,
            .heap_allocated_bytes_since_last_gc_offset = offsetof(Heap, m_allocated_bytes_since_last_gc),
            .heap_gc_bytes_threshold_offset = offsetof(Heap, m_gc_bytes_threshold),
            .heap_total_allocated_bytes_offset = offsetof(Heap, m_total_allocated_bytes),
            .heap_region_offset_mask = HEAP_REGION_OFFSET_MASK,
            .primitive_storage_cage_offset_mask = PrimitiveStorage::cage_offset_mask,
        };
    }

    static Cell* allocate_cell(Heap& heap, CellAllocatorDescriptorBase& descriptor, bool& must_mark)
    {
        auto* cell = heap.allocate_cell(descriptor);
        must_mark = heap.mark_if_allocated_during_incremental_sweep(*cell);
        return cell;
    }

    static Cell* allocate_storage_cell(Heap& heap, CellAllocatorDescriptorBase& descriptor, bool& must_mark)
    {
        auto* cell = heap.allocate_cell(descriptor, Heap::TriggersCollection::No);
        must_mark = heap.mark_if_allocated_during_incremental_sweep(*cell);
        return cell;
    }

    static_assert(sizeof(Heap::m_allocated_bytes_since_last_gc) == sizeof(size_t));
    static_assert(sizeof(Heap::m_gc_bytes_threshold) == sizeof(size_t));
    static_assert(sizeof(Heap::m_total_allocated_bytes) == sizeof(size_t));

    // Foreign code reads and writes the head of the list as a plain pointer.
    static_assert(sizeof(RawPtr<Cell>) == sizeof(Cell*));
    static RawPtr<Cell>* local_free_list(Heap& heap, CellAllocatorDescriptorBase& descriptor) { return descriptor.for_heap(heap).local_free_list({}); }
    static void set_foreign_context(Heap& heap, void* context) { heap.m_foreign_context = context; }
    static void* foreign_context(Heap& heap) { return heap.m_foreign_context; }
    static void defer_gc(Heap& heap) { heap.defer_gc(); }
    static void undefer_gc(Heap& heap) { heap.undefer_gc(); }
    static StackInfo const& stack_info(Heap const& heap) { return heap.m_stack_info; }
};

}

using namespace GC;

static Heap& as_heap(GCHeap* heap) { return *reinterpret_cast<Heap*>(heap); }
static Heap const& as_heap(GCHeap const* heap) { return *reinterpret_cast<Heap const*>(heap); }
static Cell* from_gc_cell(GCCell* cell) { return reinterpret_cast<Cell*>(cell); }
static GCCell* as_gc_cell(Cell* cell) { return reinterpret_cast<GCCell*>(cell); }
static Cell::Visitor& as_visitor(GCVisitor* visitor) { return *reinterpret_cast<Cell::Visitor*>(visitor); }
static WeakImpl* as_weak_impl(GCWeakImpl* impl) { return reinterpret_cast<WeakImpl*>(impl); }
static GCWeakImpl* as_gc_weak_impl(WeakImpl* impl) { return reinterpret_cast<GCWeakImpl*>(impl); }
static PrimitiveStorage::ZeroFillNewBytes as_zero_fill_new_bytes(bool zero_fill) { return zero_fill ? PrimitiveStorage::ZeroFillNewBytes::Yes : PrimitiveStorage::ZeroFillNewBytes::No; }

static void store_primitive_storage_layout(PrimitiveStorage const& storage, PrimitiveStorageHandle handle, GCPrimitiveStorageLayout* out_layout)
{
    if (!out_layout)
        return;
    auto layout = storage.layout(handle);
    *out_layout = { .offset = layout.offset, .size = layout.size, .capacity = layout.capacity };
}

static bool store_created_primitive_storage(PrimitiveStorage const& storage, ErrorOr<PrimitiveStorageHandle> handle_or_error, GCPrimitiveStorageHandle* out_handle, GCPrimitiveStorageLayout* out_layout)
{
    if (handle_or_error.is_error()) {
        *out_handle = GC_PRIMITIVE_STORAGE_NULL_HANDLE;
        return false;
    }
    *out_handle = encode_primitive_storage_handle(handle_or_error.value());
    store_primitive_storage_layout(storage, handle_or_error.value(), out_layout);
    return true;
}

static bool store_resized_primitive_storage(PrimitiveStorage const& storage, ErrorOr<void> result, PrimitiveStorageHandle handle, GCPrimitiveStorageLayout* out_layout)
{
    if (result.is_error())
        return false;
    store_primitive_storage_layout(storage, handle, out_layout);
    return true;
}

extern "C" {

void gc_get_layout(GCLayout* layout)
{
    CAPI::get_layout(*layout);
}

uintptr_t gc_heap_region_base(void)
{
    return BlockAllocator::heap_region_start();
}

uintptr_t gc_primitive_storage_cage_base(void)
{
    MUST(PrimitiveStorage::the().ensure_cage());
    return js_primitive_storage_cage_base;
}

GCHeap* gc_heap_create(GCGatherRootsCallback gather_roots, void* context, bool become_process_default)
{
    auto* heap = new Heap(
        [gather_roots, context](HashMap<Cell*, HeapRoot>& roots) {
            RootGatheringVisitor visitor { roots };
            gather_roots(context, reinterpret_cast<GCVisitor*>(static_cast<Cell::Visitor*>(&visitor)));
        },
        become_process_default ? Heap::BecomeProcessDefault::Yes : Heap::BecomeProcessDefault::No);
    CAPI::set_foreign_context(*heap, context);
    return reinterpret_cast<GCHeap*>(heap);
}

void gc_heap_destroy(GCHeap* heap)
{
    delete &as_heap(heap);
}

void gc_heap_collect_garbage(GCHeap* heap, int collection_type, bool print_report)
{
    VERIFY(collection_type == GC_COLLECTION_TYPE_COLLECT_GARBAGE || collection_type == GC_COLLECTION_TYPE_COLLECT_EVERYTHING);
    auto type = collection_type == GC_COLLECTION_TYPE_COLLECT_EVERYTHING ? Heap::CollectionType::CollectEverything : Heap::CollectionType::CollectGarbage;
    as_heap(heap).collect_garbage(type, print_report);
}

bool gc_heap_is_collecting_everything(GCHeap const* heap)
{
    return as_heap(heap).is_collecting_everything();
}

void gc_heap_defer_gc(GCHeap* heap)
{
    CAPI::defer_gc(as_heap(heap));
}

void gc_heap_undefer_gc(GCHeap* heap)
{
    CAPI::undefer_gc(as_heap(heap));
}

bool gc_heap_should_collect_on_every_allocation(GCHeap const* heap)
{
    return as_heap(heap).should_collect_on_every_allocation();
}

void gc_heap_set_should_collect_on_every_allocation(GCHeap* heap, bool should_collect)
{
    as_heap(heap).set_should_collect_on_every_allocation(should_collect);
}

void gc_heap_set_incremental_sweep_enabled(GCHeap* heap, bool enabled)
{
    as_heap(heap).set_incremental_sweep_enabled(enabled);
}

void gc_heap_uproot_cell(GCHeap* heap, GCCell* cell)
{
    as_heap(heap).uproot_cell(from_gc_cell(cell));
}

void gc_heap_stack_bounds(GCHeap const* heap, uintptr_t* base, uintptr_t* top)
{
    auto const& stack_info = CAPI::stack_info(as_heap(heap));
    *base = stack_info.base();
    *top = stack_info.top();
}

void gc_heap_enqueue_post_gc_task(GCHeap* heap, GCCallback callback, void* context)
{
    as_heap(heap).enqueue_post_gc_task([callback, context] { callback(context); });
}

void gc_heap_register_sweep_callback(GCHeap* heap, GCCallback callback, void* context)
{
    as_heap(heap).register_sweep_callback([callback, context] { callback(context); });
}

void gc_heap_did_allocate_external_memory(GCHeap* heap, size_t size)
{
    as_heap(heap).did_allocate_external_memory(size);
}

void gc_heap_did_free_external_memory(GCHeap* heap, size_t size)
{
    as_heap(heap).did_free_external_memory(size);
}

GCAllocator* gc_allocator_create(GCCellTypeInfo const* type_info, char const* name, size_t name_length)
{
    VERIFY(type_info->visit_edges);
    VERIFY(type_info->cell_size >= HeapBlock::min_possible_cell_size);
    VERIFY(type_info->cell_size <= HeapBlock::BLOCK_SIZE - sizeof(HeapBlock));
    VERIFY(type_info->alignment <= __BIGGEST_ALIGNMENT__);
    auto* allocator = new CAPICellAllocator(*reinterpret_cast<CellTypeInfo const*>(type_info), StringView { name, name_length });
    return reinterpret_cast<GCAllocator*>(allocator);
}

void gc_allocator_destroy(GCAllocator* allocator)
{
    delete reinterpret_cast<CAPICellAllocator*>(allocator);
}

GCCell* gc_heap_allocate_cell(GCHeap* heap, GCAllocator* allocator, bool* must_mark)
{
    auto& descriptor = *reinterpret_cast<CAPICellAllocator*>(allocator);
    return as_gc_cell(CAPI::allocate_cell(as_heap(heap), descriptor, *must_mark));
}

GCCell* gc_heap_allocate_storage_cell(GCHeap* heap, GCAllocator* allocator, bool* must_mark)
{
    auto& descriptor = *reinterpret_cast<CAPICellAllocator*>(allocator);
    return as_gc_cell(CAPI::allocate_storage_cell(as_heap(heap), descriptor, *must_mark));
}

GCCell** gc_heap_allocator_local_free_list(GCHeap* heap, GCAllocator* allocator)
{
#ifdef HAS_ADDRESS_SANITIZER
    (void)heap;
    (void)allocator;
    return nullptr;
#else
    auto& descriptor = *reinterpret_cast<CAPICellAllocator*>(allocator);
    return reinterpret_cast<GCCell**>(CAPI::local_free_list(as_heap(heap), descriptor));
#endif
}

void* gc_cell_heap_context(GCCell const* cell)
{
    return CAPI::foreign_context(HeapBlockBase::from_cell(reinterpret_cast<Cell const*>(cell))->heap());
}

GCCellTypeInfo const* gc_cell_type_info(GCCell const* cell)
{
    return reinterpret_cast<GCCellTypeInfo const*>(&reinterpret_cast<Cell const*>(cell)->type_info());
}

GCRoot* gc_root_create(GCCell* cell)
{
    return reinterpret_cast<GCRoot*>(new Root<Cell>(from_gc_cell(cell)));
}

void gc_root_destroy(GCRoot* root)
{
    delete reinterpret_cast<Root<Cell>*>(root);
}

GCCell* gc_root_cell(GCRoot const* root)
{
    return as_gc_cell(reinterpret_cast<Root<Cell> const*>(root)->cell());
}

GCWeakImpl* gc_heap_create_weak_impl(GCHeap* heap, GCCell* cell)
{
    auto* impl = as_heap(heap).create_weak_impl(from_gc_cell(cell));
    // NB: A weak impl without references is reclaimed by the next weak block sweep.
    impl->ref();
    return as_gc_weak_impl(impl);
}

GCWeakImpl* gc_weak_impl_null(void)
{
    WeakImpl::the_null_weak_impl.ref();
    return as_gc_weak_impl(&WeakImpl::the_null_weak_impl);
}

void gc_weak_impl_ref(GCWeakImpl* impl)
{
    as_weak_impl(impl)->ref();
}

void gc_weak_impl_unref(GCWeakImpl* impl)
{
    as_weak_impl(impl)->unref();
}

void gc_visitor_visit_cell(GCVisitor* visitor, GCCell* cell)
{
    as_visitor(visitor).visit(from_gc_cell(cell));
}

void gc_visitor_visit_cells(GCVisitor* visitor, GCCell* const* cells, size_t count)
{
    auto& cell_visitor = as_visitor(visitor);
    for (size_t i = 0; i < count; ++i)
        cell_visitor.visit(from_gc_cell(cells[i]));
}

void gc_visitor_visit_values(GCVisitor* visitor, uint64_t const* values, size_t count)
{
    as_visitor(visitor).visit(ReadonlySpan<NanBoxedValue> { reinterpret_cast<NanBoxedValue const*>(values), count });
}

void gc_visitor_visit_possible_values(GCVisitor* visitor, uint8_t const* data, size_t size)
{
    as_visitor(visitor).visit_possible_values(ReadonlyBytes { data, size });
}

bool gc_primitive_storage_allocate(size_t size, bool zero_fill, GCPrimitiveStorageHandle* out_handle, GCPrimitiveStorageLayout* out_layout)
{
    auto& storage = PrimitiveStorage::the();
    return store_created_primitive_storage(storage, storage.try_allocate(size, as_zero_fill_new_bytes(zero_fill)), out_handle, out_layout);
}

bool gc_primitive_storage_reserve(size_t size, size_t capacity, bool zero_fill, size_t guard_size, GCPrimitiveStorageHandle* out_handle, GCPrimitiveStorageLayout* out_layout)
{
    auto& storage = PrimitiveStorage::the();
    return store_created_primitive_storage(storage, storage.try_reserve(size, capacity, as_zero_fill_new_bytes(zero_fill), guard_size), out_handle, out_layout);
}

bool gc_primitive_storage_adopt_shared_fd(int fd, size_t size, GCPrimitiveStorageHandle* out_handle, GCPrimitiveStorageLayout* out_layout)
{
    auto& storage = PrimitiveStorage::the();
    return store_created_primitive_storage(storage, storage.try_adopt_shared_fd(fd, size), out_handle, out_layout);
}

bool gc_primitive_storage_resize(GCPrimitiveStorageHandle handle, size_t new_size, bool zero_fill, GCPrimitiveStorageLayout* out_layout)
{
    auto& storage = PrimitiveStorage::the();
    auto decoded_handle = decode_primitive_storage_handle(handle);
    return store_resized_primitive_storage(storage, storage.try_resize(decoded_handle, new_size, as_zero_fill_new_bytes(zero_fill)), decoded_handle, out_layout);
}

bool gc_primitive_storage_reserve_capacity(GCPrimitiveStorageHandle handle, size_t new_capacity, GCPrimitiveStorageLayout* out_layout)
{
    auto& storage = PrimitiveStorage::the();
    auto decoded_handle = decode_primitive_storage_handle(handle);
    return store_resized_primitive_storage(storage, storage.try_reserve(decoded_handle, new_capacity), decoded_handle, out_layout);
}

bool gc_primitive_storage_resize_and_reserve(GCPrimitiveStorageHandle handle, size_t new_size, size_t new_capacity, bool zero_fill, GCPrimitiveStorageLayout* out_layout)
{
    auto& storage = PrimitiveStorage::the();
    auto decoded_handle = decode_primitive_storage_handle(handle);
    return store_resized_primitive_storage(storage, storage.try_resize_and_reserve(decoded_handle, new_size, new_capacity, as_zero_fill_new_bytes(zero_fill)), decoded_handle, out_layout);
}

void gc_primitive_storage_free(GCPrimitiveStorageHandle handle)
{
    PrimitiveStorage::the().free(decode_primitive_storage_handle(handle));
}

bool gc_primitive_storage_is_valid(GCPrimitiveStorageHandle handle)
{
    return PrimitiveStorage::the().is_valid(decode_primitive_storage_handle(handle));
}

size_t gc_primitive_storage_offset(GCPrimitiveStorageHandle handle)
{
    return PrimitiveStorage::the().offset(decode_primitive_storage_handle(handle));
}

size_t gc_primitive_storage_size(GCPrimitiveStorageHandle handle)
{
    return PrimitiveStorage::the().size(decode_primitive_storage_handle(handle));
}

size_t gc_primitive_storage_capacity(GCPrimitiveStorageHandle handle)
{
    return PrimitiveStorage::the().capacity(decode_primitive_storage_handle(handle));
}

size_t gc_primitive_storage_committed_size(GCPrimitiveStorageHandle handle)
{
    return PrimitiveStorage::the().committed_size(decode_primitive_storage_handle(handle));
}

uint8_t* gc_primitive_storage_data(GCPrimitiveStorageHandle handle)
{
    return PrimitiveStorage::the().data(decode_primitive_storage_handle(handle));
}

bool gc_shared_memory_create(size_t size, int* out_fd)
{
    *out_fd = -1;
    auto buffer = Core::AnonymousBuffer::create_with_size(size, Core::AnonymousBuffer::Sealability::Sealable);
    if (buffer.is_error())
        return false;
    // NB: The buffer closes its descriptor and unmaps its own view of the memory when it goes away; the caller gets a
    //     duplicate of the descriptor.
    auto fd = Core::System::dup(buffer.value().fd());
    if (fd.is_error())
        return false;
    *out_fd = fd.value();
    return true;
}

struct GCSharedMemoryViewOutsideCage {
    AK_ALLOC_WITH_KMALLOC;

    Core::AnonymousBuffer buffer;
};

GCSharedMemoryViewOutsideCage* gc_shared_memory_view_outside_cage_create(int fd, size_t size)
{
    if (size == 0)
        return nullptr;
    auto duplicate_fd = Core::System::dup(fd);
    if (duplicate_fd.is_error())
        return nullptr;
    // NB: The buffer owns the duplicate from here on, and closes it if mapping fails.
    auto buffer = Core::AnonymousBuffer::create_from_anon_fd(duplicate_fd.value(), size);
    if (buffer.is_error() || buffer.value().validate_backing_size().is_error())
        return nullptr;
    return new GCSharedMemoryViewOutsideCage { buffer.release_value() };
}

uint8_t* gc_shared_memory_view_outside_cage_data(GCSharedMemoryViewOutsideCage* view)
{
    return view->buffer.data<uint8_t>();
}

void gc_shared_memory_view_outside_cage_destroy(GCSharedMemoryViewOutsideCage* view)
{
    delete view;
}
}
