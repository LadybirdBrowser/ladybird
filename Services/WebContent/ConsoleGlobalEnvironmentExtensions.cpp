/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConsoleGlobalEnvironmentExtensions.h"
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace WebContent {

GC_DEFINE_ALLOCATOR(ConsoleGlobalEnvironmentExtensions);

static constexpr JSHostClass console_global_environment_extensions_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "ConsoleGlobalEnvironmentExtensions"sv, nullptr, nullptr, nullptr, 0);

GC::Ref<ConsoleGlobalEnvironmentExtensions> ConsoleGlobalEnvironmentExtensions::create(JS::Realm& realm, Web::HTML::Window& window)
{
    auto extensions = realm.create<ConsoleGlobalEnvironmentExtensions>(window);

    auto binding_object = JS::HostObject::create(realm, console_global_environment_extensions_host_class, nullptr, nullptr, extensions);
    binding_object->define_native_accessor(realm, "$0"_utf16_fly_string, $0_getter, nullptr, 0);
    binding_object->define_native_accessor(realm, "$_"_utf16_fly_string, $__getter, nullptr, 0);
    binding_object->define_native_function(realm, "$"_utf16_fly_string, $_function, 2, JS::default_attributes);
    binding_object->define_native_function(realm, "$$"_utf16_fly_string, $$_function, 2, JS::default_attributes);
    extensions->m_binding_object = binding_object;

    return extensions;
}

ConsoleGlobalEnvironmentExtensions::ConsoleGlobalEnvironmentExtensions(Web::HTML::Window& window)
    : m_window_object(window)
{
}

void ConsoleGlobalEnvironmentExtensions::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window_object);
    visitor.visit(m_binding_object);
    visitor.visit(m_most_recent_result);
}

static JS::ThrowCompletionOr<ConsoleGlobalEnvironmentExtensions*> get_console(JS::VM& vm)
{
    if (auto this_value = vm.this_value(); this_value.is_object()) {
        if (auto* extensions = JS::host_data_if<ConsoleGlobalEnvironmentExtensions>(this_value.as_object()))
            return extensions;
    }
    return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "ConsoleGlobalEnvironmentExtensions");
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$0_getter)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto& window = *extensions->m_window_object;
    auto inspected_node = window.associated_document().inspected_node();
    if (!inspected_node)
        return JS::js_undefined();

    return Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { const_cast<Web::DOM::Node&>(*inspected_node) });
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$__getter)
{
    auto* extensions = TRY(get_console(vm));
    return extensions->m_most_recent_result;
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto& window = *extensions->m_window_object;

    auto selector = TRY(vm.argument(0).to_utf16_string(vm));

    if (vm.argument_count() > 1) {
        auto node = vm.argument(1).is_object() ? Web::Bindings::impl_from<Web::DOM::ParentNode>(&vm.argument(1).as_object()) : nullptr;
        if (!node)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Node");

        auto element = TRY(Web::WebIDL::throw_dom_exception_if_needed(vm, realm, [&]() {
            return node->query_selector(selector);
        }));
        if (!element)
            return JS::js_null();
        return Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, element);
    }

    auto element = TRY(Web::WebIDL::throw_dom_exception_if_needed(vm, realm, [&]() {
        return window.associated_document().query_selector(selector);
    }));
    if (!element)
        return JS::js_null();
    return Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, element);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$$_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto& window = *extensions->m_window_object;

    auto selector = TRY(vm.argument(0).to_utf16_string(vm));

    Web::DOM::ParentNode* element = &window.associated_document();

    if (vm.argument_count() > 1) {
        auto node = vm.argument(1).is_object() ? Web::Bindings::impl_from<Web::DOM::ParentNode>(&vm.argument(1).as_object()) : nullptr;
        if (!node)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Node");
        element = node;
    }

    auto node_list = TRY(Web::WebIDL::throw_dom_exception_if_needed(vm, realm, [&]() {
        return element->query_selector_all(selector);
    }));

    auto array = TRY(JS::Array::create(realm, node_list->length()));
    for (auto i = 0u; i < node_list->length(); ++i) {
        auto* node = node_list->item(i);
        VERIFY(node);
        auto wrapped_node = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { const_cast<Web::DOM::Node&>(*node) });
        TRY(array->create_data_property_or_throw(i, wrapped_node));
    }

    return array;
}

}
