/*
 * Copyright (c) 2022, Ben Abraham <ben.d.abraham@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WorkerPrototype.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/WindowEnvironmentSettingsObject.h>
#include <LibWeb/HTML/SharedWorker.h>
#include <LibWeb/HTML/Worker.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Worker);

// https://html.spec.whatwg.org/multipage/workers.html#dedicated-workers-and-the-worker-interface
Worker::Worker(JS::Realm& realm, String const& script_url, WorkerOptions const& options)
    : DOM::EventTarget(realm)
    , m_script_url(script_url)
    , m_options(options)
{
}

void Worker::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Worker);
    Base::initialize(realm);
}

void Worker::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_outside_port);
    visitor.visit(m_agent);
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-worker
// https://whatpr.org/html/9893/workers.html#dom-worker
WebIDL::ExceptionOr<GC::Ref<Worker>> Worker::create(JS::Realm& realm, TrustedTypes::TrustedScriptURLOrString const& script_url, WorkerOptions const& options)
{
    // Returns a new Worker object. scriptURL will be fetched and executed in the background,
    // creating a new global environment for which worker represents the communication channel.
    // options can be used to define the name of that global environment via the name option,
    // primarily for debugging purposes. It can also ensure this new global environment supports
    // JavaScript modules (specify type: "module"), and if that is specified, can also be used
    // to specify how scriptURL is fetched through the credentials option.

    // 1. Let compliantScriptURL be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedScriptURL, this's relevant global object, scriptURL, "Worker constructor", and "script".
    auto const compliant_script_url = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedScriptURL,
        realm.global_object(),
        script_url,
        TrustedTypes::InjectionSink::Worker_constructor,
        TrustedTypes::Script.to_string()));

    dbgln_if(WEB_WORKER_DEBUG, "WebWorker: Creating worker with compliant_script_url = {}", compliant_script_url);

    // 2. Let outsideSettings be this's relevant settings object.
    // NOTE: We don't have a `this` yet, so we use the definition: the environment setting object of the realm.
    auto& outside_settings = HTML::principal_realm_settings_object(realm);

    // 3. Let workerURL be the result of encoding-parsing a URL given compliantScriptURL, relative to outsideSettings.
    auto worker_url = outside_settings.encoding_parse_url(compliant_script_url.to_utf8_but_should_be_ported_to_utf16());

    // 4. If workerURL is failure, then throw a "SyntaxError" DOMException.
    if (!worker_url.has_value()) {
        dbgln_if(WEB_WORKER_DEBUG, "WebWorker: Invalid URL loaded '{}'.", compliant_script_url);
        return WebIDL::SyntaxError::create(realm, "url is not valid"_utf16);
    }

    // 5. Let outsidePort be a new MessagePort in outsideSettings's realm.
    auto outside_port = MessagePort::create(outside_settings.realm());

    // 8. Let worker be this.
    // AD-HOC: AD-HOC: We do this first so that we can use `this`.
    auto worker = realm.create<Worker>(realm, compliant_script_url.to_utf8_but_should_be_ported_to_utf16(), options);

    // 6. Set outsidePort's message event target to this.
    outside_port->set_worker_event_target(worker);

    // 7. Set this's outside port to outsidePort.
    worker->m_outside_port = outside_port;

    // 8. Let worker be this.
    // NB: This is done earlier.

    // 9. Run this step in parallel:
    // 1. Run a worker given worker, workerURL, outsideSettings, outsidePort, and options.
    run_a_worker(worker, worker_url.value(), outside_settings, *outside_port, options);

    return worker;
}

// https://html.spec.whatwg.org/multipage/workers.html#run-a-worker
void run_a_worker(Variant<GC::Ref<Worker>, GC::Ref<SharedWorker>> worker, URL::URL& url, EnvironmentSettingsObject& outside_settings, GC::Ptr<MessagePort> port, WorkerOptions const& options)
{
    // 1. Let is shared be true if worker is a SharedWorker object, and false otherwise.
    Bindings::AgentType agent_type = worker.has<GC::Ref<SharedWorker>>() ? Bindings::AgentType::SharedWorker : Bindings::AgentType::DedicatedWorker;

    // FIXME: 2. Let owner be the relevant owner to add given outside settings.

    // 3. Let unsafeWorkerCreationTime be the unsafe shared current time.

    // 4. Let agent be the result of obtaining a dedicated/shared worker agent given outside settings and is shared.
    //    Run the rest of these steps in that agent.

    // Note: This spawns a new process to act as the 'agent' for the worker.
    auto agent = outside_settings.realm().create<WorkerAgentParent>(url, options, port, outside_settings, agent_type);
    worker.visit([&](auto worker) { worker->set_agent(agent); });
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-worker-terminate
WebIDL::ExceptionOr<void> Worker::terminate()
{
    dbgln_if(WEB_WORKER_DEBUG, "WebWorker: Terminate");

    // FIXME: The terminate() method steps are to terminate a worker given this's worker.
    return {};
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-worker-postmessage
WebIDL::ExceptionOr<void> Worker::post_message(JS::Value message, StructuredSerializeOptions const& options)
{
    dbgln_if(WEB_WORKER_DEBUG, "WebWorker: Post Message: {}", message);

    // The postMessage(message, transfer) and postMessage(message, options) methods on Worker objects act as if,
    // when invoked, they immediately invoked the respective postMessage(message, transfer) and
    // postMessage(message, options) on the port, with the same arguments, and returned the same return value.

    return m_outside_port->post_message(message, options);
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-worker-postmessage
WebIDL::ExceptionOr<void> Worker::post_message(JS::Value message, Vector<GC::Root<JS::Object>> const& transfer)
{
    // The postMessage(message, transfer) and postMessage(message, options) methods on Worker objects act as if,
    // when invoked, they immediately invoked the respective postMessage(message, transfer) and
    // postMessage(message, options) on the port, with the same arguments, and returned the same return value.

    return m_outside_port->post_message(message, transfer);
}

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                    \
    void Worker::set_##attribute_name(WebIDL::CallbackType* value) \
    {                                                              \
        set_event_handler_attribute(event_name, move(value));      \
    }                                                              \
    WebIDL::CallbackType* Worker::attribute_name()                 \
    {                                                              \
        return event_handler_attribute(event_name);                \
    }
ENUMERATE_WORKER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

} // namespace Web::HTML
