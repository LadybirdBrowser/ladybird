/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

// Each host class gets an allocator of its own, named after the class, so that objects of different host classes never
// share heap blocks, as GC_DECLARE_ALLOCATOR ensures for C++ classes. Like those, the allocators live as long as the
// process. A class with JS_HOST_CLASS_SHARES_ALLOCATOR_WITH_PARENT uses the allocator of its nearest ancestor without
// that flag instead.
template<typename HostCell>
GC::TypeIsolatingCellAllocator<HostCell>& cell_allocator_for_host_class(JSHostClass const& host_class)
{
    using Allocator = GC::TypeIsolatingCellAllocator<HostCell>;
    static NeverDestroyed<HashMap<JSHostClass const*, Allocator*>> allocators;
    static JSHostClass const* last_host_class = nullptr;
    static Allocator* last_allocator = nullptr;

    if (&host_class == last_host_class)
        return *last_allocator;

    auto const* allocating_class = &host_class;
    while (allocating_class->flags & JS_HOST_CLASS_SHARES_ALLOCATOR_WITH_PARENT) {
        VERIFY(allocating_class->parent && allocating_class->parent->kind == allocating_class->kind);
        allocating_class = allocating_class->parent;
    }

    auto& allocator = *allocators->ensure(allocating_class, [&] {
        return new Allocator(StringView { allocating_class->name, allocating_class->name_length });
    });
    last_host_class = &host_class;
    last_allocator = &allocator;
    return allocator;
}

inline void copy_host_class_flags_into_object(JSHostClass const& host_class, Object& object)
{
    auto flags = host_class.flags;
    if (flags & JS_HOST_CLASS_IS_PLATFORM_OBJECT)
        object.set_is_platform_object();
    if (flags & JS_HOST_CLASS_REQUIRES_SLOW_ADD_OWN_PROPERTY)
        object.set_requires_slow_add_own_property();
    if (flags & JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS)
        object.set_may_interfere_with_indexed_property_access();
    if (flags & JS_HOST_CLASS_IS_HTMLDDA)
        object.set_is_htmldda();
    if (flags & JS_HOST_CLASS_IS_GLOBAL_OBJECT)
        object.set_global_object_flag();
}

}
