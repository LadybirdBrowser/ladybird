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
// cell with any other per-object state.
class JS_API HostObject : public Object {
    JS_OBJECT_WITH_CUSTOM_CLASS_NAME(HostObject, Object);

public:
    static GC::Ref<HostObject> create(Realm&, JSHostClass const&, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> wrappable = {}, GC::Ptr<GC::Cell> host_data = {});

    virtual ~HostObject() override = default;

    JSHostClass const& host_class() const { return *m_host_class; }
    GC::Ptr<GC::Cell> wrappable() const { return m_wrappable; }
    GC::Ptr<GC::Cell> host_data() const { return m_host_data; }
    void set_host_data(GC::Ptr<GC::Cell> host_data) { m_host_data = host_data; }

    static constexpr size_t wrappable_offset() { return offsetof(HostObject, m_wrappable); }

    virtual StringView class_name() const override;

    virtual ThrowCompletionOr<Object*> internal_get_prototype_of() const override;
    virtual ThrowCompletionOr<bool> internal_set_prototype_of(Object* prototype) override;
    virtual ThrowCompletionOr<bool> internal_is_extensible() const override;
    virtual ThrowCompletionOr<bool> internal_prevent_extensions() override;
    virtual ThrowCompletionOr<Optional<PropertyDescriptor>> internal_get_own_property(PropertyKey const&) const override;
    virtual ThrowCompletionOr<bool> internal_define_own_property(PropertyKey const&, PropertyDescriptor&, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr) override;
    virtual ThrowCompletionOr<bool> internal_has_property(PropertyKey const&) const override;
    virtual ThrowCompletionOr<Value> internal_get(PropertyKey const&, Value receiver, CacheableGetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty) const override;
    virtual ThrowCompletionOr<bool> internal_set(PropertyKey const&, Value value, Value receiver, CacheableSetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty) override;
    virtual ThrowCompletionOr<bool> internal_delete(PropertyKey const&) override;
    virtual ThrowCompletionOr<GC::RootVector<Value>> internal_own_property_keys() const override;

    virtual bool is_cacheable_for_property_absence() const override;
    virtual bool is_cacheable_for_inherited_property() const override;
    virtual bool eligible_for_own_property_enumeration_fast_path() const override;

    virtual ErrorData* error_data() override;
    virtual ErrorData const* error_data() const override;

protected:
    HostObject(Realm&, JSHostClass const&, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> wrappable, GC::Ptr<GC::Cell> host_data);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    virtual JSHostClass const* host_class_if_host_object() const override { return m_host_class; }

private:
    JSHostObjectHooks const& hooks() const;
    JSObject* as_abi_object() const;

    JSHostClass const* m_host_class { nullptr };
    GC::Ptr<GC::Cell> m_wrappable;
    GC::Ptr<GC::Cell> m_host_data;
};

template<>
inline bool Object::fast_is<HostObject>() const
{
    auto const* host_class = host_class_of(*this);
    return host_class && host_class->kind == JS_HOST_CLASS_OBJECT;
}

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
