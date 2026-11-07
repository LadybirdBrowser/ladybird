/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Diagnostics.h>
#include <AK/HashMap.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/Worker.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerAgentParent);

static HashMap<WorkerAgentOwnerToken, GC::Ref<WorkerAgentParent>>& worker_agent_parents()
{
    using Parents = HashMap<WorkerAgentOwnerToken, GC::Ref<WorkerAgentParent>>;
    // NB: Free what this thread kept when it exits.
#ifdef AK_COMPILER_CLANG
    AK_IGNORE_DIAGNOSTIC("-Wexit-time-destructors", static thread_local Parents map)
#else
    static thread_local Parents map;
#endif
    return map;
}

static void report_worker_exception(WindowOrWorkerGlobalScopeMixin& window_or_worker, ErrorEvent& error)
{
    auto& realm = relevant_realm(window_or_worker);
    auto wrapped_error = Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { error });
    ErrorInformation error_info {
        .message = error.message(),
        .filename = error.filename(),
        .error = wrapped_error,
        .lineno = error.lineno(),
        .colno = error.colno(),
    };
    report_an_exception(relevant_global_object(window_or_worker), wrapped_error, move(error_info), OmitError::Yes);
}

GC::Ref<WorkerAgentParent> WorkerAgentParent::create(URL::URL url, WorkerOptions const& options, bool extended_lifetime,
    GC::Ptr<MessagePort> outside_port, GC::Ref<EnvironmentSettingsObject> outside_settings,
    GC::Ref<DOM::EventTarget> worker_event_target, AgentType agent_type)
{
    auto agent = GC::Heap::the().allocate<WorkerAgentParent>(move(url), options, extended_lifetime, outside_port, outside_settings, worker_event_target, agent_type);
    agent->start();
    return agent;
}

WorkerAgentParent::WorkerAgentParent(URL::URL url, WorkerOptions const& options, bool extended_lifetime, GC::Ptr<MessagePort> outside_port,
    GC::Ref<EnvironmentSettingsObject> outside_settings, GC::Ref<DOM::EventTarget> worker_event_target,
    AgentType agent_type)
    : m_worker_options(options)
    , m_extended_lifetime(extended_lifetime)
    , m_agent_type(agent_type)
    , m_url(move(url))
    , m_owner_token(next_owner_token())
    , m_outside_port(outside_port)
    , m_outside_settings(outside_settings)
    , m_worker_event_target(worker_event_target)
{
}

void WorkerAgentParent::start()
{
    auto& realm = m_outside_settings->realm();
    m_outside_settings->add_owned_worker_agent(*this);

    m_message_port = MessagePort::create(realm);
    m_message_port->entangle_with(*m_outside_port);

    // NB: A dedicated worker always runs in the process of its owner, and the browser process picks that of a shared one.
    auto [data_holder, parked_end_token] = MUST(m_message_port->transfer_to_worker(realm,
        m_agent_type == AgentType::DedicatedWorker ? MessagePort::WorkerMayBeInOtherProcess::No : MessagePort::WorkerMayBeInOtherProcess::Yes));
    m_parked_end_token = parked_end_token;

    // FIXME: Specification says this supposed to happen in step 11 of onComplete handler defined in https://html.spec.whatwg.org/multipage/workers.html#run-a-worker
    //        but that would require introducing a new IPC message type to communicate this from the worker to its owner,
    //        so let's do it here for now.
    m_outside_port->enable();

    auto serialized_outside_settings = m_outside_settings->serialize();

    // A worker has no display connection of its own, so capture the spawning page's
    // rendering rate to let the worker pace its rendering updates to match.
    auto maximum_frames_per_second = [&]() -> double {
        auto& global = m_outside_settings->global_object();
        if (auto* window = window_from_global_object(global))
            return window->page().client().maximum_frames_per_second();
        if (auto* worker_global_scope = Bindings::worker_global_scope_from_global_object(global))
            return worker_global_scope->page()->client().maximum_frames_per_second();
        return 60.0;
    }();

    // 8. Let callerIsSecureContext be true if outside settings is a secure context; otherwise, false.
    // 9. Let outsideStorageKey be the result of running obtain a storage key for non-storage purposes
    //    given outsideSettings.
    // NB: The browser process obtains outsideStorageKey from outsideSettings, as it holds that environment too.
    WorkerAgentStartRequest request {
        .url = m_url,
        .agent_type = m_agent_type,
        .type = m_worker_options.type,
        .credentials = m_worker_options.credentials,
        .name = m_worker_options.name.to_utf8(),
        .extended_lifetime = m_extended_lifetime,
        .outside_port = move(data_holder),
        .outside_settings = serialized_outside_settings,
        .caller_is_secure_context = is_secure_context(*m_outside_settings),
        .maximum_frames_per_second = maximum_frames_per_second,
        .owner_token = m_owner_token,
    };

    worker_agent_parents().set(m_owner_token, *this);
    auto& page_client = Bindings::principal_host_defined_page(realm).client();

    if (m_agent_type == AgentType::DedicatedWorker) {
        m_dedicated_worker_thread_link = page_client.start_dedicated_worker(move(request));
        if (!m_dedicated_worker_thread_link) {
            dispatch_error_event();
            forget_agent();
        }
        return;
    }

    // NB: This blocking IPC call may launch another process.
    //    If spinning the event loop for this can cause other javascript to execute, we're in trouble.
    auto started_agent = page_client.start_worker_agent(move(request));
    m_agent_id = started_agent.agent_id;
    if (m_agent_id == 0) {
        dispatch_error_event();
        forget_agent();
        return;
    }

    // NB: This is before any script can use the port, so that it finds out right now whether its peer is in this process.
    if (!started_agent.is_hosted_in_this_process)
        give_up_on_parked_end();
}

