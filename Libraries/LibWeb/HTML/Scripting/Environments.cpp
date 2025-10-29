/*
 * Copyright (c) 2021-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/SyntheticHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Infrastructure/FetchRecord.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/WindowEnvironmentSettingsObject.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/SecureContexts/AbstractOperations.h>
#include <LibWeb/ServiceWorker/ServiceWorker.h>
#include <LibWeb/ServiceWorker/ServiceWorkerRegistration.h>
#include <LibWeb/StorageAPI/StorageManager.h>

namespace Web::HTML {

Environment::~Environment() = default;

void Environment::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(target_browsing_context);
}

EnvironmentSettingsObject::EnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext> realm_execution_context)
    : m_realm_execution_context(move(realm_execution_context))
{
    m_realm_execution_context->context_owner = this;

    // Register with the responsible event loop so we can perform step 4 of "perform a microtask checkpoint".
    responsible_event_loop().register_environment_settings_object({}, *this);
}

void EnvironmentSettingsObject::finalize()
{
    responsible_event_loop().unregister_environment_settings_object({}, *this);
    Base::finalize();
}

void EnvironmentSettingsObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    m_module_map = realm.heap().allocate<ModuleMap>();
}

void EnvironmentSettingsObject::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_responsible_event_loop);
    visitor.visit(m_module_map);
    m_realm_execution_context->visit_edges(visitor);
    visitor.visit(m_storage_manager);
    visitor.visit(m_service_worker_registration_object_map);
    visitor.visit(m_service_worker_object_map);
}

void EnvironmentSettingsObject::discard_environment()
{
    // https://w3c.github.io/ServiceWorker/#ref-for-environment-discarding-steps
    // Each service worker client has the following environment discarding steps:

    // 1. Set client’s discarded flag.
    set_discarded(true);
}

JS::ExecutionContext& EnvironmentSettingsObject::realm_execution_context()
{
    // NOTE: All environment settings objects are created with a realm execution context, so it's stored and returned here in the base class.
    return *m_realm_execution_context;
}

JS::ExecutionContext const& EnvironmentSettingsObject::realm_execution_context() const
{
    // NOTE: All environment settings objects are created with a realm execution context, so it's stored and returned here in the base class.
    return *m_realm_execution_context;
}

ModuleMap& EnvironmentSettingsObject::module_map()
{
    return *m_module_map;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#environment-settings-object%27s-realm
JS::Realm& EnvironmentSettingsObject::realm()
{
    // An environment settings object's realm execution context's Realm component is the environment settings object's Realm.
    return *realm_execution_context().realm;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-global
JS::Object& EnvironmentSettingsObject::global_object()
{
    // An environment settings object's Realm then has a [[GlobalObject]] field, which contains the environment settings object's global object.
    return realm().global_object();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#responsible-event-loop
EventLoop& EnvironmentSettingsObject::responsible_event_loop()
{
    // An environment settings object's responsible event loop is its global object's relevant agent's event loop.
    // This is here in case the realm that is holding onto this ESO is destroyed before the ESO is. The responsible event loop pointer is needed in the ESO destructor to deregister from the event loop.
    // FIXME: Figure out why the realm can be destroyed before the ESO, as the realm is holding onto this with an OwnPtr, but the heap block deallocator calls the ESO destructor directly instead of through the realm destructor.
    if (m_responsible_event_loop)
        return *m_responsible_event_loop;

    m_responsible_event_loop = relevant_agent(global_object()).event_loop;
    return *m_responsible_event_loop;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#check-if-we-can-run-script
// https://whatpr.org/html/9893/webappapis.html#check-if-we-can-run-script
RunScriptDecision can_run_script(JS::Realm const& realm)
{
    // 1. If the global object specified by realm is a Window object whose Document object is not fully active, then return "do not run".
    if (is<HTML::Window>(realm.global_object()) && !as<HTML::Window>(realm.global_object()).associated_document().is_fully_active())
        return RunScriptDecision::DoNotRun;

    // 2. If scripting is disabled for realm, then return "do not run".
    if (is_scripting_disabled(realm))
        return RunScriptDecision::DoNotRun;

    // 3. Return "run".
    return RunScriptDecision::Run;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#prepare-to-run-script
// https://whatpr.org/html/9893/b8ea975...df5706b/webappapis.html#prepare-to-run-script
void prepare_to_run_script(JS::Realm& realm)
{
    // 1. Push realms's execution context onto the JavaScript execution context stack; it is now the running JavaScript execution context.
    realm.global_object().vm().push_execution_context(execution_context_of_realm(realm));

    // FIXME: 2. If realm is a principal realm, then:
    // FIXME: 2.1 Let settings be realm's settings object.
    // FIXME: 2.2 Add settings to the currently running task's script evaluation environment settings object set.
}

// https://whatpr.org/html/9893/b8ea975...df5706b/webappapis.html#concept-realm-execution-context
JS::ExecutionContext const& execution_context_of_realm(JS::Realm const& realm)
{
    VERIFY(realm.host_defined());

    // 1. If realm is a principal realm, then return the realm execution context of the environment settings object of realm.
    if (is<Bindings::PrincipalHostDefined>(*realm.host_defined()))
        return static_cast<Bindings::PrincipalHostDefined const&>(*realm.host_defined()).environment_settings_object->realm_execution_context();

    // 2. Assert: realm is a synthetic realm.
    // 3. Return the execution context of the synthetic realm settings object of realm.
    return *as<Bindings::SyntheticHostDefined>(*realm.host_defined()).synthetic_realm_settings.execution_context;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#clean-up-after-running-script
// https://whatpr.org/html/9893/webappapis.html#clean-up-after-running-script
void clean_up_after_running_script(JS::Realm const& realm)
{
    auto& vm = realm.global_object().vm();

    // 1. Assert: realm's execution context is the running JavaScript execution context.
    VERIFY(&execution_context_of_realm(realm) == &vm.running_execution_context());

    // 2. Remove realm's execution context from the JavaScript execution context stack.
    vm.pop_execution_context();

    // 3. If the JavaScript execution context stack is now empty, perform a microtask checkpoint. (If this runs scripts, these algorithms will be invoked reentrantly.)
    if (vm.execution_context_stack().is_empty())
        main_thread_event_loop().perform_a_microtask_checkpoint();
}

static JS::ExecutionContext* top_most_script_having_execution_context(JS::VM& vm)
{
    // Here, the topmost script-having execution context is the topmost entry of the JavaScript execution context stack that has a non-null ScriptOrModule component,
    // or null if there is no such entry in the JavaScript execution context stack.
    auto execution_context = vm.execution_context_stack().last_matching([&](JS::ExecutionContext* context) {
        return !context->script_or_module.has<Empty>();
    });

    if (!execution_context.has_value())
        return nullptr;

    return execution_context.value();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#prepare-to-run-a-callback
void prepare_to_run_callback(JS::Realm& realm)
{
    auto& vm = realm.global_object().vm();

    // 1. Push realm onto the backup incumbent settings object stack.
    // NOTE: The spec doesn't say which event loop's stack to put this on. However, all the examples of the incumbent settings object use iframes and cross browsing context communication to demonstrate the concept.
    //       This means that it must rely on some global state that can be accessed by all browsing contexts, which is the main thread event loop.
    HTML::main_thread_event_loop().push_onto_backup_incumbent_realm_stack(realm);

    // 2. Let context be the topmost script-having execution context.
    auto* context = top_most_script_having_execution_context(vm);

    // 3. If context is not null, increment context's skip-when-determining-incumbent counter.
    if (context) {
        context->skip_when_determining_incumbent_counter++;
    }
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#parse-a-url
Optional<URL::URL> EnvironmentSettingsObject::parse_url(StringView url)
{
    // 1. Let baseURL be environment's base URL, if environment is a Document object; otherwise environment's API base URL.
    auto base_url = api_base_url();

    // 2. Return the result of applying the URL parser to url, with baseURL.
    return DOMURL::parse(url, base_url);
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#encoding-parsing-a-url
Optional<URL::URL> EnvironmentSettingsObject::encoding_parse_url(StringView url)
{
    // 1. Let encoding be UTF-8.
    auto encoding = "UTF-8"_string;

    // 2. If environment is a Document object, then set encoding to environment's character encoding.

    // 3. Otherwise, if environment's relevant global object is a Window object, set encoding to environment's relevant
    //    global object's associated Document's character encoding.
    if (is<HTML::Window>(global_object()))
        encoding = static_cast<HTML::Window const&>(global_object()).associated_document().encoding_or_default();

    // 4. Let baseURL be environment's base URL, if environment is a Document object; otherwise environment's API base URL.
    auto base_url = api_base_url();

    // 5. Return the result of applying the URL parser to url, with baseURL and encoding.
    return DOMURL::parse(url, base_url, encoding);
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#encoding-parsing-and-serializing-a-url
Optional<String> EnvironmentSettingsObject::encoding_parse_and_serialize_url(StringView url)
{
    // 1. Let url be the result of encoding-parsing a URL given url, relative to environment.
    auto parsed_url = encoding_parse_url(url);

    // 2. If url is failure, then return failure.
    if (!parsed_url.has_value())
        return {};

    // 3. Return the result of applying the URL serializer to url.
    return parsed_url->serialize();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#clean-up-after-running-a-callback
// https://whatpr.org/html/9893/b8ea975...df5706b/webappapis.html#clean-up-after-running-a-callback
void clean_up_after_running_callback(JS::Realm const& realm)
{
    auto& vm = realm.global_object().vm();

    // 1. Let context be the topmost script-having execution context.
    auto* context = top_most_script_having_execution_context(vm);

    // 2. If context is not null, decrement context's skip-when-determining-incumbent counter.
    if (context) {
        context->skip_when_determining_incumbent_counter--;
    }

    // 3. Assert: the topmost entry of the backup incumbent realm stack is realm.
    auto& event_loop = HTML::main_thread_event_loop();
    VERIFY(&event_loop.top_of_backup_incumbent_realm_stack() == &realm);

    // 4. Remove realm from the backup incumbent realm stack.
    event_loop.pop_backup_incumbent_realm_stack();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-script
// https://whatpr.org/html/9893/webappapis.html#concept-environment-script
bool is_scripting_enabled(JS::Realm const& realm)
{
    // Scripting is enabled for a realm realm when all of the following conditions are true:
    // The user agent supports scripting.
    // NOTE: This is always true in LibWeb :^)

    // FIXME: Do the right thing for workers.
    if (!is<HTML::Window>(realm.global_object()))
        return true;

    // The user has not disabled scripting for realm at this time. (User agents may provide users with the option to disable scripting globally, or in a finer-grained manner, e.g., on a per-origin basis, down to the level of individual realms.)
    auto const& document = as<HTML::Window>(realm.global_object()).associated_document();
    if (!document.page().is_scripting_enabled())
        return false;

    // Either settings's global object is not a Window object, or settings's global object's associated Document's active sandboxing flag set does not have its sandboxed scripts browsing context flag set.
    if (has_flag(document.active_sandboxing_flag_set(), SandboxingFlagSet::SandboxedScripts))
        return false;

    return true;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-noscript
// https://whatpr.org/html/9893/webappapis.html#concept-environment-noscript
bool is_scripting_disabled(JS::Realm const& realm)
{
    // Scripting is disabled for a realm when scripting is not enabled for it, i.e., when any of the above conditions are false.
    return !is_scripting_enabled(realm);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#module-type-allowed
// https://whatpr.org/html/9893/webappapis.html#module-type-allowed
bool module_type_allowed(JS::Realm const&, StringView module_type)
{
    // 1. If moduleType is not "javascript", "css", or "json", then return false.
    if (module_type != "javascript"sv && module_type != "css"sv && module_type != "json"sv)
        return false;

    // FIXME: 2. If moduleType is "css" and the CSSStyleSheet interface is not exposed in realm, then return false.

    // 3. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#add-module-to-resolved-module-set
void add_module_to_resolved_module_set(JS::Realm& realm, String const& serialized_base_url, String const& normalized_specifier, Optional<URL::URL> const& as_url)
{
    // 1. Let global be realm's global object.
    auto& global = realm.global_object();

    // 2. If global does not implement Window, then return.
    if (!is<Window>(global))
        return;

    // 3. Let record be a new specifier resolution record, with serialized base URL set to serializedBaseURL,
    //    specifier set to normalizedSpecifier, and specifier as a URL set to asURL.
    //
    // NOTE: We set 'specifier as a URL set to asURL' as a bool to simplify logic when merging import maps.
    SpecifierResolution resolution {
        .serialized_base_url = serialized_base_url,
        .specifier = normalized_specifier,
        .specifier_is_null_or_url_like_that_is_special = !as_url.has_value() || as_url->is_special(),
    };

    // 4. Append record to global's resolved module set.
    return as<Window>(global).append_resolved_module(move(resolution));
}

// https://whatpr.org/html/9893/webappapis.html#concept-realm-module-map
ModuleMap& module_map_of_realm(JS::Realm& realm)
{
    VERIFY(realm.host_defined());

    // 1. If realm is a principal realm, then return the module map of the environment settings object of realm.
    if (is<Bindings::PrincipalHostDefined>(*realm.host_defined()))
        return static_cast<Bindings::PrincipalHostDefined const&>(*realm.host_defined()).environment_settings_object->module_map();

    // 2. Assert: realm is a synthetic realm.
    // 3. Return the module map of the synthetic realm settings object of realm.
    return *as<Bindings::SyntheticHostDefined>(*realm.host_defined()).synthetic_realm_settings.module_map;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-incumbent-realm
// https://whatpr.org/html/9893/b8ea975...df5706b/webappapis.html#concept-incumbent-realm
JS::Realm& incumbent_realm()
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto& vm = event_loop.vm();

    // 1. Let context be the topmost script-having execution context.
    auto* context = top_most_script_having_execution_context(vm);

    // 2. If context is null, or if context's skip-when-determining-incumbent counter is greater than zero, then:
    if (!context || context->skip_when_determining_incumbent_counter > 0) {
        // 1. Assert: the backup incumbent settings object stack is not empty.
        // 1. Assert: the backup incumbent realm stack is not empty.
        // NOTE: If this assertion fails, it's because the incumbent realm was used with no involvement of JavaScript.
        VERIFY(!event_loop.is_backup_incumbent_realm_stack_empty());

        // 2. Return the topmost entry of the backup incumbent realm stack.
        return event_loop.top_of_backup_incumbent_realm_stack();
    }

    // 3. Return context's Realm component.
    return *context->realm;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#incumbent-settings-object
// https://whatpr.org/html/9893/b8ea975...df5706b/webappapis.html#incumbent-settings-object
EnvironmentSettingsObject& incumbent_settings_object()
{
    // Then, the incumbent settings object is the incumbent realm's principal realm settings object.
    return principal_realm_settings_object(principal_realm(incumbent_realm()));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-incumbent-global
JS::Object& incumbent_global_object()
{
    // Similarly, the incumbent global object is the global object of the incumbent settings object.
    return incumbent_settings_object().global_object();
}

// https://whatpr.org/html/9893/webappapis.html#current-principal-realm
JS::Realm& current_principal_realm()
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto& vm = event_loop.vm();

    // The current principal realm is the principal realm of the current realm.
    return principal_realm(*vm.current_realm());
}

// https://whatpr.org/html/9893/webappapis.html#concept-principal-realm-of-realm
JS::Realm& principal_realm(GC::Ref<JS::Realm> realm)
{
    VERIFY(realm->host_defined());

    // 1. If realm.[[HostDefined]] is a synthetic realm settings object, then:
    if (is<Bindings::SyntheticHostDefined>(*realm->host_defined())) {
        // 1. Assert: realm is a synthetic realm.
        // 2. Set realm to the principal realm of realm.[[HostDefined]].
        realm = static_cast<Bindings::SyntheticHostDefined const&>(*realm->host_defined()).synthetic_realm_settings.principal_realm;
    }

    // 2. Assert: realm.[[HostDefined]] is an environment settings object and realm is a principal realm.
    VERIFY(is<Bindings::PrincipalHostDefined>(*realm->host_defined()));

    // 3. Return realm.
    return realm;
}

// https://whatpr.org/html/9893/webappapis.html#concept-realm-settings-object
EnvironmentSettingsObject& principal_realm_settings_object(JS::Realm& realm)
{
    // A principal realm has a [[HostDefined]] field, which contains the principal realm's settings object.
    return Bindings::principal_host_defined_environment_settings_object(realm);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#current-settings-object
// https://whatpr.org/html/9893/webappapis.html#current-principal-settings-object
EnvironmentSettingsObject& current_principal_settings_object()
{
    // Then, the current principal settings object is the environment settings object of the current principal realm.
    return principal_realm_settings_object(current_principal_realm());
}

// https://html.spec.whatwg.org/multipage/webappapis.html#current-global-object
// https://whatpr.org/html/9893/webappapis.html#current-principal-global-object
JS::Object& current_principal_global_object()
{
    // Similarly, the current principal global object is the global object of the current principal realm.
    return current_principal_realm().global_object();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-relevant-realm
JS::Realm& relevant_realm(JS::Object const& object)
{
    // The relevant Realm for a platform object is the value of its [[Realm]] field.
    return object.shape().realm();
}

// https://whatpr.org/html/9893/webappapis.html#relevant-principal-realm
JS::Realm& relevant_principal_realm(JS::Object const& object)
{
    // The relevant principal realm for a platform object o is o's relevant realm's principal realm.
    return principal_realm(relevant_realm(object));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#relevant-settings-object
EnvironmentSettingsObject& relevant_settings_object(JS::Object const& object)
{
    // Then, the relevant settings object for a platform object o is the environment settings object of the relevant Realm for o.
    return Bindings::principal_host_defined_environment_settings_object(relevant_realm(object));
}

EnvironmentSettingsObject& relevant_settings_object(DOM::Node const& node)
{
    // Then, the relevant settings object for a platform object o is the environment settings object of the relevant Realm for o.
    return const_cast<DOM::Document&>(node.document()).relevant_settings_object();
}

// https://whatpr.org/html/9893/webappapis.html#relevant-principal-settings-object
EnvironmentSettingsObject& relevant_principal_settings_object(JS::Object const& object)
{
    // The relevant principal settings object for a platform object o is o's relevant principal realm's environment settings object.
    return Bindings::principal_host_defined_environment_settings_object(relevant_principal_realm(object));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-relevant-global
JS::Object& relevant_global_object(JS::Object const& object)
{
    // Similarly, the relevant global object for a platform object o is the global object of the relevant Realm for o.
    return relevant_realm(object).global_object();
}

// https://whatpr.org/html/9893/webappapis.html#relevant-principal-global
JS::Object& relevant_principal_global_object(JS::Object const& object)
{
    // The relevant principal global object for a platform object o is o's relevant principal realm's global object.
    return relevant_principal_realm(object).global_object();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-entry-realm
// https://whatpr.org/html/9893/webappapis.html#concept-entry-realm
JS::Realm& entry_realm()
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto& vm = event_loop.vm();

    // With this in hand, we define the entry execution context to be the most recently pushed item in the JavaScript execution context stack that is a realm execution context.
    // The entry realm is the principal realm of the entry execution context's Realm component.
    // NOTE: Currently all execution contexts in LibJS are realm execution contexts
    return principal_realm(*vm.running_execution_context().realm);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#entry-settings-object
EnvironmentSettingsObject& entry_settings_object()
{
    // Then, the entry settings object is the environment settings object of the entry realm.
    return Bindings::principal_host_defined_environment_settings_object(entry_realm());
}

// https://html.spec.whatwg.org/multipage/webappapis.html#entry-global-object
JS::Object& entry_global_object()
{
    // Similarly, the entry global object is the global object of the entry realm.
    return entry_realm().global_object();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#secure-context
bool is_secure_context(Environment const& environment)
{
    // 1. If environment is an environment settings object, then:
    if (is<EnvironmentSettingsObject>(environment)) {
        // 1. Let global be environment's global object.
        // FIXME: Add a const global_object() getter to ESO
        auto& global = static_cast<EnvironmentSettingsObject&>(const_cast<Environment&>(environment)).global_object();

        // 2. If global is a WorkerGlobalScope, then:
        if (is<WorkerGlobalScope>(global)) {
            // FIXME: 1. If global's owner set[0]'s relevant settings object is a secure context, then return true.
            // NOTE: We only need to check the 0th item since they will necessarily all be consistent.

            // 2. Return false.
            return false;
        }

        // FIXME: 3. If global is a WorkletGlobalScope, then return true.
        // NOTE: Worklets can only be created in secure contexts.
    }

    // 2. If the result of Is url potentially trustworthy? given environment's top-level creation URL is "Potentially Trustworthy", then return true.
    if (SecureContexts::is_url_potentially_trustworthy(environment.top_level_creation_url.value()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy)
        return true;

    // 3. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#non-secure-context
bool is_non_secure_context(Environment const& environment)
{
    // An environment is a non-secure context if it is not a secure context.
    return !is_secure_context(environment);
}

SerializedEnvironmentSettingsObject EnvironmentSettingsObject::serialize()
{
    return SerializedEnvironmentSettingsObject {
        .id = this->id,
        .creation_url = this->creation_url,
        .top_level_creation_url = this->top_level_creation_url,
        .top_level_origin = this->top_level_origin,
        .api_url_character_encoding = api_url_character_encoding(),
        .api_base_url = api_base_url(),
        .origin = origin(),
        .has_cross_site_ancestor = has_cross_site_ancestor(),
        .policy_container = policy_container()->serialize(),
        .cross_origin_isolated_capability = cross_origin_isolated_capability(),
        .time_origin = this->time_origin(),
    };
}

GC::Ref<StorageAPI::StorageManager> EnvironmentSettingsObject::storage_manager()
{
    if (!m_storage_manager)
        m_storage_manager = realm().create<StorageAPI::StorageManager>(realm());
    return *m_storage_manager;
}

// https://w3c.github.io/ServiceWorker/#get-the-service-worker-registration-object
GC::Ref<ServiceWorker::ServiceWorkerRegistration> EnvironmentSettingsObject::get_service_worker_registration_object(ServiceWorker::Registration const& registration)
{
    // 1. Let objectMap be environment’s service worker registration object map.
    auto& object_map = this->m_service_worker_registration_object_map;

    // FIXME: File spec issue asking if this should be keyed on the registration's scope url only or on the url and the storage key
    auto const key = ServiceWorker::RegistrationKey { registration.storage_key(), registration.scope_url().serialize(URL::ExcludeFragment::Yes).to_byte_string() };

    // 2. If objectMap[registration] does not exist, then:
    if (!object_map.contains(key)) {
        // 1. Let registrationObject be a new ServiceWorkerRegistration in environment’s Realm.
        // 2. Set registrationObject’s service worker registration to registration.
        // 3. Set registrationObject’s installing attribute to null.
        // 4. Set registrationObject’s waiting attribute to null.
        // 5. Set registrationObject’s active attribute to null.
        auto registration_object = ServiceWorker::ServiceWorkerRegistration::create(realm(), registration);

        // 6. If registration’s installing worker is not null, then set registrationObject’s installing attribute to the result of getting the service worker object that represents registration’s installing worker in environment.
        if (registration.installing_worker())
            registration_object->set_installing(get_service_worker_object(registration.installing_worker()));

        // 7. If registration’s waiting worker is not null, then set registrationObject’s waiting attribute to the result of getting the service worker object that represents registration’s waiting worker in environment.
        if (registration.waiting_worker())
            registration_object->set_waiting(get_service_worker_object(registration.waiting_worker()));

        // 8. If registration’s active worker is not null, then set registrationObject’s active attribute to the result of getting the service worker object that represents registration’s active worker in environment.
        if (registration.active_worker())
            registration_object->set_active(get_service_worker_object(registration.active_worker()));

        // 9. Set objectMap[registration] to registrationObject.
        object_map.set(key, registration_object);
    }

    // 3. Return objectMap[registration].
    return *object_map.get(key);
}

GC::Ref<ServiceWorker::ServiceWorker> EnvironmentSettingsObject::get_service_worker_object(ServiceWorker::ServiceWorkerRecord* service_worker)
{
    // 1. Let objectMap be environment’s service worker object map.
    auto& object_map = this->m_service_worker_object_map;

    // 2. If objectMap[serviceWorker] does not exist, then:
    if (!object_map.contains(service_worker)) {
        // 1. Let serviceWorkerObj be a new ServiceWorker in environment’s Realm, and associate it with serviceWorker.
        auto service_worker_obj = ServiceWorker::ServiceWorker::create(realm(), service_worker);

        // 2. Set serviceWorkerObj’s state to serviceWorker’s state.
        service_worker_obj->set_service_worker_state(service_worker->state);

        // 3. Set objectMap[serviceWorker] to serviceWorkerObj.
        object_map.set(service_worker, service_worker_obj);
    }

    // 3. Return objectMap[serviceWorker].
    return *object_map.get(service_worker);
}

}
