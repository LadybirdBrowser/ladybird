/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Console.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ImportMap.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Infra/JSON.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#parse-an-import-map-string
WebIDL::ExceptionOr<ImportMap> parse_import_map_string(JS::Realm& realm, ByteString const& input, URL::URL base_url)
{
    HTML::TemporaryExecutionContext execution_context { realm };

    // 1. Let parsed be the result of parsing a JSON string to an Infra value given input.
    auto parsed = TRY(Infra::parse_json_string_to_javascript_value(realm, input));

    // 2. If parsed is not an ordered map, then throw a TypeError indicating that the top-level value needs to be a JSON object.
    if (!parsed.is_object())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("The top-level value of an importmap needs to be a JSON object.").release_value_but_fixme_should_propagate_errors() };
    auto& parsed_object = parsed.as_object();

    // 3. Let sortedAndNormalizedImports be an empty ordered map.
    ModuleSpecifierMap sorted_and_normalised_imports;

    // 4. If parsed["imports"] exists, then:
    if (TRY(parsed_object.has_property("imports"))) {
        auto imports = TRY(parsed_object.get("imports"));

        // If parsed["imports"] is not an ordered map, then throw a TypeError indicating that the value for the "imports" top-level key needs to be a JSON object.
        if (!imports.is_object())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("The 'imports' top-level value of an importmap needs to be a JSON object.").release_value_but_fixme_should_propagate_errors() };

        // Set sortedAndNormalizedImports to the result of sorting and normalizing a module specifier map given parsed["imports"] and baseURL.
        sorted_and_normalised_imports = TRY(sort_and_normalise_module_specifier_map(realm, imports.as_object(), base_url));
    }

    // 5. Let sortedAndNormalizedScopes be an empty ordered map.
    HashMap<URL::URL, ModuleSpecifierMap> sorted_and_normalised_scopes;

    // 6. If parsed["scopes"] exists, then:
    if (TRY(parsed_object.has_property("scopes"))) {
        auto scopes = TRY(parsed_object.get("scopes"));

        // If parsed["scopes"] is not an ordered map, then throw a TypeError indicating that the value for the "scopes" top-level key needs to be a JSON object.
        if (!scopes.is_object())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("The 'scopes' top-level value of an importmap needs to be a JSON object.").release_value_but_fixme_should_propagate_errors() };

        // Set sortedAndNormalizedScopes to the result of sorting and normalizing scopes given parsed["scopes"] and baseURL.
        sorted_and_normalised_scopes = TRY(sort_and_normalise_scopes(realm, scopes.as_object(), base_url));
    }

    // 7. Let normalizedIntegrity be an empty ordered map.
    ModuleIntegrityMap normalised_integrity;

    // 8. If parsed["integrity"] exists, then:
    if (TRY(parsed_object.has_property("integrity"))) {
        auto integrity = TRY(parsed_object.get("integrity"));

        // 1. If parsed["integrity"] is not an ordered map, then throw a TypeError indicating that the value for the "integrity" top-level key needs to be a JSON object.
        if (!integrity.is_object())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("The 'integrity' top-level value of an importmap needs to be a JSON object.").release_value_but_fixme_should_propagate_errors() };

        // 2. Set normalizedIntegrity to the result of normalizing a module integrity map given parsed["integrity"] and baseURL.
        normalised_integrity = TRY(normalize_module_integrity_map(realm, integrity.as_object(), base_url));
    }

    // 9. If parsed's keys contains any items besides "imports", "scopes", or "integrity", then the user agent should report a warning to the console indicating that an invalid top-level key was present in the import map.
    for (auto& key : parsed_object.shape().property_table().keys()) {
        if (key.as_string().is_one_of("imports", "scopes", "integrity"))
            continue;

        auto& console = realm.intrinsics().console_object()->console();
        console.output_debug_message(JS::Console::LogLevel::Warn,
            TRY_OR_THROW_OOM(realm.vm(), String::formatted("An invalid top-level key ({}) was present in the import map", key.as_string())));
    }

    // 10. Return an import map whose imports are sortedAndNormalizedImports, whose scopes are sortedAndNormalizedScopes, and whose integrity are normalizedIntegrity.
    ImportMap import_map;
    import_map.set_imports(sorted_and_normalised_imports);
    import_map.set_scopes(sorted_and_normalised_scopes);
    import_map.set_integrity(normalised_integrity);
    return import_map;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#normalizing-a-specifier-key
