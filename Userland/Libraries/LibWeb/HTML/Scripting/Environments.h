/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
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

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#environment
struct Environment : public JS::Cell {
    JS_CELL(Environment, JS::Cell);

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
    JS::GCPtr<BrowsingContext> target_browsing_context;

    // FIXME: An active service worker https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-active-service-worker

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-execution-ready-flag
    bool execution_ready { false };

protected:
    virtual void visit_edges(Cell::Visitor&) override;
};

enum class RunScriptDecision {
    Run,
    DoNotRun,
};

// https://html.spec.whatwg.org/multipage/webappapis.html#environment-settings-object
struct EnvironmentSettingsObject : public Environment {
    JS_CELL(EnvironmentSettingsObject, Environment);

public:
    virtual ~EnvironmentSettingsObject() override;
    virtual void initialize(JS::Realm&) override;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-target-browsing-context
    JS::ExecutionContext& realm_execution_context();

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-module-map
    ModuleMap& module_map();

    // https://html.spec.whatwg.org/multipage/webappapis.html#responsible-document
    virtual JS::GCPtr<DOM::Document> responsible_document() = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#api-url-character-encoding
    virtual String api_url_character_encoding() = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#api-base-url
    virtual URL::URL api_base_url() = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-origin
    virtual URL::Origin origin() = 0;

    // A policy container https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-policy-container
    virtual PolicyContainer policy_container() = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-cross-origin-isolated-capability
    virtual CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() = 0;

    URL::URL parse_url(StringView);

    JS::Realm& realm();
    JS::Object& global_object();
    EventLoop& responsible_event_loop();

    // https://fetch.spec.whatwg.org/#concept-fetch-group
    Vector<JS::NonnullGCPtr<Fetch::Infrastructure::FetchRecord>>& fetch_group() { return m_fetch_group; }

    RunScriptDecision can_run_script();
    void prepare_to_run_script();
    void clean_up_after_running_script();

    void prepare_to_run_callback();
    void clean_up_after_running_callback();

    bool is_scripting_enabled() const;
    bool is_scripting_disabled() const;

    bool module_type_allowed(StringView module_type) const;

    void disallow_further_import_maps();

    SerializedEnvironmentSettingsObject serialize();

    JS::NonnullGCPtr<StorageAPI::StorageManager> storage_manager();

    [[nodiscard]] bool discarded() const { return m_discarded; }
    void set_discarded(bool b) { m_discarded = b; }

protected:
    explicit EnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext>);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    NonnullOwnPtr<JS::ExecutionContext> m_realm_execution_context;
    JS::GCPtr<ModuleMap> m_module_map;

    JS::GCPtr<EventLoop> m_responsible_event_loop;

    // https://fetch.spec.whatwg.org/#concept-fetch-record
    // A fetch group holds an ordered list of fetch records
    Vector<JS::NonnullGCPtr<Fetch::Infrastructure::FetchRecord>> m_fetch_group;

    // https://storage.spec.whatwg.org/#api
    // Each environment settings object has an associated StorageManager object.
    JS::GCPtr<StorageAPI::StorageManager> m_storage_manager;

    // https://w3c.github.io/ServiceWorker/#service-worker-client-discarded-flag
    // A service worker client has an associated discarded flag. It is initially unset.
    bool m_discarded { false };
};

EnvironmentSettingsObject& incumbent_settings_object();
JS::Realm& incumbent_realm();
JS::Object& incumbent_global_object();
EnvironmentSettingsObject& current_settings_object();
JS::Object& current_global_object();
JS::Realm& relevant_realm(JS::Object const&);
EnvironmentSettingsObject& relevant_settings_object(JS::Object const&);
EnvironmentSettingsObject& relevant_settings_object(DOM::Node const&);
JS::Object& relevant_global_object(JS::Object const&);
JS::Realm& entry_realm();
EnvironmentSettingsObject& entry_settings_object();
JS::Object& entry_global_object();
JS::VM& relevant_agent(JS::Object const&);
[[nodiscard]] bool is_secure_context(Environment const&);
[[nodiscard]] bool is_non_secure_context(Environment const&);

}
