/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibCore/EventLoop.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWebView/Debugger.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/DevToolsDebugger.h>
#include <WebContent/PageClient.h>
#include <WebContent/PageHost.h>

namespace WebContent {

DevToolsDebugger::DevToolsDebugger(ConnectionFromClient& client)
    : m_client(client)
{
}

DevToolsDebugger::~DevToolsDebugger()
{
    auto& vm = Web::Bindings::main_thread_vm();
    if (auto* debugger = vm.debugger(); debugger && !debugger->is_paused())
        vm.disable_debugging();
}

void DevToolsDebugger::attach(PageClient& page)
{
    if (m_attached_page_ids.set(page.id()) != AK::HashSetResult::InsertedNewEntry)
        return;

    auto& vm = Web::Bindings::main_thread_vm();
    vm.enable_debugging();
    if (!m_pause_callback_is_installed) {
        vm.debugger()->set_pause_callback([this](auto const& pause) {
            handle_pause(pause);
        });
        m_pause_callback_is_installed = true;
    }
}

void DevToolsDebugger::configure(PageClient& page, WebView::DebuggerConfiguration configuration)
{
    m_configurations.set(page.id(), configuration);
}

void DevToolsDebugger::detach(PageClient& page)
{
    m_attached_page_ids.remove(page.id());
    m_configurations.remove(page.id());
    remove_breakpoints_for_page(page.id());
    if (m_paused_page_id == page.id())
        m_resume_requested = true;

    disable_if_unused();
}

void DevToolsDebugger::interrupt(PageClient& page)
{
    if (!m_attached_page_ids.contains(page.id()))
        return;

    auto& vm = Web::Bindings::main_thread_vm();
    if (auto* debugger = vm.debugger())
        debugger->request_pause_on_next_bytecode_execution();
}

ErrorOr<void> DevToolsDebugger::set_breakpoint(PageClient& page, WebView::DebuggerBreakpointLocation location)
{
    if (auto registrations = m_breakpoints.find(page.id()); registrations != m_breakpoints.end()) {
        if (registrations->value.find_if([&](auto const& registration) { return registration.location.represents_same_breakpoint_as(location); }) != registrations->value.end())
            return {};
    }

    auto column = location.column;
    if (column.has_value() && *column == NumericLimits<u32>::max())
        return Error::from_string_literal("Breakpoint column is too large");
    if (column.has_value())
        ++*column;

    auto& vm = Web::Bindings::main_thread_vm();
    vm.enable_debugging();
    ErrorOr<JS::BreakpointID> id = [&]() -> ErrorOr<JS::BreakpointID> {
        if (!location.source_id.has_value())
            return vm.debugger()->add_breakpoint(location.filename, location.line, column);
        auto source_code = page.devtools_source_code(*location.source_id);
        if (!source_code.has_value())
            return Error::from_string_literal("Unable to locate debugger source");
        return vm.debugger()->add_breakpoint(source_code.release_value(), location.line, column);
    }();
    if (id.is_error()) {
        disable_if_unused();
        return id.release_error();
    }
    m_breakpoints.ensure(page.id()).append({ move(location), id.release_value() });
    return {};
}

ErrorOr<void> DevToolsDebugger::remove_breakpoint(PageClient& page, WebView::DebuggerBreakpointLocation const& location)
{
    auto registrations = m_breakpoints.find(page.id());
    if (registrations == m_breakpoints.end())
        return Error::from_string_literal("Unable to locate debugger breakpoint");

    auto registration = registrations->value.find_if([&](auto const& candidate) {
        return candidate.location.represents_same_breakpoint_as(location);
    });
    if (registration == registrations->value.end())
        return Error::from_string_literal("Unable to locate debugger breakpoint");

    auto breakpoint_id = registration->id;
    registrations->value.remove(registration.index());
    if (registrations->value.is_empty())
        m_breakpoints.remove(registrations);

    bool breakpoint_is_still_used = false;
    for (auto const& page_breakpoints : m_breakpoints) {
        if (page_breakpoints.value.find_if([&](auto const& registration) { return registration.id == breakpoint_id; }) != page_breakpoints.value.end()) {
            breakpoint_is_still_used = true;
            break;
        }
    }

    auto& vm = Web::Bindings::main_thread_vm();
    if (!breakpoint_is_still_used && vm.debugger())
        vm.debugger()->remove_breakpoint(breakpoint_id);
    disable_if_unused();
    return {};
}

WebView::DebuggerValue DevToolsDebugger::serialize_value(JS::Value value)
{
    WebView::DebuggerValue result;
    if (value.is_special_empty_value()) {
        result.type = WebView::DebuggerValueType::Uninitialized;
        return result;
    }
    if (value.is_undefined())
        return result;
    if (value.is_null()) {
        result.type = WebView::DebuggerValueType::Null;
        return result;
    }
    if (value.is_boolean()) {
        result.type = WebView::DebuggerValueType::Boolean;
        result.boolean_value = value.as_bool();
        return result;
    }
    if (value.is_number()) {
        result.type = WebView::DebuggerValueType::Number;
        result.number_value = value.as_double();
        return result;
    }
    if (value.is_string()) {
        result.type = WebView::DebuggerValueType::String;
        result.text = value.as_string().utf16_string();
        return result;
    }
    if (value.is_bigint()) {
        result.type = WebView::DebuggerValueType::BigInt;
        result.text = value.as_bigint().to_utf16_string();
        return result;
    }
    if (value.is_symbol()) {
        // FIXME: Represent symbols with dedicated SymbolActors instead of converting them to strings.
        result.type = WebView::DebuggerValueType::String;
        result.text = value.to_utf16_string_without_side_effects();
        return result;
    }

    VERIFY(value.is_object());
    auto& object = value.as_object();
    Optional<u64> object_id;
    if (auto existing_id = m_paused_object_ids.get(&object); existing_id.has_value())
        object_id = *existing_id;
    if (!object_id.has_value()) {
        object_id = m_next_object_id++;
        m_paused_object_ids.set(&object, *object_id);
        m_paused_objects.set(*object_id, GC::make_root(object));
    }
    result.type = WebView::DebuggerValueType::Object;
    result.object_id = *object_id;
    result.object_class = object.is_function() ? "Function"_string : MUST(String::from_utf8(object.class_name()));
    return result;
}

Vector<WebView::DebuggerEnvironment> DevToolsDebugger::environments_for_frame(PageClient& page, u64 frame_id)
{
    if (m_paused_page_id != page.id())
        return {};

    auto* context = m_paused_frames.get(frame_id).value_or(nullptr);
    if (!context)
        return {};

    Vector<WebView::DebuggerEnvironment> environments;
    auto append_declarative_environment = [&](JS::DeclarativeEnvironment& environment, WebView::DebuggerEnvironmentType type, Optional<Utf16String> function_name = {}) {
        Vector<WebView::DebuggerBinding> bindings;
        for (auto const& name : environment.bindings()) {
            auto value = environment.get_binding_value(Web::Bindings::main_thread_vm(), name, false);
            auto writable = environment.binding_is_mutable_by_name(name);
            WebView::DebuggerValue debugger_value;
            if (!value.is_error())
                debugger_value = serialize_value(value.release_value());
            else
                debugger_value.type = WebView::DebuggerValueType::Uninitialized;
            bindings.append({
                .name = name.to_utf16_string(),
                .value = move(debugger_value),
                .writable = writable,
            });
        }
        WebView::DebuggerEnvironment debugger_environment;
        debugger_environment.id = m_next_environment_id++;
        debugger_environment.type = type;
        debugger_environment.function_name = move(function_name);
        debugger_environment.bindings = move(bindings);
        environments.append(move(debugger_environment));
    };

    for (auto* environment = context->lexical_environment.ptr(); environment; environment = environment->outer_environment()) {
        if (is<JS::GlobalEnvironment>(*environment)) {
            auto& global_environment = static_cast<JS::GlobalEnvironment&>(*environment);
            append_declarative_environment(global_environment.declarative_record(), WebView::DebuggerEnvironmentType::Block);
            WebView::DebuggerEnvironment debugger_environment;
            debugger_environment.id = m_next_environment_id++;
            debugger_environment.type = WebView::DebuggerEnvironmentType::Object;
            debugger_environment.object = serialize_value(&global_environment.global_this_value());
            environments.append(move(debugger_environment));
            continue;
        }

        if (is<JS::DeclarativeEnvironment>(*environment)) {
            auto type = environment->is_function_environment() ? WebView::DebuggerEnvironmentType::Function : WebView::DebuggerEnvironmentType::Block;
            Optional<Utf16String> function_name;
            if (environment->is_function_environment())
                function_name = static_cast<JS::FunctionEnvironment&>(*environment).function_object().name_for_call_stack();
            append_declarative_environment(static_cast<JS::DeclarativeEnvironment&>(*environment), type, move(function_name));
            continue;
        }

        if (is<JS::ObjectEnvironment>(*environment)) {
            WebView::DebuggerEnvironment debugger_environment;
            debugger_environment.id = m_next_environment_id++;
            debugger_environment.type = WebView::DebuggerEnvironmentType::Object;
            debugger_environment.object = serialize_value(&static_cast<JS::ObjectEnvironment&>(*environment).binding_object());
            environments.append(move(debugger_environment));
        }
    }

    for (size_t index = 0; index + 1 < environments.size(); ++index)
        environments[index].parent_id = environments[index + 1].id;
    return environments;
}

ErrorOr<WebView::DebuggerObjectProperties> DevToolsDebugger::properties_for_object(PageClient& page, u64 object_id)
{
    if (m_paused_page_id != page.id())
        return Error::from_string_literal("Debugger is not paused for this page");

    auto object_iterator = m_paused_objects.find(object_id);
    if (object_iterator == m_paused_objects.end())
        return Error::from_string_literal("Unable to locate paused object");
    // serialize_value() may add entries to m_paused_objects, so retain a root
    // independent of the map's storage.
    auto object = object_iterator->value;

    WebView::DebuggerObjectProperties result;
    auto prototype = object->internal_get_prototype_of();
    if (prototype.is_error())
        return Error::from_string_literal("Unable to inspect object prototype");
    if (auto* prototype_object = prototype.release_value()) {
        result.prototype = serialize_value(prototype_object);
    } else {
        WebView::DebuggerValue null_prototype;
        null_prototype.type = WebView::DebuggerValueType::Null;
        result.prototype = move(null_prototype);
    }

    auto own_property_keys = object->internal_own_property_keys();
    if (own_property_keys.is_error())
        return Error::from_string_literal("Unable to inspect object properties");

    for (auto const& key_value : own_property_keys.release_value()) {
        // FIXME: Expose symbol properties through a symbol iterator actor.
        if (!key_value.is_string())
            continue;

        auto name = key_value.as_string().utf16_string();
        auto descriptor = object->internal_get_own_property(JS::PropertyKey { name });
        if (descriptor.is_error())
            return Error::from_string_literal("Unable to inspect object property");
        if (!descriptor.value().has_value())
            continue;

        auto const& property_descriptor = descriptor.value().value();
        WebView::DebuggerProperty property;
        property.name = move(name);
        property.writable = property_descriptor.writable.value_or(false);
        property.enumerable = property_descriptor.enumerable.value_or(false);
        property.configurable = property_descriptor.configurable.value_or(false);
        if (property_descriptor.value.has_value()) {
            property.value = serialize_value(*property_descriptor.value);
        } else {
            if (property_descriptor.get.has_value())
                property.getter = serialize_value(property_descriptor.get->ptr() ? JS::Value { property_descriptor.get->ptr() } : JS::js_undefined());
            if (property_descriptor.set.has_value())
                property.setter = serialize_value(property_descriptor.set->ptr() ? JS::Value { property_descriptor.set->ptr() } : JS::js_undefined());
        }
        result.properties.append(move(property));
    }

    return result;
}

void DevToolsDebugger::resume(PageClient& page)
{
    if (m_paused_page_id == page.id())
        m_resume_requested = true;
}

PageClient* DevToolsDebugger::paused_page_client() const
{
    auto& vm = Web::Bindings::main_thread_vm();
    if (!vm.current_realm())
        return nullptr;

    auto* window = Web::Bindings::window_from_global_object(vm.current_realm()->global_object());
    if (!window)
        return nullptr;

    for (auto page_id : m_attached_page_ids) {
        auto page = m_client.page_host().page(page_id);
        if (page.has_value() && &page->page() == &window->page())
            return &*page;
    }
    return nullptr;
}

void DevToolsDebugger::handle_pause(JS::Debugger::PauseInfo const& pause)
{
    auto* page = paused_page_client();
    if (!page) {
        Web::Bindings::main_thread_vm().debugger()->continue_execution();
        return;
    }

    auto configuration = m_configurations.get(page->id()).value_or(WebView::DebuggerConfiguration {});
    if ((pause.reason == JS::Debugger::PauseReason::DebuggerStatement && !configuration.should_pause_on_debugger_statement)
        || (pause.reason == JS::Debugger::PauseReason::Breakpoint && configuration.skip_breakpoints)) {
        Web::Bindings::main_thread_vm().debugger()->continue_execution();
        return;
    }

    WebView::DebuggerPause debugger_pause {
        .reason = [&] {
            switch (pause.reason) {
            case JS::Debugger::PauseReason::Breakpoint:
                return WebView::DebuggerPauseReason::Breakpoint;
            case JS::Debugger::PauseReason::DebuggerStatement:
                return WebView::DebuggerPauseReason::DebuggerStatement;
            case JS::Debugger::PauseReason::Entry:
                return WebView::DebuggerPauseReason::Entry;
            }
            VERIFY_NOT_REACHED();
        }(),
        .frames = {},
    };

    m_paused_frames.clear();
    m_paused_objects.clear();
    m_paused_object_ids.clear();

    for (auto const& stack_frame : pause.stack_trace) {
        auto* executable = stack_frame.execution_context->executable.ptr();
        if (!executable)
            continue;

        auto source = page->devtools_source_description(*executable->source_code);
        if (!source.has_value())
            continue;

        auto line = source->source_start_line;
        auto column = source->source_start_column;
        if (stack_frame.source_range.has_value()) {
            line = stack_frame.source_range->start.line;
            column = stack_frame.source_range->start.column > 0 ? stack_frame.source_range->start.column - 1 : 0;
        }

        auto frame_id = m_next_frame_id++;
        m_paused_frames.set(frame_id, stack_frame.execution_context);
        Vector<WebView::DebuggerValue> arguments;
        for (auto argument : stack_frame.execution_context->arguments_span().slice(0, stack_frame.execution_context->passed_argument_count))
            arguments.append(serialize_value(argument));
        debugger_pause.frames.append(WebView::DebuggerFrame {
            .id = frame_id,
            .display_name = executable->name.to_utf16_string(),
            .location = {
                .source = source.release_value(),
                .line = line,
                .column = column,
            },
            .this_value = serialize_value(stack_frame.execution_context->this_value.value_or(JS::js_undefined())),
            .arguments = move(arguments),
        });
    }

    if (debugger_pause.frames.is_empty()) {
        m_paused_frames.clear();
        m_paused_objects.clear();
        m_paused_object_ids.clear();
        Web::Bindings::main_thread_vm().debugger()->continue_execution();
        return;
    }

    m_paused_page_id = page->id();
    m_resume_requested = false;
    m_is_handling_pause = true;

    auto event_loop_pause = Web::HTML::main_thread_event_loop().pause();
    m_client.async_did_pause_debugger(page->id(), move(debugger_pause));
    Core::EventLoop::current().spin_until([this] {
        return m_resume_requested;
    });

    m_is_handling_pause = false;
    m_paused_page_id = {};
    m_paused_frames.clear();
    m_paused_objects.clear();
    m_paused_object_ids.clear();
    Web::Bindings::main_thread_vm().debugger()->continue_execution();
    schedule_disable_if_unused();
}

void DevToolsDebugger::disable_if_unused()
{
    if (!m_attached_page_ids.is_empty() || !m_breakpoints.is_empty() || m_is_handling_pause)
        return;

    auto& vm = Web::Bindings::main_thread_vm();
    if (vm.debugging_enabled()) {
        vm.disable_debugging();
        m_pause_callback_is_installed = false;
    }
}

void DevToolsDebugger::schedule_disable_if_unused()
{
    Core::deferred_invoke([weak_this = make_weak_ptr<DevToolsDebugger>()] {
        if (weak_this)
            weak_this->disable_if_unused();
    });
}

void DevToolsDebugger::remove_breakpoints_for_page(u64 page_id)
{
    auto registrations = m_breakpoints.take(page_id);
    if (!registrations.has_value())
        return;

    auto& vm = Web::Bindings::main_thread_vm();
    for (auto const& registration : *registrations) {
        bool breakpoint_is_still_used = false;
        for (auto const& page_breakpoints : m_breakpoints) {
            if (page_breakpoints.value.find_if([&](auto const& candidate) { return candidate.id == registration.id; }) != page_breakpoints.value.end()) {
                breakpoint_is_still_used = true;
                break;
            }
        }
        if (!breakpoint_is_still_used && vm.debugger())
            vm.debugger()->remove_breakpoint(registration.id);
    }
}

}
