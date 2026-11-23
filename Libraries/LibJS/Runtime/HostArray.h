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

// An Array exotic object with host-class hooks for its internal methods and a companion cell for its own state.
class JS_API HostArray : public Array {
public:
    // The prototype defaults to %Array.prototype%.
    static GC::Ref<HostArray> create(Realm&, JSHostClass const&, GC::Ptr<Object> prototype = {}, GC::Ptr<GC::Cell> host_data = {});

    JSHostClass const& host_class() const;
    GC::Ptr<GC::Cell> host_data() const;
    void set_host_data(GC::Ptr<GC::Cell> host_data);

    // The underlying Array operations that host-class hooks can delegate to.
    ThrowCompletionOr<bool> array_define_own_property(PropertyKey const&, PropertyDescriptor&, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr);
    ThrowCompletionOr<bool> array_set(PropertyKey const&, Value value, Value receiver, CacheableSetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty);
    ThrowCompletionOr<bool> array_delete(PropertyKey const&);

    // The class of every host array is derived from the runtime's host array class, whose id it keeps.
    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_HOST_ARRAY; }
};

}
