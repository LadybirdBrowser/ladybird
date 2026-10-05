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
public:
    // The prototype defaults to %Array.prototype%.
    static GC::Ref<HostArray> create(Realm&, JSHostClass const&, GC::Ptr<Object> prototype = {}, GC::Ptr<GC::Cell> host_data = {});

    JSHostClass const& host_class() const;
    GC::Ptr<GC::Cell> host_data() const;
    void set_host_data(GC::Ptr<GC::Cell> host_data);

    // The Array exotic object's own [[Set]] and [[Delete]], for hooks that add to them rather than replace them.
    ThrowCompletionOr<bool> array_set(PropertyKey const&, Value value, Value receiver, CacheableSetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty);
    ThrowCompletionOr<bool> array_delete(PropertyKey const&);

    // The class of every host array is derived from the runtime's host array class, whose id it keeps.
    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_HOST_ARRAY; }
};

}
