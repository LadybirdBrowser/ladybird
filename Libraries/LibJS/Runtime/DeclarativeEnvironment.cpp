/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

GC_DEFINE_ALLOCATOR(DeclarativeEnvironment);

DeclarativeEnvironment::DeclarativeEnvironment()
    : Environment(nullptr, IsDeclarative::Yes)
{
}

DeclarativeEnvironment::DeclarativeEnvironment(Environment* parent_environment)
    : Environment(parent_environment, IsDeclarative::Yes)
{
}

DeclarativeEnvironment::DeclarativeEnvironment(Environment* parent_environment, ReadonlySpan<Binding> bindings)
    : Environment(parent_environment, IsDeclarative::Yes)
{
    ensure_capacity(bindings.size());
    for (auto binding : bindings)
        append_binding(move(binding));
}

void DeclarativeEnvironment::RareData::visit_edges(Visitor& visitor) const
{
    m_dispose_capability.visit_edges(visitor);
}

size_t DeclarativeEnvironment::RareData::external_memory_size() const
{
    auto size = sizeof(RareData);
    size = saturating_add_external_memory_size(size, vector_external_memory_size(m_binding_names));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(m_binding_flags));
    size = saturating_add_external_memory_size(size, m_deleted_bindings.size_in_bytes());
    size = saturating_add_external_memory_size(size, hash_map_external_memory_size(m_bindings_assoc));
    if (m_dispose_capability.disposable_resource_stack) {
        size = saturating_add_external_memory_size(size, sizeof(Vector<DisposableResource>));
        size = saturating_add_external_memory_size(size, vector_external_memory_size(*m_dispose_capability.disposable_resource_stack));
    }
    return size;
}

bool DeclarativeEnvironment::RareData::is_empty() const
{
    return m_binding_names.is_empty()
        && m_binding_flags.is_empty()
        && m_deleted_bindings.is_null()
        && m_bindings_assoc.is_empty()
        && !m_dispose_capability.disposable_resource_stack
        && !m_environment_shape_cache
        && m_expected_binding_count == 0;
}

DeclarativeEnvironment::RareData& DeclarativeEnvironment::ensure_rare_data()
{
    if (!m_rare_data)
        m_rare_data = make<RareData>();
    return *m_rare_data;
}

void DeclarativeEnvironment::drop_rare_data_if_empty()
{
    if (m_rare_data && m_rare_data->is_empty())
        m_rare_data.clear();
}

void DeclarativeEnvironment::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_rare_data)
        m_rare_data->visit_edges(visitor);
    visitor.visit(m_shape);

    for (auto& value : m_binding_values)
        visitor.visit(value);
}

size_t DeclarativeEnvironment::external_memory_size() const
{
    auto size = vector_external_memory_size(m_binding_values);
    if (m_rare_data)
        size = saturating_add_external_memory_size(size, m_rare_data->external_memory_size());
    return size;
}

DisposeCapability const& DeclarativeEnvironment::dispose_capability() const
{
    static NeverDestroyed<DisposeCapability> empty_dispose_capability;
    if (!m_rare_data)
        return *empty_dispose_capability;
    return m_rare_data->m_dispose_capability;
}

DisposeCapability& DeclarativeEnvironment::dispose_capability()
{
    return ensure_rare_data().m_dispose_capability;
}

DisposeCapability* DeclarativeEnvironment::dispose_capability_if_exists()
{
    if (!m_rare_data || !m_rare_data->m_dispose_capability.disposable_resource_stack)
        return nullptr;
    return &m_rare_data->m_dispose_capability;
}

void DeclarativeEnvironment::append_binding(Binding binding)
{
    auto index = m_binding_values.size();

    u8 flags = 0;
    if (binding.strict)
        flags |= BindingFlagStrict;
    if (binding.mutable_)
        flags |= BindingFlagMutable;
    if (binding.can_be_deleted)
        flags |= BindingFlagCanBeDeleted;

    if (m_shape && index < shape_binding_count()) {
        VERIFY(m_shape->binding_name(index) == binding.name);
        VERIFY(m_shape->binding_flags(index) == flags);
    } else {
        auto& rare_data = ensure_rare_data();
        rare_data.m_bindings_assoc.set(binding.name, index);
        rare_data.m_binding_names.append(move(binding.name));
        rare_data.m_binding_flags.append(flags);
    }

    m_binding_values.append(binding.initialized ? binding.value : js_special_empty_value());
}

