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
    JS_OBJECT_WITH_CUSTOM_CLASS_NAME(HostFunction, NativeFunction);

public:
    // Defines "length" and then "name", as CreateBuiltinFunction does. The prototype defaults to %Function.prototype%.
    static GC::Ref<HostFunction> create(Realm&, JSHostClass const&, Utf16FlyString name, i32 length, GC::Ptr<Object> prototype = {}, GC::Ptr<GC::Cell> host_data = {});

    // For a caller that defines the function's own properties itself, in an order of its own.
    static GC::Ref<HostFunction> create_without_own_properties(Realm&, JSHostClass const&, Utf16FlyString name, GC::Ptr<Object> prototype = {}, GC::Ptr<GC::Cell> host_data = {});

    virtual ~HostFunction() override = default;

    JSHostClass const& host_class() const { return *m_host_class; }
    GC::Ptr<GC::Cell> host_data() const { return m_host_data; }
    void set_host_data(GC::Ptr<GC::Cell> host_data) { m_host_data = host_data; }

    virtual StringView class_name() const override;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject& new_target) override;
    virtual bool has_constructor() const override;

protected:
    HostFunction(JSHostClass const&, Utf16FlyString name, Object& prototype, GC::Ptr<GC::Cell> host_data);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    virtual JSHostClass const* host_class_if_host_object() const override { return m_host_class; }

private:
    JSHostFunctionHooks const& hooks() const;

    JSHostClass const* m_host_class { nullptr };
    GC::Ptr<GC::Cell> m_host_data;
};

template<>
inline bool Object::fast_is<HostFunction>() const
{
    auto const* host_class = host_class_of(*this);
    return host_class && host_class->kind == JS_HOST_CLASS_FUNCTION;
}

}
