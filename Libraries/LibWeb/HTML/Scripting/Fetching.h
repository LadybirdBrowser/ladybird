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

using OnFetchScriptComplete = GC::Ref<GC::Function<void(GC::Ptr<Script>)>>;
using PerformTheFetchHook = GC::Ptr<GC::Function<WebIDL::ExceptionOr<void>(GC::Ref<Fetch::Infrastructure::Request>, TopLevelModule, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction)>>;

OnFetchScriptComplete create_on_fetch_script_complete(GC::Heap& heap, Function<void(GC::Ptr<Script>)> function);
PerformTheFetchHook create_perform_the_fetch_hook(GC::Heap& heap, Function<WebIDL::ExceptionOr<void>(GC::Ref<Fetch::Infrastructure::Request>, TopLevelModule, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction)> function);

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
    GC_CELL(FetchContext, JS::GraphLoadingState::HostDefined);
    GC_DECLARE_ALLOCATOR(FetchContext);

public:
    JS::Value parse_error;                                   // [[ParseError]]
    Fetch::Infrastructure::Request::Destination destination; // [[Destination]]
    PerformTheFetchHook perform_fetch;                       // [[PerformFetch]]
    GC::Ref<EnvironmentSettingsObject> fetch_client;         // [[FetchClient]]

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

String module_type_from_module_request(JS::ModuleRequest const&);
WebIDL::ExceptionOr<URL::URL> resolve_module_specifier(Optional<Script&> referring_script, String const& specifier);
WebIDL::ExceptionOr<Optional<URL::URL>> resolve_imports_match(ByteString const& normalized_specifier, Optional<URL::URL> as_url, ModuleSpecifierMap const&);
Optional<URL::URL> resolve_url_like_module_specifier(StringView specifier, URL::URL const& base_url);
ScriptFetchOptions get_descendant_script_fetch_options(ScriptFetchOptions const& original_options, URL::URL const& url, EnvironmentSettingsObject& settings_object);
String resolve_a_module_integrity_metadata(URL::URL const& url, EnvironmentSettingsObject& settings_object);
WebIDL::ExceptionOr<void> fetch_classic_script(GC::Ref<HTMLScriptElement>, URL::URL const&, EnvironmentSettingsObject& settings_object, ScriptFetchOptions options, CORSSettingAttribute cors_setting, String character_encoding, OnFetchScriptComplete on_complete);
WebIDL::ExceptionOr<void> fetch_classic_worker_script(URL::URL const&, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook, OnFetchScriptComplete);
WebIDL::ExceptionOr<GC::Ref<ClassicScript>> fetch_a_classic_worker_imported_script(URL::URL const&, HTML::EnvironmentSettingsObject&, PerformTheFetchHook = nullptr);
WebIDL::ExceptionOr<void> fetch_module_worker_script_graph(URL::URL const&, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook, OnFetchScriptComplete);
WebIDL::ExceptionOr<void> fetch_worklet_module_worker_script_graph(URL::URL const&, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook, OnFetchScriptComplete);
void fetch_external_module_script_graph(JS::Realm&, URL::URL const&, EnvironmentSettingsObject& settings_object, ScriptFetchOptions const&, OnFetchScriptComplete on_complete);
void fetch_inline_module_script_graph(JS::Realm&, ByteString const& filename, ByteString const& source_text, URL::URL const& base_url, EnvironmentSettingsObject& settings_object, OnFetchScriptComplete on_complete);
void fetch_single_imported_module_script(JS::Realm&, URL::URL const&, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination, ScriptFetchOptions const&, JS::Realm& module_map_realm, Fetch::Infrastructure::Request::ReferrerType, JS::ModuleRequest const&, PerformTheFetchHook, OnFetchScriptComplete on_complete);

void fetch_descendants_of_and_link_a_module_script(JS::Realm&, JavaScriptModuleScript&, EnvironmentSettingsObject&, Fetch::Infrastructure::Request::Destination, PerformTheFetchHook, OnFetchScriptComplete on_complete);

Fetch::Infrastructure::Request::Destination fetch_destination_from_module_type(Fetch::Infrastructure::Request::Destination, ByteString const&);

void fetch_single_module_script(JS::Realm&, URL::URL const&, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination, ScriptFetchOptions const&, JS::Realm& module_map_realm, Web::Fetch::Infrastructure::Request::ReferrerType const&, Optional<JS::ModuleRequest> const&, TopLevelModule, PerformTheFetchHook, OnFetchScriptComplete callback);
}
