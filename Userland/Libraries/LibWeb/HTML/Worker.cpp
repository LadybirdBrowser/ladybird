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
#include <LibWeb/HTML/Worker.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(Worker);

// https://html.spec.whatwg.org/multipage/workers.html#dedicated-workers-and-the-worker-interface
Worker::Worker(String const& script_url, WorkerOptions const& options, DOM::Document& document)
    : DOM::EventTarget(document.realm())
    , m_script_url(script_url)
    , m_options(options)
    , m_document(&document)
{
}

void Worker::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Worker);
}

void Worker::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_outside_port);
    visitor.visit(m_agent);
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-worker
WebIDL::ExceptionOr<JS::NonnullGCPtr<Worker>> Worker::create(String const& script_url, WorkerOptions const& options, DOM::Document& document)
{
    dbgln_if(WEB_WORKER_DEBUG, "WebWorker: Creating worker with script_url = {}", script_url);

    // Returns a new Worker object. scriptURL will be fetched and executed in the background,
    // creating a new global environment for which worker represents the communication channel.
    // options can be used to define the name of that global environment via the name option,
    // primarily for debugging purposes. It can also ensure this new global environment supports
    // JavaScript modules (specify type: "module"), and if that is specified, can also be used
    // to specify how scriptURL is fetched through the credentials option.

    // FIXME: 1. The user agent may throw a "SecurityError" DOMException if the request violates
    // a policy decision (e.g. if the user agent is configured to not allow the page to start dedicated workers).
    // Technically not a fixme if our policy is not to throw errors :^)

    // 2. Let outside settings be the current settings object.
    auto& outside_settings = current_settings_object();

    // 3. Parse the scriptURL argument relative to outside settings.
    auto url = document.parse_url(script_url);

    // 4. If this fails, throw a "SyntaxError" DOMException.
    if (!url.is_valid()) {
        dbgln_if(WEB_WORKER_DEBUG, "WebWorker: Invalid URL loaded '{}'.", script_url);
        return WebIDL::SyntaxError::create(document.realm(), "url is not valid"_string);
    }

    // 5. Let worker URL be the resulting URL record.

    // 6. Let worker be a new Worker object.
    auto worker = document.heap().allocate<Worker>(document.realm(), script_url, options, document);

    // 7. Let outside port be a new MessagePort in outside settings's Realm.
    auto outside_port = MessagePort::create(outside_settings.realm());

    // 8. Associate the outside port with worker
    worker->m_outside_port = outside_port;
    worker->m_outside_port->set_worker_event_target(worker);

    // 9. Run this step in parallel:
    //    1. Run a worker given worker, worker URL, outside settings, outside port, and options.
    worker->run_a_worker(url, outside_settings, *outside_port, options);

    // 10. Return worker
    return worker;
}

// https://html.spec.whatwg.org/multipage/workers.html#run-a-worker
void Worker::run_a_worker(URL::URL& url, EnvironmentSettingsObject& outside_settings, JS::GCPtr<MessagePort> port, WorkerOptions const& options)
{
    // 1. Let is shared be true if worker is a SharedWorker object, and false otherwise.
    // FIXME: SharedWorker support

    // 2. Let owner be the relevant owner to add given outside settings.
    // FIXME: Support WorkerGlobalScope options
    if (!is<HTML::WindowEnvironmentSettingsObject>(outside_settings))
        TODO();

    // 3. Let parent worker global scope be null.
    // 4. If owner is a WorkerGlobalScope object (i.e., we are creating a nested dedicated worker),
    //    then set parent worker global scope to owner.
    // FIXME: Support for nested workers.

    // 5. Let unsafeWorkerCreationTime be the unsafe shared current time.

    // 6. Let agent be the result of obtaining a dedicated/shared worker agent given outside settings
    // and is shared. Run the rest of these steps in that agent.

    // Note: This spawns a new process to act as the 'agent' for the worker.
    m_agent = heap().allocate<WorkerAgent>(outside_settings.realm(), url, options, port, outside_settings);
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-worker-terminate
WebIDL::ExceptionOr<void> Worker::terminate()
{
    dbgln_if(WEB_WORKER_DEBUG, "WebWorker: Terminate");

    return {};
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-worker-postmessage
WebIDL::ExceptionOr<void> Worker::post_message(JS::Value message, StructuredSerializeOptions const& options)
{
    dbgln_if(WEB_WORKER_DEBUG, "WebWorker: Post Message: {}", message.to_string_without_side_effects());

    // The postMessage(message, transfer) and postMessage(message, options) methods on Worker objects act as if,
    // when invoked, they immediately invoked the respective postMessage(message, transfer) and
    // postMessage(message, options) on the port, with the same arguments, and returned the same return value.

    return m_outside_port->post_message(message, options);
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-worker-postmessage
WebIDL::ExceptionOr<void> Worker::post_message(JS::Value message, Vector<JS::Handle<JS::Object>> const& transfer)
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
