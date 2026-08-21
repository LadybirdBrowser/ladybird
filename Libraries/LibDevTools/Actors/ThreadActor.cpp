/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <LibDevTools/Actors/DebuggerFrameActor.h>
#include <LibDevTools/Actors/EnvironmentActor.h>
#include <LibDevTools/Actors/ObjectActor.h>
#include <LibDevTools/Actors/PropertyIteratorActor.h>
#include <LibDevTools/Actors/SourceActor.h>
#include <LibDevTools/Actors/SymbolIteratorActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/Actors/WatcherActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<ThreadActor> ThreadActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<WatcherActor> watcher)
{
    return adopt_ref(*new ThreadActor(devtools, move(name), move(tab), move(watcher)));
}

ThreadActor::ThreadActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<WatcherActor> watcher)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_watcher(move(watcher))
{
}

ThreadActor::~ThreadActor()
{
    clear_pause_actors();
    for (auto const& actor : m_source_actors) {
        if (auto source_actor = actor.value.strong_ref())
            devtools().unregister_actor(source_actor->name());
    }
}

void ThreadActor::handle_message(Message const& message)
{
    if (message.type == "attach"sv) {
        attach();
        JsonObject response;
        send_response(message, move(response));
        return;
    }

    if (message.type == "resume"sv) {
        if (!m_is_paused) {
            send_wrong_state_error(message, "Cannot resume while the thread is running"sv);
            return;
        }

        WebView::DebuggerResumeMode mode = WebView::DebuggerResumeMode::Continue;
        if (auto resume_limit = message.data.get_object("resumeLimit"sv); resume_limit.has_value()) {
            auto type = resume_limit->get_string("type"sv);
            if (!type.has_value()) {
                send_missing_parameter_error(message, "resumeLimit.type"sv);
                return;
            }
            if (*type == "step"sv)
                mode = WebView::DebuggerResumeMode::StepInto;
            else if (*type == "next"sv || *type == "break"sv)
                mode = WebView::DebuggerResumeMode::StepOver;
            else if (*type == "finish"sv)
                mode = WebView::DebuggerResumeMode::StepOut;
            else {
                JsonObject response;
                response.set("error"sv, "badParameterType"sv);
                response.set("message"sv, "Unknown resumeLimit type"sv);
                send_response(message, move(response));
                return;
            }
        }

        if (auto frame_actor_id = message.data.get_string("frameActorID"sv); frame_actor_id.has_value()) {
            auto frame = m_frame_actors.find_if([&](auto const& weak_frame) {
                auto frame = weak_frame.strong_ref();
                return frame && frame->name() == *frame_actor_id;
            });
            if (frame == m_frame_actors.end()) {
                send_unknown_actor_error(message, *frame_actor_id);
                return;
            }

            auto youngest_frame = m_frame_actors.first().strong_ref();
            auto selected_frame = frame->strong_ref();
            if (!youngest_frame || !selected_frame || youngest_frame.ptr() != selected_frame.ptr()) {
                JsonObject response;
                response.set("error"sv, "unsupported"sv);
                response.set("message"sv, "Stepping from a non-youngest frame is not supported"sv);
                send_response(message, move(response));
                return;
            }
        }

        resume(mode);
        JsonObject response;
        send_response(message, move(response));
        return;
    }

    if (message.type == "interrupt"sv) {
        auto when = get_required_parameter<String>(message, "when"sv);
        if (!when.has_value())
            return;
        if (*when != "onNext"sv) {
            JsonObject response;
            response.set("error"sv, "unsupported"sv);
            response.set("message"sv, "Only interrupting on the next bytecode execution is supported"sv);
            send_response(message, move(response));
            return;
        }
        if (m_is_paused) {
            send_wrong_state_error(message, "Cannot interrupt while the thread is paused"sv);
            return;
        }

        auto tab = m_tab.strong_ref();
        if (!tab) {
            send_wrong_state_error(message, "Cannot interrupt a detached thread"sv);
            return;
        }

        m_pause_requested_on_next = true;
        devtools().delegate().interrupt_debugger(tab->description());
        send_response(message, {});
        return;
    }

    if (message.type == "reconfigure"sv || message.type == "skipBreakpoints"sv) {
        JsonObject response;
        send_response(message, move(response));
        return;
    }

    if (message.type == "getAvailableEventBreakpoints"sv) {
        JsonObject response;
        JsonArray breakpoints;
        response.set("value"sv, move(breakpoints));
        send_response(message, move(response));
        return;
    }

    if (message.type == "sources"sv) {
        auto tab = m_tab.strong_ref();
        if (!tab) {
            JsonObject response;
            JsonArray sources;
            response.set("sources"sv, move(sources));
            send_response(message, move(response));
            return;
        }

        devtools().delegate().retrieve_sources(tab->description(),
            async_handler<ThreadActor>(message, [](auto& self, auto sources, auto& response) {
                response.set("sources"sv, self.serialize_sources(sources));
            }));
        return;
    }

    if (message.type == "frames"sv) {
        if (!m_is_paused) {
            send_wrong_state_error(message, "Stack frames are only available while the thread is paused"sv);
            return;
        }

        auto start = message.data.get_integer<size_t>("start"sv).value_or(0);
        auto count = message.data.get_integer<size_t>("count"sv);
        start = min(start, m_frame_actors.size());
        auto end = count.has_value()
            ? start + min(*count, m_frame_actors.size() - start)
            : m_frame_actors.size();

        JsonArray frames;
        for (auto index = start; index < end; ++index) {
            if (auto frame = m_frame_actors[index].strong_ref())
                frames.must_append(frame->serialize_frame());
        }

        JsonObject response;
        response.set("frames"sv, move(frames));
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

void ThreadActor::attach()
{
    if (auto watcher = m_watcher.strong_ref())
        watcher->start_watching_thread_state_resources();
}

void ThreadActor::resume(WebView::DebuggerResumeMode mode)
{
    if (!m_is_paused)
        return;

    if (auto tab = m_tab.strong_ref())
        devtools().delegate().resume_debugger(tab->description(), mode);

    did_resume();
}

void ThreadActor::did_resume()
{
    if (!m_is_paused)
        return;

    m_is_paused = false;
    clear_pause_actors();
    if (auto watcher = m_watcher.strong_ref()) {
        JsonObject resource;
        resource.set("state"sv, "resumed"sv);
        watcher->send_thread_state_available_message(move(resource));
    }
}

void ThreadActor::did_pause(WebView::DebuggerPause pause)
{
    auto pause_was_requested_on_next = m_pause_requested_on_next;
    m_pause_requested_on_next = false;
    clear_pause_actors();
    if (pause.frames.is_empty()) {
        if (auto tab = m_tab.strong_ref())
            devtools().delegate().resume_debugger(tab->description(), WebView::DebuggerResumeMode::Continue);
        did_resume();
        return;
    }

    auto paused_source_id = pause.frames.first().location.source.id;
    DebuggerFrameActor* youngest_frame_actor = nullptr;
    for (size_t index = 0; index < pause.frames.size(); ++index) {
        auto& frame = pause.frames[index];
        auto& source_actor = source_actor_for(frame.location.source);
        auto& frame_actor = devtools().register_actor<DebuggerFrameActor>(make_weak_ptr<ThreadActor>(), move(frame), source_actor.name(), index == pause.frames.size() - 1);
        add_child_actor(frame_actor);
        m_frame_actors.append(frame_actor);
        m_pause_scoped_actors.append(frame_actor);
        if (!youngest_frame_actor)
            youngest_frame_actor = &frame_actor;
    }
    VERIFY(youngest_frame_actor);
    m_paused_source_id = paused_source_id;

    auto reason = [&] {
        switch (pause.reason) {
        case WebView::DebuggerPauseReason::Breakpoint:
            return "breakpoint"sv;
        case WebView::DebuggerPauseReason::BreakpointConditionThrown:
            return "breakpointConditionThrown"sv;
        case WebView::DebuggerPauseReason::DebuggerStatement:
            return "debuggerStatement"sv;
        case WebView::DebuggerPauseReason::Entry:
            return "interrupted"sv;
        case WebView::DebuggerPauseReason::Exception:
            return "exception"sv;
        case WebView::DebuggerPauseReason::ResumeLimit:
            return "resumeLimit"sv;
        }
        VERIFY_NOT_REACHED();
    }();

    JsonObject why;
    why.set("type"sv, reason);
    if (pause.reason == WebView::DebuggerPauseReason::Entry && pause_was_requested_on_next)
        why.set("onNext"sv, true);
    if (pause.reason_message.has_value())
        why.set("message"sv, pause.reason_message->to_utf8());
    if (pause.exception.has_value())
        why.set("exception"sv, serialize_debugger_value(*pause.exception));

    JsonObject resource;
    resource.set("state"sv, "paused"sv);
    resource.set("why"sv, move(why));
    resource.set("frame"sv, youngest_frame_actor->serialize_frame());

    m_is_paused = true;
    if (auto watcher = m_watcher.strong_ref())
        watcher->send_thread_state_available_message(move(resource));
}

void ThreadActor::clear_pause_actors()
{
    for (auto const& weak_actor : m_pause_scoped_actors) {
        if (auto actor = weak_actor.strong_ref()) {
            unregister_child_actor(*actor);
        }
    }
    m_pause_scoped_actors.clear();
    m_frame_actors.clear();
    m_object_actors.clear();
    m_paused_source_id.clear();
}

void ThreadActor::release_pause_actor(Actor& actor)
{
    auto* actor_ptr = &actor;
    m_pause_scoped_actors.remove_all_matching([&](auto const& weak_actor) {
        auto pause_actor = weak_actor.strong_ref();
        return !pause_actor || pause_actor.ptr() == actor_ptr;
    });
    unregister_child_actor(actor);
}

void ThreadActor::send_wrong_state_error(Message const& message, StringView error_message)
{
    JsonObject response;
    response.set("error"sv, "wrongState"sv);
    response.set("message"sv, error_message);
    send_response(message, move(response));
}

ObjectActor& ThreadActor::object_actor_for(WebView::DebuggerValue const& value)
{
    VERIFY(value.type == WebView::DebuggerValueType::Object);
    if (auto actor = m_object_actors.find(value.object_id); actor != m_object_actors.end()) {
        if (auto object_actor = actor->value.strong_ref())
            return *object_actor;
    }

    auto& actor = devtools().register_actor<ObjectActor>(make_weak_ptr<ThreadActor>(), value.object_id, value.object_class);
    add_child_actor(actor);
    m_pause_scoped_actors.append(actor);
    m_object_actors.set(value.object_id, actor);
    return actor;
}

JsonValue ThreadActor::serialize_debugger_value(WebView::DebuggerValue const& value)
{
    switch (value.type) {
    case WebView::DebuggerValueType::Boolean:
        return value.boolean_value;
    case WebView::DebuggerValueType::Number:
        if (isnan(value.number_value)) {
            JsonObject grip;
            grip.set("type"sv, "NaN"sv);
            return grip;
        }
        if (isinf(value.number_value)) {
            JsonObject grip;
            grip.set("type"sv, value.number_value < 0 ? "-Infinity"sv : "Infinity"sv);
            return grip;
        }
        if (value.number_value == 0 && signbit(value.number_value)) {
            JsonObject grip;
            grip.set("type"sv, "-0"sv);
            return grip;
        }
        return value.number_value;
    case WebView::DebuggerValueType::String:
        return value.text.to_utf8();
    case WebView::DebuggerValueType::BigInt: {
        JsonObject grip;
        grip.set("type"sv, "BigInt"sv);
        grip.set("text"sv, value.text.to_utf8());
        return grip;
    }
    case WebView::DebuggerValueType::Object:
        return object_actor_for(value).grip();
    case WebView::DebuggerValueType::Null: {
        JsonObject grip;
        grip.set("type"sv, "null"sv);
        return grip;
    }
    case WebView::DebuggerValueType::Uninitialized: {
        JsonObject grip;
        grip.set("type"sv, "null"sv);
        grip.set("uninitialized"sv, true);
        return grip;
    }
    case WebView::DebuggerValueType::Undefined: {
        JsonObject grip;
        grip.set("type"sv, "undefined"sv);
        return grip;
    }
    }
    VERIFY_NOT_REACHED();
}

Optional<u64> ThreadActor::frame_id_for_actor(StringView actor_name) const
{
    for (auto const& weak_frame : m_frame_actors) {
        if (auto frame = weak_frame.strong_ref(); frame && frame->name() == actor_name)
            return frame->frame_id();
    }
    return {};
}

JsonObject ThreadActor::serialize_environment_chain(Vector<WebView::DebuggerEnvironment> environments)
{
    HashMap<u64, JsonObject> forms;
    for (auto& environment : environments) {
        JsonObject form;
        switch (environment.type) {
        case WebView::DebuggerEnvironmentType::Block:
            form.set("type"sv, "block"sv);
            break;
        case WebView::DebuggerEnvironmentType::Function:
            form.set("type"sv, "function"sv);
            break;
        case WebView::DebuggerEnvironmentType::Object:
            form.set("type"sv, "object"sv);
            break;
        }

        if (environment.function_name.has_value()) {
            JsonObject function;
            function.set("displayName"sv, environment.function_name->to_utf8());
            form.set("function"sv, move(function));
        }
        if (environment.object.has_value())
            form.set("object"sv, serialize_debugger_value(*environment.object));

        JsonObject variables;
        for (auto const& binding : environment.bindings) {
            JsonObject descriptor;
            descriptor.set("value"sv, serialize_debugger_value(binding.value));
            descriptor.set("configurable"sv, false);
            descriptor.set("enumerable"sv, true);
            descriptor.set("writable"sv, binding.writable);
            variables.set(binding.name.to_utf8(), move(descriptor));
        }
        JsonObject bindings;
        bindings.set("arguments"sv, JsonArray {});
        bindings.set("variables"sv, move(variables));
        form.set("bindings"sv, move(bindings));
        forms.set(environment.id, move(form));
    }

    JsonObject parent;
    for (size_t index = environments.size(); index-- > 0;) {
        auto form = forms.take(environments[index].id).release_value();
        if (!parent.is_empty())
            form.set("parent"sv, move(parent));
        auto& actor = devtools().register_actor<EnvironmentActor>(move(form));
        add_child_actor(actor);
        m_pause_scoped_actors.append(actor);
        parent = actor.form();
    }
    return parent;
}

void ThreadActor::get_frame_environment(DebuggerFrameActor& frame_actor, Actor::Message const& message, u64 frame_id)
{
    auto tab = m_tab.strong_ref();
    if (!tab) {
        frame_actor.send_response(message, {});
        return;
    }

    auto message_id = message.id;
    devtools().delegate().retrieve_debugger_environments(tab->description(), frame_id,
        [weak_self = make_weak_ptr<ThreadActor>(), weak_frame = frame_actor.make_weak_ptr<DebuggerFrameActor>(), message_id](auto result) mutable {
            auto frame = weak_frame.strong_ref();
            auto self = weak_self.strong_ref();
            if (!frame || !self)
                return;
            JsonObject response;
            if (!result.is_error() && !result.value().is_empty())
                response = self->serialize_environment_chain(result.release_value());
            frame->send_response({ .id = message_id }, move(response));
        });
}

JsonObject ThreadActor::serialize_property_descriptor(WebView::DebuggerProperty const& property)
{
    JsonObject descriptor;
    descriptor.set("configurable"sv, property.configurable);
    descriptor.set("enumerable"sv, property.enumerable);
    if (property.value.has_value()) {
        descriptor.set("writable"sv, property.writable);
        descriptor.set("value"sv, serialize_debugger_value(*property.value));
    } else {
        if (property.getter.has_value())
            descriptor.set("get"sv, serialize_debugger_value(*property.getter));
        if (property.setter.has_value())
            descriptor.set("set"sv, serialize_debugger_value(*property.setter));
    }
    return descriptor;
}

void ThreadActor::get_object_properties(ObjectActor& object_actor, Actor::Message const& message, ObjectPropertiesRequest request, Optional<String> property_name)
{
    auto tab = m_tab.strong_ref();
    if (!tab) {
        object_actor.send_unknown_actor_error(message, object_actor.name());
        return;
    }

    auto message_id = message.id;
    auto options = message.data.get_object("options"sv).value_or({});
    devtools().delegate().retrieve_debugger_object_properties(tab->description(), object_actor.object_id(),
        [weak_self = make_weak_ptr<ThreadActor>(), weak_object = object_actor.make_weak_ptr<ObjectActor>(), message_id, request, property_name = move(property_name), options = move(options)](auto result) mutable {
            auto self = weak_self.strong_ref();
            auto object = weak_object.strong_ref();
            if (!self || !object)
                return;
            if (result.is_error()) {
                JsonObject response;
                response.set("error"sv, "unknownActor"sv);
                response.set("message"sv, result.release_error());
                object->send_response({ .id = message_id }, move(response));
                return;
            }

            auto properties = result.release_value();
            JsonObject response;
            if (request == ObjectPropertiesRequest::Prototype) {
                if (properties.prototype.has_value())
                    response.set("prototype"sv, self->serialize_debugger_value(*properties.prototype));
                object->send_response({ .id = message_id }, move(response));
                return;
            }

            Vector<PropertyIteratorActor::Property> serialized_properties;
            for (auto const& property : properties.properties) {
                auto name = property.name.to_utf8();
                if (property_name.has_value() && name != *property_name)
                    continue;
                serialized_properties.append({ move(name), self->serialize_property_descriptor(property) });
            }

            if (request == ObjectPropertiesRequest::Property) {
                if (!serialized_properties.is_empty())
                    response.set("descriptor"sv, move(serialized_properties.first().descriptor));
                object->send_response({ .id = message_id }, move(response));
                return;
            }

            if (request == ObjectPropertiesRequest::PrototypeAndProperties) {
                JsonObject own_properties;
                for (auto& property : serialized_properties)
                    own_properties.set(property.name, move(property.descriptor));
                if (properties.prototype.has_value())
                    response.set("prototype"sv, self->serialize_debugger_value(*properties.prototype));
                response.set("ownProperties"sv, move(own_properties));
                // FIXME: Serialize the object's actual symbol-keyed properties.
                response.set("ownSymbols"sv, JsonArray {});
                response.set("safeGetterValues"sv, JsonObject {});
                object->send_response({ .id = message_id }, move(response));
                return;
            }

            auto is_indexed_property = [](String const& name) {
                auto view = name.bytes_as_string_view();
                if (view.is_empty() || (view.length() > 1 && view[0] == '0'))
                    return false;
                for (auto code_unit : view) {
                    if (!is_ascii_digit(code_unit))
                        return false;
                }
                auto index = view.to_number<u32>(TrimWhitespace::No);
                return index.has_value() && *index < NumericLimits<u32>::max();
            };
            auto ignore_indexed_properties = options.get_bool("ignoreIndexedProperties"sv).value_or(false);
            auto ignore_non_indexed_properties = options.get_bool("ignoreNonIndexedProperties"sv).value_or(false);
            if (ignore_indexed_properties || ignore_non_indexed_properties) {
                serialized_properties.remove_all_matching([&](auto const& property) {
                    auto is_indexed = is_indexed_property(property.name);
                    return (ignore_indexed_properties && is_indexed) || (ignore_non_indexed_properties && !is_indexed);
                });
            }

            if (options.get_bool("sort"sv).value_or(false)) {
                quick_sort(serialized_properties, [](auto const& left, auto const& right) {
                    return left.name < right.name;
                });
            }

            if (auto query = options.get_string("query"sv); query.has_value() && !query->is_empty()) {
                serialized_properties.remove_all_matching([&](auto const& property) {
                    return !property.name.contains(*query, CaseSensitivity::CaseInsensitive);
                });
            }

            auto& iterator = self->devtools().register_actor<PropertyIteratorActor>(self->make_weak_ptr<ThreadActor>(), move(serialized_properties));
            self->add_child_actor(iterator);
            self->m_pause_scoped_actors.append(iterator);
            response.set("iterator"sv, iterator.form());
            object->send_response({ .id = message_id }, move(response));
        });
}

void ThreadActor::get_object_symbols(ObjectActor& object_actor, Actor::Message const& message)
{
    auto& iterator = devtools().register_actor<SymbolIteratorActor>(make_weak_ptr<ThreadActor>());
    add_child_actor(iterator);
    m_pause_scoped_actors.append(iterator);

    JsonObject response;
    response.set("iterator"sv, iterator.form());
    object_actor.send_response(message, move(response));
}

JsonObject ThreadActor::serialize_source(Web::HTML::ScriptRegistry::Description const& source)
{
    return source_actor_for(source).serialize_source();
}

JsonArray ThreadActor::serialize_sources(Vector<Web::HTML::ScriptRegistry::Description> const& sources)
{
    prune_source_actors(sources);

    JsonArray serialized_sources;
    for (auto const& source : sources)
        serialized_sources.must_append(serialize_source(source));
    return serialized_sources;
}

void ThreadActor::prune_source_actors(Vector<Web::HTML::ScriptRegistry::Description> const& sources)
{
    HashTable<Web::HTML::ScriptRegistry::Identifier> current_sources;
    for (auto const& source : sources)
        current_sources.set(source.id);

    Vector<Web::HTML::ScriptRegistry::Identifier> stale_sources;
    for (auto const& actor : m_source_actors) {
        if (!current_sources.contains(actor.key))
            stale_sources.append(actor.key);
    }

    for (auto const& source_id : stale_sources) {
        auto actor = m_source_actors.take(source_id);
        if (actor.has_value()) {
            if (auto source_actor = actor->strong_ref())
                devtools().unregister_actor(source_actor->name());
        }
    }
}

SourceActor& ThreadActor::source_actor_for(Web::HTML::ScriptRegistry::Description const& source)
{
    if (auto actor = m_source_actors.find(source.id); actor != m_source_actors.end()) {
        if (auto source_actor = actor->value.strong_ref())
            return *source_actor;
    }

    auto& actor = devtools().register_actor<SourceActor>(m_tab, m_watcher, source);
    m_source_actors.set(source.id, actor);
    return actor;
}

}
