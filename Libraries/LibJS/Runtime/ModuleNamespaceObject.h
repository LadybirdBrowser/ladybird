/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Module.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class JS_API ModuleNamespaceObject final : public Object {
    JS_OBJECT(ModuleNamespaceObject, Object);
    GC_DECLARE_ALLOCATOR(ModuleNamespaceObject);

public:
    // 10.4.6 Module Namespace Exotic Objects, https://tc39.es/ecma262/#sec-module-namespace-exotic-objects

    virtual ThrowCompletionOr<Object*> internal_get_prototype_of() const override;
    virtual ThrowCompletionOr<bool> internal_set_prototype_of(Object* prototype) override;
    virtual ThrowCompletionOr<bool> internal_is_extensible() const override;
    virtual ThrowCompletionOr<bool> internal_prevent_extensions() override;
    virtual ThrowCompletionOr<Optional<PropertyDescriptor>> internal_get_own_property(PropertyKey const&) const override;
    virtual ThrowCompletionOr<bool> internal_define_own_property(PropertyKey const&, PropertyDescriptor const&, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr) override;
    virtual ThrowCompletionOr<bool> internal_has_property(PropertyKey const&) const override;
    virtual ThrowCompletionOr<Value> internal_get(PropertyKey const&, Value receiver, CacheablePropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty) const override;
    virtual ThrowCompletionOr<bool> internal_set(PropertyKey const&, Value value, Value receiver, CacheablePropertyMetadata*, PropertyLookupPhase) override;
    virtual ThrowCompletionOr<bool> internal_delete(PropertyKey const&) override;
    virtual ThrowCompletionOr<GC::RootVector<Value>> internal_own_property_keys() const override;
    virtual void initialize(Realm&) override;

private:
    ModuleNamespaceObject(Realm&, Module* module, Vector<FlyString> exports);

    virtual void visit_edges(Visitor&) override;

    GC::Ptr<Module> m_module;    // [[Module]]
    Vector<FlyString> m_exports; // [[Exports]]
};

}
