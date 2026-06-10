/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedWorker.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HTML/Worker.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SharedWorker);

WebIDL::ExceptionOr<GC::Ref<SharedWorker>> SharedWorker::create_for_constructor(JS::Realm& realm, TrustedTypes::TrustedScriptURLOrString const& script_url, Variant<String, WorkerOptions> options)
{
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(realm.global_object());
    VERIFY(global_scope);
    return create(*global_scope, script_url, move(options));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-sharedworker
WebIDL::ExceptionOr<GC::Ref<SharedWorker>> SharedWorker::create(WindowOrWorkerGlobalScopeMixin& global_scope, TrustedTypes::TrustedScriptURLOrString const& script_url, Variant<String, WorkerOptions> options_value)
{
    // 1. Let compliantScriptURL be the result of invoking the get trusted type compliant string algorithm with
    //    TrustedScriptURL, this's relevant global object, scriptURL, "SharedWorker constructor", and "script".
    auto const compliant_script_url = TRY(get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedScriptURL,
        HTML::relevant_global_object(global_scope),
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
    auto& outside_settings = HTML::relevant_settings_object(global_scope);

    // 4. Let urlRecord be the result of encoding-parsing a URL given compliantScriptURL, relative to outsideSettings.
    auto url = outside_settings.encoding_parse_url(compliant_script_url.to_utf8_but_should_be_ported_to_utf16());

    // 5. If urlRecord is failure, then throw a "SyntaxError" DOMException.
    if (!url.has_value())
        return WebIDL::SyntaxError::create("SharedWorker constructed with invalid URL"_utf16);

    // 6. Let outsidePort be a new MessagePort in outsideSettings's realm.
    auto outside_port = MessagePort::create(global_scope.this_impl());

    // 10. Let worker be this.
    // AD-HOC: We do this first so that we can use `this`.

    // 7. Set this's port to outsidePort.
    auto worker = GC::Heap::the().allocate<SharedWorker>(
        global_scope.this_impl(), url.release_value(), options, outside_port);

    // 10. Let worker be this.
    // NB: This is done earlier.

    // 11. Enqueue the following steps to the shared worker manager:
    // The actual shared worker manager is owned by the UI process. It will either start a new
    // WebWorker agent or transfer this port to an existing same-key SharedWorkerGlobalScope.
    run_a_worker(worker, worker->m_script_url, outside_settings, outside_port, worker->m_options);

    return worker;
}

SharedWorker::SharedWorker(GC::Ref<DOM::EventTarget> relevant_global_object, URL::URL script_url,
    WorkerOptions options, MessagePort& port)
    : DOM::EventTarget()
    , m_script_url(move(script_url))
    , m_options(move(options))
    , m_port(port)
    , m_global_object(relevant_global_object)
{
}

SharedWorker::~SharedWorker() = default;

JS::Object& SharedWorker::relevant_global_object() const
{
    return HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

void SharedWorker::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_port);
    visitor.visit(m_global_object);
    visitor.visit(m_agent);
}

}
