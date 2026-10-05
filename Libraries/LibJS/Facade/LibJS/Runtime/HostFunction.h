/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

// A built-in function whose [[Call]] and [[Construct]] behavior comes from a JSHostClass of kind
// JS_HOST_CLASS_FUNCTION, with a companion cell for any state of its own.
class JS_API HostFunction : public NativeFunction {
public:
    // Defines "length" and then "name", as CreateBuiltinFunction does. The prototype defaults to %Function.prototype%.
    static GC::Ref<HostFunction> create(Realm&, JSHostClass const&, Utf16FlyString name, i32 length, GC::Ptr<Object> prototype = {}, GC::Ptr<GC::Cell> host_data = {});

    // For a caller that defines the function's own properties itself, in an order of its own.
    static GC::Ref<HostFunction> create_without_own_properties(Realm&, JSHostClass const&, Utf16FlyString name, GC::Ptr<Object> prototype = {}, GC::Ptr<GC::Cell> host_data = {});

    JSHostClass const& host_class() const;
    GC::Ptr<GC::Cell> host_data() const;
    void set_host_data(GC::Ptr<GC::Cell> host_data);

    // The class of every host function is derived from the runtime's host function class, whose id it keeps.
    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_HOST_FUNCTION; }
};

}
