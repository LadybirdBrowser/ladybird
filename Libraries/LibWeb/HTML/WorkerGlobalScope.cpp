/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/ContentSecurityPolicy/Directives/HTMLIntegration.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/HTML/DedicatedWorkerGlobalScope.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerLocation.h>
#include <LibWeb/HTML/WorkerNavigator.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerGlobalScope);

WorkerGlobalScope::WorkerGlobalScope(GC::Ref<Web::Page> page)
    : DOM::EventTarget()
    , m_page(page)
{
}

WorkerGlobalScope::~WorkerGlobalScope() = default;

JS::Realm& WorkerGlobalScope::realm() const
{
    return Bindings::main_world_realm(*this);
}

void WorkerGlobalScope::initialize_web_interfaces_impl()
{
    WindowOrWorkerGlobalScopeMixin::initialize();

    m_navigator = WorkerNavigator::create(*this);
}

void WorkerGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    WindowOrWorkerGlobalScopeMixin::visit_edges(visitor);

    visitor.visit(m_location);
    visitor.visit(m_navigator);
    visitor.visit(m_internal_port);
    visitor.visit(m_page);
    visitor.visit(m_fonts);
    visitor.visit(m_font_computer);
    visitor.visit(m_policy_container);
}

void WorkerGlobalScope::finalize()
{
    Base::finalize();
    WindowOrWorkerGlobalScopeMixin::finalize();
}

void WorkerGlobalScope::set_internal_port(GC::Ref<MessagePort> port)
{
    m_internal_port = port;
    m_internal_port->set_worker_event_target(*this);
}

// https://html.spec.whatwg.org/multipage/workers.html#close-a-worker
void WorkerGlobalScope::close_a_worker()
{
    // 1. Discard any tasks that have been added to workerGlobal's relevant agent's event loop's task queues.
    relevant_settings_object(*this).responsible_event_loop().task_queue().remove_tasks_matching([](HTML::Task const&) {
        return true;
    });

    // 2. Set workerGlobal's closing flag to true. (This prevents any further tasks from being queued.)
    m_closing = true;
    relevant_settings_object(*this).responsible_event_loop().task_queue().discard_further_tasks();
}