void WorkerAgentParent::give_up_on_parked_end()
{
    MessagePort::give_up_on_parked_end(exchange(m_parked_end_token, 0));
}

void WorkerAgentParent::did_fail_loading_worker_script(WorkerAgentOwnerToken owner_token)
{
    auto parent = worker_agent_parents().find(owner_token);
    if (parent == worker_agent_parents().end())
        return;
    auto agent_parent = parent->value;
    agent_parent->dispatch_error_event();
    agent_parent->forget_agent();
}

void WorkerAgentParent::did_report_worker_exception(WorkerAgentOwnerToken owner_token, Utf16String message, Utf16String filename, u32 lineno, u32 colno)
{
    auto parent = worker_agent_parents().find(owner_token);
    if (parent == worker_agent_parents().end())
        return;
    parent->value->dispatch_worker_exception(move(message), move(filename), lineno, colno);
}

void WorkerAgentParent::did_report_content_security_policy_violation(WorkerAgentOwnerToken owner_token, u64 policy_index, String effective_directive, URL::URL resource)
{
    auto parent = worker_agent_parents().find(owner_token);
    if (parent == worker_agent_parents().end())
        return;
    parent->value->report_content_security_policy_violation(policy_index, move(effective_directive), move(resource));
}

void WorkerAgentParent::did_close_worker(WorkerAgentOwnerToken owner_token)
{
    auto parent = worker_agent_parents().find(owner_token);
    if (parent == worker_agent_parents().end())
        return;
    parent->value->forget_agent();
}

void WorkerAgentParent::did_worker_agent_die(WorkerAgentOwnerToken owner_token)
{
    auto parent = worker_agent_parents().find(owner_token);
    if (parent == worker_agent_parents().end())
        return;
    auto agent_parent = parent->value;
    agent_parent->dispatch_error_event();
    agent_parent->forget_agent();
}

void WorkerAgentParent::terminate()
{
    if (auto dedicated_worker_thread_link = m_dedicated_worker_thread_link) {
        forget_agent();
        dedicated_worker_thread_link->terminate();
        return;
    }

    if (m_agent_id == 0)
        return;

    auto agent_id = m_agent_id;
    forget_agent();
    Bindings::principal_host_defined_page(m_outside_settings->realm()).client().close_worker_agent(agent_id, m_owner_token);
}

void WorkerAgentParent::forget_agent()
{
    m_agent_id = 0;
    m_dedicated_worker_thread_link = nullptr;
    give_up_on_parked_end();
    worker_agent_parents().remove(m_owner_token);
    m_outside_settings->remove_owned_worker_agent(*this);
}

void WorkerAgentParent::dispatch_error_event()
{
    // See: https://html.spec.whatwg.org/multipage/workers.html#worker-processing-model, onComplete handler for fetching script.
    // 1. Queue a global task on the DOM manipulation task source given worker's relevant global object to fire an event named error at worker.
    auto outside_global_object = GC::Ref { m_outside_settings->global_object() };
    auto event_target = GC::Ref { *m_worker_event_target };
    queue_global_task(Task::Source::DOMManipulation, *outside_global_object,
        GC::create_function(GC::Heap::the(), GC::captures(event_target, outside_global_object), [](auto& event_target, auto& outside_global_object) {
            event_target->dispatch_event(DOM::Event::create(
                EventNames::error,
                HighResolutionTime::current_high_resolution_time(*outside_global_object)));
        }));
}

