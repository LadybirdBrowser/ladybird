/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/AudioWorkletNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Worklet/WorkletEventController.h>
#include <LibWebAudio/SessionClientOfWebAudioWorker.h>

namespace Web::WebAudio::Render {

static HashMap<u64, GC::Weak<Web::WebAudio::BaseAudioContext>> s_context_by_session;

struct WorkletRegistrationState {
    HashMap<String, Vector<Web::WebAudio::AudioParamDescriptor>> registered_processor_descriptors;
    HashTable<String> failed_processor_registrations;
    u64 last_registration_generation { 0 };
};

static HashMap<u64, WorkletRegistrationState> s_registration_state_by_session;

void register_worklet_context(u64 session_id, GC::Weak<Web::WebAudio::BaseAudioContext> context)
{
    if (session_id == 0)
        return;
    s_context_by_session.set(session_id, move(context));
}

void unregister_worklet_context(u64 session_id)
{
    if (session_id == 0)
        return;
    s_context_by_session.remove(session_id);
}

void clear_all_worklet_contexts()
{
    s_context_by_session.clear();
}

static GC::Ptr<BaseAudioContext> context_for_session(u64 session_id)
{
    auto it = s_context_by_session.find(session_id);
    if (it == s_context_by_session.end())
        return nullptr;
    return it->value.ptr();
}

static void handle_worklet_processor_error(u64 session_id, NodeID node_id)
{
    auto context = context_for_session(session_id);
    if (!context)
        return;

    Vector<GC::Ref<AudioWorkletNode>> nodes_to_notify;
    for (auto const& weak_node : context->audio_nodes_for_snapshot()) {
        if (!weak_node)
            continue;
        auto& node = *weak_node.ptr();
        if (!is<AudioWorkletNode>(node))
            continue;
        auto& worklet_node = static_cast<AudioWorkletNode&>(node);
        if (worklet_node.node_id() == node_id)
            nodes_to_notify.append(worklet_node);
    }

    for (auto& node : nodes_to_notify) {
        auto* target_context = node->context().ptr();
        target_context->queue_a_media_element_task("audio worklet processorerror fired"sv, GC::create_function(target_context->heap(), [node] {
            HTML::TemporaryExecutionContext execution_context(node->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            HTML::ErrorEventInit event_init;
            event_init.error = JS::js_undefined();
            node->dispatch_event(HTML::ErrorEvent::create(node->realm(), HTML::EventNames::processorerror, event_init));
        }));
    }
}

static void enqueue_worklet_processor_error_task(u64 session_id, u64 node_id)
{
    auto context = context_for_session(session_id);
    if (!context)
        return;

    HTML::queue_a_task(HTML::Task::Source::AudioWorklet, HTML::main_thread_event_loop(), nullptr,
        GC::create_function(HTML::main_thread_event_loop().heap(), [session_id, node_id] {
            handle_worklet_processor_error(session_id, NodeID { node_id });
        }));
}

static void handle_worklet_processor_registration(u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation)
{
    note_worklet_processor_registration(session_id, name, descriptors, generation);

    auto context = context_for_session(session_id);
    if (!context)
        return;

    auto worklet = context->audio_worklet();
    if (worklet->has_loaded_any_module() || worklet->has_pending_module_promises())
        worklet->register_processor_from_worker(name, descriptors);
    if (worklet->has_loaded_any_module() || worklet->has_pending_module_promises())
        worklet->set_registration_generation(generation);
}

static void enqueue_worklet_processor_registration_task(u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation)
{
    auto context = context_for_session(session_id);
    if (!context)
        return;

    HTML::queue_a_task(HTML::Task::Source::AudioWorklet, HTML::main_thread_event_loop(), nullptr,
        GC::create_function(HTML::main_thread_event_loop().heap(), [session_id, name, descriptors, generation] {
            handle_worklet_processor_registration(session_id, name, descriptors, generation);
        }));
}

static void handle_worklet_module_evaluated(u64 session_id, u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> const& failed_processor_registrations)
{
    auto context = context_for_session(session_id);
    if (!context)
        return;

    auto worklet = context->audio_worklet();

    if (!failed_processor_registrations.is_empty()) {
        note_worklet_failed_processor_registrations(session_id, failed_processor_registrations);
        worklet->register_failed_processors_from_worker(failed_processor_registrations);
    }

    if (required_generation > worklet->registration_generation()
        && (worklet->has_loaded_any_module() || worklet->has_pending_module_promises())) {
        HashMap<String, Vector<Web::WebAudio::AudioParamDescriptor>> registered_descriptors;
        HashTable<String> failed_registrations;
        u64 last_generation = 0;

        if (get_worklet_replay_state_for_generation(session_id, required_generation, registered_descriptors, failed_registrations, last_generation)) {
            for (auto const& entry : registered_descriptors)
                worklet->register_processor_from_worker(entry.key, entry.value);

            if (!failed_registrations.is_empty()) {
                Vector<String> failed_names;
                failed_names.ensure_capacity(failed_registrations.size());
                for (auto const& failed_name : failed_registrations)
                    failed_names.append(failed_name);
                worklet->register_failed_processors_from_worker(failed_names);
            }

            worklet->set_registration_generation(last_generation);
        }
    }

    u64 const local_module_id = module_id & 0xffffffffu;
    auto message_copy = error_message;
    auto error_name_copy = error_name;
    context->queue_a_media_element_task("audio worklet module evaluated"sv, GC::create_function(context->heap(), [worklet, local_module_id, required_generation, success, error_name_copy = move(error_name_copy), message_copy = move(message_copy)] {
        worklet->handle_module_evaluated(local_module_id, required_generation, success, error_name_copy, message_copy);
    }));
}

static void enqueue_worklet_module_evaluated_task(u64 session_id, u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> const& failed_processor_registrations)
{
    auto context = context_for_session(session_id);
    if (!context)
        return;

    HTML::queue_a_task(HTML::Task::Source::AudioWorklet, HTML::main_thread_event_loop(), nullptr,
        GC::create_function(HTML::main_thread_event_loop().heap(), [session_id, module_id, required_generation, success, error_name, error_message, failed_processor_registrations] {
            handle_worklet_module_evaluated(session_id, module_id, required_generation, success, error_name, error_message, failed_processor_registrations);
        }));
}

void install_worklet_event_callbacks(SessionClientOfWebAudioWorker& client)
{
    client.on_worklet_processor_error = [](u64 session_id, u64 node_id) {
        enqueue_worklet_processor_error_task(session_id, node_id);
    };
    client.on_worklet_processor_registered = [](u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation) {
        enqueue_worklet_processor_registration_task(session_id, name, descriptors, generation);
    };
    client.on_worklet_module_evaluated = [](u64 session_id, u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> const& failed_processor_registrations) {
        enqueue_worklet_module_evaluated_task(session_id, module_id, required_generation, success, error_name, error_message, failed_processor_registrations);
    };
}

void note_worklet_processor_registration(u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation)
{
    auto& state = s_registration_state_by_session.ensure(session_id, [] { return WorkletRegistrationState {}; });
    state.last_registration_generation = max(state.last_registration_generation, generation);
    state.registered_processor_descriptors.set(name, descriptors);
    state.failed_processor_registrations.remove(name);
}

void note_worklet_failed_processor_registrations(u64 session_id, Vector<String> const& failed_processor_registrations)
{
    if (failed_processor_registrations.is_empty())
        return;

    auto& state = s_registration_state_by_session.ensure(session_id, [] { return WorkletRegistrationState {}; });
    for (auto const& name : failed_processor_registrations)
        state.failed_processor_registrations.set(name);
}

bool get_worklet_replay_state_for_generation(u64 session_id, u64 required_generation, HashMap<String, Vector<Web::WebAudio::AudioParamDescriptor>>& out_registered_descriptors, HashTable<String>& out_failed_registrations, u64& out_last_generation)
{
    auto it = s_registration_state_by_session.find(session_id);
    if (it == s_registration_state_by_session.end())
        return false;
    if (it->value.last_registration_generation < required_generation)
        return false;

    out_registered_descriptors = it->value.registered_processor_descriptors;
    out_failed_registrations = it->value.failed_processor_registrations;
    out_last_generation = it->value.last_registration_generation;
    return true;
}

void clear_worklet_session_state(u64 session_id)
{
    if (session_id == 0)
        return;
    s_registration_state_by_session.remove(session_id);
}

void clear_all_worklet_session_state()
{
    s_registration_state_by_session.clear();
}

}
