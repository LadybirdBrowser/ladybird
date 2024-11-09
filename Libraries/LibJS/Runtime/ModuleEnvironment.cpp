/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ModuleEnvironment);

// 9.1.2.6 NewModuleEnvironment ( E ), https://tc39.es/ecma262/#sec-newmoduleenvironment
ModuleEnvironment::ModuleEnvironment(Environment* outer_environment)
    : DeclarativeEnvironment(outer_environment)
{
}

// 9.1.1.5.1 GetBindingValue ( N, S ), https://tc39.es/ecma262/#sec-module-environment-records-getbindingvalue-n-s
ThrowCompletionOr<Value> ModuleEnvironment::get_binding_value(VM& vm, DeprecatedFlyString const& name, bool strict)
{
    // 1. Assert: S is true.
    VERIFY(strict);

    // 2. Assert: envRec has a binding for N.
    auto* indirect_binding = get_indirect_binding(name);
    VERIFY(indirect_binding || !DeclarativeEnvironment::has_binding(name).is_error());

    // 3. If the binding for N is an indirect binding, then
    if (indirect_binding) {
        // a. Let M and N2 be the indirection values provided when this binding for N was created.

        // b. Let targetEnv be M.[[Environment]].
        auto* target_env = indirect_binding->module->environment();

        // c. If targetEnv is empty, throw a ReferenceError exception.
        if (!target_env)
            return vm.throw_completion<ReferenceError>(ErrorType::ModuleNoEnvironment);

        // d. Return ? targetEnv.GetBindingValue(N2, true).
        return target_env->get_binding_value(vm, indirect_binding->binding_name, true);
    }

    // 4. If the binding for N in envRec is an uninitialized binding, throw a ReferenceError exception.
    // 5. Return the value currently bound to N in envRec.
    // Note: Step 4 & 5 are the steps performed by declarative environment GetBindingValue
    return DeclarativeEnvironment::get_binding_value(vm, name, strict);
}

// 9.1.1.5.2 DeleteBinding ( N ), https://tc39.es/ecma262/#sec-module-environment-records-deletebinding-n
ThrowCompletionOr<bool> ModuleEnvironment::delete_binding(VM&, DeprecatedFlyString const&)
{
    // The DeleteBinding concrete method of a module Environment Record is never used within this specification.
    VERIFY_NOT_REACHED();
}

// 9.1.1.5.4 GetThisBinding ( ), https://tc39.es/ecma262/#sec-module-environment-records-getthisbinding
ThrowCompletionOr<Value> ModuleEnvironment::get_this_binding(VM&) const
{
    // 1. Return undefined.
    return js_undefined();
}

// 9.1.1.5.5 CreateImportBinding ( N, M, N2 ), https://tc39.es/ecma262/#sec-createimportbinding
ThrowCompletionOr<void> ModuleEnvironment::create_import_binding(DeprecatedFlyString name, Module* module, DeprecatedFlyString binding_name)
{
    // 1. Assert: envRec does not already have a binding for N.
    VERIFY(!get_indirect_binding(name));
    // 2. Assert: When M.[[Environment]] is instantiated it will have a direct binding for N2.
    // FIXME: I don't know what this means or how to check it.

    // 3. Create an immutable indirect binding in envRec for N that references M and N2 as its target binding and record that the binding is initialized.
    // Note: We use the fact that the binding is in this list as it being initialized.
    m_indirect_bindings.append({ move(name),
        module,
        move(binding_name) });

    // 4. Return unused.
    return {};
}

ModuleEnvironment::IndirectBinding const* ModuleEnvironment::get_indirect_binding(DeprecatedFlyString const& name) const
{
    auto binding_or_end = m_indirect_bindings.find_if([&](IndirectBinding const& binding) {
        return binding.name == name;
    });
    if (binding_or_end.is_end())
        return nullptr;

    return &(*binding_or_end);
}

Optional<ModuleEnvironment::BindingAndIndex> ModuleEnvironment::find_binding_and_index(DeprecatedFlyString const& name) const
{
    auto* indirect_binding = get_indirect_binding(name);
    if (indirect_binding != nullptr) {
        auto* target_env = indirect_binding->module->environment();
        if (!target_env)
            return {};

        VERIFY(is<ModuleEnvironment>(target_env));
        auto& target_module_environment = static_cast<ModuleEnvironment&>(*target_env);
        auto result = target_module_environment.find_binding_and_index(indirect_binding->binding_name);
        if (!result.has_value())
            return {};

        // NOTE: We must pretend this binding is actually from this environment
        //       so as specified by
        //       9.1.1.5.5 CreateImportBinding ( N, M, N2 ), https://tc39.es/ecma262/#sec-createimportbinding
        //       It creates a new initialized immutable indirect binding for the
        //       name N. A binding must not already exist in this Environment
        //       Record for N. N2 is the name of a binding that exists in M's
        //       Module Environment Record. Accesses to the value of the new
        //       binding will indirectly access the bound value of the target
        //       binding.
        //       We don't alter the name of the binding as the name is only used
        //       for lookup.
        Binding copy_binding = result->binding();
        copy_binding.mutable_ = false;
        copy_binding.can_be_deleted = false;
        copy_binding.initialized = true;
        return BindingAndIndex { copy_binding };
    }

    return DeclarativeEnvironment::find_binding_and_index(name);
}

void ModuleEnvironment::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& indirect_binding : m_indirect_bindings)
        visitor.visit(indirect_binding.module);
}

}
