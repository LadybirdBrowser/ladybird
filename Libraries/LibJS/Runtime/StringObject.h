/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class JS_API StringObject : public Object {
    JS_OBJECT(StringObject, Object);
    GC_DECLARE_ALLOCATOR(StringObject);

public:
    [[nodiscard]] static GC::Ref<StringObject> create(Realm&, PrimitiveString&, Object& prototype);

    virtual void initialize(Realm&) override;
    virtual ~StringObject() override = default;

    PrimitiveString const& primitive_string() const { return m_string; }
    PrimitiveString& primitive_string() { return m_string; }

protected:
    StringObject(PrimitiveString&, Object& prototype);

private:
    virtual ThrowCompletionOr<Optional<PropertyDescriptor>> internal_get_own_property(PropertyKey const&) const override;
    virtual ThrowCompletionOr<bool> internal_define_own_property(PropertyKey const&, PropertyDescriptor&, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr) override;
    virtual ThrowCompletionOr<GC::RootVector<Value>> internal_own_property_keys() const override;

    virtual bool is_string_object() const final { return true; }
    virtual bool eligible_for_own_property_enumeration_fast_path() const override final { return false; }
    virtual void visit_edges(Visitor&) override;

    GC::Ref<PrimitiveString> m_string;
};

template<>
inline bool Object::fast_is<StringObject>() const { return is_string_object(); }

}
