/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Root.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>

static JS::Realm& realm_of_thrown_type_error(JS::VM& vm)
{
    auto completion = vm.throw_completion<JS::TypeError>();
    return completion.value().as_object().shape().realm();
}

TEST_CASE(type_error_realm_scope_overrides_only_at_its_own_depth)
{
    auto vm = JS::VM::create();

    auto override_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto override_realm = GC::make_root(*override_context->realm);
    vm->pop_execution_context();

    auto current_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& current_realm = *current_context->realm;

    // Without a scope, TypeErrors are created in the current realm.
    EXPECT(&realm_of_thrown_type_error(*vm) == &current_realm);

    {
        JS::VM::TypeErrorRealmScope scope { *vm, *override_realm };
        EXPECT(&realm_of_thrown_type_error(*vm) == override_realm.ptr());

        // A callee pushed while the scope is alive must not see the override.
        auto nested_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
        auto& nested_realm = *nested_context->realm;
        EXPECT(&realm_of_thrown_type_error(*vm) == &nested_realm);
        vm->pop_execution_context();

        // Back at the scope's depth, the override applies again.
        EXPECT(&realm_of_thrown_type_error(*vm) == override_realm.ptr());
    }

    // Once the scope is gone, TypeErrors come from the current realm again.
    EXPECT(&realm_of_thrown_type_error(*vm) == &current_realm);

    vm->pop_execution_context();
}

TEST_CASE(type_error_realm_scopes_nest)
{
    auto vm = JS::VM::create();

    auto outer_override_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto outer_override_realm = GC::make_root(*outer_override_context->realm);
    vm->pop_execution_context();

    auto inner_override_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto inner_override_realm = GC::make_root(*inner_override_context->realm);
    vm->pop_execution_context();

    auto current_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& current_realm = *current_context->realm;

    {
        JS::VM::TypeErrorRealmScope outer_scope { *vm, *outer_override_realm };
        {
            JS::VM::TypeErrorRealmScope inner_scope { *vm, *inner_override_realm };
            EXPECT(&realm_of_thrown_type_error(*vm) == inner_override_realm.ptr());
        }
        EXPECT(&realm_of_thrown_type_error(*vm) == outer_override_realm.ptr());
    }
    EXPECT(&realm_of_thrown_type_error(*vm) == &current_realm);

    vm->pop_execution_context();
}
