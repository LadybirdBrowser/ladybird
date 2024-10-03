/*
 * Copyright (c) 2022-2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/Scripting/ImportMap.h>
#include <LibWeb/HTML/Scripting/ModuleMap.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

enum class TopLevelModule {
    Yes,
    No
};

using OnFetchScriptComplete = JS::NonnullGCPtr<JS::HeapFunction<void(JS::GCPtr<Script>)>>;
using PerformTheFetchHook = JS::GCPtr<JS::HeapFunction<WebIDL::ExceptionOr<void>(JS::NonnullGCPtr<Fetch::Infrastructure::Request>, TopLevelModule, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction)>>;

OnFetchScriptComplete create_on_fetch_script_complete(JS::Heap& heap, Function<void(JS::GCPtr<Script>)> function);
PerformTheFetchHook create_perform_the_fetch_hook(JS::Heap& heap, Function<WebIDL::ExceptionOr<void>(JS::NonnullGCPtr<Fetch::Infrastructure::Request>, TopLevelModule, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction)> function);

// https://html.spec.whatwg.org/multipage/webappapis.html#script-fetch-options
struct ScriptFetchOptions {
    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-nonce
    String cryptographic_nonce {};

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-integrity
    String integrity_metadata {};

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-parser
    Fetch::Infrastructure::Request::ParserMetadata parser_metadata { Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted };

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-credentials
    Fetch::Infrastructure::Request::CredentialsMode credentials_mode { Fetch::Infrastructure::Request::CredentialsMode::SameOrigin };

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-referrer-policy
    ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-render-blocking
    bool render_blocking { false };

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-fetch-priority
    Fetch::Infrastructure::Request::Priority fetch_priority {};
};

// https://html.spec.whatwg.org/multipage/webappapis.html#default-script-fetch-options
ScriptFetchOptions default_script_fetch_options();

class FetchContext : public JS::GraphLoadingState::HostDefined {
    JS_CELL(FetchContext, JS::GraphLoadingState::HostDefined);
    JS_DECLARE_ALLOCATOR(FetchContext);

public:
    JS::Value parse_error;                                    // [[ParseError]]
    Fetch::Infrastructure::Request::Destination destination;  // [[Destination]]
    PerformTheFetchHook perform_fetch;                        // [[PerformFetch]]
    JS::NonnullGCPtr<EnvironmentSettingsObject> fetch_client; // [[FetchClient]]

private:
    FetchContext(JS::Value parse_error, Fetch::Infrastructure::Request::Destination destination, PerformTheFetchHook perform_fetch, EnvironmentSettingsObject& fetch_client)
        : parse_error(parse_error)
        , destination(destination)
        , perform_fetch(perform_fetch)
        , fetch_client(fetch_client)
    {
    }

    void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(parse_error);
        visitor.visit(perform_fetch);
        visitor.visit(fetch_client);
    }
};

ByteString module_type_from_module_request(JS::ModuleRequest const&);
WebIDL::ExceptionOr<URL::URL> resolve_module_specifier(Optional<Script&> referring_script, ByteString const& specifier);
WebIDL::ExceptionOr<Optional<URL::URL>> resolve_imports_match(ByteString const& normalized_specifier, Optional<URL::URL> as_url, ModuleSpecifierMap const&);
Optional<URL::URL> resolve_url_like_module_specifier(ByteString const& specifier, URL::URL const& base_url);
WebIDL::ExceptionOr<ScriptFetchOptions> get_descendant_script_fetch_options(ScriptFetchOptions const& original_options, URL::URL const& url, EnvironmentSettingsObject& settings_object);
WebIDL::ExceptionOr<String> resolve_a_module_integrity_metadata(URL::URL const& url, EnvironmentSettingsObject& settings_object);
WebIDL::ExceptionOr<void> fetch_classic_script(JS::NonnullGCPtr<HTMLScriptElement>, URL::URL const&, EnvironmentSettingsObject& settings_object, ScriptFetchOptions options, CORSSettingAttribute cors_setting, String character_encoding, OnFetchScriptComplete on_complete);
WebIDL::ExceptionOr<void> fetch_classic_worker_script(URL::URL const&, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook, OnFetchScriptComplete);
WebIDL::ExceptionOr<JS::NonnullGCPtr<ClassicScript>> fetch_a_classic_worker_imported_script(URL::URL const&, HTML::EnvironmentSettingsObject&, PerformTheFetchHook = nullptr);
WebIDL::ExceptionOr<void> fetch_module_worker_script_graph(URL::URL const&, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook, OnFetchScriptComplete);
WebIDL::ExceptionOr<void> fetch_worklet_module_worker_script_graph(URL::URL const&, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook, OnFetchScriptComplete);
void fetch_internal_module_script_graph(JS::Realm&, JS::ModuleRequest const& module_request, EnvironmentSettingsObject& fetch_client_settings_object, Fetch::Infrastructure::Request::Destination, ScriptFetchOptions const&, Script& referring_script, HashTable<ModuleLocationTuple> const& visited_set, PerformTheFetchHook, OnFetchScriptComplete on_complete);
void fetch_external_module_script_graph(JS::Realm&, URL::URL const&, EnvironmentSettingsObject& settings_object, ScriptFetchOptions const&, OnFetchScriptComplete on_complete);
void fetch_inline_module_script_graph(JS::Realm&, ByteString const& filename, ByteString const& source_text, URL::URL const& base_url, EnvironmentSettingsObject& settings_object, OnFetchScriptComplete on_complete);
void fetch_single_imported_module_script(JS::Realm&, URL::URL const&, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination, ScriptFetchOptions const&, EnvironmentSettingsObject& settings_object, Fetch::Infrastructure::Request::ReferrerType, JS::ModuleRequest const&, PerformTheFetchHook, OnFetchScriptComplete on_complete);

void fetch_descendants_of_a_module_script(JS::Realm&, JavaScriptModuleScript& module_script, EnvironmentSettingsObject& fetch_client_settings_object, Fetch::Infrastructure::Request::Destination, HashTable<ModuleLocationTuple> visited_set, PerformTheFetchHook, OnFetchScriptComplete callback);
void fetch_descendants_of_and_link_a_module_script(JS::Realm&, JavaScriptModuleScript&, EnvironmentSettingsObject&, Fetch::Infrastructure::Request::Destination, PerformTheFetchHook, OnFetchScriptComplete on_complete);

Fetch::Infrastructure::Request::Destination fetch_destination_from_module_type(Fetch::Infrastructure::Request::Destination, ByteString const&);

void fetch_single_module_script(JS::Realm&, URL::URL const&, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination, ScriptFetchOptions const&, EnvironmentSettingsObject& settings_object, Web::Fetch::Infrastructure::Request::ReferrerType const&, Optional<JS::ModuleRequest> const&, TopLevelModule, PerformTheFetchHook, OnFetchScriptComplete callback);
}