void DeclarativeEnvironment::clear_binding(Utf16FlyString const& name, size_t index)
{
    if (index < shape_binding_count()) {
        ensure_deleted_bindings_capacity(index + 1);
        m_rare_data->m_deleted_bindings.set(index, true);
        m_binding_values[index] = js_special_empty_value();
        return;
    }

    VERIFY(m_rare_data);
    m_rare_data->m_bindings_assoc.remove(name);
    auto local_index = local_binding_index(index);
    m_rare_data->m_binding_names[local_index] = Utf16FlyString {};
    m_binding_values[index] = js_special_empty_value();
    m_rare_data->m_binding_flags[local_index] = 0;
}

DeclarativeEnvironment::Binding DeclarativeEnvironment::binding_at(size_t index) const
{
    return Binding {
        .name = binding_name(index),
        .value = m_binding_values[index],
        .strict = binding_is_strict(index),
        .mutable_ = binding_is_mutable(index),
        .can_be_deleted = binding_can_be_deleted(index),
        .initialized = binding_is_initialized(index),
    };
}

void DeclarativeEnvironment::set_environment_shape_cache(GC::Ptr<EnvironmentShape>& cache, size_t expected_binding_count)
{
    if (expected_binding_count == 0)
        return;

    if (cache) {
        VERIFY(cache->size() == expected_binding_count);
        set_environment_shape(GC::Ref { *cache });
        return;
    }

    auto& rare_data = ensure_rare_data();
    rare_data.m_environment_shape_cache = &cache;
    rare_data.m_expected_binding_count = expected_binding_count;
}

void DeclarativeEnvironment::set_environment_shape(GC::Ref<EnvironmentShape> shape)
{
    VERIFY(!m_shape);
    VERIFY(m_binding_values.size() <= shape->size());

    m_shape = shape;
    if (m_rare_data) {
        m_rare_data->m_binding_names.clear();
        m_rare_data->m_binding_flags.clear();
        m_rare_data->m_bindings_assoc.clear();
        m_rare_data->m_environment_shape_cache = nullptr;
        m_rare_data->m_expected_binding_count = 0;
        drop_rare_data_if_empty();
    }
}

void DeclarativeEnvironment::maybe_finalize_environment_shape(VM& vm)
{
    if (!m_rare_data || !m_rare_data->m_environment_shape_cache || m_shape || m_rare_data->m_expected_binding_count == 0 || m_binding_values.size() != m_rare_data->m_expected_binding_count)
        return;

    if (*m_rare_data->m_environment_shape_cache) {
        auto shape = GC::Ref { **m_rare_data->m_environment_shape_cache };
        VERIFY(shape->size() == m_binding_values.size());
        for (size_t i = 0; i < m_binding_values.size(); ++i) {
            VERIFY(shape->binding_name(i) == m_rare_data->m_binding_names[i]);
            VERIFY(shape->binding_flags(i) == m_rare_data->m_binding_flags[i]);
        }
        set_environment_shape(shape);
        return;
    }

    auto shape = EnvironmentShape::create(vm, m_rare_data->m_binding_names, m_rare_data->m_binding_flags);
    *m_rare_data->m_environment_shape_cache = shape;
    set_environment_shape(shape);
}

// 9.1.1.1.1 HasBinding ( N ), https://tc39.es/ecma262/#sec-declarative-environment-records-hasbinding-n
ThrowCompletionOr<bool> DeclarativeEnvironment::has_binding(Utf16FlyString const& name, Optional<size_t>* out_index) const
{
    auto binding_and_index = find_binding_and_index(name);
    if (!binding_and_index.has_value())
        return false;
    if (!is_permanently_screwed_by_eval() && out_index && binding_and_index->index().has_value())
        *out_index = *(binding_and_index->index());
    return true;
}

