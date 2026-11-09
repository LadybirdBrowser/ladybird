/*
 * Copyright (c) 2022-2023, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefCounted.h>
#include <AK/Utf16String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/ScriptCompilation.h>
#include <LibJS/SourceCode.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/CompiledScriptCache.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/ThreadPoolWork.h>
#include <LibWeb/ReferrerPolicy/AbstractOperations.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>
#include <LibWebCommon/Infra/Strings.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::HTML {

struct OffThreadCompiledProgram {
    JS::ParsedProgram parsed {};
    JS::CompiledProgram compiled {};
};

}

// NB: A parsed or compiled program is held outside of the GC heap.
GC_UNTRACED_CAPTURE(Web::HTML::OffThreadCompiledProgram);

namespace Web::HTML {

static ReadonlyBytes body_bytes_view(Fetch::Infrastructure::FetchAlgorithms::BodyBytes const& body_bytes)
{
    return body_bytes.get<Core::ImmutableBytes>().bytes();
}

static ByteBuffer take_body_bytes_as_byte_buffer(Fetch::Infrastructure::FetchAlgorithms::BodyBytes& body_bytes)
{
    return body_bytes.get<Core::ImmutableBytes>().copy_to_byte_buffer().release_value_but_fixme_should_propagate_errors();
}

// The key that a script compiled from a response is stored under, if the rules of HTTP caching let a cache store the
// response, and the request has a partition to store it in.
static Optional<CompiledScriptCacheKey> compiled_script_cache_key(Fetch::Infrastructure::Request const& request, URL::URL const& url, u32 status_code, HTTP::HeaderList const& response_headers, JS::ProgramType type, size_t line_number_offset)
{
    if (!Fetch::Fetching::http_memory_cache_enabled() || request.cache_mode() == HTTP::CacheMode::NoStore)
        return {};
    if (!HTTP::is_cacheable(request.method(), request.header_list()) || !HTTP::is_cacheable(status_code, response_headers))
        return {};

    auto partition = Fetch::Fetching::http_cache_partition(request);
    if (!partition.has_value())
        return {};

    return CompiledScriptCacheKey {
        .partition = partition.release_value(),
        .url_without_fragment = HTTP::serialize_url_for_cache_storage(url),
        .program_type = type,
        .line_number_offset = line_number_offset,
    };
}

// Submit parsing and top-level bytecode generation to the thread pool, then bounce back to the main thread via
// deferred_invoke once the worker is done, and on to a task of the settings object's global there. Syntax errors still
// come back as a ParsedProgram so the main thread can report them through the same Script/ModuleScript construction
// paths; successful programs come back as CompiledProgram artifacts whose GC-backed Executable materialization must
// still happen on the main thread.
// With a cache key, a program that the cache of compiled scripts has for the source code skips all of this, and
// otherwise the program goes there before anything runs it.
static void compile_off_thread(EnvironmentSettingsObject& settings_object, Utf16String source_filename, Utf16String source_text, JS::ProgramType type, size_t line_number_offset, Optional<CompiledScriptCacheKey> cache_key, GC::Ref<GC::Function<void(OffThreadCompiledProgram, NonnullRefPtr<JS::SourceCode const>)>> on_compiled)
{
    // The script runs in a task of the settings object's global, so never beside a rendering update in flight that holds
    // back the tasks of its document, and the microtasks it queues (e.g. promise reactions from react_to_promise during
    // module linking) are drained after it.
    auto run_callback = [settings_object = GC::make_root(settings_object), on_compiled = GC::make_root(on_compiled), source_filename = move(source_filename)](OffThreadCompiledProgram result, Utf16String source_text) mutable {
        auto source_code = JS::SourceCode::create(move(source_filename), move(source_text));
        queue_global_task(Task::Source::Networking, settings_object->global_object(), GC::create_function(GC::Heap::the(), GC::captures(on_compiled, move(source_code), move(result)), [](auto& on_compiled, auto& source_code, auto& result) {
            on_compiled->function()(move(result), move(source_code));
        }));
    };

    if (cache_key.has_value()) {
        if (auto hit = CompiledScriptCache::the().find(*cache_key, source_text); hit.has_value()) {
            run_callback(OffThreadCompiledProgram { .compiled = move(hit->program) }, move(hit->source_text));
            return;
        }
    }

    struct OffThreadCompilation {
        OffThreadCompiledProgram result;
        Utf16String source_text;
    };

    auto compile = [source_text = move(source_text), type, line_number_offset]() mutable {
        OffThreadCompiledProgram result { .parsed = JS::ParsedProgram::parse(source_text, type, line_number_offset) };
        if (result.parsed && !result.parsed.has_errors())
            result.compiled = JS::CompiledProgram::compile(move(result.parsed));
        return OffThreadCompilation { .result = move(result), .source_text = move(source_text) };
    };
    Platform::run_on_thread_pool<OffThreadCompilation>(move(compile),
        [cache_key = move(cache_key), run_callback = move(run_callback)](OffThreadCompilation compilation) mutable {
            auto result = move(compilation.result);
            if (cache_key.has_value() && result.compiled) {
                // NB: Another load of the script may have stored it while this one compiled.
                if (auto hit = CompiledScriptCache::the().find(*cache_key, compilation.source_text); hit.has_value()) {
                    result.compiled = move(hit->program);
                    compilation.source_text = move(hit->source_text);
                } else {
                    CompiledScriptCache::the().store(cache_key.release_value(), compilation.source_text, result.compiled);
                }
            }

            run_callback(move(result), move(compilation.source_text));
        });
}

GC_DEFINE_ALLOCATOR(FetchContext);

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
Utf16String module_type_from_module_request(JS::ModuleRequest const& module_request)
{
    // 1. Let moduleType be "javascript-or-wasm".
    auto module_type = "javascript-or-wasm"_utf16;

    // 2. If moduleRequest.[[Attributes]] has a Record entry such that entry.[[Key]] is "type", then:
    for (auto const& entry : module_request.attributes) {
        if (entry.key != "type"_string)
            continue;

        // 1. If entry.[[Value]] is "javascript-or-wasm", then set moduleType to null.
        if (entry.value == u"javascript-or-wasm"sv)
            module_type = ""_utf16; // FIXME: This should be null!
        // 2. Otherwise, set moduleType to entry.[[Value]].
        else
            module_type = entry.value;
    }

    // 3. Return moduleType.
    return module_type;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#resolve-a-module-specifier
WebIDL::ExceptionOr<URL::URL> resolve_module_specifier(Optional<Script&> referring_script, Utf16View specifier)
{
    auto& vm = JS::VM::the();

    // 1. Let settingsObject and baseURL be null.
    GC::Ptr<EnvironmentSettingsObject> settings_object;
    Optional<URL::URL> base_url;

    // 2. If referringScript is not null, then:
    if (referring_script.has_value()) {
        // 1. Set settingsObject to referringScript's settings object.
        settings_object = referring_script->settings_object();

        // 2. Set baseURL to referringScript's base URL.
        base_url = referring_script->base_url();
    }
    // 3. Otherwise:
    else {
        // 1. Assert: there is a current realm.
        VERIFY(vm.current_realm());

        // 2. Set settingsObject to the current settings object.
        settings_object = current_settings_object();

        // 3. Set baseURL to settingsObject's API base URL.
        base_url = settings_object->api_base_url();
    }

    // 4. Let importMap be an empty import map.
    ImportMap import_map;

    // 5. If settingsObject's global object implements Window, then set importMap to settingsObject's global object's import map.
    if (auto* window = window_from_global_object(settings_object->global_object()))
        import_map = window->import_map();

    // 6. Let serializedBaseURL be baseURL, serialized.
    auto serialized_base_url = utf16_string_from_url_ascii(base_url->serialize());

    // 7. Let asURL be the result of resolving a URL-like module specifier given specifier and baseURL.
    auto as_url = resolve_url_like_module_specifier(specifier, *base_url);

    // 8. Let normalizedSpecifier be the serialization of asURL, if asURL is non-null; otherwise, specifier.
    auto normalized_specifier = as_url.has_value() ? utf16_string_from_url_ascii(as_url->serialize()) : Utf16String::from_utf16(specifier);

    // 9. Let result be a URL-or-null, initially null.
    Optional<URL::URL> result;

    // 10. For each scopePrefix → scopeImports of importMap's scopes:
    for (auto const& [scope_prefix, scope_imports] : import_map.scopes()) {
        // 1. If scopePrefix is serializedBaseURL, or if scopePrefix ends with U+002F (/) and scopePrefix is a code unit prefix of serializedBaseURL, then:
        if (scope_prefix == serialized_base_url
            || (scope_prefix.utf16_view().ends_with('/')
                && Infra::is_code_unit_prefix(scope_prefix.utf16_view(), serialized_base_url.utf16_view()))) {
            // 1. Let scopeImportsMatch be the result of resolving an imports match given normalizedSpecifier, asURL, and scopeImports.
            auto scope_imports_match = TRY(resolve_imports_match(normalized_specifier.utf16_view(), as_url, scope_imports));

            // 2. If scopeImportsMatch is not null, then set result to scopeImportsMatch, and break.
            if (scope_imports_match.has_value()) {
                result = scope_imports_match.release_value();
                break;
            }
        }
    }

    // 11. If result is null, set result to the result of resolving an imports match given normalizedSpecifier, asURL, and importMap's imports.
    if (!result.has_value())
        result = TRY(resolve_imports_match(normalized_specifier.utf16_view(), as_url, import_map.imports()));

    // 12. If result is null, set it to asURL.
    // NOTE: By this point, if result was null, specifier wasn't remapped to anything by importMap, but it might have
    //       been able to be turned into a URL.
    if (!result.has_value())
        result = as_url;

    // 13. If result is not null, then:
    if (result.has_value()) {
        // 1. Add module to resolved module set given settingsObject, serializedBaseURL, normalizedSpecifier, and asURL.
        add_module_to_resolved_module_set(*settings_object, serialized_base_url, normalized_specifier, as_url);

        // 2. Return result.
        return result.release_value();
    }

    // 14. Throw a TypeError indicating that specifier was a bare specifier, but was not remapped to anything by importMap.
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Failed to resolve non relative module specifier '{}' from an import map.", specifier) };
}

// https://html.spec.whatwg.org/multipage/webappapis.html#resolving-an-imports-match
WebIDL::ExceptionOr<Optional<URL::URL>> resolve_imports_match(Utf16View normalized_specifier, Optional<URL::URL> as_url, ModuleSpecifierMap const& specifier_map)
{
    // 1. For each specifierKey → resolutionResult of specifierMap:
    for (auto const& [specifier_key, resolution_result] : specifier_map) {
        // 1. If specifierKey is normalizedSpecifier, then:
        if (specifier_key.utf16_view() == normalized_specifier) {
            // 1. If resolutionResult is null, then throw a TypeError indicating that resolution of specifierKey was blocked by a null entry.
            if (!resolution_result.has_value())
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Import resolution of '{}' was blocked by a null entry.", specifier_key) };

            // 2. Assert: resolutionResult is a URL.
            // 3. Return resolutionResult.
            return resolution_result;
        }

        // 2. If all of the following are true:
        if (
            // - specifierKey ends with U+002F (/);
            specifier_key.utf16_view().ends_with('/') &&
            // - specifierKey is a code unit prefix of normalizedSpecifier; and
            Infra::is_code_unit_prefix(specifier_key.utf16_view(), normalized_specifier) &&
            // - either asURL is null, or asURL is special,
            (!as_url.has_value() || as_url->is_special())
            // then:
        ) {
            // 1. If resolutionResult is null, then throw a TypeError indicating that the resolution of specifierKey was blocked by a null entry.
            if (!resolution_result.has_value())
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Import resolution of '{}' was blocked by a null entry.", specifier_key) };

            // 2. Assert: resolutionResult is a URL.
            // 3. Let afterPrefix be the portion of normalizedSpecifier after the initial specifierKey prefix.
            // FIXME: Clarify if this is meant by the portion after the initial specifierKey prefix.
            auto after_prefix = normalized_specifier.substring_view(specifier_key.length_in_code_units());

            // 4. Assert: resolutionResult, serialized, ends with U+002F (/), as enforced during parsing.
            VERIFY(resolution_result->serialize().ends_with('/'));

            // 5. Let url be the result of URL parsing afterPrefix with resolutionResult.
            auto url = DOMURL::parse(after_prefix, *resolution_result);

            // 6. If url is failure, then throw a TypeError indicating that resolution of normalizedSpecifier was blocked since the afterPrefix portion
            //    could not be URL-parsed relative to the resolutionResult mapped to by the specifierKey prefix.
            if (!url.has_value())
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Could not resolve '{}' as the after prefix portion could not be URL-parsed.", normalized_specifier) };

            // 7. Assert: url is a URL.
            VERIFY(url.has_value());

            // 8. If the serialization of resolutionResult is not a code unit prefix of the serialization of url, then throw a TypeError indicating
            //    that the resolution of normalizedSpecifier was blocked due to it backtracking above its prefix specifierKey.
            auto serialized_resolution_result = utf16_string_from_url_ascii(resolution_result->serialize());
            auto serialized_url = utf16_string_from_url_ascii(url->serialize());
            if (!Infra::is_code_unit_prefix(serialized_resolution_result.utf16_view(), serialized_url.utf16_view()))
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Could not resolve '{}' as it backtracks above its prefix specifierKey.", normalized_specifier) };

            // 9. Return url.
            return url;
        }
    }

    // 2. Return null.
    return Optional<URL::URL> {};
}

// https://html.spec.whatwg.org/multipage/webappapis.html#resolving-a-url-like-module-specifier
Optional<URL::URL> resolve_url_like_module_specifier(Utf16View specifier, URL::URL const& base_url)
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
void set_up_module_script_request(Fetch::Infrastructure::Request& request, ScriptFetchOptions const& options)
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
    auto integrity = resolve_a_module_integrity_metadata(url, settings_object);

    // 3. Set newOptions's integrity metadata to integrity.
    new_options.integrity_metadata = integrity;

    // 4. Set newOptions's fetch priority to "auto".
    new_options.fetch_priority = Fetch::Infrastructure::Request::Priority::Auto;

    // 5. Return newOptions.
    return new_options;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#resolving-a-module-integrity-metadata
Utf16String resolve_a_module_integrity_metadata(URL::URL const& url, EnvironmentSettingsObject& settings_object)
{
    // 1. Let map be settingsObject's global object's import map.
    // NB: Only a Window's import map is ever modified from the initial empty one.
    auto const* window = window_from_global_object(settings_object.global_object());
    if (!window)
        return {};

    // 2. If map's integrity[url] does not exist, then return the empty string.
    // 3. Return map's integrity[url].
    return window->import_map().integrity().get(url).value_or(""_utf16);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-classic-script
void fetch_classic_script(JS::Realm& realm, URL::URL const& url, EnvironmentSettingsObject& settings_object, ScriptFetchOptions options, CORSSettingAttribute cors_setting, Utf16String character_encoding, OnFetchScriptComplete on_complete)
{
    // 1. Let request be the result of creating a potential-CORS request given url, "script", and CORS setting.
    auto request = create_potential_CORS_request(url, Fetch::Infrastructure::Request::Destination::Script, cors_setting);

    // 2. Set request's client to settings object.
    request->set_client(settings_object.address());

    // 3. Set request's initiator type to "script".
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Script);

    // 4. Set up the classic script request given request and options.
    set_up_classic_script_request(*request, options);

    // NB: The script is created in several places below. They all hand it to onComplete, which is where we set its
    //     fetch options to options.
    on_complete = create_on_fetch_script_complete(GC::Heap::the(), GC::captures(options, on_complete), [](auto& options, auto& on_complete, GC::Ptr<Script> script) {
        if (script)
            script->set_fetch_options(options);
        on_complete->function()(script);
    });

    // 5. Fetch request with the following processResponseConsumeBody steps given response response and null, failure,
    //    or a byte sequence bodyBytes:
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::ConsumeForCallback };
    fetch_algorithms_input.process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(request, settings_object, move(character_encoding), on_complete), [](NonnullRefPtr<Fetch::Infrastructure::Request>& request, EnvironmentSettingsObject& settings_object, Utf16String& character_encoding, OnFetchScriptComplete& on_complete, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
        // NB: Whether response was CORS-cross-origin must be determined before unwrapping it, since the internal
        //     response of an opaque filtered response is never CORS-cross-origin itself.
        auto response_was_cors_cross_origin = response->is_cors_cross_origin();

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
        auto potential_mime_type_for_encoding = Fetch::Infrastructure::extract_mime_type(response->header_list());

        // 4. Set character encoding to the result of legacy extracting an encoding given potentialMIMETypeForEncoding
        //    and character encoding.
        auto fallback_character_encoding = TextCodec::get_standardized_encoding(character_encoding);
        VERIFY(fallback_character_encoding.has_value());
        auto extracted_character_encoding = Fetch::Infrastructure::legacy_extract_an_encoding(potential_mime_type_for_encoding, *fallback_character_encoding);

        // 5. Let source text be the result of decoding bodyBytes to Unicode, using character encoding as the fallback
        //    encoding.
        auto fallback_decoder = TextCodec::decoder_for(extracted_character_encoding);
        VERIFY(fallback_decoder.has_value());
        auto source_text = TextCodec::decode_to_utf16(body_bytes_view(body_bytes), *fallback_decoder).release_value_but_fixme_should_propagate_errors();

        // 6. Let muted errors be true if response was CORS-cross-origin, and false otherwise.
        auto muted_errors = response_was_cors_cross_origin ? ClassicScript::MutedErrors::Yes : ClassicScript::MutedErrors::No;

        // 7. Let script be the result of creating a classic script given source text, settings object's realm, response's URL,
        //    options, and muted errors.
        auto response_url = response->url().value_or({});

        // NB: The filename is the request's URL. The response's URL could reveal where a cross-origin request was
        //     redirected.
        auto filename = request->url().to_byte_string();
        auto source_filename = utf16_string_from_url_ascii(filename.view());

        auto cache_key = compiled_script_cache_key(request, request->url(), response->status(), response->header_list(), JS::ProgramType::Script, 1);

        compile_off_thread(settings_object, move(source_filename), move(source_text), JS::ProgramType::Script, 1, move(cache_key),
            GC::create_function(GC::Heap::the(), GC::captures(move(response_url), move(filename), muted_errors, on_complete, settings_object), [](URL::URL& response_url, ByteString& filename, ClassicScript::MutedErrors muted_errors, OnFetchScriptComplete& on_complete, EnvironmentSettingsObject& settings_object, OffThreadCompiledProgram result, NonnullRefPtr<JS::SourceCode const> source_code) {
                auto script = result.compiled
                    ? ClassicScript::create_from_pre_compiled(move(filename), source_code, settings_object, move(response_url), move(result.compiled), muted_errors)
                    : ClassicScript::create_from_pre_parsed(move(filename), source_code, settings_object, move(response_url), move(result.parsed), muted_errors);
                on_complete->function()(script);
            }));
    });

    Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-classic-worker-script
WebIDL::ExceptionOr<void> fetch_classic_worker_script(URL::URL const& url, FetchClient const& fetch_client, Fetch::Infrastructure::Request::Destination destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook perform_fetch, OnFetchScriptComplete on_complete)
{
    auto& realm = settings_object.realm();
    // 1. Let request be a new request whose URL is url, client is fetchClient, destination is destination, initiator type is "other",
    //    mode is "same-origin", credentials mode is "same-origin", parser metadata is "not parser-inserted",
    //    and whose use-URL-credentials flag is set.
    auto request = Fetch::Infrastructure::Request::create();
    request->set_url(url);
    request->set_client(Fetch::Fetching::request_client(fetch_client));
    request->set_destination(destination);
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Other);
    request->set_mode(Fetch::Infrastructure::Request::Mode::SameOrigin);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::SameOrigin);
    request->set_parser_metadata(Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted);
    request->set_use_url_credentials(true);

    auto process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(settings_object, on_complete), [](EnvironmentSettingsObject& settings_object, OnFetchScriptComplete& on_complete, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
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
        auto maybe_mime_type = Fetch::Infrastructure::extract_mime_type(response->header_list());
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
        auto source_text = Utf16String::from_utf8_with_replacement_character(body_bytes_view(body_bytes));

        // 5. Let script be the result of creating a classic script using sourceText, settingsObject,
        //    response's URL, and the default classic script fetch options.
        auto response_url = response->url().value_or({});
        auto script = ClassicScript::create(response_url.to_byte_string(), source_text, settings_object, response_url);

        // 6. Run onComplete given script.
        on_complete->function()(script);
    });

    // 2. If performFetch was given, run performFetch with request, true, and with processResponseConsumeBody as defined below.
    if (perform_fetch != nullptr) {
        TRY(perform_fetch->function()(request, TopLevelModule::Yes, process_response_consume_body));
    }

    // Otherwise, fetch request with processResponseConsumeBody set to processResponseConsumeBody as defined below.
    else {
        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::ConsumeForCallback };
        fetch_algorithms_input.process_response_consume_body = process_response_consume_body;
        Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));
    }
    return {};
}

// NB: The response and body bytes of a classic worker imported script's fetch, which its processResponseConsumeBody
//     sets while the fetching algorithm pauses.
struct ImportedScriptResponseHolder : RefCounted<ImportedScriptResponseHolder> {
    void visit_edges(GC::Cell::Visitor& visitor) const { visitor.visit(response); }

    GC::Ptr<Fetch::Infrastructure::ResponseView> response;
    Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes;
};

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-classic-worker-imported-script
WebIDL::ExceptionOr<GC::Ref<ClassicScript>> fetch_a_classic_worker_imported_script(URL::URL const& url, HTML::EnvironmentSettingsObject& settings_object, PerformTheFetchHook perform_fetch)
{
    auto& realm = settings_object.realm();

    // 1. Let response be null.
    // 2. Let bodyBytes be null.
    auto holder = make_ref_counted<ImportedScriptResponseHolder>();

    // 3. Let request be a new request whose URL is url, client is settingsObject, destination is "script", initiator type is "other",
    //    parser metadata is "not parser-inserted", and whose use-URL-credentials flag is set.
    auto request = Fetch::Infrastructure::Request::create();
    request->set_url(url);
    request->set_client(settings_object.address());
    request->set_destination(Fetch::Infrastructure::Request::Destination::Script);
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Other);
    request->set_parser_metadata(Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted);
    request->set_use_url_credentials(true);

    auto process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(holder), [](NonnullRefPtr<ImportedScriptResponseHolder>& holder, GC::Ref<Fetch::Infrastructure::ResponseView> res, Fetch::Infrastructure::FetchAlgorithms::BodyBytes bb) {
        // 1. Set bodyBytes to bb.
        holder->body_bytes = move(bb);

        // 2. Set response to res.
        holder->response = res;
    });

    // 4. If performFetch was given, run performFetch with request, isTopLevel, and with processResponseConsumeBody as defined below.
    if (perform_fetch) {
        TRY(perform_fetch->function()(request, TopLevelModule::Yes, process_response_consume_body));
    }
    // Otherwise, fetch request with processResponseConsumeBody set to processResponseConsumeBody as defined below.
    // AD-HOC: The fetch runs on a parallel queue, since the event loop it would otherwise queue tasks on is paused below.
    else {
        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::SyncAccumulate };
        fetch_algorithms_input.process_response_consume_body = process_response_consume_body;
        Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)), Fetch::Fetching::UseParallelQueue::Yes);
    }

    // 5. Pause until response is not null.
    auto& event_loop = settings_object.responsible_event_loop();
    auto response_is_not_null = GC::create_function(GC::Heap::the(), GC::captures(holder), [](NonnullRefPtr<ImportedScriptResponseHolder>& holder) -> bool {
        return !!holder->response;
    });
    if (perform_fetch) {
        // AD-HOC: The event loop is spun rather than paused, since a perform the fetch hook may queue tasks on it.
        event_loop.spin_until(response_is_not_null);
    } else {
        auto pause_handle = event_loop.pause(EventLoop::UpdateTheRendering::No);
        Platform::EventLoopPlugin::the().spin_until(response_is_not_null);
    }
    auto response = holder->response;
    auto body_bytes = move(holder->body_bytes);

    // NB: The event loop gives up on an aborted script before the response comes.
    if (!response)
        return WebIDL::NetworkError::create("Network error"_utf16);

    // NB: Whether response was CORS-cross-origin must be determined before unwrapping it, since the internal
    //     response of an opaque filtered response is never CORS-cross-origin itself.
    auto response_was_cors_cross_origin = response->is_cors_cross_origin();

    // 6. Set response to response's unsafe response.
    response = response->unsafe_response();

    // 7. If any of the following are true:
    //    - bodyBytes is null or failure;
    //    - response's status is not an ok status; or
    //    - the result of extracting a MIME type from response's header list is not a JavaScript MIME type,
    //    then throw a "NetworkError" DOMException.
    if (body_bytes.template has<Empty>() || body_bytes.template has<Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag>()
        || !Fetch::Infrastructure::is_ok_status(response->status())
        || !Fetch::Infrastructure::extract_mime_type(response->header_list()).has_value() || !Fetch::Infrastructure::extract_mime_type(response->header_list())->is_javascript()) {
        return WebIDL::NetworkError::create("Network error"_utf16);
    }

    // 8. Let sourceText be the result of UTF-8 decoding bodyBytes.
    auto source_text = Utf16String::from_utf8_with_replacement_character(body_bytes_view(body_bytes));

    // 9. Let mutedErrors be true if response was CORS-cross-origin, and false otherwise.
    auto muted_errors = response_was_cors_cross_origin ? ClassicScript::MutedErrors::Yes : ClassicScript::MutedErrors::No;

    // 10. Let script be the result of creating a classic script given sourceText, settingsObject, response's URL, the default classic script fetch options, and mutedErrors.
    auto response_url = response->url().value_or({});
    auto script = ClassicScript::create(url.to_byte_string(), source_text, settings_object, response_url, 1, muted_errors);

    // 11. Return script.
    return script;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-module-worker-script-tree
WebIDL::ExceptionOr<void> fetch_module_worker_script_graph(URL::URL const& url, FetchClient const& fetch_client, Fetch::Infrastructure::Request::Destination destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook perform_fetch, OnFetchScriptComplete on_complete, Fetch::Infrastructure::Request::CredentialsMode credentials_mode)
{
    return fetch_worklet_module_worker_script_graph(url, fetch_client, destination, settings_object, move(perform_fetch), move(on_complete), credentials_mode);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-worklet/module-worker-script-graph
WebIDL::ExceptionOr<void> fetch_worklet_module_worker_script_graph(URL::URL const& url, FetchClient const& fetch_client, Fetch::Infrastructure::Request::Destination destination, EnvironmentSettingsObject& settings_object, PerformTheFetchHook perform_fetch, OnFetchScriptComplete on_complete, Fetch::Infrastructure::Request::CredentialsMode credentials_mode)
{
    auto& realm = settings_object.realm();

    // 1. Let options be a script fetch options whose cryptographic nonce is the empty string,
    //    integrity metadata is the empty string, parser metadata is "not-parser-inserted",
    //    credentials mode is credentialsMode, referrer policy is the empty string, and fetch priority is "auto".
    auto options = ScriptFetchOptions {
        .cryptographic_nonce = {},
        .integrity_metadata = {},
        .parser_metadata = Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted,
        .credentials_mode = credentials_mode,
        .referrer_policy = ReferrerPolicy::ReferrerPolicy::EmptyString,
        .fetch_priority = Fetch::Infrastructure::Request::Priority::Auto
    };

    // onSingleFetchComplete given result is the following algorithm:
    auto on_single_fetch_complete = create_on_fetch_script_complete(GC::Heap::the(), GC::captures(realm, fetch_client, destination, perform_fetch, on_complete), [](auto& realm, auto& fetch_client, auto& destination, auto& perform_fetch, auto& on_complete, GC::Ptr<Script> result) {
        // 1. If result is null, run onComplete with null, and abort these steps.
        if (!result) {
            dbgln("on single fetch complete with nool");
            on_complete->function()(nullptr);
            return;
        }

        // 2. Fetch the descendants of and link result given fetchClient, destination, and onComplete. If performFetch was given, pass it along as well.
        fetch_descendants_of_and_link_a_module_script(realm, as<ModuleScript>(*result), fetch_client, destination, perform_fetch, on_complete);
    });

    // 2. Fetch a single module script given url, fetchClient, destination, options, settingsObject, "client", true,
    //    and onSingleFetchComplete as defined below. If performFetch was given, pass it along as well.
    fetch_single_module_script(realm, url, fetch_client, destination, options, settings_object, Fetch::Infrastructure::Request::Referrer::Client, {}, TopLevelModule::Yes, move(perform_fetch), on_single_fetch_complete);

    return {};
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-destination-from-module-type
Fetch::Infrastructure::Request::Destination fetch_destination_from_module_type(Fetch::Infrastructure::Request::Destination default_destination, Utf16View module_type)
{
    // 1. If moduleType is "json", then return "json".
    if (module_type == "json"sv)
        return Fetch::Infrastructure::Request::Destination::JSON;

    // 2. If moduleType is "css", then return "style".
    if (module_type == "css"sv)
        return Fetch::Infrastructure::Request::Destination::Style;

    // 3. If moduleType is "text", then return "text".
    if (module_type == "text"sv)
        return Fetch::Infrastructure::Request::Destination::Text;

    // 4. Return defaultDestination.
    return default_destination;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-module-script
void fetch_single_module_script(JS::Realm& realm,
    URL::URL const& url,
    FetchClient const& fetch_client,
    Fetch::Infrastructure::Request::Destination destination,
    ScriptFetchOptions const& options,
    EnvironmentSettingsObject& settings_object,
    Web::Fetch::Infrastructure::Request::ReferrerType const& referrer,
    Optional<JS::ModuleRequest> const& module_request,
    TopLevelModule is_top_level,
    PerformTheFetchHook perform_fetch,
    OnFetchScriptComplete on_complete)
{
    // 1. Let moduleType be "javascript-or-wasm".
    Utf16String module_type = "javascript-or-wasm"_utf16;

    // 2. If moduleRequest was given, then set moduleType to the result of running the module type from module request steps given moduleRequest.
    if (module_request.has_value())
        module_type = module_type_from_module_request(*module_request);

    // 3. Assert: the result of running the module type allowed steps given moduleType and settingsObject is true.
    //    Otherwise, we would not have reached this point because a failure would have been raised when inspecting
    //    moduleRequest.[[Attributes]] in HostLoadImportedModule or fetch a single imported module script.
    VERIFY(module_type_allowed(settings_object, module_type));

    // 4. Let moduleMap be settingsObject's module map.
    auto& module_map = settings_object.module_map();

    // 5. If moduleMap[(url, moduleType)] is a module script, run onComplete given
    //    moduleMap[(url, moduleType)], and return.
    auto entry = module_map.get(url, module_type);
    if (entry.has_value() && entry->has<GC::Ref<ModuleScript>>()) {
        on_complete->function()(entry->get<GC::Ref<ModuleScript>>());
        return;
    }

    // 6. If moduleMap[(url, moduleType)] is a list, append onComplete to moduleMap[(url, moduleType)], and return.
    if (entry.has_value() && entry->has<ModuleMap::CallbackList>()) {
        module_map.append(url, module_type, GC::create_function(GC::Heap::the(), GC::captures(on_complete), [](auto& on_complete, GC::Ptr<ModuleScript> module_script) {
            on_complete->function()(module_script);
        }));
        return;
    }

    // 7. Set moduleMap[(url, moduleType)] to « onComplete ».
    GC::RootVector<ModuleMap::CallbackFunction> callbacks;
    callbacks.append(GC::create_function(GC::Heap::the(), GC::captures(on_complete), [](auto& on_complete, GC::Ptr<ModuleScript> module_script) { on_complete->function()(module_script); }));
    module_map.set(url, module_type, move(callbacks));

    // 8. Let request be a new request whose URL is url, mode is "cors", referrer is referrer, and client is fetchClient.
    auto request = Fetch::Infrastructure::Request::create();
    request->set_url(url);
    request->set_mode(Fetch::Infrastructure::Request::Mode::CORS);
    request->set_referrer(referrer);
    request->set_client(Fetch::Fetching::request_client(fetch_client));

    // 9. Set request's destination to the result of running the fetch destination from module type steps given destination and moduleType.
    request->set_destination(fetch_destination_from_module_type(destination, module_type.utf16_view()));

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
    auto process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(request, module_map, url, module_type, settings_object, options), [](NonnullRefPtr<Fetch::Infrastructure::Request>& request, ModuleMap& module_map, URL::URL& url, Utf16String& module_type, EnvironmentSettingsObject& settings_object, ScriptFetchOptions& options, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
        auto internal_response = response->unsafe_response();

        // 1. If any of the following are true:
        //    - bodyBytes is null or failure; or
        //    - response's status is not an ok status,
        if (body_bytes.has<Empty>() || body_bytes.has<Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag>() || !Fetch::Infrastructure::is_ok_status(response->status())) {
            // then:
            // 1. Let callbacks be moduleMap[(url, moduleType)].
            // 2. Remove moduleMap[(url, moduleType)].
            // 3. For each callback of callbacks: run callback given null.
            // 4. Return.
            module_map.complete_fetch(url, module_type, nullptr);
            return;
        }

        // 2. Let mimeType be the result of extracting a MIME type from response's header list.
        auto mime_type = Fetch::Infrastructure::extract_mime_type(response->header_list());

        // 3. Let moduleScript be null.
        GC::Ptr<ModuleScript> module_script;

        // 4. Let referrerPolicy be the result of parsing the `Referrer-Policy` header given response. [REFERRERPOLICY]
        // INTEROP: Other browsers also honor the header of a CORS cross-origin response, whose filtered header list
        //          hides it. The policy only applies to the fetches of this module's descendants, whose referrer is
        //          this module's own URL.
        auto referrer_policy = ReferrerPolicy::parse_a_referrer_policy_from_a_referrer_policy_header(internal_response->record());

        // 5. If referrerPolicy is not the empty string, set options's referrer policy to referrerPolicy.
        if (referrer_policy != ReferrerPolicy::ReferrerPolicy::EmptyString)
            options.referrer_policy = referrer_policy;

        //  6. If mimeType's essence is "application/wasm" and moduleType is "javascript-or-wasm", then set moduleScript
        //     to the result of creating a WebAssembly module script given bodyBytes, settingsObject, response's URL, and
        //     options.
        if (mime_type.has_value() && mime_type->essence() == "application/wasm"sv && module_type == "javascript-or-wasm"sv) {
            module_script = ModuleScript::create_a_webassembly_module_script(url.to_byte_string(), take_body_bytes_as_byte_buffer(body_bytes), settings_object, response->url().value_or({})).release_value_but_fixme_should_propagate_errors();
            if (module_script)
                module_script->set_fetch_options(options);
        }

        // 7. Otherwise
        else {
            // 1. Let sourceText be the result of UTF-8 decoding bodyBytes.
            auto source_text = Utf16String::from_utf8_with_replacement_character(body_bytes_view(body_bytes));

            // 2. If moduleType is "text", then set moduleScript to the result of creating a text module script given
            //    sourceText and settingsObject.
            if (module_type == "text"sv) {
                module_script = ModuleScript::create_a_text_module_script(url.to_byte_string(), source_text.utf16_view(), settings_object).release_value_but_fixme_should_propagate_errors();
            }

            // 3. If mimeType is a JavaScript MIME type and moduleType is "javascript-or-wasm", then set moduleScript to
            //    the result of creating a JavaScript module script given sourceText, settingsObject, response's URL,
            //    and options.
            if (mime_type.has_value() && mime_type->is_javascript() && module_type == "javascript-or-wasm"sv) {
                auto url_string = url.to_byte_string();
                auto response_url = response->url().value_or({});
                auto module_type_string = module_type;
                auto source_filename = utf16_string_from_url_ascii(url_string.view());

                auto cache_key = compiled_script_cache_key(request, url, internal_response->status(), internal_response->header_list(), JS::ProgramType::Module, 0);

                compile_off_thread(settings_object, move(source_filename), move(source_text), JS::ProgramType::Module, 0, move(cache_key),
                    GC::create_function(GC::Heap::the(), GC::captures(url, move(url_string), move(response_url), move(module_type_string), options, settings_object), [](URL::URL& url, ByteString& url_string, URL::URL& response_url, Utf16String& module_type_string, ScriptFetchOptions& options, EnvironmentSettingsObject& settings_object, OffThreadCompiledProgram result, NonnullRefPtr<JS::SourceCode const> source_code) {
                        auto module_script = result.compiled
                            ? ModuleScript::create_from_pre_compiled(url_string, source_code, settings_object, move(response_url), move(result.compiled)).release_value_but_fixme_should_propagate_errors()
                            : ModuleScript::create_from_pre_parsed(url_string, source_code, settings_object, move(response_url), move(result.parsed)).release_value_but_fixme_should_propagate_errors();
                        if (module_script)
                            module_script->set_fetch_options(options);
                        settings_object.module_map().complete_fetch(url, module_type_string, module_script);
                    }));
                return;
            }

            // 4. If the MIME type essence of mimeType is "text/css" and moduleType is "css", then set moduleScript to
            //    the result of creating a CSS module script given sourceText and settingsObject.
            if (mime_type.has_value() && mime_type->essence() == "text/css"sv && module_type == "css"sv) {
                module_script = ModuleScript::create_a_css_module_script(url.to_byte_string(), source_text.utf16_view(), settings_object).release_value_but_fixme_should_propagate_errors();
            }

            // 5. If mimeType is a JSON MIME type and moduleType is "json", then set moduleScript to the result of
            //    creating a JSON module script given sourceText and settingsObject.
            if (mime_type.has_value() && mime_type->is_json() && module_type == "json"sv) {
                module_script = ModuleScript::create_a_json_module_script(url.to_byte_string(), source_text.utf16_view(), settings_object).release_value_but_fixme_should_propagate_errors();
            }
        }

        // 8. Let callbacks be moduleMap[(url, moduleType)].
        // 9. If moduleScript is null, then remove moduleMap[(url, moduleType)]; otherwise set
        //    moduleMap[(url, moduleType)] to moduleScript.
        // 10. For each callback of callbacks: run callback given moduleScript.
        module_map.complete_fetch(url, module_type, module_script);
    });

    if (perform_fetch != nullptr) {
        auto result = perform_fetch->function()(request, is_top_level, process_response_consume_body);
        // NB: The perform fetch hook can abort without running processResponseConsumeBody.
        if (result.is_exception())
            module_map.complete_fetch(url, module_type, nullptr);
    } else {
        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::ConsumeForCallback };
        fetch_algorithms_input.process_response_consume_body = process_response_consume_body;
        Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));
    }
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-module-script-tree
void fetch_external_module_script_graph(JS::Realm& realm, URL::URL const& url, EnvironmentSettingsObject& settings_object, ScriptFetchOptions const& options, OnFetchScriptComplete on_complete)
{
    auto steps = create_on_fetch_script_complete(GC::Heap::the(), GC::captures(realm, settings_object, on_complete), [](auto& realm, auto& settings_object, auto& on_complete, GC::Ptr<Script> result) {
        // 1. If result is null, run onComplete given null, and abort these steps.
        if (!result) {
            on_complete->function()(nullptr);
            return;
        }

        // 2. Fetch the descendants of and link result given settingsObject, "script", and onComplete.
        auto& module_script = as<ModuleScript>(*result);
        fetch_descendants_of_and_link_a_module_script(realm, module_script, GC::Ref { settings_object }, Fetch::Infrastructure::Request::Destination::Script, nullptr, on_complete);
    });

    // 1. Fetch a single module script given url, settingsObject, "script", options, settingsObject, "client", true, and with the following steps given result:
    fetch_single_module_script(realm, url, GC::Ref { settings_object }, Fetch::Infrastructure::Request::Destination::Script, options, settings_object, Web::Fetch::Infrastructure::Request::Referrer::Client, {}, TopLevelModule::Yes, nullptr, steps);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-modulepreload-module-script-graph
void fetch_modulepreload_module_script_graph(JS::Realm& realm, URL::URL const& url, Fetch::Infrastructure::Request::Destination destination, EnvironmentSettingsObject& settings_object, ScriptFetchOptions const& options, OnFetchScriptComplete on_complete)
{
    auto steps = create_on_fetch_script_complete(GC::Heap::the(), GC::captures(realm, settings_object, destination, on_complete), [](auto& realm, auto& settings_object, auto& destination, auto& on_complete, GC::Ptr<Script> result) {
        // 1. Run onComplete given result.
        on_complete->function()(result);

        // 2. Assert: settingsObject's global object implements Window.
        VERIFY(window_from_global_object(settings_object.global_object()));

        // 3. If result is not null, optionally fetch the descendants of and link result given settingsObject,
        //    destination, and an empty algorithm.
        if (result) {
            auto on_descendants_complete = create_on_fetch_script_complete(GC::Heap::the(), [](auto) { });
            fetch_descendants_of_and_link_a_module_script(realm, as<ModuleScript>(*result), GC::Ref { settings_object }, destination, nullptr, on_descendants_complete);
        }
    });

    // 1. Fetch a single module script given url, settingsObject, destination, options, settingsObject, "client", true,
    //    and with the following steps given result.
    // INTEROP: A modulepreload with an as value of "style", "json", or "text" must populate the same module map
    //          entry as an import with the corresponding type attribute. The HTML algorithm does not currently pass
    //          a module request to fetch a single module script, but this behavior is covered by web-platform-tests.
    Optional<JS::ModuleRequest> module_request;
    if (destination == Fetch::Infrastructure::Request::Destination::Style) {
        module_request.emplace();
        module_request->add_attribute("type"_utf16, "css"_utf16);
    } else if (destination == Fetch::Infrastructure::Request::Destination::JSON) {
        module_request.emplace();
        module_request->add_attribute("type"_utf16, "json"_utf16);
    } else if (destination == Fetch::Infrastructure::Request::Destination::Text) {
        module_request.emplace();
        module_request->add_attribute("type"_utf16, "text"_utf16);
    }
    fetch_single_module_script(realm, url, GC::Ref { settings_object }, destination, options, settings_object, Web::Fetch::Infrastructure::Request::Referrer::Client, module_request, TopLevelModule::Yes, nullptr, steps);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-an-inline-module-script-graph
void fetch_inline_module_script_graph(JS::Realm& realm, ByteString const& filename, Utf16View source_text, URL::URL const& base_url, EnvironmentSettingsObject& settings_object, ScriptFetchOptions const& options, size_t source_line_number, OnFetchScriptComplete on_complete)
{
    // 1. Let script be the result of creating a JavaScript module script using sourceText, settingsObject, baseURL, and options.
    auto script = ModuleScript::create_a_javascript_module_script(filename, source_text, settings_object, base_url, source_line_number, ScriptRegistry::IsInlineSource::Yes).release_value_but_fixme_should_propagate_errors();
    script->set_fetch_options(options);

    // 2. Fetch the descendants of and link script, given settingsObject, "script", and onComplete.
    fetch_descendants_of_and_link_a_module_script(realm, *script, GC::Ref { settings_object }, Fetch::Infrastructure::Request::Destination::Script, nullptr, on_complete);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-imported-module-script
void fetch_single_imported_module_script(JS::Realm& realm,
    URL::URL const& url,
    FetchClient const& fetch_client,
    Fetch::Infrastructure::Request::Destination destination,
    ScriptFetchOptions const& options,
    EnvironmentSettingsObject& settings_object,
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

    // 3. If the result of running the module type allowed steps given moduleType and settingsObject is false,
    //    then run onComplete given null, and return.
    if (!module_type_allowed(settings_object, module_type)) {
        on_complete->function()(nullptr);
        return;
    }

    // 4. Fetch a single module script given url, fetchClient, destination, options, settingsObject, referrer, moduleRequest, false,
    //    and onComplete. If performFetch was given, pass it along as well.
    fetch_single_module_script(realm, url, fetch_client, destination, options, settings_object, referrer, module_request, TopLevelModule::No, perform_fetch, on_complete);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-the-descendants-of-and-link-a-module-script
void fetch_descendants_of_and_link_a_module_script(JS::Realm& realm,
    ModuleScript& module_script,
    FetchClient const& fetch_client,
    Fetch::Infrastructure::Request::Destination destination,
    PerformTheFetchHook perform_fetch,
    OnFetchScriptComplete on_complete)
{
    // 1. Let record be moduleScript's record.
    auto record = module_script.record();

    // 2. If record is null, then:
    if (record.has<Empty>()) {
        // 1. Set moduleScript's error to rethrow to moduleScript's parse error.
        module_script.set_error_to_rethrow(module_script.parse_error());

        // 2. Run onComplete given moduleScript.
        on_complete->function()(module_script);

        // 3. Return.
        return;
    }

    // 3. Let state be Record { [[ErrorToRethrow]]: null, [[Destination]]: destination, [[PerformFetch]]: null, [[FetchClient]]: fetchClient }.
    auto state = GC::Heap::the().allocate<FetchContext>(JS::js_null(), destination, nullptr, fetch_client);

    // 4. If performFetch was given, set state.[[PerformFetch]] to performFetch.
    state->perform_fetch = perform_fetch;

    // FIXME: These should most likely be steps in the spec.
    // NOTE: For reasons beyond my understanding, we cannot use TemporaryExecutionContext here.
    //       Calling perform_a_microtask_checkpoint() on the callback settings' responsible_event_loop
    //       prevents this from functioning properly. HTMLParser::the_end would be run before
    //       HTMLScriptElement::prepare_script had a chance to setup the callback to mark_done properly,
    //       resulting in the event loop hanging forever awaiting for the script to be ready for parser
    //       execution.
    // AD-HOC: A fetch client another process hosts has no realm here, so the callback runs in the module script's
    //         settings object.
    auto& callback_settings = fetch_client.visit(
        [](GC::Ref<EnvironmentSettingsObject> const& settings_object) -> EnvironmentSettingsObject& { return settings_object; },
        [&](NonnullRefPtr<RemoteEnvironmentSettings const> const&) -> EnvironmentSettingsObject& { return module_script.settings_object(); });
    realm.vm().push_execution_context(callback_settings.realm_execution_context());
    prepare_to_run_callback(callback_settings);

    // 5. Let loadingPromise be record.LoadRequestedModules(state).
    auto loading_promise = record.visit(
        [](Empty) -> GC::Ref<JS::PromiseCapability> { VERIFY_NOT_REACHED(); },
        [&](GC::Ref<JS::Module> module) { return GC::Ref { module->load_requested_modules(state) }; });

    WebIDL::react_to_promise(loading_promise,
        // 6. Upon fulfillment of loadingPromise, run the following steps:
        GC::create_function(GC::Heap::the(), GC::captures(realm, record, module_script, on_complete), [](auto& realm, auto& record, auto& module_script, auto& on_complete, JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform record.Link().
            auto linking_result = record.visit(
                [](Empty) -> JS::ThrowCompletionOr<void> { VERIFY_NOT_REACHED(); },
                [&](GC::Ref<JS::Module> module) { return module->link(realm.vm()); });

            // If this throws an exception, set result's error to rethrow to that exception.
            if (linking_result.is_throw_completion())
                module_script.set_error_to_rethrow(linking_result.release_error().value());

            // 2. Run onComplete given moduleScript.
            on_complete->function()(module_script);

            return JS::js_undefined();
        }),

        // 7. Upon rejection of loadingPromise, run the following steps:
        GC::create_function(GC::Heap::the(), GC::captures(state, module_script, on_complete), [](auto& state, auto& module_script, auto& on_complete, JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. If state.[[ErrorToRethrow]] is not null, set moduleScript's error to rethrow to state.[[ErrorToRethrow]] and run
            //    onComplete given moduleScript.
            if (!state->error_to_rethrow.is_null()) {
                module_script.set_error_to_rethrow(state->error_to_rethrow);

                on_complete->function()(module_script);
            }
            // 2. Otherwise, run onComplete given null.
            else {
                on_complete->function()(nullptr);
            }

            return JS::js_undefined();
        }));

    clean_up_after_running_callback(callback_settings);

    realm.vm().pop_execution_context();
}

}
