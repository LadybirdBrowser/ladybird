/*
 * Copyright (c) 2022-2023, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGC/Function.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(FetchContext);

OnFetchScriptComplete create_on_fetch_script_complete(GC::Heap& heap, Function<void(GC::Ptr<Script>)> function)
{
    return GC::create_function(heap, move(function));
}

PerformTheFetchHook create_perform_the_fetch_hook(GC::Heap& heap, Function<WebIDL::ExceptionOr<void>(GC::Ref<Fetch::Infrastructure::Request>, TopLevelModule, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction)> function)
{
    return GC::create_function(heap, move(function));
}

ScriptFetchOptions default_script_fetch_options()
{
    // The default script fetch options are a script fetch options whose cryptographic nonce is the empty string,
    // integrity metadata is the empty string, parser metadata is "not-parser-inserted", credentials mode is "same-origin",
    // referrer policy is the empty string, and fetch priority is "auto".
    return ScriptFetchOptions {
        .cryptographic_nonce = {},
        .integrity_metadata = {},
        .parser_metadata = Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted,
        .credentials_mode = Fetch::Infrastructure::Request::CredentialsMode::SameOrigin,
        .referrer_policy = {},
        .fetch_priority = Fetch::Infrastructure::Request::Priority::Auto
    };
}

// https://html.spec.whatwg.org/multipage/webappapis.html#module-type-from-module-request
String module_type_from_module_request(JS::ModuleRequest const& module_request)
{
    // 1. Let moduleType be "javascript".
    String module_type = "javascript"_string;

    // 2. If moduleRequest.[[Attributes]] has a Record entry such that entry.[[Key]] is "type", then:
    for (auto const& entry : module_request.attributes) {
        if (entry.key != "type"_string)
            continue;

        // 1. If entry.[[Value]] is "javascript", then set moduleType to null.
        if (entry.value == "javascript"_string)
            module_type = ""_string; // FIXME: This should be null!
        // 2. Otherwise, set moduleType to entry.[[Value]].
        else
            module_type = entry.value;
    }

    // 3. Return moduleType.
    return module_type;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#resolve-a-module-specifier
// https://whatpr.org/html/9893/webappapis.html#resolve-a-module-specifier
WebIDL::ExceptionOr<URL::URL> resolve_module_specifier(Optional<Script&> referring_script, String const& specifier)
{
    auto& vm = Bindings::main_thread_vm();

    // 1. Let realm and baseURL be null.
    GC::Ptr<JS::Realm> realm;
    Optional<URL::URL> base_url;

    // 2. If referringScript is not null, then:
    if (referring_script.has_value()) {
        // 1. Set realm to referringScript's realm.
        realm = referring_script->realm();

        // 2. Set baseURL to referringScript's base URL.
        base_url = referring_script->base_url();
    }
    // 3. Otherwise:
    else {
        // 1. Assert: there is a current realm.
        VERIFY(vm.current_realm());

        // 2. Set realm to the current realm.
        realm = *vm.current_realm();

        // 3. Set baseURL to realm's principal realm's settings object's API base URL.
        base_url = Bindings::principal_host_defined_environment_settings_object(HTML::principal_realm(*realm)).api_base_url();
    }

    // 4. Let importMap be an empty import map.
    ImportMap import_map;

    // 5. If realm's global object implements Window, then set importMap to settingsObject's global object's import map.
    if (is<Window>(realm->global_object()))
        import_map = as<Window>(realm->global_object()).import_map();

    // 6. Let serializedBaseURL be baseURL, serialized.
    auto serialized_base_url = base_url->serialize();

    // 7. Let asURL be the result of resolving a URL-like module specifier given specifier and baseURL.
    auto as_url = resolve_url_like_module_specifier(specifier.to_byte_string(), *base_url);

    // 8. Let normalizedSpecifier be the serialization of asURL, if asURL is non-null; otherwise, specifier.
    auto normalized_specifier = as_url.has_value() ? as_url->serialize() : specifier;

    // 9. Let result be a URL-or-null, initially null.
    Optional<URL::URL> result;

    // 10. For each scopePrefix → scopeImports of importMap's scopes:
    for (auto const& entry : import_map.scopes()) {
        // FIXME: Clarify if the serialization steps need to be run here. The steps below assume
        //        scopePrefix to be a string.
        auto const& scope_prefix = entry.key.serialize();
        auto const& scope_imports = entry.value;

        // 1. If scopePrefix is serializedBaseURL, or if scopePrefix ends with U+002F (/) and scopePrefix is a code unit prefix of serializedBaseURL, then:
        if (scope_prefix == serialized_base_url || (scope_prefix.ends_with('/') && Infra::is_code_unit_prefix(scope_prefix, serialized_base_url))) {
            // 1. Let scopeImportsMatch be the result of resolving an imports match given normalizedSpecifier, asURL, and scopeImports.
            auto scope_imports_match = TRY(resolve_imports_match(normalized_specifier.to_byte_string(), as_url, scope_imports));

            // 2. If scopeImportsMatch is not null, then set result to scopeImportsMatch, and break.
            if (scope_imports_match.has_value()) {
                result = scope_imports_match.release_value();
                break;
            }
        }
    }

    // 11. If result is null, set result be the result of resolving an imports match given normalizedSpecifier, asURL, and importMap's imports.
    if (!result.has_value())
        result = TRY(resolve_imports_match(normalized_specifier.to_byte_string(), as_url, import_map.imports()));

    // 12. If result is null, set it to asURL.
    // Spec-Note: By this point, if result was null, specifier wasn't remapped to anything by importMap, but it might have been able to be turned into a URL.
    if (!result.has_value())
        result = as_url;

    // 13. If result is not null, then:
    if (result.has_value()) {
        // 1. Add module to resolved module set given realm, serializedBaseURL, normalizedSpecifier, and asURL.
        add_module_to_resolved_module_set(*realm, serialized_base_url, normalized_specifier, as_url);

        // 2. Return result.
        return result.release_value();
    }

    // 14. Throw a TypeError indicating that specifier was a bare specifier, but was not remapped to anything by importMap.
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Failed to resolve non relative module specifier '{}' from an import map.", specifier)) };
}

// https://html.spec.whatwg.org/multipage/webappapis.html#resolving-an-imports-match
WebIDL::ExceptionOr<Optional<URL::URL>> resolve_imports_match(ByteString const& normalized_specifier, Optional<URL::URL> as_url, ModuleSpecifierMap const& specifier_map)
{
    // 1. For each specifierKey → resolutionResult of specifierMap:
    for (auto const& [specifier_key, resolution_result] : specifier_map) {
        // 1. If specifierKey is normalizedSpecifier, then:
        if (specifier_key.bytes_as_string_view() == normalized_specifier) {
            // 1. If resolutionResult is null, then throw a TypeError indicating that resolution of specifierKey was blocked by a null entry.
            if (!resolution_result.has_value())
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("Import resolution of '{}' was blocked by a null entry.", specifier_key).release_value_but_fixme_should_propagate_errors() };

            // 2. Assert: resolutionResult is a URL.
            // 3. Return resolutionResult.
            return resolution_result;
        }

        // 2. If all of the following are true:
        if (
            // - specifierKey ends with U+002F (/);
            specifier_key.bytes_as_string_view().ends_with("/"sv) &&
            // - specifierKey is a code unit prefix of normalizedSpecifier; and
            Infra::is_code_unit_prefix(specifier_key, normalized_specifier) &&
            // - either asURL is null, or asURL is special,
            (!as_url.has_value() || as_url->is_special())
            // then:
        ) {
            // 1. If resolutionResult is null, then throw a TypeError indicating that the resolution of specifierKey was blocked by a null entry.
            if (!resolution_result.has_value())
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("Import resolution of '{}' was blocked by a null entry.", specifier_key).release_value_but_fixme_should_propagate_errors() };

            // 2. Assert: resolutionResult is a URL.
            // 3. Let afterPrefix be the portion of normalizedSpecifier after the initial specifierKey prefix.
            // FIXME: Clarify if this is meant by the portion after the initial specifierKey prefix.
            auto after_prefix = normalized_specifier.substring(specifier_key.bytes_as_string_view().length());

            // 4. Assert: resolutionResult, serialized, ends with U+002F (/), as enforced during parsing.
            VERIFY(resolution_result->serialize().ends_with('/'));

            // 5. Let url be the result of URL parsing afterPrefix with resolutionResult.
            auto url = DOMURL::parse(after_prefix, *resolution_result);

            // 6. If url is failure, then throw a TypeError indicating that resolution of normalizedSpecifier was blocked since the afterPrefix portion
            //    could not be URL-parsed relative to the resolutionResult mapped to by the specifierKey prefix.
            if (!url.has_value())
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("Could not resolve '{}' as the after prefix portion could not be URL-parsed.", normalized_specifier).release_value_but_fixme_should_propagate_errors() };

            // 7. Assert: url is a URL.
            VERIFY(url.has_value());

            // 8. If the serialization of resolutionResult is not a code unit prefix of the serialization of url, then throw a TypeError indicating
            //    that the resolution of normalizedSpecifier was blocked due to it backtracking above its prefix specifierKey.
            if (!Infra::is_code_unit_prefix(resolution_result->serialize(), url->serialize()))
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("Could not resolve '{}' as it backtracks above its prefix specifierKey.", normalized_specifier).release_value_but_fixme_should_propagate_errors() };

            // 9. Return url.
            return url;
        }
    }

    // 2. Return null.
    return Optional<URL::URL> {};
}

// https://html.spec.whatwg.org/multipage/webappapis.html#resolving-a-url-like-module-specifier
Optional<URL::URL> resolve_url_like_module_specifier(StringView specifier, URL::URL const& base_url)
{
    // 1. If specifier starts with "/", "./", or "../", then:
    if (specifier.starts_with("/"sv) || specifier.starts_with("./"sv) || specifier.starts_with("../"sv)) {
        // 1. Let url be the result of URL parsing specifier with baseURL.
        auto url = DOMURL::parse(specifier, base_url);

        // 2. If url is failure, then return null.
        if (!url.has_value())
            return {};

        // 3. Return url.
        return url;
    }

    // 2. Let url be the result of URL parsing specifier (with no base URL).
    auto url = DOMURL::parse(specifier);

    // 3. If url is failure, then return null.
    if (!url.has_value())
        return {};

    // 4. Return url.
    return url;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#set-up-the-classic-script-request
static void set_up_classic_script_request(Fetch::Infrastructure::Request& request, ScriptFetchOptions const& options)
{
    // Set request's cryptographic nonce metadata to options's cryptographic nonce, its integrity metadata to options's
    // integrity metadata, its parser metadata to options's parser metadata, its referrer policy to options's referrer
    // policy, its render-blocking to options's render-blocking, and its priority to options's fetch priority.
    request.set_cryptographic_nonce_metadata(options.cryptographic_nonce);
    request.set_integrity_metadata(options.integrity_metadata);
    request.set_parser_metadata(options.parser_metadata);
    request.set_referrer_policy(options.referrer_policy);
    request.set_render_blocking(options.render_blocking);
    request.set_priority(options.fetch_priority);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#set-up-the-module-script-request
static void set_up_module_script_request(Fetch::Infrastructure::Request& request, ScriptFetchOptions const& options)
{
    // Set request's cryptographic nonce metadata to options's cryptographic nonce, its integrity metadata to options's
    // integrity metadata, its parser metadata to options's parser metadata, its credentials mode to options's credentials
    // mode, its referrer policy to options's referrer policy, its render-blocking to options's render-blocking, and its
    // priority to options's fetch priority.
    request.set_cryptographic_nonce_metadata(options.cryptographic_nonce);
    request.set_integrity_metadata(options.integrity_metadata);
    request.set_parser_metadata(options.parser_metadata);
    request.set_credentials_mode(options.credentials_mode);
    request.set_referrer_policy(options.referrer_policy);
    request.set_render_blocking(options.render_blocking);
    request.set_priority(options.fetch_priority);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#get-the-descendant-script-fetch-options
ScriptFetchOptions get_descendant_script_fetch_options(ScriptFetchOptions const& original_options, URL::URL const& url, EnvironmentSettingsObject& settings_object)
{
    // 1. Let newOptions be a copy of originalOptions.
    auto new_options = original_options;

    // 2. Let integrity be the result of resolving a module integrity metadata with url and settingsObject.
    String integrity = resolve_a_module_integrity_metadata(url, settings_object);

    // 3. Set newOptions's integrity metadata to integrity.
    new_options.integrity_metadata = integrity;

    // 4. Set newOptions's fetch priority to "auto".
    new_options.fetch_priority = Fetch::Infrastructure::Request::Priority::Auto;

    // 5. Return newOptions.
    return new_options;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#resolving-a-module-integrity-metadata
String resolve_a_module_integrity_metadata(const URL::URL& url, EnvironmentSettingsObject& settings_object)
{
    // 1. Let map be settingsObject's global object's import map.
    auto map = as<UniversalGlobalScopeMixin>(settings_object.global_object()).import_map();

    // 2. If map's integrity[url] does not exist, then return the empty string.
    // 3. Return map's integrity[url].
    return map.integrity().get(url).value_or(""_string);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-classic-script
WebIDL::ExceptionOr<void> fetch_classic_script(GC::Ref<HTMLScriptElement> element, URL::URL const& url, EnvironmentSettingsObject& settings_object, ScriptFetchOptions options, CORSSettingAttribute cors_setting, String character_encoding, OnFetchScriptComplete on_complete)
{
    auto& realm = element->realm();
    auto& vm = realm.vm();

    // 1. Let request be the result of creating a potential-CORS request given url, "script", and CORS setting.
    auto request = create_potential_CORS_request(vm, url, Fetch::Infrastructure::Request::Destination::Script, cors_setting);

    // 2. Set request's client to settings object.
    request->set_client(&settings_object);

    // 3. Set request's initiator type to "script".
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Script);

    // 4. Set up the classic script request given request and options.
    set_up_classic_script_request(*request, options);

    // 5. Fetch request with the following processResponseConsumeBody steps given response response and null, failure,
    //    or a byte sequence bodyBytes:
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response_consume_body = [&settings_object, options = move(options), character_encoding = move(character_encoding), on_complete = move(on_complete)](auto response, auto body_bytes) {
        // 1. Set response to response's unsafe response.
        response = response->unsafe_response();

        // 2. If either of the following conditions are met:
        // - bodyBytes is null or failure; or
        // - response's status is not an ok status,
        if (body_bytes.template has<Empty>() || body_bytes.template has<Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag>() || !Fetch::Infrastructure::is_ok_status(response->status())) {
            // then run onComplete given null, and abort these steps.
            on_complete->function()(nullptr);
            return;
        }

        // 3. Let potentialMIMETypeForEncoding be the result of extracting a MIME type given response's header list.
        auto potential_mime_type_for_encoding = response->header_list()->extract_mime_type();

        // 4. Set character encoding to the result of legacy extracting an encoding given potentialMIMETypeForEncoding
        //    and character encoding.
        auto extracted_character_encoding = Fetch::Infrastructure::legacy_extract_an_encoding(potential_mime_type_for_encoding, character_encoding);

        // 5. Let source text be the result of decoding bodyBytes to Unicode, using character encoding as the fallback
        //    encoding.
        auto fallback_decoder = TextCodec::decoder_for(extracted_character_encoding);
        VERIFY(fallback_decoder.has_value());

        auto source_text = TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*fallback_decoder, body_bytes.template get<ByteBuffer>()).release_value_but_fixme_should_propagate_errors();

        // 6. Let muted errors be true if response was CORS-cross-origin, and false otherwise.
        auto muted_errors = response->is_cors_cross_origin() ? ClassicScript::MutedErrors::Yes : ClassicScript::MutedErrors::No;

        // 7. Let script be the result of creating a classic script given source text, settings object's realm, response's URL,
        //    options, and muted errors.
        // FIXME: Pass options.
        auto response_url = response->url().value_or({});
        auto script = ClassicScript::create(response_url.to_byte_string(), source_text, settings_object.realm(), response_url, 1, muted_errors);

        // 8. Run onComplete given script.
        on_complete->function()(script);
    };

    TRY(Fetch::Fetching::fetch(element->realm(), request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input))));
    return {};
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-classic-worker-script
WebIDL::ExceptionOr<void> fetch_classic_worker_script(URL::URL const& url, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook perform_fetch, OnFetchScriptComplete on_complete)
{
    auto& realm = settings_object.realm();
    auto& vm = realm.vm();

    // 1. Let request be a new request whose URL is url, client is fetchClient, destination is destination, initiator type is "other",
    //    mode is "same-origin", credentials mode is "same-origin", parser metadata is "not parser-inserted",
    //    and whose use-URL-credentials flag is set.
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(url);
    request->set_client(&fetch_client);
    request->set_destination(destination);
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Other);

    // FIXME: Use proper SameOrigin CORS mode once Origins are set properly in WorkerHost processes
    request->set_mode(Fetch::Infrastructure::Request::Mode::NoCORS);

    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::SameOrigin);
    request->set_parser_metadata(Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted);
    request->set_use_url_credentials(true);

    auto process_response_consume_body = [&settings_object, on_complete = move(on_complete)](auto response, auto body_bytes) {
        // 1. Set response to response's unsafe response.
        response = response->unsafe_response();

        // 2. If either of the following conditions are met:
        // - bodyBytes is null or failure; or
        // - response's status is not an ok status,
        if (body_bytes.template has<Empty>() || body_bytes.template has<Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag>() || !Fetch::Infrastructure::is_ok_status(response->status())) {
            // then run onComplete given null, and abort these steps.
            on_complete->function()(nullptr);
            return;
        }

        // 3. If all of the following are true:
        // - response's URL's scheme is an HTTP(S) scheme; and
        // - the result of extracting a MIME type from response's header list is not a JavaScript MIME type,
        auto maybe_mime_type = response->header_list()->extract_mime_type();
        auto mime_type_is_javascript = maybe_mime_type.has_value() && maybe_mime_type->is_javascript();

        if (response->url().has_value() && Fetch::Infrastructure::is_http_or_https_scheme(response->url()->scheme()) && !mime_type_is_javascript) {
            auto mime_type_serialized = maybe_mime_type.has_value() ? maybe_mime_type->serialized() : "unknown"_string;
            dbgln("Invalid non-javascript mime type \"{}\" for worker script at {}", mime_type_serialized, response->url().value());

            // then run onComplete given null, and abort these steps.
            on_complete->function()(nullptr);
            return;
        }
        // NOTE: Other fetch schemes are exempted from MIME type checking for historical web-compatibility reasons.
        //       We might be able to tighten this in the future; see https://github.com/whatwg/html/issues/3255.

        // 4. Let sourceText be the result of UTF-8 decoding bodyBytes.
        auto decoder = TextCodec::decoder_for("UTF-8"sv);
        VERIFY(decoder.has_value());
        auto source_text = TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, body_bytes.template get<ByteBuffer>()).release_value_but_fixme_should_propagate_errors();

        // 5. Let script be the result of creating a classic script using sourceText, settingsObject's realm,
        //    response's URL, and the default classic script fetch options.
        auto response_url = response->url().value_or({});
        auto script = ClassicScript::create(response_url.to_byte_string(), source_text, settings_object.realm(), response_url);

        // 6. Run onComplete given script.
        on_complete->function()(script);
    };

    // 2. If performFetch was given, run performFetch with request, true, and with processResponseConsumeBody as defined below.
    if (perform_fetch != nullptr) {
        TRY(perform_fetch->function()(request, TopLevelModule::Yes, move(process_response_consume_body)));
    }

    // Otherwise, fetch request with processResponseConsumeBody set to processResponseConsumeBody as defined below.
    else {
        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
        fetch_algorithms_input.process_response_consume_body = move(process_response_consume_body);
        TRY(Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input))));
    }
    return {};
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-classic-worker-imported-script
WebIDL::ExceptionOr<GC::Ref<ClassicScript>> fetch_a_classic_worker_imported_script(URL::URL const& url, HTML::EnvironmentSettingsObject& settings_object, PerformTheFetchHook perform_fetch)
{
    auto& realm = settings_object.realm();
    auto& vm = realm.vm();

    // 1. Let response be null.
    IGNORE_USE_IN_ESCAPING_LAMBDA GC::Ptr<Fetch::Infrastructure::Response> response = nullptr;

    // 2. Let bodyBytes be null.
    Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes;

    // 3. Let request be a new request whose URL is url, client is settingsObject, destination is "script", initiator type is "other",
    //    parser metadata is "not parser-inserted", and whose use-URL-credentials flag is set.
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(url);
    request->set_client(&settings_object);
    request->set_destination(Fetch::Infrastructure::Request::Destination::Script);
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Other);
    request->set_parser_metadata(Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted);
    request->set_use_url_credentials(true);

    auto process_response_consume_body = [&response, &body_bytes](GC::Ref<Fetch::Infrastructure::Response> res, Fetch::Infrastructure::FetchAlgorithms::BodyBytes bb) {
        // 1. Set bodyBytes to bb.
        body_bytes = move(bb);

        // 2. Set response to res.
        response = res;
    };

    // 4. If performFetch was given, run performFetch with request, isTopLevel, and with processResponseConsumeBody as defined below.
    if (perform_fetch) {
        TRY(perform_fetch->function()(request, TopLevelModule::Yes, move(process_response_consume_body)));
    }
    // Otherwise, fetch request with processResponseConsumeBody set to processResponseConsumeBody as defined below.
    else {
        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
        fetch_algorithms_input.process_response_consume_body = move(process_response_consume_body);
        TRY(Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input))));
    }

    // 5. Pause until response is not null.
    // FIXME: Consider using a "response holder" to avoid needing to annotate response as IGNORE_USE_IN_ESCAPING_LAMBDA.
    auto& event_loop = settings_object.responsible_event_loop();
    event_loop.spin_until(GC::create_function(vm.heap(), [&]() -> bool {
        return response;
    }));

    // 6. Set response to response's unsafe response.
    response = response->unsafe_response();

    // 7. If any of the following are true:
    //    - bodyBytes is null or failure;
    //    - response's status is not an ok status; or
    //    - the result of extracting a MIME type from response's header list is not a JavaScript MIME type,
    //    then throw a "NetworkError" DOMException.
    if (body_bytes.template has<Empty>() || body_bytes.template has<Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag>()
        || !Fetch::Infrastructure::is_ok_status(response->status())
        || !response->header_list()->extract_mime_type().has_value() || !response->header_list()->extract_mime_type()->is_javascript()) {
        return WebIDL::NetworkError::create(realm, "Network error"_string);
    }

    // 8. Let sourceText be the result of UTF-8 decoding bodyBytes.
    auto decoder = TextCodec::decoder_for("UTF-8"sv);
    VERIFY(decoder.has_value());
    auto source_text = TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, body_bytes.get<ByteBuffer>()).release_value_but_fixme_should_propagate_errors();

    // 9. Let mutedErrors be true if response was CORS-cross-origin, and false otherwise.
    auto muted_errors = response->is_cors_cross_origin() ? ClassicScript::MutedErrors::Yes : ClassicScript::MutedErrors::No;

    // 10. Let script be the result of creating a classic script given sourceText, settingsObject's realm, response's URL, the default classic script fetch options, and mutedErrors.
    auto response_url = response->url().value_or({});
    auto script = ClassicScript::create(response_url.to_byte_string(), source_text, settings_object.realm(), response_url, 1, muted_errors);

    // 11. Return script.
    return script;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-module-worker-script-tree
WebIDL::ExceptionOr<void> fetch_module_worker_script_graph(URL::URL const& url, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook perform_fetch, OnFetchScriptComplete on_complete)
{
    return fetch_worklet_module_worker_script_graph(url, fetch_client, destination, settings_object, move(perform_fetch), move(on_complete));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-worklet/module-worker-script-graph
// https://whatpr.org/html/9893/webappapis.html#fetch-a-worklet/module-worker-script-graph
WebIDL::ExceptionOr<void> fetch_worklet_module_worker_script_graph(URL::URL const& url, EnvironmentSettingsObject& fetch_client, Fetch::Infrastructure::Request::Destination destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook perform_fetch, OnFetchScriptComplete on_complete)
{
    auto& realm = settings_object.realm();
    auto& vm = realm.vm();

    // 1. Let options be a script fetch options whose cryptographic nonce is the empty string,
    //    integrity metadata is the empty string, parser metadata is "not-parser-inserted",
    //    credentials mode is credentialsMode, referrer policy is the empty string, and fetch priority is "auto".
    // FIXME: credentialsMode
    auto options = ScriptFetchOptions {
        .cryptographic_nonce = String {},
        .integrity_metadata = String {},
        .parser_metadata = Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted,
        .credentials_mode = Fetch::Infrastructure::Request::CredentialsMode::SameOrigin,
        .referrer_policy = ReferrerPolicy::ReferrerPolicy::EmptyString,
        .fetch_priority = Fetch::Infrastructure::Request::Priority::Auto
    };

    // onSingleFetchComplete given result is the following algorithm:
    auto on_single_fetch_complete = create_on_fetch_script_complete(vm.heap(), [&realm, &fetch_client, destination, perform_fetch = perform_fetch, on_complete = move(on_complete)](auto result) mutable {
        // 1. If result is null, run onComplete with null, and abort these steps.
        if (!result) {
            dbgln("on single fetch complete with nool");
            on_complete->function()(nullptr);
            return;
        }

        // 2. Fetch the descendants of and link result given fetchClient, destination, and onComplete. If performFetch was given, pass it along as well.
        fetch_descendants_of_and_link_a_module_script(realm, as<JavaScriptModuleScript>(*result), fetch_client, destination, move(perform_fetch), on_complete);
    });

    // 2. Fetch a single module script given url, fetchClient, destination, options, settingsObject's realm, "client", true,
    //    and onSingleFetchComplete as defined below. If performFetch was given, pass it along as well.
    fetch_single_module_script(realm, url, fetch_client, destination, options, settings_object.realm(), Fetch::Infrastructure::Request::Referrer::Client, {}, TopLevelModule::Yes, move(perform_fetch), on_single_fetch_complete);

    return {};
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-destination-from-module-type
Fetch::Infrastructure::Request::Destination fetch_destination_from_module_type(Fetch::Infrastructure::Request::Destination default_destination, ByteString const& module_type)
{
    // 1. If moduleType is "json", then return "json".
    if (module_type == "json"sv)
        return Fetch::Infrastructure::Request::Destination::JSON;

    // 2. If moduleType is "css", then return "style".
    if (module_type == "css"sv)
        return Fetch::Infrastructure::Request::Destination::Style;

    // 3. Return defaultDestination.
    return default_destination;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-module-script
// https://whatpr.org/html/9893/webappapis.html#fetch-a-single-module-script
void fetch_single_module_script(JS::Realm& realm,
    URL::URL const& url,
    EnvironmentSettingsObject& fetch_client,
    Fetch::Infrastructure::Request::Destination destination,
    ScriptFetchOptions const& options,
    JS::Realm& module_map_realm,
    Web::Fetch::Infrastructure::Request::ReferrerType const& referrer,
    Optional<JS::ModuleRequest> const& module_request,
    TopLevelModule is_top_level,
    PerformTheFetchHook perform_fetch,
    OnFetchScriptComplete on_complete)
{
    // 1. Let moduleType be "javascript".
    String module_type = "javascript"_string;

    // 2. If moduleRequest was given, then set moduleType to the result of running the module type from module request steps given moduleRequest.
    if (module_request.has_value())
        module_type = module_type_from_module_request(*module_request);

    // 3. Assert: the result of running the module type allowed steps given moduleType and moduleMapRealm is true.
    //    Otherwise we would not have reached this point because a failure would have been raised when inspecting moduleRequest.[[Assertions]]
    //    in create a JavaScript module script or fetch a single imported module script.
    VERIFY(module_type_allowed(module_map_realm, module_type));

    // 4. Let moduleMap be moduleMapRealm's module map.
    auto& module_map = module_map_of_realm(module_map_realm);

    // 5. If moduleMap[(url, moduleType)] is "fetching", wait in parallel until that entry's value changes,
    //    then queue a task on the networking task source to proceed with running the following steps.
    if (module_map.is_fetching(url, module_type.to_byte_string())) {
        module_map.wait_for_change(realm.heap(), url, module_type.to_byte_string(), [on_complete, &realm](auto entry) -> void {
            HTML::queue_global_task(HTML::Task::Source::Networking, realm.global_object(), GC::create_function(realm.heap(), [on_complete, entry] {
                // FIXME: This should run other steps, for now we just assume the script loaded.
                VERIFY(entry.type == ModuleMap::EntryType::ModuleScript || entry.type == ModuleMap::EntryType::Failed);

                on_complete->function()(entry.module_script);
            }));
        });

        return;
    }

    // 6. If moduleMap[(url, moduleType)] exists, run onComplete given moduleMap[(url, moduleType)], and return.
    auto entry = module_map.get(url, module_type.to_byte_string());
    if (entry.has_value() && entry->type == ModuleMap::EntryType::ModuleScript) {
        on_complete->function()(entry->module_script);
        return;
    }

    // 7. Set moduleMap[(url, moduleType)] to "fetching".
    module_map.set(url, module_type.to_byte_string(), { ModuleMap::EntryType::Fetching, nullptr });

    // 8. Let request be a new request whose URL is url, mode is "cors", referrer is referrer, and client is fetchClient.
    auto request = Fetch::Infrastructure::Request::create(realm.vm());
    request->set_url(url);
    request->set_mode(Fetch::Infrastructure::Request::Mode::CORS);
    request->set_referrer(referrer);
    request->set_client(&fetch_client);

    // 9. Set request's destination to the result of running the fetch destination from module type steps given destination and moduleType.
    request->set_destination(fetch_destination_from_module_type(destination, module_type.to_byte_string()));

    // 10. If destination is "worker", "sharedworker", or "serviceworker", and isTopLevel is true, then set request's mode to "same-origin".
    if ((destination == Fetch::Infrastructure::Request::Destination::Worker || destination == Fetch::Infrastructure::Request::Destination::SharedWorker || destination == Fetch::Infrastructure::Request::Destination::ServiceWorker) && is_top_level == TopLevelModule::Yes)
        request->set_mode(Fetch::Infrastructure::Request::Mode::SameOrigin);

    // 11. Set request's initiator type to "script".
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Script);

    // 12. Set up the module script request given request and options.
    set_up_module_script_request(request, options);

    // 13. If performFetch was given, run performFetch with request, isTopLevel, and with processResponseConsumeBody as defined below.
    //     Otherwise, fetch request with processResponseConsumeBody set to processResponseConsumeBody as defined below.
    //     In both cases, let processResponseConsumeBody given response response and null, failure, or a byte sequence bodyBytes be the following algorithm:
    auto process_response_consume_body = [&module_map, url, module_type, &module_map_realm, on_complete](GC::Ref<Fetch::Infrastructure::Response> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
        // 1. If either of the following conditions are met:
        //    - bodyBytes is null or failure; or
        //    - response's status is not an ok status,
        if (body_bytes.has<Empty>() || body_bytes.has<Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag>() || !Fetch::Infrastructure::is_ok_status(response->status())) {
            // then set moduleMap[(url, moduleType)] to null, run onComplete given null, and abort these steps.
            module_map.set(url, module_type.to_byte_string(), { ModuleMap::EntryType::Failed, nullptr });
            on_complete->function()(nullptr);
            return;
        }

        // 2. Let sourceText be the result of UTF-8 decoding bodyBytes.
        auto decoder = TextCodec::decoder_for("UTF-8"sv);
        VERIFY(decoder.has_value());
        auto source_text = TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, body_bytes.get<ByteBuffer>()).release_value_but_fixme_should_propagate_errors();

        // 3. Let mimeType be the result of extracting a MIME type from response's header list.
        auto mime_type = response->header_list()->extract_mime_type();

        // 4. Let moduleScript be null.
        GC::Ptr<JavaScriptModuleScript> module_script;

        // FIXME: 5. Let referrerPolicy be the result of parsing the `Referrer-Policy` header given response. [REFERRERPOLICY]
        // FIXME: 6. If referrerPolicy is not the empty string, set options's referrer policy to referrerPolicy.

        // 7. If mimeType is a JavaScript MIME type and moduleType is "javascript", then set moduleScript to the result of creating a JavaScript module script given sourceText, moduleMapRealm, response's URL, and options.
        // FIXME: Pass options.
        if (mime_type->is_javascript() && module_type == "javascript")
            module_script = JavaScriptModuleScript::create(url.basename(), source_text, module_map_realm, response->url().value_or({})).release_value_but_fixme_should_propagate_errors();

        // FIXME: 8. If the MIME type essence of mimeType is "text/css" and moduleType is "css", then set moduleScript to the result of creating a CSS module script given sourceText and settingsObject.
        // FIXME: 9. If mimeType is a JSON MIME type and moduleType is "json", then set moduleScript to the result of creating a JSON module script given sourceText and settingsObject.

        // 10. Set moduleMap[(url, moduleType)] to moduleScript, and run onComplete given moduleScript.
        module_map.set(url, module_type.to_byte_string(), { ModuleMap::EntryType::ModuleScript, module_script });
        on_complete->function()(module_script);
    };

    if (perform_fetch != nullptr) {
        perform_fetch->function()(request, is_top_level, move(process_response_consume_body)).release_value_but_fixme_should_propagate_errors();
    } else {
        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
        fetch_algorithms_input.process_response_consume_body = move(process_response_consume_body);
        Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(realm.vm(), move(fetch_algorithms_input))).release_value_but_fixme_should_propagate_errors();
    }
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-module-script-tree
// https://whatpr.org/html/9893/webappapis.html#fetch-a-module-script-tree
void fetch_external_module_script_graph(JS::Realm& realm, URL::URL const& url, EnvironmentSettingsObject& settings_object, ScriptFetchOptions const& options, OnFetchScriptComplete on_complete)
{
    auto steps = create_on_fetch_script_complete(realm.heap(), [&realm, &settings_object, on_complete, url](auto result) mutable {
        // 1. If result is null, run onComplete given null, and abort these steps.
        if (!result) {
            on_complete->function()(nullptr);
            return;
        }

        // 2. Fetch the descendants of and link result given settingsObject, "script", and onComplete.
        auto& module_script = as<JavaScriptModuleScript>(*result);
        fetch_descendants_of_and_link_a_module_script(realm, module_script, settings_object, Fetch::Infrastructure::Request::Destination::Script, nullptr, on_complete);
    });

    // 1. Fetch a single module script given url, settingsObject, "script", options, settingsObject's realm, "client", true, and with the following steps given result:
    fetch_single_module_script(realm, url, settings_object, Fetch::Infrastructure::Request::Destination::Script, options, settings_object.realm(), Web::Fetch::Infrastructure::Request::Referrer::Client, {}, TopLevelModule::Yes, nullptr, steps);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-an-inline-module-script-graph
void fetch_inline_module_script_graph(JS::Realm& realm, ByteString const& filename, ByteString const& source_text, URL::URL const& base_url, EnvironmentSettingsObject& settings_object, OnFetchScriptComplete on_complete)
{
    // 1. Let script be the result of creating a JavaScript module script using sourceText, settingsObject's realm, baseURL, and options.
    auto script = JavaScriptModuleScript::create(filename, source_text.view(), settings_object.realm(), base_url).release_value_but_fixme_should_propagate_errors();

    // 2. Fetch the descendants of and link script, given settingsObject, "script", and onComplete.
    fetch_descendants_of_and_link_a_module_script(realm, *script, settings_object, Fetch::Infrastructure::Request::Destination::Script, nullptr, on_complete);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-imported-module-script
void fetch_single_imported_module_script(JS::Realm& realm,
    URL::URL const& url,
    EnvironmentSettingsObject& fetch_client,
    Fetch::Infrastructure::Request::Destination destination,
    ScriptFetchOptions const& options,
    JS::Realm& module_map_realm,
    Fetch::Infrastructure::Request::ReferrerType referrer,
    JS::ModuleRequest const& module_request,
    PerformTheFetchHook perform_fetch,
    OnFetchScriptComplete on_complete)
{
    // 1. Assert: moduleRequest.[[Attributes]] does not contain any Record entry such that entry.[[Key]] is not "type",
    //    because we only asked for "type" attributes in HostGetSupportedImportAttributes.
    for (auto const& entry : module_request.attributes)
        VERIFY(entry.key == "type"sv);

    // 2. Let moduleType be the result of running the module type from module request steps given moduleRequest.
    auto module_type = module_type_from_module_request(module_request);

    // 3. If the result of running the module type allowed steps given moduleType and moduleMapRealm is false,
    //    then run onComplete given null, and return.
    if (!module_type_allowed(module_map_realm, module_type)) {
        on_complete->function()(nullptr);
        return;
    }

    // 4. Fetch a single module script given url, fetchClient, destination, options, moduleMapRealm, referrer, moduleRequest, false,
    //    and onComplete. If performFetch was given, pass it along as well.
    fetch_single_module_script(realm, url, fetch_client, destination, options, module_map_realm, referrer, module_request, TopLevelModule::No, perform_fetch, on_complete);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-the-descendants-of-and-link-a-module-script
void fetch_descendants_of_and_link_a_module_script(JS::Realm& realm,
    JavaScriptModuleScript& module_script,
    EnvironmentSettingsObject& fetch_client,
    Fetch::Infrastructure::Request::Destination destination,
    PerformTheFetchHook perform_fetch,
    OnFetchScriptComplete on_complete)
{
    // 1. Let record be moduleScript's record.
    auto* record = module_script.record();

    // 2. If record is null, then:
    if (!record) {
        // 1. Set moduleScript's error to rethrow to moduleScript's parse error.
        module_script.set_error_to_rethrow(module_script.parse_error());

        // 2. Run onComplete given moduleScript.
        on_complete->function()(module_script);

        // 3. Return.
        return;
    }

    // 3. Let state be Record { [[ParseError]]: null, [[Destination]]: destination, [[PerformFetch]]: null, [[FetchClient]]: fetchClient }.
    auto state = realm.heap().allocate<FetchContext>(JS::js_null(), destination, nullptr, fetch_client);

    // 4. If performFetch was given, set state.[[PerformFetch]] to performFetch.
    state->perform_fetch = perform_fetch;

    // FIXME: These should most likely be steps in the spec.
    // NOTE: For reasons beyond my understanding, we cannot use TemporaryExecutionContext here.
    //       Calling perform_a_microtask_checkpoint() on the fetch_client's responsible_event_loop
    //       prevents this from functioning properly. HTMLParser::the_end would be run before
    //       HTMLScriptElement::prepare_script had a chance to setup the callback to mark_done properly,
    //       resulting in the event loop hanging forever awaiting for the script to be ready for parser
    //       execution.
    realm.vm().push_execution_context(fetch_client.realm_execution_context());
    prepare_to_run_callback(realm);

    // 5. Let loadingPromise be record.LoadRequestedModules(state).
    auto& loading_promise = record->load_requested_modules(state);

    // 6. Upon fulfillment of loadingPromise, run the following steps:
    WebIDL::upon_fulfillment(loading_promise, GC::create_function(realm.heap(), [&realm, record, &module_script, on_complete](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Perform record.Link().
        auto linking_result = record->link(realm.vm());

        // If this throws an exception, set result's error to rethrow to that exception.
        if (linking_result.is_throw_completion())
            module_script.set_error_to_rethrow(linking_result.release_error().value().value());

        // 2. Run onComplete given moduleScript.
        on_complete->function()(module_script);

        return JS::js_undefined();
    }));

    // 7. Upon rejection of loadingPromise, run the following steps:
    WebIDL::upon_rejection(loading_promise, GC::create_function(realm.heap(), [state, &module_script, on_complete](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. If state.[[ParseError]] is not null, set moduleScript's error to rethrow to state.[[ParseError]] and run
        //    onComplete given moduleScript.
        if (!state->parse_error.is_null()) {
            module_script.set_error_to_rethrow(state->parse_error);

            on_complete->function()(module_script);
        }
        // 2. Otherwise, run onComplete given null.
        else {
            on_complete->function()(nullptr);
        }

        return JS::js_undefined();
    }));

    clean_up_after_running_callback(realm);

    realm.vm().pop_execution_context();
}

}