// 9.1.1.1.2 CreateMutableBinding ( N, D ), https://tc39.es/ecma262/#sec-declarative-environment-records-createmutablebinding-n-d
ThrowCompletionOr<void> DeclarativeEnvironment::create_mutable_binding(VM& vm, Utf16FlyString const& name, bool can_be_deleted)
{
    // 1. Assert: envRec does not already have a binding for N.
    // NOTE: We skip this to avoid O(n) traversal of m_binding_names.

    // 2. Create a mutable binding in envRec for N and record that it is uninitialized. If D is true, record that the newly created binding may be deleted by a subsequent DeleteBinding call.
    append_binding(Binding {
        .name = name,
        .value = {},
        .strict = false,
        .mutable_ = true,
        .can_be_deleted = can_be_deleted,
        .initialized = false,
    });
    maybe_finalize_environment_shape(vm);

    ++m_environment_serial_number;

    // 3. Return unused.
    return {};
}

// 9.1.1.1.3 CreateImmutableBinding ( N, S ), https://tc39.es/ecma262/#sec-declarative-environment-records-createimmutablebinding-n-s
ThrowCompletionOr<void> DeclarativeEnvironment::create_immutable_binding(VM& vm, Utf16FlyString const& name, bool strict)
{
    // 1. Assert: envRec does not already have a binding for N.
    // NOTE: We skip this to avoid O(n) traversal of m_binding_names.

    // 2. Create an immutable binding in envRec for N and record that it is uninitialized. If S is true, record that the newly created binding is a strict binding.
    append_binding(Binding {
        .name = name,
        .value = {},
        .strict = strict,
        .mutable_ = false,
        .can_be_deleted = false,
        .initialized = false,
    });
    maybe_finalize_environment_shape(vm);

    ++m_environment_serial_number;

    // 3. Return unused.
    return {};
}

// 9.1.1.1.4 InitializeBinding ( N, V ), https://tc39.es/ecma262/#sec-declarative-environment-records-initializebinding-n-v
// 4.1.1.1.1 InitializeBinding ( N, V, hint ), https://tc39.es/proposal-explicit-resource-management/#sec-declarative-environment-records
ThrowCompletionOr<void> DeclarativeEnvironment::initialize_binding(VM& vm, Utf16FlyString const& name, Value value, Environment::InitializeBindingHint hint)
{
    return initialize_binding_direct(vm, find_binding_and_index(name)->index().value(), value, hint);
}

ThrowCompletionOr<void> DeclarativeEnvironment::initialize_binding_direct(VM& vm, size_t index, Value value, Environment::InitializeBindingHint hint)
{
    // 1. Assert: envRec must have an uninitialized binding for N.
    VERIFY(!binding_is_initialized(index));
    VERIFY(!value.is_special_empty_value());

    // 2. If hint is not normal, perform ? AddDisposableResource(envRec.[[DisposeCapability]], V, hint).
    if (hint != Environment::InitializeBindingHint::Normal)
        TRY(add_disposable_resource(vm, ensure_rare_data().m_dispose_capability, value, hint));

    // 3. Set the bound value for N in envRec to V.
    m_binding_values[index] = value;

    // 5. Return unused.
    return {};
}

// 9.1.1.1.5 SetMutableBinding ( N, V, S ), https://tc39.es/ecma262/#sec-declarative-environment-records-setmutablebinding-n-v-s
ThrowCompletionOr<void> DeclarativeEnvironment::set_mutable_binding(VM& vm, Utf16FlyString const& name, Value value, bool strict)
{
    // 1. If envRec does not have a binding for N, then
    auto binding_and_index = find_binding_and_index(name);
    if (!binding_and_index.has_value()) {
        // a. If S is true, throw a ReferenceError exception.
        if (strict)
            return vm.throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, name);

        // b. Perform ! envRec.CreateMutableBinding(N, true).
        MUST(create_mutable_binding(vm, name, true));

        // c. Perform ! envRec.InitializeBinding(N, V, normal).
        MUST(initialize_binding(vm, name, value, Environment::InitializeBindingHint::Normal));

        // d. Return unused.
        return {};
    }

    // 2-5. (extracted into a non-standard function below)
    if (binding_and_index->index().has_value()) {
        TRY(set_mutable_binding_direct(vm, *binding_and_index->index(), value, strict));
    } else {
        auto binding = binding_and_index->binding();
        TRY(set_mutable_binding_direct(vm, binding, value, strict));
    }

    // 6. Return unused.
    return {};
}