WebIDL::ExceptionOr<Optional<DeprecatedFlyString>> normalise_specifier_key(JS::Realm& realm, DeprecatedFlyString specifier_key, URL::URL base_url)
{
    // 1. If specifierKey is the empty string, then:
    if (specifier_key.is_empty()) {
        // 1. The user agent may report a warning to the console indicating that specifier keys may not be the empty string.
        auto& console = realm.intrinsics().console_object()->console();
        console.output_debug_message(JS::Console::LogLevel::Warn,
            TRY_OR_THROW_OOM(realm.vm(), String::formatted("Specifier keys may not be empty")));

        // 2. Return null.
        return Optional<DeprecatedFlyString> {};
    }

    // 2. Let url be the result of resolving a URL-like module specifier, given specifierKey and baseURL.
    auto url = resolve_url_like_module_specifier(specifier_key, base_url);

    // 3. If url is not null, then return the serialization of url.
    if (url.has_value())
        return url->serialize();

    // 4. Return specifierKey.
    return specifier_key;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#sorting-and-normalizing-a-module-specifier-map
WebIDL::ExceptionOr<ModuleSpecifierMap> sort_and_normalise_module_specifier_map(JS::Realm& realm, JS::Object& original_map, URL::URL base_url)
{
    // 1. Let normalized be an empty ordered map.
    ModuleSpecifierMap normalised;

    // 2. For each specifierKey → value of originalMap:
    for (auto& specifier_key : original_map.shape().property_table().keys()) {
        auto value = TRY(original_map.get(specifier_key.as_string()));

        // 1. Let normalizedSpecifierKey be the result of normalizing a specifier key given specifierKey and baseURL.
        auto normalised_specifier_key = TRY(normalise_specifier_key(realm, specifier_key.as_string(), base_url));

        // 2. If normalizedSpecifierKey is null, then continue.
        if (!normalised_specifier_key.has_value())
            continue;

        // 3. If value is not a string, then:
        if (!value.is_string()) {
            // 1. The user agent may report a warning to the console indicating that addresses need to be strings.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                TRY_OR_THROW_OOM(realm.vm(), String::formatted("Addresses need to be strings")));

            // 2. Set normalized[normalizedSpecifierKey] to null.
            normalised.set(normalised_specifier_key.value(), {});

            // 3. Continue.
            continue;
        }

        // 4. Let addressURL be the result of resolving a URL-like module specifier given value and baseURL.
        auto address_url = resolve_url_like_module_specifier(value.as_string().byte_string(), base_url);

        // 5. If addressURL is null, then:
        if (!address_url.has_value()) {
            // 1. The user agent may report a warning to the console indicating that the address was invalid.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                TRY_OR_THROW_OOM(realm.vm(), String::formatted("Address was invalid")));

            // 2. Set normalized[normalizedSpecifierKey] to null.
            normalised.set(normalised_specifier_key.value(), {});

            // 3. Continue.
            continue;
        }

        // 6. If specifierKey ends with U+002F (/), and the serialization of addressURL does not end with U+002F (/), then:
        if (specifier_key.as_string().ends_with("/"sv) && !address_url->serialize().ends_with("/"sv)) {
            // 1. The user agent may report a warning to the console indicating that an invalid address was given for the specifier key specifierKey; since specifierKey ends with a slash, the address needs to as well.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                TRY_OR_THROW_OOM(realm.vm(), String::formatted("An invalid address was given for the specifier key ({}); since specifierKey ends with a slash, the address needs to as well", specifier_key.as_string())));

            // 2. Set normalized[normalizedSpecifierKey] to null.
            normalised.set(normalised_specifier_key.value(), {});

            // 3. Continue.
            continue;
        }

        // 7. Set normalized[normalizedSpecifierKey] to addressURL.
        normalised.set(normalised_specifier_key.value(), address_url.value());
    }

    // 3. Return the result of sorting in descending order normalized, with an entry a being less than an entry b if a's key is code unit less than b's key.
    return normalised;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#sorting-and-normalizing-scopes
WebIDL::ExceptionOr<HashMap<URL::URL, ModuleSpecifierMap>> sort_and_normalise_scopes(JS::Realm& realm, JS::Object& original_map, URL::URL base_url)
{
    // 1. Let normalized be an empty ordered map.
    HashMap<URL::URL, ModuleSpecifierMap> normalised;

    // 2. For each scopePrefix → potentialSpecifierMap of originalMap:
    for (auto& scope_prefix : original_map.shape().property_table().keys()) {
        auto potential_specifier_map = TRY(original_map.get(scope_prefix.as_string()));

        // 1. If potentialSpecifierMap is not an ordered map, then throw a TypeError indicating that the value of the scope with prefix scopePrefix needs to be a JSON object.
        if (!potential_specifier_map.is_object())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("The value of the scope with the prefix '{}' needs to be a JSON object.", scope_prefix.as_string()).release_value_but_fixme_should_propagate_errors() };

        // 2. Let scopePrefixURL be the result of URL parsing scopePrefix with baseURL.
        auto scope_prefix_url = DOMURL::parse(scope_prefix.as_string(), base_url);

        // 3. If scopePrefixURL is failure, then:
        if (!scope_prefix_url.is_valid()) {
            // 1. The user agent may report a warning to the console that the scope prefix URL was not parseable.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                TRY_OR_THROW_OOM(realm.vm(), String::formatted("The scope prefix URL ({}) was not parseable", scope_prefix.as_string())));

            // 2. Continue.
            continue;
        }

        // 4. Let normalizedScopePrefix be the serialization of scopePrefixURL.
        auto normalised_scope_prefix = scope_prefix_url.serialize();

        // 5. Set normalized[normalizedScopePrefix] to the result of sorting and normalizing a module specifier map given potentialSpecifierMap and baseURL.
        normalised.set(normalised_scope_prefix, TRY(sort_and_normalise_module_specifier_map(realm, potential_specifier_map.as_object(), base_url)));
    }

    // 3. Return the result of sorting in descending order normalized, with an entry a being less than an entry b if a's key is code unit less than b's key.
    return normalised;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#normalizing-a-module-integrity-map
WebIDL::ExceptionOr<ModuleIntegrityMap> normalize_module_integrity_map(JS::Realm& realm, JS::Object& original_map, URL::URL base_url)
{
    // 1. Let normalized be an empty ordered map.
    ModuleIntegrityMap normalised;

    // 2. For each key → value of originalMap:
    for (auto& key : original_map.shape().property_table().keys()) {
        auto value = TRY(original_map.get(key.as_string()));

        // 1. Let resolvedURL be the result of resolving a URL-like module specifier given key and baseURL.
        auto resolved_url = resolve_url_like_module_specifier(key.as_string(), base_url);

        // 2. If resolvedURL is null, then:
        if (!resolved_url.has_value()) {
            // 1. The user agent may report a warning to the console indicating that the key failed to resolve.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                TRY_OR_THROW_OOM(realm.vm(), String::formatted("Failed to resolve key ({})", key.as_string())));

            // 2. Continue.
            continue;
        }

        // 3. If value is not a string, then:
        if (!value.is_string()) {
            // 1. The user agent may report a warning to the console indicating that integrity metadata values need to be strings.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                TRY_OR_THROW_OOM(realm.vm(), String::formatted("Integrity metadata value for '{}' needs to be a string", key.as_string())));

            // 2. Continue.
            continue;
        }

        // 4. Set normalized[resolvedURL] to value.
        normalised.set(resolved_url.release_value(), value.as_string().byte_string());
    }

    // 3. Return normalized.
    return normalised;
}

}
