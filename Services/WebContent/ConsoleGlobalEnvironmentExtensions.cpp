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
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/NodeList.h>
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

    define_native_accessor(realm, "$0"_fly_string, $0_getter, nullptr, 0);
    define_native_accessor(realm, "$_"_fly_string, $__getter, nullptr, 0);
    define_native_function(realm, "$"_fly_string, $_function, 2, JS::default_attributes);
    define_native_function(realm, "$$"_fly_string, $$_function, 2, JS::default_attributes);
}

void ConsoleGlobalEnvironmentExtensions::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window_object);
    visitor.visit(m_most_recent_result);
}

static JS::ThrowCompletionOr<ConsoleGlobalEnvironmentExtensions*> get_console(JS::VM& vm)
{
    auto this_value = vm.this_value();
    if (!this_value.is_object() || !is<ConsoleGlobalEnvironmentExtensions>(this_value.as_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "ConsoleGlobalEnvironmentExtensions");

    return &static_cast<ConsoleGlobalEnvironmentExtensions&>(this_value.as_object());
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$0_getter)
{
    auto* console_global_object = TRY(get_console(vm));
    auto& window = *console_global_object->m_window_object;
    auto inspected_node = window.associated_document().inspected_node();
    if (!inspected_node)
        return JS::js_undefined();

    return inspected_node;
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
        auto element_value = vm.argument(1);
        if (!(element_value.is_object() && is<Web::DOM::ParentNode>(element_value.as_object()))) {
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Node");
        }

        auto& element = static_cast<Web::DOM::ParentNode&>(element_value.as_object());
        return TRY(Web::Bindings::throw_dom_exception_if_needed(vm, [&]() {
            return element.query_selector(selector);
        }));
    }

    return TRY(Web::Bindings::throw_dom_exception_if_needed(vm, [&]() {
        return window.associated_document().query_selector(selector);
    }));
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$$_function)
{
    auto* console_global_object = TRY(get_console(vm));
    auto& window = *console_global_object->m_window_object;

    auto selector = TRY(vm.argument(0).to_string(vm));

    Web::DOM::ParentNode* element = &window.associated_document();

    if (vm.argument_count() > 1) {
        auto element_value = vm.argument(1);
        if (!(element_value.is_object() && is<Web::DOM::ParentNode>(element_value.as_object()))) {
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Node");
        }

        element = static_cast<Web::DOM::ParentNode*>(&element_value.as_object());
    }

    auto node_list = TRY(Web::Bindings::throw_dom_exception_if_needed(vm, [&]() {
        return element->query_selector_all(selector);
    }));

    auto array = TRY(JS::Array::create(*vm.current_realm(), node_list->length()));
    for (auto i = 0u; i < node_list->length(); ++i) {
        TRY(array->create_data_property_or_throw(i, *node_list->item_value(i)));
    }

    return array;
}

}
