/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Export.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

// An object whose internal methods come from a JSHostClass of kind JS_HOST_CLASS_OBJECT. It carries two cells for the
// embedder: the implementation object it wraps, at the fixed offset that direct getter functions read, and a companion
// cell with any other per-object state. LibJS/HostObjectABI.h fixes where the object keeps its class and both cells.
class JS_API HostObject : public Object {
public:
    static GC::Ref<HostObject> create(Realm&, JSHostClass const&, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> wrappable = {}, GC::Ptr<GC::Cell> host_data = {});

    JSHostClass const& host_class() const { return *engine_field<JSHostClass const*>(JS_HOST_OBJECT_HOST_CLASS_OFFSET); }
    GC::Ptr<GC::Cell> wrappable() const { return engine_field<GC::Cell*>(JS_HOST_OBJECT_WRAPPABLE_OFFSET); }
    GC::Ptr<GC::Cell> host_data() const { return engine_field<GC::Cell*>(JS_HOST_OBJECT_HOST_DATA_OFFSET); }
    void set_host_data(GC::Ptr<GC::Cell> host_data);

    static constexpr size_t wrappable_offset() { return JS_HOST_OBJECT_WRAPPABLE_OFFSET; }

    // The class of every host object is derived from the runtime's host object class, whose id it keeps.
    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_HOST_OBJECT; }
};

// Whether the object's host class is the given one or derives from it through JSHostClass::parent.
JS_API bool is_host_instance_of(Object const&, JSHostClass const&);

// The companion cell of a host object of any kind, or null.
JS_API GC::Ptr<GC::Cell> host_data_of(Object const&);

// The object's companion cell if it is exactly a T, which must be allocated with Heap::allocate<T>(). The type is
// checked through the allocator the cell came from, so this needs neither RTTI nor a virtual call on the cell.
template<typename T>
T* host_data_if(Object const& object)
{
    static_assert(IsSame<typename decltype(T::cell_allocator)::CellType, T>, "T must declare its own allocator");
    auto host_data = host_data_of(object);
    if (!host_data || !GC::cell_was_allocated_from(*host_data, T::cell_allocator))
        return nullptr;
    return static_cast<T*>(host_data.ptr());
}

}
