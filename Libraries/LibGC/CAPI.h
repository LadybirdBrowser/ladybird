/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Export.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A C interface to LibGC for cell types that are defined outside of C++.
//
// A foreign cell type describes itself with a GCCellTypeInfo, which LibGC dispatches through exactly as it does for
// C++ cells. The foreign side owns the whole cell: gc_heap_allocate_cell() hands out uninitialized storage, and the
// caller writes every byte of the cell, including the header fields at the offsets reported by gc_get_layout(),
// before anything that can collect garbage runs: another allocation, external memory accounting, undeferring,
// an explicit collection, or a return to the event loop.
//
// The first word of a cell, which holds the vtable pointer of a C++ cell, belongs to the foreign side. LibGC never
// reads it, but -fsanitize=vptr instrumentation of LibGC does, so that sanitizer cannot be used with foreign cells.
//
// Like the rest of LibGC, every heap in a process must be used from the same thread.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GCHeap GCHeap;
typedef struct GCCell GCCell;
typedef struct GCVisitor GCVisitor;
typedef struct GCAllocator GCAllocator;
typedef struct GCRoot GCRoot;
typedef struct GCWeakImpl GCWeakImpl;

typedef void (*GCCallback)(void* context);

// Called whenever the heap gathers its roots. Every cell passed to root_visitor is kept alive by this collection.
// root_visitor only accepts cells and values, not possible values, and the callback must not allocate.
typedef void (*GCGatherRootsCallback)(void* context, GCVisitor* root_visitor);

enum {
    GC_CELL_KIND_OTHER = 0,
    GC_CELL_KIND_OBJECT = 1,
    GC_CELL_KIND_PRIMITIVE_STRING = 2,
    GC_CELL_KIND_SYMBOL = 3,
    GC_CELL_KIND_BIGINT = 4,
    GC_CELL_KIND_ACCESSOR = 5,
};

enum {
    GC_CELL_STATE_LIVE = 0,
    GC_CELL_STATE_DEAD = 1,
};

enum {
    GC_COLLECTION_TYPE_COLLECT_GARBAGE = 0,
    GC_COLLECTION_TYPE_COLLECT_EVERYTHING = 1,
};

// Mirrors GC::CellTypeInfo. visit_edges is required; every other function may be null.
typedef struct GCCellTypeInfo {
    uint32_t cell_size;
    uint32_t alignment;
    uint8_t kind;
    void (*visit_edges)(GCCell*, GCVisitor*);
    void (*finalize)(GCCell*);
    void (*destroy)(GCCell*);
    size_t (*external_memory_size)(GCCell const*);
    char const* (*class_name)(GCCell const*, size_t* length);
} GCCellTypeInfo;

// Everything a foreign implementation of cells needs to agree on with this build of LibGC.
typedef struct GCLayout {
    uint32_t cell_mark_offset;
    uint32_t cell_state_offset;
    uint32_t cell_kind_offset;
    uint32_t min_cell_size;
    uint32_t max_cell_size;
    uint32_t max_cell_alignment;
    uint32_t cell_type_info_size;
    uint32_t weak_impl_pointer_offset;
    uint64_t heap_region_offset_mask;
    uint64_t primitive_storage_cage_offset_mask;
} GCLayout;

GC_API void gc_get_layout(GCLayout*);

// The base of the region all cells are allocated from, which NaN-boxed cell pointers are relative to. Reserves the
// region on first use.
GC_API uintptr_t gc_heap_region_base(void);
GC_API uintptr_t gc_primitive_storage_cage_base(void);

GC_API GCHeap* gc_heap_create(GCGatherRootsCallback, void* context, bool become_process_default);
// Finalizes and destroys every remaining cell. Roots created with gc_root_create() must be destroyed first. The final
// collection also runs every sweep callback and pending post-GC task, with gc_heap_is_collecting_everything() true,
// so their contexts must stay valid until this returns; nothing they allocate then is destroyed.
GC_API void gc_heap_destroy(GCHeap*);
GC_API void gc_heap_collect_garbage(GCHeap*, int collection_type, bool print_report);
GC_API bool gc_heap_is_collecting_everything(GCHeap const*);
GC_API void gc_heap_defer_gc(GCHeap*);
GC_API void gc_heap_undefer_gc(GCHeap*);
GC_API void gc_heap_set_should_collect_on_every_allocation(GCHeap*, bool);
GC_API void gc_heap_set_incremental_sweep_enabled(GCHeap*, bool);
GC_API void gc_heap_uproot_cell(GCHeap*, GCCell*);
// The bounds of the stack of the thread that created the heap; base is the lowest address.
GC_API void gc_heap_stack_bounds(GCHeap const*, uintptr_t* base, uintptr_t* top);
GC_API void gc_heap_enqueue_post_gc_task(GCHeap*, GCCallback, void* context);
// Runs during every collection after marking and finalization and before dead cells are swept, when weak holders can
// drop the cells that did not survive. There is no way to unregister a sweep callback, and it also runs when the heap
// is destroyed.
GC_API void gc_heap_register_sweep_callback(GCHeap*, GCCallback, void* context);
GC_API void gc_heap_did_allocate_external_memory(GCHeap*, size_t);
GC_API void gc_heap_did_free_external_memory(GCHeap*, size_t);

// The type info and the name must outlive the allocator, and the allocator must outlive every heap it allocated from.
GC_API GCAllocator* gc_allocator_create(GCCellTypeInfo const*, char const* name, size_t name_length);
GC_API void gc_allocator_destroy(GCAllocator*);

// Returns uninitialized storage for one cell. *must_mark tells the caller what to store as the cell's mark: cells that
// are allocated while an incremental sweep is in progress have to start out marked.
GC_API GCCell* gc_heap_allocate_cell(GCHeap*, GCAllocator*, bool* must_mark);
// The type info the cell's block was allocated with.
GC_API GCCellTypeInfo const* gc_cell_type_info(GCCell const*);

GC_API GCRoot* gc_root_create(GCCell*);
GC_API void gc_root_destroy(GCRoot*);
GC_API GCCell* gc_root_cell(GCRoot const*);

// Weak references are returned with one reference already taken, which the caller releases with
// gc_weak_impl_unref(). The pointer lives at weak_impl_pointer_offset and becomes null once the cell is collected.
GC_API GCWeakImpl* gc_heap_create_weak_impl(GCHeap*, GCCell*);
GC_API GCWeakImpl* gc_weak_impl_null(void);
GC_API void gc_weak_impl_ref(GCWeakImpl*);
GC_API void gc_weak_impl_unref(GCWeakImpl*);

GC_API void gc_visitor_visit_cell(GCVisitor*, GCCell*);
GC_API void gc_visitor_visit_cells(GCVisitor*, GCCell* const*, size_t count);
GC_API void gc_visitor_visit_values(GCVisitor*, uint64_t const*, size_t count);
// Treats every aligned word in the range as a possible pointer to a cell or NaN-boxed cell value.
GC_API void gc_visitor_visit_possible_values(GCVisitor*, uint8_t const*, size_t size);

#ifdef __cplusplus
}
#endif
