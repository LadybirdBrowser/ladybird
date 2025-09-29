/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/Environment.h>

namespace JS {

class JS_API GlobalEnvironment final : public Environment {
    JS_ENVIRONMENT(GlobalEnvironment, Environment);
    GC_DECLARE_ALLOCATOR(GlobalEnvironment);

public:
    virtual bool has_this_binding() const final { return true; }
    virtual ThrowCompletionOr<Value> get_this_binding(VM&) const final;

    virtual ThrowCompletionOr<bool> has_binding(Utf16FlyString const& name, Optional<size_t>* = nullptr) const override;
    virtual ThrowCompletionOr<void> create_mutable_binding(VM&, Utf16FlyString const& name, bool can_be_deleted) override;
    virtual ThrowCompletionOr<void> create_immutable_binding(VM&, Utf16FlyString const& name, bool strict) override;
    virtual ThrowCompletionOr<void> initialize_binding(VM&, Utf16FlyString const& name, Value, Environment::InitializeBindingHint) override;
    virtual ThrowCompletionOr<void> set_mutable_binding(VM&, Utf16FlyString const& name, Value, bool strict) override;
    virtual ThrowCompletionOr<Value> get_binding_value(VM&, Utf16FlyString const& name, bool strict) override;
    virtual ThrowCompletionOr<bool> delete_binding(VM&, Utf16FlyString const& name) override;

    ObjectEnvironment& object_record() const { return *m_object_record; }
    Object& global_this_value() const { return *m_global_this_value; }
    DeclarativeEnvironment& declarative_record() const { return *m_declarative_record; }

    bool has_lexical_declaration(Utf16FlyString const& name) const;
    ThrowCompletionOr<bool> has_restricted_global_property(Utf16FlyString const& name) const;
    ThrowCompletionOr<bool> can_declare_global_var(Utf16FlyString const& name) const;
    ThrowCompletionOr<bool> can_declare_global_function(Utf16FlyString const& name) const;
    ThrowCompletionOr<void> create_global_var_binding(Utf16FlyString const& name, bool can_be_deleted);
    ThrowCompletionOr<void> create_global_function_binding(Utf16FlyString const& name, Value, bool can_be_deleted);

private:
    GlobalEnvironment(Object&, Object& this_value);

    virtual bool is_global_environment() const override { return true; }
    virtual void visit_edges(Visitor&) override;

    GC::Ref<ObjectEnvironment> m_object_record;           // [[ObjectRecord]]
    GC::Ref<Object> m_global_this_value;                  // [[GlobalThisValue]]
    GC::Ref<DeclarativeEnvironment> m_declarative_record; // [[DeclarativeRecord]]
};

template<>
inline bool Environment::fast_is<GlobalEnvironment>() const { return is_global_environment(); }

}
