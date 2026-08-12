/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Worklet.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Worklet.h>
#include <LibWeb/HTML/WorkletGlobalScope.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Worklet);

Worklet::Worklet(GC::Ref<DOM::EventTarget> relevant_global)
    : m_relevant_global(relevant_global)
{
}

Worklet::~Worklet() = default;

void Worklet::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_relevant_global);
    visitor.visit(m_global_scope);
}

// https://html.spec.whatwg.org/multipage/worklets.html#dom-worklet-addmodule
GC::Ref<WebIDL::Promise> Worklet::add_module(Utf16String const& module_url, Bindings::WorkletOptions const& options)
{
    // 1. Let outsideSettings be the relevant settings object of this.
    auto& outside_settings = relevant_settings_object(relevant_window_or_worker_global_scope(*m_relevant_global));
    auto& realm = outside_settings.realm();

    // 2. Let moduleURLRecord be the result of encoding-parsing a URL given moduleURL, relative to outsideSettings.
    auto module_url_record = outside_settings.encoding_parse_url(module_url);

    // 3. If moduleURLRecord is failure, then return a promise rejected with a "SyntaxError" DOMException.
    if (!module_url_record.has_value()) {
        return WebIDL::create_rejected_promise_from_exception(realm,
            WebIDL::SyntaxError::create(Utf16String::formatted("addModule: '{}' is not a valid URL", module_url)));
    }

    // 4. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 5. Run the following steps in parallel:
    //    1. If this's global scopes is empty, then:
    //       1. For each i in [0, this's expected number of global scopes): create a worklet global scope given this.
    //       2. Wait for all steps of the creations to complete.
    // AD-HOC: Our worklet types have exactly one global scope, created synchronously; and module
    //         fetching below is already asynchronous, so no "in parallel" machinery is needed here.
    if (!m_global_scope)
        m_global_scope = create_global_scope();

    //    2. Let pendingTasks be this's global scopes's size.
    //    3. For each workletGlobalScope of this's global scopes, queue a global task on the networking task
    //       source given workletGlobalScope to fetch and invoke a worklet script given workletGlobalScope,
    //       moduleURLRecord, credentialOptions, and promise.
    // AD-HOC: Collapsed to the single global scope; "fetch and invoke a worklet script" follows.

    // https://html.spec.whatwg.org/multipage/worklets.html#fetch-and-invoke-a-worklet-script
    // 1. Let insideSettings be workletGlobalScope's associated environment settings object.
    auto& inside_settings = m_global_scope->settings_object();

    // 2. Let script be the result of fetching a worklet script graph given moduleURLRecord, outsideSettings,
    //    workletType's fetch destination, credentialOptions's credentials, insideSettings, and the following
    //    options:...
    // FIXME: Plumb options.credentials through once fetch_worklet_module_worker_script_graph honors it.
    (void)options;
    auto on_complete = create_on_fetch_script_complete(realm.heap(),
        [promise, &realm](GC::Ptr<Script> script) {
            // 3. If script is null, then reject promise with an "AbortError" DOMException and abort these steps.
            TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
            if (!script) {
                WebIDL::reject_promise(realm, promise,
                    WebIDL::AbortError::create("Failed to fetch worklet module script"_utf16));
                return;
            }

            auto& module_script = as<ModuleScript>(*script);

            // 4. If script's error to rethrow is not null, then reject promise with script's error to rethrow
            //    and abort these steps.
            if (!module_script.error_to_rethrow().is_null()) {
                WebIDL::reject_promise(realm, promise, module_script.error_to_rethrow());
                return;
            }

            // 5. Run a module script given script. (This runs inside the worklet's own realm; Script::run
            //    enters the script's settings object's realm execution context.)
            auto evaluation_promise = module_script.run();
            VERIFY(evaluation_promise);

            // 6. Resolve promise with undefined.
            WebIDL::react_to_promise(*evaluation_promise,
                GC::create_function(realm.heap(), [promise, &realm](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
                    WebIDL::resolve_promise(realm, promise, JS::js_undefined());
                    return JS::js_undefined();
                }),
                GC::create_function(realm.heap(), [promise, &realm](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
                    WebIDL::reject_promise(realm, promise, reason);
                    return JS::js_undefined();
                }));
        });

    auto fetch_result = fetch_worklet_module_worker_script_graph(
        *module_url_record, outside_settings, worklet_destination(), inside_settings, nullptr, on_complete);
    if (fetch_result.is_exception())
        WebIDL::reject_promise_with_exception(realm, promise, fetch_result.release_error());

    // 6. Return promise.
    return promise;
}

}
