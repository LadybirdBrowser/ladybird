/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibGC/Function.h>
#include <LibJS/Console.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebDriver/BiDi/BindingsGlue.h>
#include <LibWeb/WebDriver/BiDi/RemoteValue.h>
#include <LibWeb/WebDriver/BiDi/Script.h>

namespace Web::WebDriver::BiDi {

bool g_is_active { false };

static BiDiSessionState& state()
{
    static auto& state = *new BiDiSessionState;
    return state;
}

static HashTable<String>& enabled_events()
{
    static auto& events = *new HashTable<String>;
    return events;
}

static EventSink& event_sink()
{
    static auto& sink = *new EventSink;
    return sink;
}

static void install_console_clients_for_existing_documents();

// NB: The ids of the realms that are reported as destroyed, whose documents are not destroyed yet.
static HashTable<String>& destroyed_realm_ids()
{
    static auto& ids = *new HashTable<String>;
    return ids;
}

void set_session_state(BiDiSessionState session_state)
{
    auto was_active = g_is_active;

    enabled_events().clear();
    for (auto const& event_name : session_state.enabled_events)
        enabled_events().set(event_name);

    state() = move(session_state);
    g_is_active = state().is_active;

    // NB: The documents that existed before the session started run the console steps from now on.
    if (g_is_active && !was_active)
        install_console_clients_for_existing_documents();

    // NB: The objects the handles of a session that ended name are not kept alive any longer.
    if (!g_is_active && was_active) {
        fail_all_pending_evaluations();
        clear_all_handle_object_maps();
        destroyed_realm_ids().clear();
    }
}

BiDiSessionState const& session_state()
{
    return state();
}

bool is_event_enabled(StringView event_name)
{
    return enabled_events().contains(event_name);
}

void set_event_sink(EventSink sink)
{
    event_sink() = move(sink);
}

// https://w3c.github.io/webdriver-bidi/#type-browsingContext-Browsingcontext
// For navigables with an associated WebDriver window handle the navigable id must be the same as the window handle.
String navigable_id(HTML::Navigable const& navigable)
{
    if (auto const* traversable = as_if<HTML::LocalTraversableNavigable>(navigable); traversable && !traversable->window_handle().is_empty())
        return traversable->window_handle().to_utf8();

    // NB: This is how the browser process names a navigable that is not a top-level traversable.
    return MUST(String::formatted("{}-{}", navigable.id().namespace_id, navigable.id().local_id));
}

void emit_event(HTML::Navigable const& navigable, String name, JsonValue params)
{
    if (event_sink().on_event)
        event_sink().on_event(navigable, name, move(params));
}

void navigation_event(HTML::Navigable const& navigable, StringView event_name, Optional<Utf16String> const& navigation_id, URL::URL const& url)
{
    if (event_sink().on_navigation_event)
        event_sink().on_navigation_event(navigable, event_name, navigation_id, url);
}

// https://w3c.github.io/webdriver-bidi/#event-script-realmCreated
static void emit_realm_created(DOM::Document& document)
{
    if (!is_event_enabled("script.realmCreated"sv))
        return;

    auto navigable = document.navigable();
    if (!navigable || navigable->active_document().ptr() != &document)
        return;

    // 1. Let environment settings be the newly created environment settings object.
    // 2. Let realm info be the result of get the realm info given environment settings.
    // 3. If realm info is null, return.
    auto realm_info = get_the_realm_info(document.relevant_settings_object());
    if (!realm_info.has_value())
        return;

    // 4. Let related navigables be the result of get related navigables given environment settings.
    // 5. Let body be a map matching the script.RealmCreated production, with the params field set to realm info.
    // 6. For each session in the set of sessions for which an event is enabled given "script.realmCreated" and related
    //    navigables:
    //     1. Emit an event with session and body.
    emit_event(*navigable, "script.realmCreated"_string, realm_info.release_value());
}

void document_is_ready_for_scripts(DOM::Document& document)
{
    ensure_console_steps_run_for(document);

    // https://w3c.github.io/webdriver-bidi/#event-script-realmCreated
    // When any of the set up a window environment settings object, set up a worker environment settings object or set up
    // a worklet environment settings object algorithms are invoked, immediately prior to returning the settings object:
    // AD-HOC: The settings object of a window is set up before the window has an associated Document with a navigable,
    //         when getting its realm info returns null. The realm is reported as created once its document is active
    //         in a navigable instead.
    emit_realm_created(document);

    // https://w3c.github.io/webdriver-bidi/#run-webdriver-bidi-preload-scripts
    run_webdriver_bidi_preload_scripts(document.relevant_settings_object());
}

// https://w3c.github.io/webdriver-bidi/#event-script-realmDestroyed
void unloading_document_cleanup_steps(DOM::Document& document)
{
    auto& settings = document.relevant_settings_object();

    // 1. Let related navigables be an empty set.
    // 2. Append document's navigable to related navigables.
    auto navigable = document.navigable();

    // 3. For each worklet global scope in document's worklet global scopes:
    // FIXME: Report the realms of worklets.

    // 4. Let environment settings be the environment settings object whose relevant global object's associated Document
    //    is document.
    // 5. Let realm be environment settings' realm execution context's Realm component.
    // 6. Let realm id be the realm id for realm.
    // 7. Let params be a map matching the script.RealmDestroyedParameters production, with the realm field set to realm
    //    id.
    // 8. Let body be a map matching the script.RealmDestroyed production, with the params field set to params.
    // 9. For each session in the set of sessions for which an event is enabled given "script.realmDestroyed" and
    //    related navigables:
    //     1. Emit an event with session and body.
    // NB: A document whose unloading destroys it runs the unloading document cleanup steps when it is unloaded and again
    //     when it is destroyed, while its realm is destroyed once.
    auto id = realm_id(settings);
    if (destroyed_realm_ids().remove(id))
        return;
    destroyed_realm_ids().set(id);

    if (navigable && is_event_enabled("script.realmDestroyed"sv)) {
        JsonObject params;
        params.set("realm"sv, move(id));
        emit_event(*navigable, "script.realmDestroyed"_string, move(params));
    }

    // NB: The realm is discarded, and the objects its handles name with it.
    clear_handle_object_map(settings);
    fail_pending_evaluations(settings);
}

static StringView method_for_log_level(JS::Console::LogLevel log_level)
{
    switch (log_level) {
    case JS::Console::LogLevel::Assert:
        return "assert"sv;
    case JS::Console::LogLevel::Count:
        return "count"sv;
    case JS::Console::LogLevel::CountReset:
        return "countReset"sv;
    case JS::Console::LogLevel::Debug:
        return "debug"sv;
    case JS::Console::LogLevel::Dir:
        return "dir"sv;
    case JS::Console::LogLevel::DirXML:
        return "dirxml"sv;
    case JS::Console::LogLevel::Error:
        return "error"sv;
    case JS::Console::LogLevel::Group:
        return "group"sv;
    case JS::Console::LogLevel::GroupCollapsed:
        return "groupCollapsed"sv;
    case JS::Console::LogLevel::Info:
        return "info"sv;
    case JS::Console::LogLevel::Log:
        return "log"sv;
    case JS::Console::LogLevel::TimeEnd:
        return "timeEnd"sv;
    case JS::Console::LogLevel::TimeLog:
        return "timeLog"sv;
    case JS::Console::LogLevel::Table:
        return "table"sv;
    case JS::Console::LogLevel::Trace:
        return "trace"sv;
    case JS::Console::LogLevel::Warn:
        return "warn"sv;
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/webdriver-bidi/#get-related-navigables
static GC::Ptr<HTML::Navigable> related_navigable(HTML::EnvironmentSettingsObject& settings)
{
    // 1. Let related navigables be an empty set.
    // 2. If settings' relevant global object is a Window:
    //     1. Let navigable be relevant global object's associated Document's node navigable.
    //     2. If navigable is not null, append navigable to related navigables.
    // FIXME: 3. Otherwise if the global object specified by settings is a WorkerGlobalScope, for each owner in the global
    //           object's owner set:
    if (auto* window = HTML::window_from_global_object(settings.global_object()))
        return window->associated_document().navigable();

    // 4. Return related navigables.
    return nullptr;
}

// https://w3c.github.io/webdriver-bidi/#event-log-entryAdded
void console_steps(HTML::EnvironmentSettingsObject& settings, JS::Console::LogLevel log_level, JS::ConsoleClient::PrinterArguments const& arguments)
{
    if (!is_active())
        return;

    auto navigable = related_navigable(settings);
    if (!navigable)
        return;

    auto& vm = settings.global_object().vm();
    auto method = method_for_log_level(log_level);

    // NB: The console groups and traces give their label, which the console formatted from the arguments.
    GC::RootVector<JS::Value> args;
    Optional<JsonObject> trace_stack;
    arguments.visit(
        [&](JS::Console::Group const& group) {
            args.append(JS::PrimitiveString::create(vm, group.label));
        },
        [&](JS::Console::Trace const& trace) {
            if (!trace.label.is_empty())
                args.append(JS::PrimitiveString::create(vm, trace.label));

            JsonArray call_frames;
            for (auto const& frame : trace.stack) {
                if (!frame.source_file.has_value())
                    continue;
                JsonObject frame_info;
                frame_info.set("columnNumber"sv, frame.column.has_value() && *frame.column > 0 ? *frame.column - 1 : 0);
                frame_info.set("functionName"sv, frame.function_name.to_utf8());
                frame_info.set("lineNumber"sv, frame.line.has_value() && *frame.line > 0 ? *frame.line - 1 : 0);
                frame_info.set("url"sv, frame.source_file->to_utf8());
                call_frames.must_append(move(frame_info));
            }
            JsonObject stack;
            stack.set("callFrames"sv, move(call_frames));
            trace_stack = move(stack);
        },
        [&](GC::RootVector<JS::Value> const& values) {
            for (auto value : values)
                args.append(value);
        });

    // 1. For each session in active BiDi sessions:

    // 1. If method is "error" or "assert", let level be "error". If method is "debug" or "trace" let level be "debug". If
    //    method is "warn", let level be "warn". Otherwise let level be "info".
    auto level = "info"sv;
    if (method.is_one_of("error"sv, "assert"sv))
        level = "error"sv;
    else if (method.is_one_of("debug"sv, "trace"sv))
        level = "debug"sv;
    else if (method == "warn"sv)
        level = "warn"sv;

    // 2. Let timestamp be a time value representing the current date and time in UTC.
    auto timestamp = UnixDateTime::now().milliseconds_since_epoch();

    // 3. Let text be an empty string.
    StringBuilder text;

    // 4. If Type(args[0]) is String, and args[0] contains a formatting specifier, let formatted args be Formatter(args).
    //    Otherwise let formatted args be args.
    // NB: The console runs the formatter before it gives the arguments to its client.

    // 5. For each arg in formatted args:
    for (size_t index = 0; index < args.size(); ++index) {
        auto arg = args[index];

        // 1. If arg is not the first entry in args, append a U+0020 SPACE to text.
        if (index > 0)
            text.append(' ');

        // 2. If arg is a primitive ECMAScript value, append ToString(arg) to text. Otherwise append an
        //    implementation-defined string to text.
        if (arg.is_symbol() || !arg.is_object())
            text.append(arg.to_utf16_string_without_side_effects().to_utf8());
        else if (arg.as_object().is_function())
            text.append("function"sv);
        else
            text.append("Object"sv);
    }

    // 6. Let realm be the realm id of the current Realm Record.
    // 7. Let serialized args be a new list.
    JsonArray serialized_args;

    // 8. Let serialization options be a map matching the script.SerializationOptions production with the fields set to
    //    their default values.
    SerializationOptions serialization_options;

    // 9. For each arg of args:
    for (auto arg : args) {
        // 1. Let serialized arg be the result of serialize as a remote value with arg as value, serialization options,
        //    none as ownership type, a new map as serialization internal map, realm and session.
        // 2. Add serialized arg to serialized args.
        serialized_args.must_append(serialize_as_a_remote_value(arg, serialization_options, ResultOwnership::None, settings));
    }

    // 10. Let source be the result of get the source given current Realm Record.
    auto source = get_the_source(settings);

    // 11. Let stack be the current stack trace.
    auto stack = trace_stack.has_value() ? trace_stack.release_value() : current_stack_trace(vm);

    // 12. Let entry be a map matching the log.ConsoleLogEntry production, with the the level field set to level, the
    //     text field set to text, the timestamp field set to timestamp, the stackTrace field set to stack, the method
    //     field set to method, the source field set to source, and the args field set to serialized args.
    JsonObject entry;
    entry.set("type"sv, "console"sv);
    entry.set("level"sv, level);
    entry.set("text"sv, text.to_string_without_validation());
    entry.set("timestamp"sv, timestamp);
    entry.set("stackTrace"sv, move(stack));
    entry.set("method"sv, method);
    entry.set("source"sv, move(source));
    entry.set("args"sv, move(serialized_args));

    // 13. Let body be a map matching the log.EntryAdded production, with the params field set to entry.
    // 14. Let settings be the current settings object
    // 15. Let related navigables be the result of get related navigables given settings.
    // 16. If event is enabled with session, "log.entryAdded" and related navigables, emit an event with session and body.
    //     Otherwise, buffer a log event with session, related browsing contexts, and body.
    // NB: WebDriver emits the event, or buffers it.
    emit_event(*navigable, "log.entryAdded"_string, move(entry));
}

// https://w3c.github.io/webdriver-bidi/#event-log-entryAdded
void error_reporting_steps(HTML::EnvironmentSettingsObject& settings, JS::Value exception)
{
    // 1. If handled is true return.
    // NB: The caller only runs these steps for an error that is not handled.

    // 2. Let settings be script's settings object.
    auto navigable = related_navigable(settings);
    if (!navigable)
        return;

    // 3. Let timestamp be a time value representing the current date and time in UTC.
    auto timestamp = UnixDateTime::now().milliseconds_since_epoch();

    // 4. Let stack be the stack trace for an exception with the exception corresponding to the error being reported.
    auto stack = stack_trace_for_an_exception(exception);

    // 5. Let source be the result of get the source given current Realm Record.
    auto source = get_the_source(settings);

    // NB: The message of an error is its name and message, as the error's toString() gives them.
    auto message = textual_description_of_an_exception(settings, exception);

    // 6. Let entry be a map matching the log.JavascriptLogEntry production, with level set to "error", text set to
    //    message, source set to source, timestamp set to timestamp, and the stackTrace field set to stack.
    JsonObject entry;
    entry.set("type"sv, "javascript"sv);
    entry.set("level"sv, "error"sv);
    entry.set("text"sv, move(message));
    entry.set("source"sv, move(source));
    entry.set("timestamp"sv, timestamp);
    entry.set("stackTrace"sv, move(stack));

    // 7. Let body be a map matching the log.EntryAdded production, with the params field set to entry.
    // 8. Let related navigables be the result of get related navigables given settings.
    // 9. For each session in active BiDi sessions:
    //     1. If event is enabled with session, "log.entryAdded" and related navigables, emit an event with session and
    //        body.
    //        Otherwise, buffer a log event with session, related browsing contexts, and body.
    // NB: WebDriver emits the event, or buffers it.
    emit_event(*navigable, "log.entryAdded"_string, move(entry));
}

namespace {

// NB: The console client of a realm whose console has none of its own, which only runs the console steps.
class ConsoleStepsClient final : public JS::ConsoleClient {
    GC_CELL(ConsoleStepsClient, JS::ConsoleClient);
    GC_DECLARE_ALLOCATOR(ConsoleStepsClient);

public:
    explicit ConsoleStepsClient(JS::Console& console)
        : ConsoleClient(console)
    {
    }

    virtual void clear() override { }
    virtual void end_group() override { }

    virtual JS::ThrowCompletionOr<JS::Value> printer(JS::Console::LogLevel log_level, PrinterArguments arguments) override
    {
        console_steps(BindingsGlue::console_settings(*m_console), log_level, arguments);
        return JS::js_undefined();
    }
};

GC_DEFINE_ALLOCATOR(ConsoleStepsClient);

}

void ensure_console_steps_run_for(DOM::Document& document)
{
    // NB: A document that has a console client of its own runs the console steps through it.
    if (document.console_client())
        return;

    auto& settings = document.relevant_settings_object();
    auto& console = BindingsGlue::console_for(settings);
    console.set_client(GC::Heap::the().allocate<ConsoleStepsClient>(console));
}

static void install_console_clients_for_existing_documents()
{
    for (auto navigable : HTML::all_local_navigables()) {
        if (auto document = navigable->active_document())
            ensure_console_steps_run_for(*document);
    }
}

}
