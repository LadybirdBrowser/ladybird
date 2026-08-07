/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/NumericLimits.h>
#include <LibCore/EventLoop.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWebView/Debugger.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/DevToolsConsoleClient.h>
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
    update_exception_pause_mode();
}

void DevToolsDebugger::configure(PageClient& page, WebView::DebuggerConfiguration configuration)
{
    m_configurations.set(page.id(), configuration);
    update_exception_pause_mode();
}

void DevToolsDebugger::detach(PageClient& page)
{
    m_attached_page_ids.remove(page.id());
    m_configurations.remove(page.id());
    update_exception_pause_mode();
    remove_breakpoints_for_page(page.id());
    m_blackboxed_sources.remove(page.id());
    if (m_paused_page_id == page.id()) {
        m_resume_mode = WebView::DebuggerResumeMode::Continue;
        m_resume_requested = true;
    }

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

void DevToolsDebugger::update_blackboxing(PageClient& page, Utf16String url, Vector<WebView::DebuggerBlackboxRange> ranges, WebView::DebuggerBlackboxingOperation operation)
{
    auto page_sources = m_blackboxed_sources.find(page.id());
    Optional<size_t> source_index;
    if (page_sources != m_blackboxed_sources.end()) {
        for (auto const& [index, candidate] : enumerate(page_sources->value)) {
            if (candidate.url == url) {
                source_index = index;
                break;
            }
        }
    }

    if (!source_index.has_value()) {
        if (operation == WebView::DebuggerBlackboxingOperation::Unblackbox)
            return;
        auto& sources = m_blackboxed_sources.ensure(page.id());
        source_index = sources.size();
        sources.append({ move(url), {} });
        page_sources = m_blackboxed_sources.find(page.id());
    }

    auto& source = page_sources->value[*source_index];
    source.state.update(ranges, operation);
    if (source.state.is_empty())
        page_sources->value.remove(*source_index);

    if (page_sources->value.is_empty())
        m_blackboxed_sources.remove(page_sources);
}

ErrorOr<void> DevToolsDebugger::set_breakpoint(PageClient& page, WebView::DebuggerBreakpointLocation location, WebView::DebuggerBreakpointOptions options)
{
    if (auto registrations = m_breakpoints.find(page.id()); registrations != m_breakpoints.end()) {
        if (auto registration = registrations->value.find_if([&](auto const& registration) { return registration.location.represents_same_breakpoint_as(location); }); registration != registrations->value.end()) {
            registration->options = move(options);
            return {};
        }
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
    m_breakpoints.ensure(page.id()).append({ move(location), move(options), id.release_value() });
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

    if (context->executable) {
        WebView::DebuggerEnvironment local_environment;
        local_environment.id = m_next_environment_id++;
        local_environment.type = WebView::DebuggerEnvironmentType::Function;
        local_environment.function_name = context->executable->name.to_utf16_string();

        for (auto const& binding : Web::Bindings::main_thread_vm().debugger()->bindings_for_frame(*context)) {
            local_environment.bindings.append({
                .name = binding.name.to_utf16_string(),
                .value = serialize_value(binding.value),
                .writable = binding.is_mutable,
            });
        }
        if (!local_environment.bindings.is_empty())
            environments.append(move(local_environment));
    }

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

ErrorOr<WebView::DebuggerEvaluationResult> DevToolsDebugger::evaluate_in_frame(PageClient& page, u64 frame_id, Utf16View source_text)
{
    if (m_paused_page_id != page.id())
        return Error::from_string_literal("Debugger is not paused for this page");

    auto* context = m_paused_frames.get(frame_id).value_or(nullptr);
    if (!context)
        return Error::from_string_literal("Unable to locate paused frame");

    auto& vm = Web::Bindings::main_thread_vm();
    VERIFY(vm.debugger());
    auto completion = vm.debugger()->evaluate_in_frame(*context, source_text);
    if (completion.is_throw_completion()) {
        return WebView::DebuggerEvaluationResult {
            .value = serialize_value(completion.throw_completion().value()),
            .is_throw = true,
        };
    }
    return WebView::DebuggerEvaluationResult {
        .value = serialize_value(completion.release_value()),
    };
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

void DevToolsDebugger::resume(PageClient& page, WebView::DebuggerResumeMode mode)
{
    if (m_paused_page_id != page.id())
        return;

    m_resume_mode = mode;
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

static Optional<WebView::StackFrame> console_stack_frame(PageClient& page, JS::StackTraceElement const& stack_frame)
{
    auto* executable = stack_frame.execution_context->executable.ptr();
    if (!executable)
        return {};

    auto source = page.devtools_source_description(*executable->source_code);
    if (!source.has_value())
        return {};

    auto line = source->source_start_line;
    auto column = source->source_start_column + 1;
    if (stack_frame.source_range.has_value()) {
        line = stack_frame.source_range->start.line;
        column = stack_frame.source_range->start.column;
    }

    return WebView::StackFrame {
        .function = executable->name.to_utf16_string().to_utf8(),
        .file = source->display_url.to_utf8(),
        .line = line,
        .column = column,
    };
}

void DevToolsDebugger::emit_logpoint(PageClient& page, JS::Debugger::PauseInfo const& pause, JS::ExecutionContext& context, WebView::DebuggerBreakpointOptions const& options)
{
    VERIFY(options.log_value.has_value());
    auto expression = Utf16String::formatted("[{}]", *options.log_value);
    auto result = Web::Bindings::main_thread_vm().debugger()->evaluate_in_frame(context, expression);

    WebView::ConsoleLogType type = WebView::ConsoleLogType::LogPoint;
    Vector<JsonValue> arguments;
    auto append_error = [&](String message) {
        type = WebView::ConsoleLogType::LogPointError;
        arguments.clear();
        arguments.append(move(message));
    };

    if (result.is_throw_completion()) {
        append_error(result.throw_completion().value().to_utf16_string_without_side_effects().to_utf8());
    } else {
        auto value = result.release_value();
        if (!value.is_object()) {
            append_error("Logpoint expression did not produce an argument list"_string);
        } else {
            auto& array = value.as_object();
            auto length = JS::length_of_array_like(Web::Bindings::main_thread_vm(), array);
            if (length.is_throw_completion()) {
                append_error(length.throw_completion().value().to_utf16_string_without_side_effects().to_utf8());
            } else {
                static constexpr u64 maximum_logpoint_argument_count = 1000;
                auto argument_count = min(length.value(), maximum_logpoint_argument_count);
                arguments.ensure_capacity(argument_count);
                for (u64 i = 0; i < argument_count; ++i) {
                    auto argument = array.get(JS::PropertyKey { i });
                    if (argument.is_throw_completion()) {
                        append_error(argument.throw_completion().value().to_utf16_string_without_side_effects().to_utf8());
                        break;
                    }
                    arguments.unchecked_append(DevToolsConsoleClient::serialize_value(*context.realm, argument.release_value()));
                }
            }
        }
    }

    Optional<Vector<WebView::StackFrame>> stacktrace;
    if (options.show_stacktrace) {
        stacktrace = Vector<WebView::StackFrame> {};
        for (auto const& stack_frame : pause.stack_trace) {
            if (auto frame = console_stack_frame(page, stack_frame); frame.has_value())
                stacktrace->append(frame.release_value());
        }
    }

    page.did_output_js_console_message({
        .timestamp = UnixDateTime::now(),
        .output = WebView::ConsoleLog {
            .level = type == WebView::ConsoleLogType::LogPoint ? JS::Console::LogLevel::Log : JS::Console::LogLevel::Error,
            .arguments = move(arguments),
            .type = type,
            .location = console_stack_frame(page, pause.stack_trace.first()),
            .stacktrace = move(stacktrace),
        },
    });
}

static Utf16String debugger_source_url(Web::HTML::ScriptRegistry::Description const& source)
{
    if (source.url.has_value())
        return Utf16String::from_utf8(source.url->serialize());
    return source.display_url;
}

static bool debugger_position_is_before(WebView::DebuggerSourcePosition const& left, WebView::DebuggerSourcePosition const& right)
{
    return left.line < right.line || (left.line == right.line && left.column < right.column);
}

bool DevToolsDebugger::pause_is_blackboxed(PageClient& page, JS::Debugger::PauseInfo const& pause) const
{
    auto page_sources = m_blackboxed_sources.find(page.id());
    if (page_sources == m_blackboxed_sources.end() || pause.stack_trace.is_empty())
        return false;

    auto const& stack_frame = pause.stack_trace.first();
    auto* executable = stack_frame.execution_context->executable.ptr();
    if (!executable)
        return false;

    auto source = page.devtools_source_description(*executable->source_code);
    if (!source.has_value())
        return false;

    auto source_url = debugger_source_url(*source);
    auto blackboxed_source = page_sources->value.find_if([&](auto const& candidate) {
        return candidate.url == source_url;
    });
    if (blackboxed_source == page_sources->value.end())
        return false;
    WebView::DebuggerSourcePosition position {
        .line = source->source_start_line,
        .column = source->source_start_column,
    };
    if (stack_frame.source_range.has_value()) {
        position.line = stack_frame.source_range->start.line;
        position.column = stack_frame.source_range->start.column > 0
            ? stack_frame.source_range->start.column - 1
            : 0;
    }

    auto position_is_in_ranges = [&](auto const& ranges) {
        return ranges.find_if([&](auto const& range) {
            return !debugger_position_is_before(position, range.start)
                && !debugger_position_is_before(range.end, position);
        }) != ranges.end();
    };

    if (blackboxed_source->state.fully_blackboxed)
        return !position_is_in_ranges(blackboxed_source->state.unblackboxed_ranges);

    return position_is_in_ranges(blackboxed_source->state.blackboxed_ranges);
}

void DevToolsDebugger::handle_pause(JS::Debugger::PauseInfo const& pause)
{
    auto* page = paused_page_client();
    if (!page) {
        Web::Bindings::main_thread_vm().debugger()->continue_execution();
        return;
    }

    auto configuration = m_configurations.get(page->id()).value_or(WebView::DebuggerConfiguration {});
    auto continue_after_filtered_pause = [&] {
        if (pause.reason == JS::Debugger::PauseReason::Breakpoint)
            Web::Bindings::main_thread_vm().debugger()->continue_execution_preserving_step_state();
        else
            Web::Bindings::main_thread_vm().debugger()->continue_execution();
    };
    auto blackboxing_applies = pause.reason == JS::Debugger::PauseReason::DebuggerStatement
        || pause.reason == JS::Debugger::PauseReason::Exception
        || pause.reason == JS::Debugger::PauseReason::Step;
    if (blackboxing_applies && pause_is_blackboxed(*page, pause)) {
        Web::Bindings::main_thread_vm().debugger()->continue_execution_preserving_step_state();
        return;
    }

    auto should_filter_pause = [&] {
        switch (pause.reason) {
        case JS::Debugger::PauseReason::Breakpoint:
            return configuration.skip_breakpoints;
        case JS::Debugger::PauseReason::DebuggerStatement:
            return !configuration.should_pause_on_debugger_statement;
        case JS::Debugger::PauseReason::Exception:
            return configuration.skip_breakpoints || !configuration.pause_on_exceptions
                || (configuration.ignore_caught_exceptions && pause.exception_will_be_caught);
        case JS::Debugger::PauseReason::Entry:
        case JS::Debugger::PauseReason::Step:
            return false;
        }
        VERIFY_NOT_REACHED();
    }();
    if (should_filter_pause) {
        continue_after_filtered_pause();
        return;
    }

    Optional<Utf16String> condition_error;
    if (pause.reason == JS::Debugger::PauseReason::Breakpoint) {
        bool should_pause = false;
        auto registrations = m_breakpoints.find(page->id());
        if (registrations != m_breakpoints.end()) {
            for (auto const& registration : registrations->value) {
                if (!pause.breakpoint_ids.contains_slow(registration.id))
                    continue;
                auto* context = pause.stack_trace.first().execution_context;
                VERIFY(context);
                Optional<Utf16String> registration_condition_error;
                if (registration.options.condition.has_value() && !registration.options.condition->is_empty()) {
                    auto result = Web::Bindings::main_thread_vm().debugger()->evaluate_in_frame(*context, *registration.options.condition);
                    if (result.is_throw_completion()) {
                        registration_condition_error = result.throw_completion().value().to_utf16_string_without_side_effects();
                    } else if (!result.release_value().to_boolean()) {
                        continue;
                    }
                }

                if (registration.options.log_value.has_value() && !registration.options.log_value->is_empty()) {
                    emit_logpoint(*page, pause, *context, registration.options);
                    continue;
                }

                should_pause = true;
                if (registration_condition_error.has_value())
                    condition_error = registration_condition_error.release_value();
            }
        }
        if (!should_pause) {
            continue_after_filtered_pause();
            return;
        }
    }

    m_paused_frames.clear();
    m_paused_objects.clear();
    m_paused_object_ids.clear();

    WebView::DebuggerPause debugger_pause {
        .reason = [&] {
            if (condition_error.has_value())
                return WebView::DebuggerPauseReason::BreakpointConditionThrown;
            switch (pause.reason) {
            case JS::Debugger::PauseReason::Breakpoint:
                return WebView::DebuggerPauseReason::Breakpoint;
            case JS::Debugger::PauseReason::DebuggerStatement:
                return WebView::DebuggerPauseReason::DebuggerStatement;
            case JS::Debugger::PauseReason::Entry:
                return WebView::DebuggerPauseReason::Entry;
            case JS::Debugger::PauseReason::Exception:
                return WebView::DebuggerPauseReason::Exception;
            case JS::Debugger::PauseReason::Step:
                return WebView::DebuggerPauseReason::ResumeLimit;
            }
            VERIFY_NOT_REACHED();
        }(),
        .reason_message = move(condition_error),
        .exception = pause.exception.map([&](auto value) { return serialize_value(value); }),
        .frames = {},
    };

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
    m_resume_mode = WebView::DebuggerResumeMode::Continue;
    m_resume_requested = false;
    m_is_handling_pause = true;
    auto paused_page_id = page->id();

    auto event_loop_pause = Web::HTML::main_thread_event_loop().pause();
    m_client.async_did_pause_debugger(page->id(), move(debugger_pause));
    Core::EventLoop::current().spin_until([this] {
        return m_resume_requested;
    });

    m_client.async_did_resume_debugger(paused_page_id);

    m_is_handling_pause = false;
    m_paused_page_id = {};
    m_paused_frames.clear();
    m_paused_objects.clear();
    m_paused_object_ids.clear();
    auto resume_mode = [&] {
        switch (m_resume_mode) {
        case WebView::DebuggerResumeMode::Continue:
            return JS::Debugger::ResumeMode::Continue;
        case WebView::DebuggerResumeMode::StepInto:
            return JS::Debugger::ResumeMode::StepInto;
        case WebView::DebuggerResumeMode::StepOut:
            return JS::Debugger::ResumeMode::StepOut;
        case WebView::DebuggerResumeMode::StepOver:
            return JS::Debugger::ResumeMode::StepOver;
        }
        VERIFY_NOT_REACHED();
    }();
    Web::Bindings::main_thread_vm().debugger()->continue_execution(resume_mode);
    schedule_disable_if_unused();
}

void DevToolsDebugger::update_exception_pause_mode()
{
    auto& vm = Web::Bindings::main_thread_vm();
    if (!vm.debugger())
        return;

    auto mode = JS::Debugger::PauseOnExceptions::None;
    for (auto const& configuration : m_configurations) {
        if (!configuration.value.pause_on_exceptions)
            continue;
        mode = configuration.value.ignore_caught_exceptions
            ? JS::Debugger::PauseOnExceptions::Uncaught
            : JS::Debugger::PauseOnExceptions::All;
        if (mode == JS::Debugger::PauseOnExceptions::All)
            break;
    }
    vm.debugger()->set_pause_on_exceptions(mode);
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
