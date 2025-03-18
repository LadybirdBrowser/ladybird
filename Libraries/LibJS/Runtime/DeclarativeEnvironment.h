/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class DeclarativeEnvironment : public Environment {
    JS_ENVIRONMENT(DeclarativeEnvironment, Environment);
    GC_DECLARE_ALLOCATOR(DeclarativeEnvironment);

    struct Binding {
        FlyString name;
        Value value;
        bool strict { false };
        bool mutable_ { false };
        bool can_be_deleted { false };
        bool initialized { false };
    };

public:
    static DeclarativeEnvironment* create_for_per_iteration_bindings(Badge<ForStatement>, DeclarativeEnvironment& other, size_t bindings_size);

    virtual ~DeclarativeEnvironment() override = default;

    virtual ThrowCompletionOr<bool> has_binding(FlyString const& name, Optional<size_t>* = nullptr) const override final;
    virtual ThrowCompletionOr<void> create_mutable_binding(VM&, FlyString const& name, bool can_be_deleted) override final;
    virtual ThrowCompletionOr<void> create_immutable_binding(VM&, FlyString const& name, bool strict) override final;
    virtual ThrowCompletionOr<void> initialize_binding(VM&, FlyString const& name, Value, InitializeBindingHint) override final;
    virtual ThrowCompletionOr<void> set_mutable_binding(VM&, FlyString const& name, Value, bool strict) override final;
    virtual ThrowCompletionOr<Value> get_binding_value(VM&, FlyString const& name, bool strict) override;
    virtual ThrowCompletionOr<bool> delete_binding(VM&, FlyString const& name) override;

    void initialize_or_set_mutable_binding(Badge<ScopeNode>, VM&, FlyString const& name, Value value);
    ThrowCompletionOr<void> initialize_or_set_mutable_binding(VM&, FlyString const& name, Value value);

    // This is not a method defined in the spec! Do not use this in any LibJS (or other spec related) code.
    [[nodiscard]] Vector<FlyString> bindings() const
    {
        Vector<FlyString> names;
        names.ensure_capacity(m_bindings.size());

        for (auto const& binding : m_bindings)
            names.unchecked_append(binding.name);

        return names;
    }

    ThrowCompletionOr<void> initialize_binding_direct(VM&, size_t index, Value, InitializeBindingHint);
    ThrowCompletionOr<void> set_mutable_binding_direct(VM&, size_t index, Value, bool strict);
    ThrowCompletionOr<Value> get_binding_value_direct(VM&, size_t index) const;

    void shrink_to_fit();

    void ensure_capacity(size_t needed_capacity)
    {
        m_bindings.ensure_capacity(needed_capacity);
    }

    [[nodiscard]] u64 environment_serial_number() const { return m_environment_serial_number; }

    DisposeCapability const& dispose_capability() const { return m_dispose_capability; }
    DisposeCapability& dispose_capability() { return m_dispose_capability; }

private:
    ThrowCompletionOr<Value> get_binding_value_direct(VM&, Binding const&) const;
    ThrowCompletionOr<void> set_mutable_binding_direct(VM&, Binding&, Value, bool strict);

protected:
    DeclarativeEnvironment();
    explicit DeclarativeEnvironment(Environment* parent_environment);
    DeclarativeEnvironment(Environment* parent_environment, ReadonlySpan<Binding> bindings);

    virtual void visit_edges(Visitor&) override;

    class BindingAndIndex {
    public:
        Binding& binding()
        {
            if (m_referenced_binding)
                return *m_referenced_binding;
            return m_temporary_binding;
        }

        BindingAndIndex(Binding* binding, Optional<size_t> index)
            : m_referenced_binding(binding)
            , m_index(move(index))
        {
        }

        explicit BindingAndIndex(Binding temporary_binding)
            : m_temporary_binding(move(temporary_binding))
        {
        }

        Optional<size_t> const& index() const { return m_index; }

    private:
        Binding* m_referenced_binding { nullptr };
        Binding m_temporary_binding {};
        Optional<size_t> m_index;
    };

    friend class ModuleEnvironment;

    virtual Optional<BindingAndIndex> find_binding_and_index(FlyString const& name) const
    {
        if (auto it = m_bindings_assoc.find(name); it != m_bindings_assoc.end()) {
            return BindingAndIndex { const_cast<Binding*>(&m_bindings.at(it->value)), it->value };
        }

        return {};
    }

private:
    Vector<Binding> m_bindings;
    HashMap<FlyString, size_t> m_bindings_assoc;
    DisposeCapability m_dispose_capability;

    u64 m_environment_serial_number { 0 };
};

inline ThrowCompletionOr<Value> DeclarativeEnvironment::get_binding_value_direct(VM& vm, size_t index) const
{
    return get_binding_value_direct(vm, m_bindings[index]);
}

inline ThrowCompletionOr<Value> DeclarativeEnvironment::get_binding_value_direct(VM&, Binding const& binding) const
{
    // 2. If the binding for N in envRec is an uninitialized binding, throw a ReferenceError exception.
    if (!binding.initialized)
        return vm().throw_completion<ReferenceError>(ErrorType::BindingNotInitialized, binding.name);

    // 3. Return the value currently bound to N in envRec.
    return binding.value;
}

template<>
inline bool Environment::fast_is<DeclarativeEnvironment>() const { return is_declarative_environment(); }

}
