/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
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
    using DefineUnforgeableAttributesFunction = void (*)(JS::Realm&, JS::Object&);
    using ConstructFunction = JS::ThrowCompletionOr<GC::Ref<JS::Object>> (*)(InterfaceConstructor&, JS::FunctionObject&);

    StringView name;
    StringView namespaced_name;
    EnsurePrototypeFunction ensure_parent_prototype { nullptr };
    EnsureConstructorFunction ensure_parent_constructor { nullptr };
    InitializeConstructorFunction initialize_constructor { nullptr };
    InitializePrototypeFunction initialize_prototype { nullptr };
    DefineUnforgeableAttributesFunction define_unforgeable_attributes { nullptr };
    ConstructFunction construct { nullptr };
    bool has_immutable_prototype { false };
};

class WEB_API InterfacePrototypeObject final : public JS::Object {
    JS_OBJECT_WITH_CUSTOM_CLASS_NAME(InterfacePrototypeObject, JS::Object);

public:
    explicit InterfacePrototypeObject(JS::Realm&, InterfaceObjectMetadata const&);
    virtual void initialize(JS::Realm&) override;
    virtual ~InterfacePrototypeObject() override = default;
    virtual StringView class_name() const override { return m_metadata.name; }
    virtual JS::ThrowCompletionOr<bool> internal_set_prototype_of(JS::Object* prototype) override;

    void define_unforgeable_attributes(JS::Realm&, JS::Object&);

    GC_DECLARE_ALLOCATOR(InterfacePrototypeObject);

private:
    InterfaceObjectMetadata const& m_metadata;
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