ThrowCompletionOr<void> DeclarativeEnvironment::set_mutable_binding_direct(VM& vm, size_t index, Value value, bool strict)
{
    if (binding_is_strict(index))
        strict = true;

    if (!binding_is_initialized(index))
        return vm.throw_completion<ReferenceError>(ErrorType::BindingNotInitialized, binding_name(index));

    if (binding_is_mutable(index)) {
        m_binding_values[index] = value;
    } else {
        if (strict)
            return vm.throw_completion<TypeError>(ErrorType::InvalidAssignToConst);
    }

    return {};
}

ThrowCompletionOr<void> DeclarativeEnvironment::set_mutable_binding_direct(VM& vm, Binding& binding, Value value, bool strict)
{
    if (binding.strict)
        strict = true;

    if (!binding.initialized)
        return vm.throw_completion<ReferenceError>(ErrorType::BindingNotInitialized, binding.name);

    if (binding.mutable_) {
        binding.value = value;
    } else {
        if (strict)
            return vm.throw_completion<TypeError>(ErrorType::InvalidAssignToConst);
    }

    return {};
}

// 9.1.1.1.6 GetBindingValue ( N, S ), https://tc39.es/ecma262/#sec-declarative-environment-records-getbindingvalue-n-s
ThrowCompletionOr<Value> DeclarativeEnvironment::get_binding_value(VM& vm, Utf16FlyString const& name, [[maybe_unused]] bool strict)
{
    // 1. Assert: envRec has a binding for N.
    auto binding_and_index = find_binding_and_index(name);
    VERIFY(binding_and_index.has_value());

    // 2-3. (extracted into a non-standard function below)
    if (binding_and_index->index().has_value())
        return get_binding_value_direct(vm, *binding_and_index->index());

    auto binding = binding_and_index->binding();
    return get_binding_value_direct(vm, binding);
}

// 9.1.1.1.7 DeleteBinding ( N ), https://tc39.es/ecma262/#sec-declarative-environment-records-deletebinding-n
ThrowCompletionOr<bool> DeclarativeEnvironment::delete_binding(VM&, Utf16FlyString const& name)
{
    // 1. Assert: envRec has a binding for the name that is the value of N.
    auto binding_and_index = find_binding_and_index(name);
    VERIFY(binding_and_index.has_value());

    // 2. If the binding for N in envRec cannot be deleted, return false.
    if (!binding_and_index->index().has_value()) {
        if (!binding_and_index->binding().can_be_deleted)
            return false;
        VERIFY_NOT_REACHED();
    }

    auto index = *binding_and_index->index();
    if (!binding_can_be_deleted(index))
        return false;

    // 3. Remove the binding for N from envRec.
    // NOTE: We keep the entry in the parallel vectors to avoid disturbing indices.
    clear_binding(name, index);

    ++m_environment_serial_number;

    // 4. Return true.
    return true;
}

ThrowCompletionOr<void> DeclarativeEnvironment::initialize_or_set_mutable_binding(VM& vm, Utf16FlyString const& name, Value value)
{
    auto binding_and_index = find_binding_and_index(name);
    VERIFY(binding_and_index.has_value());

    VERIFY(binding_and_index->index().has_value());
    auto index = *binding_and_index->index();

    if (!binding_is_initialized(index))
        TRY(initialize_binding_direct(vm, index, value, Environment::InitializeBindingHint::Normal));
    else
        TRY(set_mutable_binding_direct(vm, index, value, false));
    return {};
}

void DeclarativeEnvironment::shrink_to_fit()
{
    m_binding_values.shrink_to_fit();

    if (!m_rare_data)
        return;

    m_rare_data->m_binding_names.shrink_to_fit();
    m_rare_data->m_binding_flags.shrink_to_fit();

    if (m_binding_values.is_empty()) {
        m_rare_data->m_deleted_bindings = {};
        drop_rare_data_if_empty();
        return;
    }

    if (m_rare_data->m_deleted_bindings.is_null()) {
        drop_rare_data_if_empty();
        return;
    }

    auto deleted_bindings = MUST(Bitmap::create(m_binding_values.size(), false));
    for (size_t i = 0; i < m_binding_values.size(); ++i)
        deleted_bindings.set(i, binding_is_deleted(i));
    m_rare_data->m_deleted_bindings = move(deleted_bindings);
    drop_rare_data_if_empty();
}

}
