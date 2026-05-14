/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Bitmap.h>
#include <AK/HashMap.h>
#include <AK/Utf16FlyString.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class JS_API DeclarativeEnvironment : public Environment {
    JS_ENVIRONMENT(DeclarativeEnvironment, Environment);
    GC_DECLARE_ALLOCATOR(DeclarativeEnvironment);

    struct Binding {
        Utf16FlyString name;
        Value value;
        bool strict { false };
        bool mutable_ { false };
        bool can_be_deleted { false };
        bool initialized { false };
    };

public:
    virtual ~DeclarativeEnvironment() override = default;

    virtual bool is_catch_environment() const override { return m_is_catch_environment; }
    void set_is_catch_environment(bool value) { m_is_catch_environment = value; }

    virtual ThrowCompletionOr<bool> has_binding(Utf16FlyString const& name, Optional<size_t>* = nullptr) const override final;
    virtual ThrowCompletionOr<void> create_mutable_binding(VM&, Utf16FlyString const& name, bool can_be_deleted) override final;
    virtual ThrowCompletionOr<void> create_immutable_binding(VM&, Utf16FlyString const& name, bool strict) override final;
    virtual ThrowCompletionOr<void> initialize_binding(VM&, Utf16FlyString const& name, Value, InitializeBindingHint) override final;
    virtual ThrowCompletionOr<void> set_mutable_binding(VM&, Utf16FlyString const& name, Value, bool strict) override final;
    virtual ThrowCompletionOr<Value> get_binding_value(VM&, Utf16FlyString const& name, bool strict) override;
    virtual ThrowCompletionOr<bool> delete_binding(VM&, Utf16FlyString const& name) override;

    ThrowCompletionOr<void> initialize_or_set_mutable_binding(VM&, Utf16FlyString const& name, Value value);

    // This is not a method defined in the spec! Do not use this in any LibJS (or other spec related) code.
    [[nodiscard]] Vector<Utf16FlyString> bindings() const
    {
        Vector<Utf16FlyString> names;
        names.ensure_capacity(m_binding_names.size());

        for (auto const& name : m_binding_names) {
            if (!name.is_empty())
                names.unchecked_append(name);
        }

        return names;
    }

    ThrowCompletionOr<void> initialize_binding_direct(VM&, size_t index, Value, InitializeBindingHint);
    ThrowCompletionOr<void> set_mutable_binding_direct(VM&, size_t index, Value, bool strict);
    ThrowCompletionOr<Value> get_binding_value_direct(VM&, size_t index) const;
    Value get_initialized_binding_value_direct(size_t index) const { return m_binding_values[index]; }

    void shrink_to_fit();

    void ensure_capacity(size_t needed_capacity)
    {
        m_binding_names.ensure_capacity(needed_capacity);
        m_binding_values.ensure_capacity(needed_capacity);
        m_binding_flags.ensure_capacity(needed_capacity);
        ensure_initialized_bindings_capacity(needed_capacity);
    }

    [[nodiscard]] u64 environment_serial_number() const { return m_environment_serial_number; }

    DisposeCapability const& dispose_capability() const { return m_dispose_capability; }
    DisposeCapability& dispose_capability() { return m_dispose_capability; }

private:
    enum BindingFlag : u8 {
        BindingFlagStrict = 1 << 0,
        BindingFlagMutable = 1 << 1,
        BindingFlagCanBeDeleted = 1 << 2,
    };

    void append_binding(Binding);
    void clear_binding(Utf16FlyString const& name, size_t index);
    void ensure_initialized_bindings_capacity(size_t needed_capacity)
    {
        if (needed_capacity <= m_initialized_bindings.size())
            return;
        m_initialized_bindings.grow(ceil_div(needed_capacity, static_cast<size_t>(8)) * 8, false);
    }
    Binding binding_at(size_t index) const;
    bool binding_is_strict(size_t index) const { return (m_binding_flags[index] & BindingFlagStrict) != 0; }
    bool binding_is_mutable(size_t index) const { return (m_binding_flags[index] & BindingFlagMutable) != 0; }
    bool binding_can_be_deleted(size_t index) const { return (m_binding_flags[index] & BindingFlagCanBeDeleted) != 0; }
    bool binding_is_initialized(size_t index) const { return m_initialized_bindings.get(index); }
    void set_binding_initialized(size_t index, bool initialized) { m_initialized_bindings.set(index, initialized); }

    ThrowCompletionOr<Value> get_binding_value_direct(VM&, Binding const&) const;
    ThrowCompletionOr<void> set_mutable_binding_direct(VM&, Binding&, Value, bool strict);

protected:
    DeclarativeEnvironment();
    explicit DeclarativeEnvironment(Environment* parent_environment);
    DeclarativeEnvironment(Environment* parent_environment, ReadonlySpan<Binding> bindings);

    virtual void visit_edges(Visitor&) override;
    virtual size_t external_memory_size() const override;

    class BindingAndIndex {
    public:
        Binding binding() const
        {
            if (m_referenced_environment)
                return m_referenced_environment->binding_at(m_index.value());
            return m_temporary_binding;
        }

        BindingAndIndex(DeclarativeEnvironment const& environment, size_t index)
            : m_referenced_environment(&environment)
            , m_index(index)
        {
        }

        explicit BindingAndIndex(Binding temporary_binding)
            : m_temporary_binding(move(temporary_binding))
        {
        }

        Optional<size_t> const& index() const { return m_index; }

    private:
        GC::Ptr<DeclarativeEnvironment const> m_referenced_environment;
        Binding m_temporary_binding {};
        Optional<size_t> m_index;
    };

    friend class ModuleEnvironment;

    virtual Optional<BindingAndIndex> find_binding_and_index(Utf16FlyString const& name) const
    {
        if (auto it = m_bindings_assoc.find(name); it != m_bindings_assoc.end()) {
            return BindingAndIndex { *this, it->value };
        }

        return {};
    }

private:
    Vector<Utf16FlyString> m_binding_names;
    Vector<Value> m_binding_values;
    Vector<u8> m_binding_flags;
    Bitmap m_initialized_bindings;
    HashMap<Utf16FlyString, size_t> m_bindings_assoc;
    DisposeCapability m_dispose_capability;

    u64 m_environment_serial_number { 0 };
    bool m_is_catch_environment { false };
};

inline ThrowCompletionOr<Value> DeclarativeEnvironment::get_binding_value_direct(VM& vm, size_t index) const
{
    if (!binding_is_initialized(index))
        return vm.throw_completion<ReferenceError>(ErrorType::BindingNotInitialized, m_binding_names[index]);

    return m_binding_values[index];
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
