/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Export.h>

namespace Web::Bindings {

class InterfaceConstructor;

struct InterfaceObjectMetadata {
    using EnsurePrototypeFunction = JS::Object& (*)(JS::Realm&);
    using EnsureConstructorFunction = JS::NativeFunction& (*)(JS::Realm&);
    using InitializeConstructorFunction = void (*)(JS::Realm&, JS::NativeFunction&);
    using InitializePrototypeFunction = void (*)(JS::Realm&, JS::Object&);
    using ConstructFunction = JS::ThrowCompletionOr<GC::Ref<JS::Object>> (*)(InterfaceConstructor&, JS::FunctionObject&);

    StringView name;
    StringView namespaced_name;
    Utf16View utf16_name;
    Utf16View utf16_namespaced_name;
    EnsureConstructorFunction ensure_parent_constructor { nullptr };
    // The value of the constructor's "prototype" property, when it is not the prototype registered under
    // namespaced_name. A legacy factory function's is the prototype of the interface it constructs.
    EnsurePrototypeFunction ensure_interface_prototype_object { nullptr };
    InitializeConstructorFunction initialize_constructor { nullptr };
    InitializePrototypeFunction initialize_prototype { nullptr };
    ConstructFunction construct { nullptr };
    i32 function_length { 0 };
    bool is_legacy_factory_function { false };
    // The class of the interface prototype object, whose user data is this metadata.
    JSHostClass prototype_host_class {};
};

class WEB_API InterfaceConstructor final : public JS::NativeFunction {
    JS_OBJECT_WITH_CUSTOM_CLASS_NAME(InterfaceConstructor, JS::NativeFunction);

public:
    explicit InterfaceConstructor(JS::Realm&, InterfaceObjectMetadata const&);
    virtual void initialize(JS::Realm&) override;
    virtual ~InterfaceConstructor() override = default;
    virtual StringView class_name() const override { return m_metadata.name; }

    virtual JS::ThrowCompletionOr<JS::Value> call() override;
    virtual JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct(JS::FunctionObject& new_target) override;

    GC_DECLARE_ALLOCATOR(InterfaceConstructor);

private:
    virtual bool has_constructor() const override { return true; }

    InterfaceObjectMetadata const& m_metadata;
};

}
