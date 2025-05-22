/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SharedWorkerPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedWorker.h>
#include <LibWeb/HTML/SharedWorkerGlobalScope.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/Worker.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SharedWorker);

// https://html.spec.whatwg.org/multipage/workers.html#dom-sharedworker
WebIDL::ExceptionOr<GC::Ref<SharedWorker>> SharedWorker::construct_impl(JS::Realm& realm, String const& script_url, Variant<String, WorkerOptions>& options_value)
{
    // FIXME: 1. Let compliantScriptURL be the result of invoking the Get Trusted Type compliant string algorithm with
    //           TrustedScriptURL, this's relevant global object, scriptURL, "SharedWorker constructor", and "script".
    auto const& compliant_script_url = script_url;

    // 2. If options is a DOMString, set options to a new WorkerOptions dictionary whose name member is set to the value
    //    of options and whose other members are set to their default values.
    auto options = options_value.visit(
        [&](String& options) {
            return WorkerOptions { .name = move(options) };
        },
        [&](WorkerOptions& options) {
            return move(options);
        });

    // 3. Let outside settings be the current settings object.
    auto& outside_settings = current_principal_settings_object();

    // 4. Let urlRecord be the result of encoding-parsing a URL given compliantScriptURL, relative to outside settings.
    auto url = outside_settings.encoding_parse_url(compliant_script_url);

    // 5. If urlRecord is failure, then throw a "SyntaxError" DOMException.
    if (!url.has_value())
        return WebIDL::SyntaxError::create(realm, "SharedWorker constructed with invalid URL"_string);

    // 7. Let outside port be a new MessagePort in outside settings's realm.
    // NOTE: We do this first so that we can store the port as a GC::Ref.
    auto outside_port = MessagePort::create(outside_settings.realm());

    // 6. Let worker be a new SharedWorker object.
    // 8. Assign outside port to the port attribute of worker.
    auto worker = realm.create<SharedWorker>(realm, url.release_value(), options, outside_port);

    // 9. Let callerIsSecureContext be true if outside settings is a secure context; otherwise, false.
    auto caller_is_secure_context = HTML::is_secure_context(outside_settings);

    // 10. Let outside storage key be the result of running obtain a storage key for non-storage purposes given outside settings.
    auto outside_storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(outside_settings);

    // 11. Enqueue the following steps to the shared worker manager:
    // FIXME: "A user agent has an associated shared worker manager which is the result of starting a new parallel queue."
    //        We just use the singular global event loop for now.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [worker, outside_port, &outside_settings, caller_is_secure_context, outside_storage_key = move(outside_storage_key)]() mutable {
        // 1. Let worker global scope be null.
        GC::Ptr<SharedWorkerGlobalScope> worker_global_scope;

        // 2. For each scope in the list of all SharedWorkerGlobalScope objects:
        for (auto& scope : all_shared_worker_global_scopes()) {
            // 1. Let worker storage key be the result of running obtain a storage key for non-storage purposes given
            //    scope's relevant settings object.
            auto worker_storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(HTML::relevant_settings_object(scope));

            // 2. If all of the following are true:
            if (
                // * worker storage key equals outside storage key;
                worker_storage_key == outside_storage_key

                // * scope's closing flag is false;
                && !scope->is_closing()

                // * scope's constructor url equals urlRecord; and
                && scope->url() == worker->m_script_url

                // * scope's name equals the value of options's name member,
                && scope->name() == worker->m_options.name)
            // then:
            {
                // 1. Set worker global scope to scope.
                worker_global_scope = scope;

                // 2. Break.
                break;
            }
        }

        // FIXME: 3. If worker global scope is not null, but the user agent has been configured to disallow communication
        //           between the worker represented by the worker global scope and the scripts whose settings object is outside
        //           settings, then set worker global scope to null.
        // FIXME: 4. If worker global scope is not null, then check if worker global scope's type and credentials match the
        //           options values. If not, queue a task to fire an event named error and abort these steps.

        // 5. If worker global scope is not null, then run these subsubsteps:
        if (worker_global_scope) {
            // 1. Let settings object be the relevant settings object for worker global scope.
            auto& settings_object = HTML::relevant_settings_object(*worker_global_scope);

            // 2. Let workerIsSecureContext be true if settings object is a secure context; otherwise, false.
            auto worker_is_secure_context = HTML::is_secure_context(settings_object);

            // 3. If workerIsSecureContext is not callerIsSecureContext, then queue a task to fire an event named error
            //    at worker and abort these steps. [SECURE-CONTEXTS]
            if (worker_is_secure_context != caller_is_secure_context) {
                queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(worker->heap(), [worker]() {
                    worker->dispatch_event(DOM::Event::create(worker->realm(), HTML::EventNames::error));
                }));

                return;
            }

            // FIXME: 4. Associate worker with worker global scope.

            // 5. Let inside port be a new MessagePort in settings object's realm.
            auto inside_port = HTML::MessagePort::create(settings_object.realm());

            // 6. Entangle outside port and inside port.
            outside_port->entangle_with(inside_port);

            // 7. Queue a task, using the DOM manipulation task source, to fire an event named connect at worker global
            //    scope, using MessageEvent, with the data attribute initialized to the empty string, the ports attribute
            //    initialized to a new frozen array containing only inside port, and the source attribute initialized to
            //    inside port.
            queue_a_task(HTML::Task::Source::DOMManipulation, nullptr, nullptr, GC::create_function(worker->heap(), [worker_global_scope, inside_port]() {
                auto& realm = worker_global_scope->realm();

                MessageEventInit init;
                init.data = JS::PrimitiveString::create(realm.vm(), String {});
                init.ports.append(inside_port);
                init.source = inside_port;

                worker_global_scope->dispatch_event(MessageEvent::create(realm, HTML::EventNames::connect, init));
            }));

            // FIXME: 8. Append the relevant owner to add given outside settings to worker global scope's owner set.
        }
        // 6. Otherwise, in parallel, run a worker given worker, urlRecord, outside settings, outside port, and options.
        else {
            run_a_worker(worker, worker->m_script_url, outside_settings, outside_port, worker->m_options);
        }
    }));

    // 12. Return worker.
    return worker;
}

SharedWorker::SharedWorker(JS::Realm& realm, URL::URL script_url, WorkerOptions options, MessagePort& port)
    : DOM::EventTarget(realm)
    , m_script_url(move(script_url))
    , m_options(move(options))
    , m_port(port)
{
}

SharedWorker::~SharedWorker() = default;

void SharedWorker::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SharedWorker);
    Base::initialize(realm);
}

void SharedWorker::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_port);
    visitor.visit(m_agent);
}

}
