/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/HostClassInternals.h>
#include <LibJS/Runtime/HostFunction.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace HostABI;

GC::Ref<HostFunction> HostFunction::create(Realm& realm, JSHostClass const& host_class, Utf16FlyString name, i32 length, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> host_data)
{
    auto& vm = realm.vm();
    auto function = create_without_own_properties(realm, host_class, move(name), prototype, host_data);
    function->define_direct_property(vm.names.length, Value(length), Attribute::Configurable);
    function->define_direct_property(vm.names.name, PrimitiveString::create(vm, function->name()), Attribute::Configurable);
    return function;
}

GC::Ref<HostFunction> HostFunction::create_without_own_properties(Realm& realm, JSHostClass const& host_class, Utf16FlyString name, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> host_data)
{
    if (!prototype)
        prototype = realm.intrinsics().function_prototype();
    auto function = realm.heap().allocate_with_descriptor(cell_allocator_for_host_class<HostFunction>(host_class), host_class, move(name), *prototype, host_data);
    static_cast<Cell&>(*function).initialize(realm);
    return function;
}

HostFunction::HostFunction(JSHostClass const& host_class, Utf16FlyString name, Object& prototype, GC::Ptr<GC::Cell> host_data)
    : NativeFunction(move(name), prototype)
    , m_host_class(&host_class)
    , m_host_data(host_data)
{
    VERIFY(host_class.abi_version == JS_HOST_ABI_VERSION && host_class.kind == JS_HOST_CLASS_FUNCTION);
    VERIFY(host_class.hooks && hooks().call);
    VERIFY(!(host_class.flags & JS_HOST_CLASS_HAS_CONSTRUCTOR) || hooks().construct);
    copy_host_class_flags_into_object(host_class, *this);
}

void HostFunction::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_host_data);
}

void HostFunction::finalize()
{
    Base::finalize();
    if (auto finalize_hook = hooks().finalize)
        finalize_hook(object_to_abi(this));
}

JSHostFunctionHooks const& HostFunction::hooks() const
{
    return *static_cast<JSHostFunctionHooks const*>(m_host_class->hooks);
}

StringView HostFunction::class_name() const
{
    return { m_host_class->name, m_host_class->name_length };
}

ThrowCompletionOr<Value> HostFunction::call()
{
    return completion_from_abi<Value>(hooks().call(object_to_abi(this), vm_to_abi(vm())));
}

ThrowCompletionOr<GC::Ref<Object>> HostFunction::construct(FunctionObject& new_target)
{
    auto hook = hooks().construct;
    if (!hook)
        return NativeFunction::construct(new_target);
    return completion_from_abi<GC::Ref<Object>>(hook(object_to_abi(this), vm_to_abi(vm()), object_to_abi(&new_target)));
}

bool HostFunction::has_constructor() const
{
    return m_host_class->flags & JS_HOST_CLASS_HAS_CONSTRUCTOR;
}

}
