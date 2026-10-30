/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/HeapRegion.h>
#include <LibJS/Heap/Cell.h>

// The copy of the heap region base that the Rust half of LibJS decodes with (see layout_forward.rs).
extern "C" FlatPtr* js_heap_region_base_copy();

namespace JS {

// NB: Nothing in LibJS allocates a cell during static initialization, so this registers the copy before the first cell
//     that the Rust half could decode exists.
static GC::HeapRegionBaseCopyRegistration s_rust_heap_region_base_copy_registration { *js_heap_region_base_copy() };

void Cell::initialize(Realm&)
{
}

}
