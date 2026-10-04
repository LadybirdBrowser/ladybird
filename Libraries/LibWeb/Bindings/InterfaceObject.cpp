/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/InterfaceObject.h>

namespace Web::Bindings {

static InterfaceObjectMetadata const& interface_object_metadata_of(JS::HostFunction const& interface_object)
{
    return *static_cast<InterfaceObjectMetadata const*>(interface_object.host_class().user_data);
}

struct InterfaceConstructorTraits {
    static JS::ThrowCompletionOr<JS::Value> call(JS::HostFunction& interface_object, JS::VM& vm)
    {
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::ConstructorWithoutNew, interface_object_metadata_of(interface_object).namespaced_name);
    }

    static JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct(JS::HostFunction& interface_object, JS::VM& vm, JS::FunctionObject& new_target)
    {
        auto const& metadata = interface_object_metadata_of(interface_object);
        if (metadata.construct)
            return metadata.construct(interface_object, new_target);
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAConstructor, metadata.namespaced_name);
    }
};

constexpr JSHostFunctionHooks interface_constructor_hooks = JS::make_host_function_hooks<InterfaceConstructorTraits>();

constexpr JSHostClass interface_prototype_object_parent_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "InterfacePrototypeObject"sv, nullptr, nullptr, nullptr, 0);
constexpr JSHostClass interface_constructor_parent_host_class = JS::make_host_class(JS_HOST_CLASS_FUNCTION, "InterfaceConstructor"sv, nullptr, &interface_constructor_hooks, nullptr, JS_HOST_CLASS_HAS_CONSTRUCTOR);

}
