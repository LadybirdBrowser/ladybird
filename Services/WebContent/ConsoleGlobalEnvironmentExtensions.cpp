/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConsoleGlobalEnvironmentExtensions.h"
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/HTML/Window.h>

namespace WebContent {

GC_DEFINE_ALLOCATOR(ConsoleGlobalEnvironmentExtensions);

ConsoleGlobalEnvironmentExtensions::ConsoleGlobalEnvironmentExtensions(JS::Realm& realm, Web::HTML::Window& window)
    : Object(realm, nullptr)
    , m_window_object(window)
{
}

void ConsoleGlobalEnvironmentExtensions::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_native_accessor(realm, "$0"_utf16_fly_string, $0_getter, nullptr, 0);
    define_native_accessor(realm, "$_"_utf16_fly_string, $__getter, nullptr, 0);
    define_native_function(realm, "$"_utf16_fly_string, $_function, 2, JS::default_attributes);
    define_native_function(realm, "$$"_utf16_fly_string, $$_function, 2, JS::default_attributes);
}

void ConsoleGlobalEnvironmentExtensions::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window_object);
    visitor.visit(m_most_recent_result);
}

static JS::ThrowCompletionOr<ConsoleGlobalEnvironmentExtensions*> get_console(JS::VM& vm)
{
    if (auto console = vm.this_value().as_if<ConsoleGlobalEnvironmentExtensions>())
        return console.ptr();
    return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "ConsoleGlobalEnvironmentExtensions");
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$0_getter)
{
    auto* console_global_object = TRY(get_console(vm));
    auto& window = *console_global_object->m_window_object;
    auto inspected_node = window.associated_document().inspected_node();
    if (!inspected_node)
        return JS::js_undefined();

    return Web::Bindings::wrap(*vm.current_realm(), GC::Ref { const_cast<Web::DOM::Node&>(*inspected_node) });
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$__getter)
{
    auto* console_global_object = TRY(get_console(vm));
    return console_global_object->m_most_recent_result;
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$_function)
{
    auto* console_global_object = TRY(get_console(vm));
    auto& window = *console_global_object->m_window_object;

    auto selector = TRY(vm.argument(0).to_string(vm));

    if (vm.argument_count() > 1) {
        auto node = vm.argument(1).is_object() ? Web::Bindings::impl_from<Web::DOM::ParentNode>(&vm.argument(1).as_object()) : nullptr;
        if (!node)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Node");

        auto element = TRY(Web::Bindings::throw_dom_exception_if_needed(vm, [&]() {
            return node->query_selector(selector);
        }));
        if (!element)
            return JS::js_null();
        return Web::Bindings::wrap(*vm.current_realm(), element);
    }

    auto element = TRY(Web::Bindings::throw_dom_exception_if_needed(vm, [&]() {
        return window.associated_document().query_selector(selector);
    }));
    if (!element)
        return JS::js_null();
    return Web::Bindings::wrap(*vm.current_realm(), element);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$$_function)
{
    auto* console_global_object = TRY(get_console(vm));
    auto& window = *console_global_object->m_window_object;

    auto selector = TRY(vm.argument(0).to_string(vm));

    Web::DOM::ParentNode* element = &window.associated_document();

    if (vm.argument_count() > 1) {
        auto node = vm.argument(1).is_object() ? Web::Bindings::impl_from<Web::DOM::ParentNode>(&vm.argument(1).as_object()) : nullptr;
        if (!node)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Node");
        element = node;
    }

    auto node_list = TRY(Web::Bindings::throw_dom_exception_if_needed(vm, [&]() {
        return element->query_selector_all(selector);
    }));

    auto array = TRY(JS::Array::create(*vm.current_realm(), node_list->length()));
    for (auto i = 0u; i < node_list->length(); ++i) {
        auto* node = node_list->item(i);
        VERIFY(node);
        auto wrapped_node = Web::Bindings::wrap(*vm.current_realm(), GC::Ref { const_cast<Web::DOM::Node&>(*node) });
        TRY(array->create_data_property_or_throw(i, wrapped_node));
    }

    return array;
}

}
