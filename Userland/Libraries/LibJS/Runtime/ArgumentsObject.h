/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class ArgumentsObject final : public Object {
    JS_OBJECT(ArgumentsObject, Object);
    JS_DECLARE_ALLOCATOR(ArgumentsObject);

public:
    virtual void initialize(Realm&) override;
    virtual ~ArgumentsObject() override = default;

    Environment& environment() { return m_environment; }

    virtual ThrowCompletionOr<Optional<PropertyDescriptor>> internal_get_own_property(PropertyKey const&) const override;
    virtual ThrowCompletionOr<bool> internal_define_own_property(PropertyKey const&, PropertyDescriptor const&) override;
    virtual ThrowCompletionOr<Value> internal_get(PropertyKey const&, Value receiver, CacheablePropertyMetadata*, PropertyLookupPhase) const override;
    virtual ThrowCompletionOr<bool> internal_set(PropertyKey const&, Value value, Value receiver, CacheablePropertyMetadata*) override;
    virtual ThrowCompletionOr<bool> internal_delete(PropertyKey const&) override;

    // [[ParameterMap]]
    Object& parameter_map() { return *m_parameter_map; }

private:
    ArgumentsObject(Realm&, Environment&);

    virtual void visit_edges(Cell::Visitor&) override;

    NonnullGCPtr<Environment> m_environment;
    GCPtr<Object> m_parameter_map;
};

}
