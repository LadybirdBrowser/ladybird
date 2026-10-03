/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/Array.h>

namespace JS {

// An Array exotic object whose [[Set]] and [[Delete]] may come from a JSHostClass of kind JS_HOST_CLASS_ARRAY, with a
// companion cell for any state of its own.
class JS_API HostArray : public Array {
    JS_OBJECT_WITH_CUSTOM_CLASS_NAME(HostArray, Array);

public:
    // The prototype defaults to %Array.prototype%.
    static GC::Ref<HostArray> create(Realm&, JSHostClass const&, GC::Ptr<Object> prototype = {}, GC::Ptr<GC::Cell> host_data = {});

    virtual ~HostArray() override = default;

    JSHostClass const& host_class() const { return *m_host_class; }
    GC::Ptr<GC::Cell> host_data() const { return m_host_data; }
    void set_host_data(GC::Ptr<GC::Cell> host_data) { m_host_data = host_data; }

    virtual StringView class_name() const override;

    virtual ThrowCompletionOr<bool> internal_set(PropertyKey const&, Value value, Value receiver, CacheableSetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty) override;
    virtual ThrowCompletionOr<bool> internal_delete(PropertyKey const&) override;

    // The Array exotic object's own [[Set]] and [[Delete]], for hooks that add to them rather than replace them.
    ThrowCompletionOr<bool> array_set(PropertyKey const&, Value value, Value receiver, CacheableSetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty);
    ThrowCompletionOr<bool> array_delete(PropertyKey const&);

protected:
    HostArray(Realm&, JSHostClass const&, Object& prototype, GC::Ptr<GC::Cell> host_data);

    virtual void visit_edges(Cell::Visitor&) override;

    virtual JSHostClass const* host_class_if_host_object() const override { return m_host_class; }

private:
    JSHostArrayHooks const& hooks() const;

    JSHostClass const* m_host_class { nullptr };
    GC::Ptr<GC::Cell> m_host_data;
};

template<>
inline bool Object::fast_is<HostArray>() const
{
    auto const* host_class = host_class_of(*this);
    return host_class && host_class->kind == JS_HOST_CLASS_ARRAY;
}

}
