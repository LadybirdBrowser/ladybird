/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/Worker.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerAgentParent);

static HashMap<WorkerAgentOwnerToken, GC::Ref<WorkerAgentParent>>& worker_agent_parents()
{
    static HashMap<WorkerAgentOwnerToken, GC::Ref<WorkerAgentParent>> map;
    return map;
}

WorkerAgentParent::WorkerAgentParent(URL::URL url, Bindings::WorkerOptions const& options, GC::Ptr<MessagePort> outside_port, GC::Ref<EnvironmentSettingsObject> outside_settings, GC::Ref<DOM::EventTarget> worker_event_target, Bindings::AgentType agent_type)
    : m_worker_options(options)
    , m_agent_type(agent_type)
    , m_url(move(url))
    , m_owner_token(next_owner_token())
    , m_outside_port(outside_port)
    , m_outside_settings(outside_settings)
    , m_worker_event_target(worker_event_target)
{
}

void WorkerAgentParent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    m_outside_settings->keep_worker_agent_alive_while_starting(*this);

    m_message_port = MessagePort::create(realm);
    m_message_port->entangle_with(*m_outside_port);

    TransferDataEncoder data_holder;
    MUST(m_message_port->transfer_steps(data_holder));

    // FIXME: Specification says this supposed to happen in step 11 of onComplete handler defined in https://html.spec.whatwg.org/multipage/workers.html#run-a-worker
    //        but that would require introducing a new IPC message type to communicate this from WebWorker to WebContent process,
    //        so let's do it here for now.
    m_outside_port->start();

    auto serialized_outside_settings = m_outside_settings->serialize();

    // 8. Let callerIsSecureContext be true if outside settings is a secure context; otherwise, false.
    // 9. Let outsideStorageKey be the result of running obtain a storage key for non-storage purposes
    //    given outsideSettings.
    WorkerAgentStartRequest request {
        .url = m_url,
        .agent_type = m_agent_type,
        .type = m_worker_options.type,
        .credentials = m_worker_options.credentials,
        .name = m_worker_options.name,
        .outside_port = move(data_holder),
        .outside_settings = serialized_outside_settings,
        .storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(*m_outside_settings),
        .caller_is_secure_context = is_secure_context(*m_outside_settings),
        .owner_token = m_owner_token,
    };

    // NOTE: This blocking IPC call may launch another process.
    //    If spinning the event loop for this can cause other javascript to execute, we're in trouble.
    worker_agent_parents().set(m_owner_token, *this);
    m_agent_id = Bindings::principal_host_defined_page(realm).client().start_worker_agent(move(request));
}

void WorkerAgentParent::did_finish_loading_worker_script(WorkerAgentOwnerToken owner_token)
{
    auto parent = worker_agent_parents().find(owner_token);
    if (parent == worker_agent_parents().end())
        return;
    parent->value->release_startup_keep_alive();
}

void WorkerAgentParent::did_fail_loading_worker_script(WorkerAgentOwnerToken owner_token)
{
    auto parent = worker_agent_parents().find(owner_token);
    if (parent == worker_agent_parents().end())
        return;
    parent->value->dispatch_error_event();
    parent->value->release_startup_keep_alive();
}

void WorkerAgentParent::did_report_worker_exception(WorkerAgentOwnerToken owner_token, String message, String filename, u32 lineno, u32 colno)
{
    auto parent = worker_agent_parents().find(owner_token);
    if (parent == worker_agent_parents().end())
        return;
    parent->value->dispatch_worker_exception(move(message), move(filename), lineno, colno);
}

void WorkerAgentParent::did_close_worker(WorkerAgentOwnerToken owner_token)
{
    auto parent = worker_agent_parents().find(owner_token);
    if (parent == worker_agent_parents().end())
        return;
    parent->value->release_startup_keep_alive();
}

void WorkerAgentParent::release_startup_keep_alive()
{
    m_outside_settings->release_worker_agent_from_startup_keep_alive(*this);
}

void WorkerAgentParent::dispatch_error_event()
{
    // See: https://html.spec.whatwg.org/multipage/workers.html#worker-processing-model, onComplete handler for fetching script.
    // 1. Queue a global task on the DOM manipulation task source given worker's relevant global object to fire an event named error at worker.
    queue_global_task(Task::Source::DOMManipulation, m_outside_settings->global_object(), GC::create_function(m_outside_settings->heap(), [this] {
        m_worker_event_target->dispatch_event(DOM::Event::create(m_outside_settings->realm(), EventNames::error));
    }));
}

void WorkerAgentParent::dispatch_worker_exception(String message, String filename, u32 lineno, u32 colno)
{
    // https://html.spec.whatwg.org/multipage/webappapis.html#report-an-exception
    // 7.2: If global implements DedicatedWorkerGlobalScope, queue a global task on the DOM manipulation task source with the global's associated Worker's relevant global object to run these steps:
    queue_global_task(Task::Source::DOMManipulation, m_outside_settings->global_object(), GC::create_function(m_outside_settings->heap(), [this, message = move(message), filename = move(filename), lineno, colno]() {
        auto& realm = m_outside_settings->realm();

        // 2. Set notHandled to the result of firing an event named error at workerObject, using ErrorEvent, with the
        //    cancelable attribute initialized to true, and additional attributes initialized according to errorInfo.
        Bindings::ErrorEventInit event_init {};
        event_init.cancelable = true;
        event_init.message = message;
        event_init.filename = filename;
        event_init.lineno = lineno;
        event_init.colno = colno;
        event_init.error = JS::js_null();
        auto error = ErrorEvent::create(realm, EventNames::error, event_init);
        bool not_handled = m_worker_event_target->dispatch_event(error);

        // 3. If notHandled is true, then report exception for workerObject's relevant global object with omitError set to true.
        if (not_handled)
            as<WindowOrWorkerGlobalScopeMixin>(m_outside_settings->global_object()).report_an_exception(error, WindowOrWorkerGlobalScopeMixin::OmitError::Yes);
    }));
}

WorkerAgentOwnerToken WorkerAgentParent::next_owner_token()
{
    static WorkerAgentOwnerToken s_next_owner_token = 0;
    return ++s_next_owner_token;
}

void WorkerAgentParent::finalize()
{
    Base::finalize();

    worker_agent_parents().remove(m_owner_token);

    if (m_agent_id != 0)
        Bindings::principal_host_defined_page(m_outside_settings->realm()).client().close_worker_agent(m_agent_id, m_owner_token);
}

void WorkerAgentParent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_message_port);
    visitor.visit(m_outside_port);
    visitor.visit(m_outside_settings);
    visitor.visit(m_worker_event_target);
}

}
