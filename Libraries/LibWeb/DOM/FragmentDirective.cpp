/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/FragmentDirective.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(FragmentDirective);

GC::Ref<FragmentDirective> FragmentDirective::create()
{
    return GC::Heap::the().allocate<FragmentDirective>();
}

}
