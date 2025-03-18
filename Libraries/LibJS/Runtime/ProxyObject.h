/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>

namespace JS {

class ProxyObject final : public FunctionObject {
    JS_OBJECT(ProxyObject, FunctionObject);
    GC_DECLARE_ALLOCATOR(ProxyObject);

public:
    static GC::Ref<ProxyObject> create(Realm&, Object& target, Object& handler);

    virtual ~ProxyObject() override = default;

    virtual FlyString const& name() const override;
    virtual bool has_constructor() const override;

    Object const& target() const { return m_target; }
    Object const& handler() const { return m_handler; }

    bool is_revoked() const { return m_is_revoked; }
    void revoke() { m_is_revoked = true; }

    // 10.5 Proxy Object Internal Methods and Internal Slots, https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots

    virtual ThrowCompletionOr<Object*> internal_get_prototype_of() const override;
    virtual ThrowCompletionOr<bool> internal_set_prototype_of(Object* prototype) override;
    virtual ThrowCompletionOr<bool> internal_is_extensible() const override;
    virtual ThrowCompletionOr<bool> internal_prevent_extensions() override;
    virtual ThrowCompletionOr<Optional<PropertyDescriptor>> internal_get_own_property(PropertyKey const&) const override;
    virtual ThrowCompletionOr<bool> internal_define_own_property(PropertyKey const&, PropertyDescriptor const&, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr) override;
    virtual ThrowCompletionOr<bool> internal_has_property(PropertyKey const&) const override;
    virtual ThrowCompletionOr<Value> internal_get(PropertyKey const&, Value receiver, CacheablePropertyMetadata*, PropertyLookupPhase) const override;
    virtual ThrowCompletionOr<bool> internal_set(PropertyKey const&, Value value, Value receiver, CacheablePropertyMetadata*) override;
    virtual ThrowCompletionOr<bool> internal_delete(PropertyKey const&) override;
    virtual ThrowCompletionOr<GC::RootVector<Value>> internal_own_property_keys() const override;
    virtual ThrowCompletionOr<Value> internal_call(Value this_argument, ReadonlySpan<Value> arguments_list) override;
    virtual ThrowCompletionOr<GC::Ref<Object>> internal_construct(ReadonlySpan<Value> arguments_list, FunctionObject& new_target) override;

private:
    ProxyObject(Object& target, Object& handler, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    virtual bool is_function() const override { return m_target->is_function(); }
    virtual bool is_proxy_object() const final { return true; }

    GC::Ref<Object> m_target;
    GC::Ref<Object> m_handler;
    bool m_is_revoked { false };
};

template<>
inline bool Object::fast_is<ProxyObject>() const { return is_proxy_object(); }

}
