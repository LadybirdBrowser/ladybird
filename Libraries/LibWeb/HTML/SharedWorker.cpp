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
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SharedWorker);

// https://html.spec.whatwg.org/multipage/workers.html#dom-sharedworker
WebIDL::ExceptionOr<GC::Ref<SharedWorker>> SharedWorker::construct_impl(JS::Realm& realm, TrustedTypes::TrustedScriptURLOrString const& script_url, Variant<String, WorkerOptions>& options_value)
{
    // 1. Let compliantScriptURL be the result of invoking the get trusted type compliant string algorithm with
    //    TrustedScriptURL, this's relevant global object, scriptURL, "SharedWorker constructor", and "script".
    auto const compliant_script_url = TRY(get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedScriptURL,
        realm.global_object(),
        script_url,
        TrustedTypes::InjectionSink::SharedWorker_constructor,
        TrustedTypes::Script.to_string()));

    // 2. If options is a DOMString, set options to a new WorkerOptions dictionary whose name member is set to the
    //    value of options and whose other members are set to their default values.
    auto options = options_value.visit(
        [&](String& options) {
            return WorkerOptions { .name = move(options) };
        },
        [&](WorkerOptions& options) {
            return move(options);
        });

    // 3. Let outside settings be this's relevant settings object.
    // FIXME: We don't have a `this` yet, so use the current principal settings object, as the previous spec did.
    auto& outside_settings = current_principal_settings_object();

    // 4. Let urlRecord be the result of encoding-parsing a URL given compliantScriptURL, relative to outsideSettings.
    auto url = outside_settings.encoding_parse_url(compliant_script_url.to_utf8_but_should_be_ported_to_utf16());

    // 5. If urlRecord is failure, then throw a "SyntaxError" DOMException.
    if (!url.has_value())
        return WebIDL::SyntaxError::create(realm, "SharedWorker constructed with invalid URL"_utf16);

    // 6. Let outsidePort be a new MessagePort in outsideSettings's realm.
    auto outside_port = MessagePort::create(outside_settings.realm());

    // 10. Let worker be this.
    // AD-HOC: We do this first so that we can use `this`.

    // 7. Set this's port to outsidePort.
    auto worker = realm.create<SharedWorker>(realm, url.release_value(), options, outside_port);

    // 8. Let callerIsSecureContext be true if outside settings is a secure context; otherwise, false.
    auto caller_is_secure_context = HTML::is_secure_context(outside_settings);

    // 9. Let outsideStorageKey be the result of running obtain a storage key for non-storage purposes given outsideSettings.
    auto outside_storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(outside_settings);

    // 10. Let worker be this.
    // NB: This is done earlier.

    // 11. Enqueue the following steps to the shared worker manager:
    // FIXME: "A user agent has an associated shared worker manager which is the result of starting a new parallel queue."
    //        We just use the singular global event loop for now.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [worker, outside_port, &outside_settings, caller_is_secure_context, outside_storage_key = move(outside_storage_key)]() mutable {
        // 1. Let workerGlobalScope be null.
        GC::Ptr<SharedWorkerGlobalScope> worker_global_scope;

        // 2. For each scope in the list of all SharedWorkerGlobalScope objects:
        for (auto& scope : all_shared_worker_global_scopes()) {
            // 1. Let workerStorageKey be the result of running obtain a storage key for non-storage purposes given
            //    scope's relevant settings object.
            auto worker_storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(HTML::relevant_settings_object(scope));

            // 2. If all of the following are true:
            if (
                // * workerStorageKey equals outsideStorageKey;
                worker_storage_key == outside_storage_key

                // * scope's closing flag is false;
                && !scope->is_closing()

                // * scope's constructor URL equals urlRecord; and
                && scope->url() == worker->m_script_url

                // * scope's name equals options["name"],
                && scope->name() == worker->m_options.name)
            // then:
            {
                // 1. Set workerGlobalScope to scope.
                worker_global_scope = scope;

                // 2. Break.
                break;
            }
        }

        // FIXME: 3. If workerGlobalScope is not null, but the user agent has been configured to disallow communication between the worker represented by the workerGlobalScope and the scripts whose settings object is outsideSettings, then set workerGlobalScope to null.
        // FIXME: 4. If workerGlobalScope is not null, and any of the following are true: ...

        // 5. If workerGlobalScope is not null:
        if (worker_global_scope) {
            // 1. Let insideSettings be workerGlobalScope's relevant settings object.
            auto& inside_settings = relevant_settings_object(*worker_global_scope);

            // 2. Let workerIsSecureContext be true if insideSettings is a secure context; otherwise, false.
            auto worker_is_secure_context = is_secure_context(inside_settings);

            // 3. If workerIsSecureContext is not callerIsSecureContext:
            if (worker_is_secure_context != caller_is_secure_context) {
                // 1. Queue a global task on the DOM manipulation task source given worker's relevant global object to fire an event named error at worker.
                queue_global_task(Task::Source::DOMManipulation, relevant_global_object(worker), GC::create_function(worker->heap(), [worker]() {
                    worker->dispatch_event(DOM::Event::create(worker->realm(), EventNames::error));
                }));

                // 2. Abort these steps.
                return;
            }

            // FIXME: 4. Associate worker with workerGlobalScope.

            // 5. Let insidePort be a new MessagePort in insideSettings's realm.
            auto inside_port = HTML::MessagePort::create(inside_settings.realm());

            // 6. Entangle outsidePort and insidePort.
            outside_port->entangle_with(inside_port);

            // 7. Queue a global task on the DOM manipulation task source given workerGlobalScope to fire an event
            //   named connect at workerGlobalScope, using MessageEvent, with the data attribute initialized to the
            //   empty string, the ports attribute initialized to a new frozen array containing only insidePort, and
            //   the source attribute initialized to insidePort.
            queue_global_task(Task::Source::DOMManipulation, *worker_global_scope, GC::create_function(worker->heap(), [worker_global_scope, inside_port]() {
                auto& realm = worker_global_scope->realm();

                MessageEventInit init;
                init.data = JS::PrimitiveString::create(realm.vm(), String {});
                init.ports.append(inside_port);
                init.source = inside_port;

                worker_global_scope->dispatch_event(MessageEvent::create(realm, EventNames::connect, init));
            }));

            // FIXME: 8. Append the relevant owner to add given outsideSettings to workerGlobalScope's owner set.

        }
        // 6. Otherwise, in parallel, run a worker given worker, urlRecord, outsideSettings, outsidePort, and options.
        else {
            run_a_worker(worker, worker->m_script_url, outside_settings, outside_port, worker->m_options);
        }
    }));

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
