/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>

static void install_test_host_defined(JS::Realm& realm, Web::Bindings::WrapperWorld::Type world_type, JS::Realm& principal_realm)
{
    auto intrinsics = realm.create<Web::Bindings::Intrinsics>(realm);
    auto wrapper_world = realm.heap().allocate<Web::Bindings::WrapperWorld>(world_type);
    realm.set_host_defined(make<Web::Bindings::HostDefined>(intrinsics, *wrapper_world, principal_realm));
}

TEST_CASE(extension_world_callback_uses_principal_settings_object)
{
    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& vm = Web::Bindings::main_thread_vm();
    auto extension_execution_context = Web::Bindings::create_a_new_javascript_realm(vm, nullptr, nullptr);
    auto& extension_realm = *extension_execution_context->realm;
    install_test_host_defined(extension_realm, Web::Bindings::WrapperWorld::Type::Extension, *principal_realm);
    vm.push_execution_context(*extension_execution_context);

    auto callback_function = JS::NativeFunction::create(extension_realm, [](JS::VM&) { return JS::js_undefined(); }, 0);
    auto& principal_settings = Web::Bindings::principal_host_defined_environment_settings_object(*principal_realm);
    auto callback = GC::Heap::the().allocate<Web::WebIDL::CallbackType>(*callback_function, extension_realm);
    EXPECT(callback->callback_context.ptr() == &principal_settings);

    auto completion = Web::WebIDL::invoke_callback(*callback, {}, Web::WebIDL::ExceptionBehavior::Rethrow, {});
    EXPECT(!completion.is_abrupt());

    auto* popped_extension_execution_context = vm.pop_execution_context();
    EXPECT(popped_extension_execution_context == extension_execution_context.ptr());
    auto* popped_principal_execution_context = vm.pop_execution_context();
    EXPECT(popped_principal_execution_context == &principal_settings.realm_execution_context());
}

TEST_CASE(extension_world_event_listener_wraps_arguments_in_listener_realm)
{
    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& vm = Web::Bindings::main_thread_vm();
    auto extension_execution_context = Web::Bindings::create_a_new_javascript_realm(vm, nullptr, nullptr);
    auto& extension_realm = *extension_execution_context->realm;
    install_test_host_defined(extension_realm, Web::Bindings::WrapperWorld::Type::Extension, *principal_realm);
    vm.push_execution_context(*extension_execution_context);

    IGNORE_USE_IN_ESCAPING_LAMBDA auto event_target = Web::DOM::EventTarget::create();
    IGNORE_USE_IN_ESCAPING_LAMBDA bool listener_was_called = false;
    auto callback_function = JS::NativeFunction::create(extension_realm, [&](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        listener_was_called = true;

        auto this_value = vm.this_value();
        EXPECT(this_value.is_object());
        EXPECT(&this_value.as_object().shape().realm() == &extension_realm);
        EXPECT(Web::Bindings::impl_from<Web::DOM::EventTarget>(&this_value.as_object()) == event_target.ptr());

        auto event_value = vm.argument(0);
        EXPECT(event_value.is_object());
        EXPECT(&event_value.as_object().shape().realm() == &extension_realm);
        EXPECT(Web::Bindings::impl_from<Web::DOM::Event>(&event_value.as_object()));

        auto current_target = TRY(event_value.as_object().internal_get(JS::PropertyKey { "currentTarget"_utf16_fly_string }, event_value));
        EXPECT(current_target.is_object());
        EXPECT(&current_target.as_object().shape().realm() == &extension_realm);
        EXPECT(Web::Bindings::impl_from<Web::DOM::EventTarget>(&current_target.as_object()) == event_target.ptr());

        return JS::js_undefined(); }, 1);
    auto& principal_settings = Web::Bindings::principal_host_defined_environment_settings_object(*principal_realm);
    auto callback = GC::Heap::the().allocate<Web::WebIDL::CallbackType>(*callback_function, extension_realm);
    EXPECT(callback->callback_context.ptr() == &principal_settings);
    auto listener = Web::DOM::IDLEventListener::create(*callback);
    event_target->add_event_listener_without_options("world-callback-test"_utf16_fly_string, *listener);

    auto event = Web::DOM::Event::create("world-callback-test"_fly_string, 0);
    EXPECT(event_target->dispatch_event(*event));
    EXPECT(listener_was_called);

    auto* popped_extension_execution_context = vm.pop_execution_context();
    EXPECT(popped_extension_execution_context == extension_execution_context.ptr());
    auto* popped_principal_execution_context = vm.pop_execution_context();
    EXPECT(popped_principal_execution_context == &principal_settings.realm_execution_context());
}