void WorkerAgentParent::dispatch_worker_exception(Utf16String message, Utf16String filename, u32 lineno, u32 colno)
{
    // https://html.spec.whatwg.org/multipage/webappapis.html#report-an-exception
    // 7.2: If global implements DedicatedWorkerGlobalScope, queue a global task on the DOM manipulation task source with the global's associated Worker's relevant global object to run these steps:
    queue_global_task(Task::Source::DOMManipulation, m_outside_settings->global_object(), GC::create_function(GC::Heap::the(), GC::captures(*this, move(message), move(filename), lineno, colno), [](WorkerAgentParent& self, Utf16String& message, Utf16String& filename, u32 lineno, u32 colno) {
        auto& realm = self.m_outside_settings->realm();

        // 2. Set notHandled to the result of firing an event named error at workerObject, using ErrorEvent, with the
        //    cancelable attribute initialized to true, and additional attributes initialized according to errorInfo.
        ErrorEventInit event_init {};
        event_init.cancelable = true;
        event_init.message = message;
        event_init.filename = filename;
        event_init.lineno = lineno;
        event_init.colno = colno;
        event_init.error = JS::js_null();
        auto error = ErrorEvent::create(EventNames::error, event_init, HighResolutionTime::current_high_resolution_time(realm.global_object()));
        bool not_handled = self.m_worker_event_target->dispatch_event(error);

        // 3. If notHandled is true, then report exception for workerObject's relevant global object with omitError set to true.
        if (not_handled) {
            auto* window_or_worker = window_or_worker_global_scope_from_global_object(self.m_outside_settings->global_object());
            VERIFY(window_or_worker);
            report_worker_exception(*window_or_worker, error);
        }
    }));
}

// NB: The worker fetches its script with outside settings as the request's client, from a snapshot of it on the
//     worker's thread. The violations of its policies are reported here, where outside settings' global object is.
void WorkerAgentParent::report_content_security_policy_violation(u64 policy_index, String effective_directive, URL::URL resource)
{
    // NB: A worker's script requests can violate only worker-src, and a module worker's json and text imports
    //     connect-src.
    auto directive = Utf16FlyString::from_utf8(effective_directive);
    if (directive != ContentSecurityPolicy::Directives::Names::WorkerSrc && directive != ContentSecurityPolicy::Directives::Names::ConnectSrc)
        return;
    ContentSecurityPolicy::report_a_violation_found_in_another_process(*m_outside_settings, policy_index, directive, move(resource));
}

WorkerAgentOwnerToken WorkerAgentParent::next_owner_token()
{
    // NB: The client of the owner tells workers apart by their tokens, so these stay unique across the agents of a process.
    static Atomic<WorkerAgentOwnerToken, AK::MemoryOrder::memory_order_relaxed> s_next_owner_token = 0;
    return s_next_owner_token.fetch_add(1) + 1;
}

void WorkerAgentParent::finalize()
{
    Base::finalize();

    worker_agent_parents().remove(m_owner_token);

    if (!heap().is_collecting_everything())
        VERIFY(m_agent_id == 0 && !m_dedicated_worker_thread_link);
}

void WorkerAgentParent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_message_port);
    visitor.visit(m_outside_port);
    visitor.visit(m_outside_settings);
    visitor.visit(m_worker_event_target);
}

void WorkerAgentParent::rtc_transform_init(u64 transform_id, SerializedTransferRecord options_record)
{
    if (!m_dedicated_worker_thread_link)
        return;
    options_record.serialized.same_agent_side_table = nullptr;
    m_dedicated_worker_thread_link->rtc_transform_init(transform_id, move(options_record));
}

void WorkerAgentParent::rtc_transform_teardown(u64 transform_id)
{
    m_transform_frame_callbacks.remove(transform_id);
    if (!m_dedicated_worker_thread_link)
        return;
    m_dedicated_worker_thread_link->rtc_transform_teardown(transform_id);
}

void WorkerAgentParent::rtc_transform_encoded_audio_frame(u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number)
{
    if (!m_dedicated_worker_thread_link)
        return;
    m_dedicated_worker_thread_link->rtc_transform_encoded_audio_frame(transform_id, move(payload), ssrc, payload_type, rtp_timestamp, sequence_number);
}

void WorkerAgentParent::register_transform_frame_callback(u64 transform_id, TransformFrameWrittenCallback callback)
{
    m_transform_frame_callbacks.set(transform_id, move(callback));
}

void WorkerAgentParent::unregister_transform_frame_callback(u64 transform_id)
{
    m_transform_frame_callbacks.remove(transform_id);
}

void WorkerAgentParent::did_write_rtc_transform_encoded_audio_frame(WorkerAgentOwnerToken owner_token, u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number)
{
    auto parent = worker_agent_parents().find(owner_token);
    if (parent == worker_agent_parents().end())
        return;
    if (auto it = parent->value->m_transform_frame_callbacks.find(transform_id); it != parent->value->m_transform_frame_callbacks.end())
        it->value(move(payload), ssrc, payload_type, rtp_timestamp, sequence_number);
}

}
