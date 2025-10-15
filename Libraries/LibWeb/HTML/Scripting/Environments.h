/*
 * Copyright (c) 2021-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/FetchRecord.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/ModuleMap.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/ServiceWorker/Registration.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#environment
struct WEB_API Environment : public JS::Cell {
    GC_CELL(Environment, JS::Cell);
    GC_DECLARE_ALLOCATOR(Environment);

public:
    virtual ~Environment() override;

    // An id https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-id
    String id;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-creation-url
    URL::URL creation_url;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-top-level-creation-url
    // Null or a URL that represents the creation URL of the "top-level" environment. It is null for workers and worklets.
    Optional<URL::URL> top_level_creation_url;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-top-level-origin
    // A for now implementation-defined value, null, or an origin. For a "top-level" potential execution environment it is null
    // (i.e., when there is no response yet); otherwise it is the "top-level" environment's origin. For a dedicated worker or worklet
    // it is the top-level origin of its creator. For a shared or service worker it is an implementation-defined value.
    Optional<URL::Origin> top_level_origin;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-target-browsing-context
    GC::Ptr<BrowsingContext> target_browsing_context;

    // FIXME: An active service worker https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-active-service-worker

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-execution-ready-flag
    bool execution_ready { false };

    // https://html.spec.whatwg.org/multipage/webappapis.html#environment-discarding-steps
    virtual void discard_environment() { }

protected:
    Environment() = default;
    Environment(String id, URL::URL creation_url, Optional<URL::URL> top_level_creation_url, Optional<URL::Origin> top_level_origin, GC::Ptr<BrowsingContext> target_browsing_context)
        : id(move(id))
        , creation_url(move(creation_url))
        , top_level_creation_url(move(top_level_creation_url))
        , top_level_origin(move(top_level_origin))
        , target_browsing_context(move(target_browsing_context))
    {
    }
    virtual void visit_edges(Cell::Visitor&) override;
};

enum class RunScriptDecision {
    Run,
    DoNotRun,
};

// https://html.spec.whatwg.org/multipage/webappapis.html#environment-settings-object
struct WEB_API EnvironmentSettingsObject : public Environment {
    GC_CELL(EnvironmentSettingsObject, Environment);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual void finalize() override;
    virtual void initialize(JS::Realm&) override;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-target-browsing-context
    JS::ExecutionContext& realm_execution_context();
    JS::ExecutionContext const& realm_execution_context() const;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-module-map
    ModuleMap& module_map();

    // https://html.spec.whatwg.org/multipage/webappapis.html#responsible-document
    virtual GC::Ptr<DOM::Document> responsible_document() = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#api-base-url
    virtual URL::URL api_base_url() const = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-origin
    virtual URL::Origin origin() const = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-has-cross-site-ancestor
    virtual bool has_cross_site_ancestor() const = 0;

    // A policy container https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-policy-container
    virtual GC::Ref<PolicyContainer> policy_container() const = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-cross-origin-isolated-capability
    virtual CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-time-origin
    virtual double time_origin() const = 0;

    Optional<URL::URL> parse_url(StringView);
    Optional<URL::URL> encoding_parse_url(StringView);
    Optional<String> encoding_parse_and_serialize_url(StringView);

    JS::Realm& realm();
    JS::Object& global_object();
    EventLoop& responsible_event_loop();

    // https://fetch.spec.whatwg.org/#concept-fetch-group
    auto& fetch_group() { return m_fetch_group; }

    SerializedEnvironmentSettingsObject serialize();

    GC::Ref<StorageAPI::StorageManager> storage_manager();

    // https://w3c.github.io/ServiceWorker/#get-the-service-worker-registration-object
    GC::Ref<ServiceWorker::ServiceWorkerRegistration> get_service_worker_registration_object(ServiceWorker::Registration const&);

    // https://w3c.github.io/ServiceWorker/#get-the-service-worker-object
    GC::Ref<ServiceWorker::ServiceWorker> get_service_worker_object(ServiceWorker::ServiceWorkerRecord*);

    [[nodiscard]] bool discarded() const { return m_discarded; }
    void set_discarded(bool b) { m_discarded = b; }

    virtual void discard_environment() override;

    // FIXME: This method below is from HighResolutionTime spec in section 3. Section for Specification Authors.
    // The following other methods are currently not supported:
    // `current relative timestamp`     https://www.w3.org/TR/hr-time-3/#dfn-current-relative-timestamp
    // `current monotonic time`         https://www.w3.org/TR/hr-time-3/#dfn-current-monotonic-time
    // `current coarsened wall time`    https://www.w3.org/TR/hr-time-3/#dfn-current-wall-time

    // https://w3c.github.io/hr-time/#dfn-eso-current-wall-time
    HighResolutionTime::DOMHighResTimeStamp current_wall_time() const
    {
        // An environment settings object settingsObject's current wall time is the result of the following steps:

        // 1. Let unsafeWallTime be the wall clock's unsafe current time.
        auto unsafe_walltime = HighResolutionTime::wall_clock_unsafe_current_time();

        // 2. Return the result of calling coarsen time with unsafeWallTime and settingsObject's cross-origin isolated capability.
        return HighResolutionTime::coarsen_time(unsafe_walltime, cross_origin_isolated_capability());
    }

protected:
    explicit EnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext>);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    NonnullOwnPtr<JS::ExecutionContext> m_realm_execution_context;
    GC::Ptr<ModuleMap> m_module_map;

    GC::Ptr<EventLoop> m_responsible_event_loop;

    // https://fetch.spec.whatwg.org/#concept-fetch-record
    // A fetch group holds an ordered list of fetch records
    Fetch::Infrastructure::FetchRecord::List m_fetch_group;

    // https://storage.spec.whatwg.org/#api
    // Each environment settings object has an associated StorageManager object.
    GC::Ptr<StorageAPI::StorageManager> m_storage_manager;

    // https://w3c.github.io/ServiceWorker/#environment-settings-object-service-worker-registration-object-map
    // An environment settings object has a service worker registration object map,
    // a map where the keys are service worker registrations and the values are ServiceWorkerRegistration objects.
    HashMap<ServiceWorker::RegistrationKey, GC::Ref<ServiceWorker::ServiceWorkerRegistration>> m_service_worker_registration_object_map;

    // https://w3c.github.io/ServiceWorker/#environment-settings-object-service-worker-object-map
    // An environment settings object has a service worker object map,
    // a map where the keys are service workers and the values are ServiceWorker objects.
    HashMap<ServiceWorker::ServiceWorkerRecord*, GC::Ref<ServiceWorker::ServiceWorker>> m_service_worker_object_map;

    // https://w3c.github.io/ServiceWorker/#service-worker-client-discarded-flag
    // A service worker client has an associated discarded flag. It is initially unset.
    bool m_discarded { false };
};

JS::ExecutionContext const& execution_context_of_realm(JS::Realm const&);
inline JS::ExecutionContext& execution_context_of_realm(JS::Realm& realm) { return const_cast<JS::ExecutionContext&>(execution_context_of_realm(const_cast<JS::Realm const&>(realm))); }

RunScriptDecision can_run_script(JS::Realm const&);
bool is_scripting_enabled(JS::Realm const&);
bool is_scripting_disabled(JS::Realm const&);
void prepare_to_run_script(JS::Realm&);
void clean_up_after_running_script(JS::Realm const&);
WEB_API void prepare_to_run_callback(JS::Realm&);
WEB_API void clean_up_after_running_callback(JS::Realm const&);
WEB_API ModuleMap& module_map_of_realm(JS::Realm&);
WEB_API bool module_type_allowed(JS::Realm const&, StringView module_type);

WEB_API void add_module_to_resolved_module_set(JS::Realm&, String const& serialized_base_url, String const& normalized_specifier, Optional<URL::URL> const& as_url);

EnvironmentSettingsObject& incumbent_settings_object();
WEB_API JS::Realm& incumbent_realm();
JS::Object& incumbent_global_object();

JS::Realm& current_principal_realm();
EnvironmentSettingsObject& principal_realm_settings_object(JS::Realm&);
EnvironmentSettingsObject& current_principal_settings_object();

WEB_API JS::Realm& principal_realm(GC::Ref<JS::Realm>);
WEB_API JS::Object& current_principal_global_object();

WEB_API JS::Realm& relevant_realm(JS::Object const&);
JS::Realm& relevant_principal_realm(JS::Object const&);

WEB_API EnvironmentSettingsObject& relevant_settings_object(JS::Object const&);
EnvironmentSettingsObject& relevant_settings_object(DOM::Node const&);
WEB_API EnvironmentSettingsObject& relevant_principal_settings_object(JS::Object const&);

WEB_API JS::Object& relevant_global_object(JS::Object const&);
WEB_API JS::Object& relevant_principal_global_object(JS::Object const&);

JS::Realm& entry_realm();
EnvironmentSettingsObject& entry_settings_object();
JS::Object& entry_global_object();
[[nodiscard]] WEB_API bool is_secure_context(Environment const&);
[[nodiscard]] bool is_non_secure_context(Environment const&);

}