// https://html.spec.whatwg.org/multipage/workers.html#importing-scripts-and-libraries
WebIDL::ExceptionOr<void> WorkerGlobalScope::import_scripts(Vector<Utf16String> const& urls, PerformTheFetchHook perform_fetch)
{
    // The algorithm may optionally be customized by supplying custom perform the fetch hooks,
    // which if provided will be used when invoking fetch a classic worker-imported script.
    // NOTE: Service Workers is an example of a specification that runs this algorithm with its own options for the perform the fetch hook.

    // FIXME: 1. If worker global scope's type is "module", throw a TypeError exception.

    // 2. Let settings object be the current settings object.
    auto& settings_object = HTML::current_settings_object();

    // 3. If urls is empty, return.
    if (urls.is_empty())
        return {};

    // 4. Let urlRecords be « ».
    Vector<URL::URL> url_records;
    url_records.ensure_capacity(urls.size());

    // 5. For each url of urls:
    for (auto const& url : urls) {
        // 1. Let urlRecord be the result of encoding-parsing a URL given url, relative to settings object.
        auto url_record = settings_object.encoding_parse_url(url);

        // 2. If urlRecord is failure, then throw a "SyntaxError" DOMException.
        if (!url_record.has_value())
            return WebIDL::SyntaxError::create("Invalid URL"_utf16);

        // 3. Append urlRecord to urlRecords.
        url_records.unchecked_append(url_record.release_value());
    }

    // 6. For each urlRecord of urlRecords:
    for (auto const& url_record : url_records) {
        // 1. Fetch a classic worker-imported script given urlRecord and settings object, passing along performFetch if provided.
        //    If this succeeds, let script be the result. Otherwise, rethrow the exception.
        auto classic_script = TRY(HTML::fetch_a_classic_worker_imported_script(url_record, settings_object, perform_fetch));

        // 2. Run the classic script script, with rethrow errors set to true.
        // NOTE: script will run until it either returns, fails to parse, fails to catch an exception,
        //       or gets prematurely aborted by the terminate a worker algorithm defined above.
        // If an exception was thrown or if the script was prematurely aborted, then abort all these steps,
        // letting the exception or aborting continue to be processed by the calling script.
        TRY(classic_script->run(ClassicScript::RethrowErrors::Yes));
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerglobalscope-location
GC::Ref<WorkerLocation> WorkerGlobalScope::location() const
{
    // The location attribute must return the WorkerLocation object whose associated WorkerGlobalScope object is the WorkerGlobalScope object.
    return *m_location;
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-worker-navigator
GC::Ref<WorkerNavigator> WorkerGlobalScope::navigator() const
{
    // The navigator attribute of the WorkerGlobalScope interface must return an instance of the WorkerNavigator interface,
    // which represents the identity and state of the user agent (the client).
    return *m_navigator;
}

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                               \
    void WorkerGlobalScope::set_##attribute_name(WebIDL::CallbackType* value) \
    {                                                                         \
        set_event_handler_attribute(event_name, move(value));                 \
    }                                                                         \
    WebIDL::CallbackType* WorkerGlobalScope::attribute_name()                 \
    {                                                                         \
        return event_handler_attribute(event_name);                           \
    }
ENUMERATE_WORKER_GLOBAL_SCOPE_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

GC::Ref<CSS::FontFaceSet> WorkerGlobalScope::fonts()
{
    if (!m_fonts) {
        m_fonts = CSS::FontFaceSet::create(relevant_settings_object(*this));
        // A worker has no document to wait for, so its font source is never pending on the environment.
        m_fonts->set_is_pending_on_the_environment(false);
    }
    return *m_fonts;
}

CSS::FontComputer& WorkerGlobalScope::font_computer()
{
    if (!m_font_computer)
        m_font_computer = heap().allocate<CSS::FontComputer>(relevant_settings_object(*this));
    return *m_font_computer;
}

GC::Ref<PolicyContainer> WorkerGlobalScope::policy_container() const
{
    if (!m_policy_container) {
        m_policy_container = GC::Heap::the().allocate<PolicyContainer>(GC::Heap::the());
    }
    return *m_policy_container;
}

// https://html.spec.whatwg.org/multipage/browsers.html#initialize-worker-policy-container
void WorkerGlobalScope::initialize_policy_container(GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ref<EnvironmentSettingsObject> environment)
{
    // 1. If workerGlobalScope's url is local but its scheme is not "blob":
    if (m_url.has_value() && Fetch::Infrastructure::is_local_url(m_url.value()) && m_url->scheme() != "blob"sv) {
        // 1. Assert: workerGlobalScope's owner set's size is 1.
        VERIFY(m_owner_set.size() == 1);

        // 2. Set workerGlobalScope's policy container to a clone of workerGlobalScope's owner set[0]'s relevant
        //    settings object's policy container.
        auto const& owner_policy_container = m_owner_set.first().visit([](auto const& owner) -> SerializedPolicyContainer const& {
            return owner.relevant_settings_object_policy_container;
        });
        m_policy_container = create_a_policy_container_from_serialized_policy_container(owner_policy_container);
        return;
    }

    // 2. Otherwise, set workerGlobalScope's policy container to the result of creating a policy container from a fetch
    //    response given response and environment.
    m_policy_container = create_a_policy_container_from_a_fetch_response(response, environment);
}

// https://html.spec.whatwg.org/multipage/browsers.html#check-a-global-object's-embedder-policy
// NB: response is only used for violation reports, which are not queued.
bool WorkerGlobalScope::check_embedder_policy(EnvironmentSettingsObject const& owner) const
{
    // 1. If workerGlobalScope is not a DedicatedWorkerGlobalScope object, then return true.
    if (!is<DedicatedWorkerGlobalScope>(*this))
        return true;

    // 2. Let policy be workerGlobalScope's embedder policy.
    // AD-HOC: Nothing sets workerGlobalScope's embedder policy. This reads its policy container's embedder policy
    //         instead. See https://github.com/whatwg/html/issues/11316.
    auto const& policy = policy_container()->embedder_policy;

    // 3. Let ownerPolicy be owner's policy container's embedder policy.
    auto const& owner_policy = owner.policy_container()->embedder_policy;

    // FIXME: 4. If ownerPolicy's report-only value is compatible with cross-origin isolation and policy's value is
    //           not, then queue a cross-origin embedder policy inheritance violation with response, "worker
    //           initialization", ownerPolicy's report-only reporting endpoint, "reporting", and owner.

    // 5. If ownerPolicy's value is not compatible with cross-origin isolation or policy's value is compatible with
    //    cross-origin isolation, then return true.
    if (!is_compatible_with_cross_origin_isolation(owner_policy.value) || is_compatible_with_cross_origin_isolation(policy.value))
        return true;

    // FIXME: 6. Queue a cross-origin embedder policy inheritance violation with response, "worker initialization",
    //           ownerPolicy's reporting endpoint, "enforce", and owner.

    // 7. Return false.
    return false;
}

// https://w3c.github.io/webappsec-csp/#run-global-object-csp-initialization
ContentSecurityPolicy::Directives::Directive::Result WorkerGlobalScope::run_csp_initialization() const
{
    // 1. Let result be "Allowed".
    auto result = ContentSecurityPolicy::Directives::Directive::Result::Allowed;

    // 2. For each policy of global’s CSP list:
    for (auto policy : policy_container()->csp_list->policies()) {
        // 1. For each directive of policy:
        for (auto const& directive : policy->directives()) {
            // 1. Execute directive’s initialization algorithm on global. If its returned value is "Blocked", then set
            //    result to "Blocked".
            auto directive_result = ContentSecurityPolicy::Directives::initialization(directive, GC::Ref { *this }, policy);
            if (directive_result == ContentSecurityPolicy::Directives::Directive::Result::Blocked) {
                result = ContentSecurityPolicy::Directives::Directive::Result::Blocked;
            }
        }
    }

    // 3. Return result.
    return result;
}

}

namespace Web::Bindings {

HTML::WorkerGlobalScope* worker_global_scope_from_global_object(JS::Object& object)
{
    return Bindings::impl_from<HTML::WorkerGlobalScope>(&object);
}

HTML::WorkerGlobalScope const* worker_global_scope_from_global_object(JS::Object const& object)
{
    return Bindings::impl_from<HTML::WorkerGlobalScope>(&object);
}

ServiceWorker::ServiceWorkerGlobalScope* service_worker_global_scope_from_global_object(JS::Object& object)
{
    return Bindings::impl_from<ServiceWorker::ServiceWorkerGlobalScope>(&object);
}

JS::Realm& main_world_realm(HTML::WorkerGlobalScope const& worker_global_scope)
{
    auto wrapper = worker_global_scope.cached_main_world_wrapper();
    VERIFY(wrapper);
    return wrapper->shape().realm();
}

void initialize_worker_web_interfaces(HTML::WorkerGlobalScope& worker_global_scope)
{
    worker_global_scope.initialize_web_interfaces_impl();
}

}
