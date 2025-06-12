/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
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
    GC_DECLARE_ALLOCATOR(ArgumentsObject);

public:
    virtual void initialize(Realm&) override;
    virtual ~ArgumentsObject() override = default;

    virtual ThrowCompletionOr<Optional<PropertyDescriptor>> internal_get_own_property(PropertyKey const&) const override;
    virtual ThrowCompletionOr<bool> internal_define_own_property(PropertyKey const&, PropertyDescriptor const&, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr) override;
    virtual ThrowCompletionOr<Value> internal_get(PropertyKey const&, Value receiver, CacheablePropertyMetadata*, PropertyLookupPhase) const override;
    virtual ThrowCompletionOr<bool> internal_set(PropertyKey const&, Value value, Value receiver, CacheablePropertyMetadata*, PropertyLookupPhase) override;
    virtual ThrowCompletionOr<bool> internal_delete(PropertyKey const&) override;

    void set_mapped_names(Vector<FlyString> mapped_names) { m_mapped_names = move(mapped_names); }

private:
    ArgumentsObject(Realm&, Environment&);

    [[nodiscard]] bool parameter_map_has(PropertyKey const&) const;
    [[nodiscard]] Value get_from_parameter_map(PropertyKey const&) const;
    void set_in_parameter_map(PropertyKey const&, Value);
    void delete_from_parameter_map(PropertyKey const&);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<Environment> m_environment;
    Vector<FlyString> m_mapped_names;
};

}
