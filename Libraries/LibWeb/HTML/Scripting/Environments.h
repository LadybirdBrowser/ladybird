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
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/ModuleMap.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWeb/ServiceWorker/Registration.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#environment
struct Environment : public JS::Cell {
    GC_CELL(Environment, JS::Cell);

public:
    virtual ~Environment() override;

    // An id https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-id
    String id;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-creation-url
    URL::URL creation_url;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-top-level-creation-url
    URL::URL top_level_creation_url;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-top-level-origin
    URL::Origin top_level_origin;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-target-browsing-context
    GC::Ptr<BrowsingContext> target_browsing_context;

    // FIXME: An active service worker https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-active-service-worker

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-execution-ready-flag
    bool execution_ready { false };

    // https://html.spec.whatwg.org/multipage/webappapis.html#environment-discarding-steps
    virtual void discard_environment() { }

protected:
    virtual void visit_edges(Cell::Visitor&) override;
};

enum class RunScriptDecision {
    Run,
    DoNotRun,
};

// https://html.spec.whatwg.org/multipage/webappapis.html#environment-settings-object
struct EnvironmentSettingsObject : public Environment {
    GC_CELL(EnvironmentSettingsObject, Environment);

public:
    virtual ~EnvironmentSettingsObject() override;
    virtual void initialize(JS::Realm&) override;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-target-browsing-context
    JS::ExecutionContext& realm_execution_context();
    JS::ExecutionContext const& realm_execution_context() const;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-module-map
    ModuleMap& module_map();

    // https://html.spec.whatwg.org/multipage/webappapis.html#responsible-document
    virtual GC::Ptr<DOM::Document> responsible_document() = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#api-url-character-encoding
    virtual String api_url_character_encoding() const = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#api-base-url
    virtual URL::URL api_base_url() const = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-origin
    virtual URL::Origin origin() const = 0;

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
    Vector<GC::Ref<Fetch::Infrastructure::FetchRecord>>& fetch_group() { return m_fetch_group; }

    SerializedEnvironmentSettingsObject serialize();

    GC::Ref<StorageAPI::StorageManager> storage_manager();

    // https://w3c.github.io/ServiceWorker/#get-the-service-worker-registration-object
    GC::Ref<ServiceWorker::ServiceWorkerRegistration> get_service_worker_registration_object(ServiceWorker::Registration const&);

    // https://w3c.github.io/ServiceWorker/#get-the-service-worker-object
    GC::Ref<ServiceWorker::ServiceWorker> get_service_worker_object(ServiceWorker::ServiceWorkerRecord*);

    [[nodiscard]] bool discarded() const { return m_discarded; }
    void set_discarded(bool b) { m_discarded = b; }

    virtual void discard_environment() override;

protected:
    explicit EnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext>);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    NonnullOwnPtr<JS::ExecutionContext> m_realm_execution_context;
    GC::Ptr<ModuleMap> m_module_map;

    GC::Ptr<EventLoop> m_responsible_event_loop;

    // https://fetch.spec.whatwg.org/#concept-fetch-record
    // A fetch group holds an ordered list of fetch records
    Vector<GC::Ref<Fetch::Infrastructure::FetchRecord>> m_fetch_group;

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
void prepare_to_run_callback(JS::Realm&);
void clean_up_after_running_callback(JS::Realm const&);
ModuleMap& module_map_of_realm(JS::Realm&);
bool module_type_allowed(JS::Realm const&, StringView module_type);

void add_module_to_resolved_module_set(JS::Realm&, String const& serialized_base_url, String const& normalized_specifier, Optional<URL::URL> const& as_url);

EnvironmentSettingsObject& incumbent_settings_object();
JS::Realm& incumbent_realm();
JS::Object& incumbent_global_object();

JS::Realm& current_principal_realm();
EnvironmentSettingsObject& principal_realm_settings_object(JS::Realm&);
EnvironmentSettingsObject& current_principal_settings_object();

JS::Realm& principal_realm(GC::Ref<JS::Realm>);
JS::Object& current_principal_global_object();

JS::Realm& relevant_realm(JS::Object const&);
JS::Realm& relevant_principal_realm(JS::Object const&);

EnvironmentSettingsObject& relevant_settings_object(JS::Object const&);
EnvironmentSettingsObject& relevant_settings_object(DOM::Node const&);
EnvironmentSettingsObject& relevant_principal_settings_object(JS::Object const&);

JS::Object& relevant_global_object(JS::Object const&);
JS::Object& relevant_principal_global_object(JS::Object const&);

JS::Realm& entry_realm();
EnvironmentSettingsObject& entry_settings_object();
JS::Object& entry_global_object();
[[nodiscard]] bool is_secure_context(Environment const&);
[[nodiscard]] bool is_non_secure_context(Environment const&);

}
