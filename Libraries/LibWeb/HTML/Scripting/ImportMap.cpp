/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Console.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ImportMap.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/JSON.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#parse-an-import-map-string
WebIDL::ExceptionOr<ImportMap> parse_import_map_string(JS::Realm& realm, ByteString const& input, URL::URL base_url)
{
    HTML::TemporaryExecutionContext execution_context { realm };

    // 1. Let parsed be the result of parsing a JSON string to an Infra value given input.
    auto parsed = TRY(Infra::parse_json_string_to_javascript_value(realm, input));

    // 2. If parsed is not an ordered map, then throw a TypeError indicating that the top-level value needs to be a JSON object.
    if (!parsed.is_object())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The top-level value of an importmap needs to be a JSON object."_string };
    auto& parsed_object = parsed.as_object();

    // 3. Let sortedAndNormalizedImports be an empty ordered map.
    ModuleSpecifierMap sorted_and_normalized_imports;

    // 4. If parsed["imports"] exists, then:
    if (TRY(parsed_object.has_property("imports"_fly_string))) {
        auto imports = TRY(parsed_object.get("imports"_fly_string));

        // If parsed["imports"] is not an ordered map, then throw a TypeError indicating that the value for the "imports" top-level key needs to be a JSON object.
        if (!imports.is_object())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The 'imports' top-level value of an importmap needs to be a JSON object."_string };

        // Set sortedAndNormalizedImports to the result of sorting and normalizing a module specifier map given parsed["imports"] and baseURL.
        sorted_and_normalized_imports = TRY(sort_and_normalise_module_specifier_map(realm, imports.as_object(), base_url));
    }

    // 5. Let sortedAndNormalizedScopes be an empty ordered map.
    HashMap<URL::URL, ModuleSpecifierMap> sorted_and_normalized_scopes;

    // 6. If parsed["scopes"] exists, then:
    if (TRY(parsed_object.has_property("scopes"_fly_string))) {
        auto scopes = TRY(parsed_object.get("scopes"_fly_string));

        // If parsed["scopes"] is not an ordered map, then throw a TypeError indicating that the value for the "scopes" top-level key needs to be a JSON object.
        if (!scopes.is_object())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The 'scopes' top-level value of an importmap needs to be a JSON object."_string };

        // Set sortedAndNormalizedScopes to the result of sorting and normalizing scopes given parsed["scopes"] and baseURL.
        sorted_and_normalized_scopes = TRY(sort_and_normalise_scopes(realm, scopes.as_object(), base_url));
    }

    // 7. Let normalizedIntegrity be an empty ordered map.
    ModuleIntegrityMap normalized_integrity;

    // 8. If parsed["integrity"] exists, then:
    if (TRY(parsed_object.has_property("integrity"_fly_string))) {
        auto integrity = TRY(parsed_object.get("integrity"_fly_string));

        // 1. If parsed["integrity"] is not an ordered map, then throw a TypeError indicating that the value for the "integrity" top-level key needs to be a JSON object.
        if (!integrity.is_object())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The 'integrity' top-level value of an importmap needs to be a JSON object."_string };

        // 2. Set normalizedIntegrity to the result of normalizing a module integrity map given parsed["integrity"] and baseURL.
        normalized_integrity = TRY(normalize_module_integrity_map(realm, integrity.as_object(), base_url));
    }

    // 9. If parsed's keys contains any items besides "imports", "scopes", or "integrity", then the user agent should report a warning to the console indicating that an invalid top-level key was present in the import map.
    for (auto& key : parsed_object.shape().property_table().keys()) {
        if (key.as_string().is_one_of("imports", "scopes", "integrity"))
            continue;

        auto& console = realm.intrinsics().console_object()->console();
        console.output_debug_message(JS::Console::LogLevel::Warn, MUST(String::formatted("An invalid top-level key ({}) was present in the import map", key.as_string())));
    }

    // 10. Return an import map whose imports are sortedAndNormalizedImports, whose scopes are sortedAndNormalizedScopes, and whose integrity are normalizedIntegrity.
    ImportMap import_map;
    import_map.set_imports(sorted_and_normalized_imports);
    import_map.set_scopes(sorted_and_normalized_scopes);
    import_map.set_integrity(normalized_integrity);
    return import_map;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#normalizing-a-specifier-key
Optional<FlyString> normalize_specifier_key(JS::Realm& realm, FlyString specifier_key, URL::URL base_url)
{
    // 1. If specifierKey is the empty string, then:
    if (specifier_key.is_empty()) {
        // 1. The user agent may report a warning to the console indicating that specifier keys may not be the empty string.
        auto& console = realm.intrinsics().console_object()->console();
        console.output_debug_message(JS::Console::LogLevel::Warn, "Specifier keys may not be empty"sv);

        // 2. Return null.
        return Optional<FlyString> {};
    }

    // 2. Let url be the result of resolving a URL-like module specifier, given specifierKey and baseURL.
    auto url = resolve_url_like_module_specifier(specifier_key.to_string().to_byte_string(), base_url);

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
    ModuleSpecifierMap normalized;

    // 2. For each specifierKey → value of originalMap:
    for (auto& specifier_key : original_map.shape().property_table().keys()) {
        auto value = TRY(original_map.get(specifier_key.as_string()));

        // 1. Let normalizedSpecifierKey be the result of normalizing a specifier key given specifierKey and baseURL.
        auto normalized_specifier_key = normalize_specifier_key(realm, specifier_key.as_string(), base_url);

        // 2. If normalizedSpecifierKey is null, then continue.
        if (!normalized_specifier_key.has_value())
            continue;

        // 3. If value is not a string, then:
        if (!value.is_string()) {
            // 1. The user agent may report a warning to the console indicating that addresses need to be strings.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn, "Addresses need to be strings"sv);

            // 2. Set normalized[normalizedSpecifierKey] to null.
            normalized.set(normalized_specifier_key.value().to_string(), {});

            // 3. Continue.
            continue;
        }

        // 4. Let addressURL be the result of resolving a URL-like module specifier given value and baseURL.
        auto address_url = resolve_url_like_module_specifier(value.as_string().utf8_string_view(), base_url);

        // 5. If addressURL is null, then:
        if (!address_url.has_value()) {
            // 1. The user agent may report a warning to the console indicating that the address was invalid.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn, "Address was invalid"sv);

            // 2. Set normalized[normalizedSpecifierKey] to null.
            normalized.set(normalized_specifier_key.value().to_string(), {});

            // 3. Continue.
            continue;
        }

        // 6. If specifierKey ends with U+002F (/), and the serialization of addressURL does not end with U+002F (/), then:
        if (specifier_key.as_string().bytes_as_string_view().ends_with("/"sv) && !address_url->serialize().ends_with('/')) {
            // 1. The user agent may report a warning to the console indicating that an invalid address was given for the specifier key specifierKey; since specifierKey ends with a slash, the address needs to as well.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                MUST(String::formatted("An invalid address was given for the specifier key ({}); since specifierKey ends with a slash, the address needs to as well", specifier_key.as_string())));

            // 2. Set normalized[normalizedSpecifierKey] to null.
            normalized.set(normalized_specifier_key.value().to_string(), {});

            // 3. Continue.
            continue;
        }

        // 7. Set normalized[normalizedSpecifierKey] to addressURL.
        normalized.set(normalized_specifier_key.value().to_string(), address_url.value());
    }

    // 3. Return the result of sorting in descending order normalized, with an entry a being less than an entry b if a's key is code unit less than b's key.
    return normalized;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#sorting-and-normalizing-scopes
WebIDL::ExceptionOr<HashMap<URL::URL, ModuleSpecifierMap>> sort_and_normalise_scopes(JS::Realm& realm, JS::Object& original_map, URL::URL base_url)
{
    // 1. Let normalized be an empty ordered map.
    HashMap<URL::URL, ModuleSpecifierMap> normalized;

    // 2. For each scopePrefix → potentialSpecifierMap of originalMap:
    for (auto& scope_prefix : original_map.shape().property_table().keys()) {
        auto potential_specifier_map = TRY(original_map.get(scope_prefix.as_string()));

        // 1. If potentialSpecifierMap is not an ordered map, then throw a TypeError indicating that the value of the scope with prefix scopePrefix needs to be a JSON object.
        if (!potential_specifier_map.is_object())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("The value of the scope with the prefix '{}' needs to be a JSON object.", scope_prefix.as_string()).release_value_but_fixme_should_propagate_errors() };

        // 2. Let scopePrefixURL be the result of URL parsing scopePrefix with baseURL.
        auto scope_prefix_url = DOMURL::parse(scope_prefix.as_string(), base_url);

        // 3. If scopePrefixURL is failure, then:
        if (!scope_prefix_url.has_value()) {
            // 1. The user agent may report a warning to the console that the scope prefix URL was not parseable.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                MUST(String::formatted("The scope prefix URL ({}) was not parseable", scope_prefix.as_string())));

            // 2. Continue.
            continue;
        }

        // 4. Let normalizedScopePrefix be the serialization of scopePrefixURL.
        // 5. Set normalized[normalizedScopePrefix] to the result of sorting and normalizing a module specifier map given potentialSpecifierMap and baseURL.
        normalized.set(scope_prefix_url.value(), TRY(sort_and_normalise_module_specifier_map(realm, potential_specifier_map.as_object(), base_url)));
    }

    // 3. Return the result of sorting in descending order normalized, with an entry a being less than an entry b if a's key is code unit less than b's key.
    return normalized;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#normalizing-a-module-integrity-map
WebIDL::ExceptionOr<ModuleIntegrityMap> normalize_module_integrity_map(JS::Realm& realm, JS::Object& original_map, URL::URL base_url)
{
    // 1. Let normalized be an empty ordered map.
    ModuleIntegrityMap normalized;

    // 2. For each key → value of originalMap:
    for (auto& key : original_map.shape().property_table().keys()) {
        auto value = TRY(original_map.get(key.as_string()));

        // 1. Let resolvedURL be the result of resolving a URL-like module specifier given key and baseURL.
        auto resolved_url = resolve_url_like_module_specifier(key.as_string().to_string().to_byte_string(), base_url);

        // 2. If resolvedURL is null, then:
        if (!resolved_url.has_value()) {
            // 1. The user agent may report a warning to the console indicating that the key failed to resolve.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                MUST(String::formatted("Failed to resolve key ({})", key.as_string())));

            // 2. Continue.
            continue;
        }

        // 3. If value is not a string, then:
        if (!value.is_string()) {
            // 1. The user agent may report a warning to the console indicating that integrity metadata values need to be strings.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                MUST(String::formatted("Integrity metadata value for '{}' needs to be a string", key.as_string())));

            // 2. Continue.
            continue;
        }

        // 4. Set normalized[resolvedURL] to value.
        normalized.set(resolved_url.release_value(), value.as_string().utf8_string());
    }

    // 3. Return normalized.
    return normalized;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#merge-module-specifier-maps
static ModuleSpecifierMap merge_module_specifier_maps(JS::Realm& realm, ModuleSpecifierMap const& new_map, ModuleSpecifierMap const& old_map)
{
    // 1. Let mergedMap be a deep copy of oldMap.
    ModuleSpecifierMap merged_map = old_map;

    // 2. For each specifier → url of newMap:
    for (auto const& [specifier, url] : new_map) {
        // 1. If specifier exists in oldMap, then:
        if (old_map.contains(specifier)) {
            // 1. The user agent may report a warning to the console indicating the ignored rule. They may choose to
            //    avoid reporting if the rule is identical to an existing one.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                MUST(String::formatted("An import map rule for specifier '{}' was ignored as one was already present in the existing import map", specifier)));

            // 2. Continue.
            continue;
        }

        // 2. Set mergedMap[specifier] to url.
        merged_map.set(specifier, url);
    }

    // 3. Return mergedMap.
    return merged_map;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#merge-existing-and-new-import-maps
void merge_existing_and_new_import_maps(Window& global, ImportMap& new_import_map)
{
    auto& realm = global.realm();

    // 1. Let newImportMapScopes be a deep copy of newImportMap's scopes.
    auto new_import_map_scopes = new_import_map.scopes();

    // Spec-Note: We're mutating these copies and removing items from them when they are used to ignore scope-specific
    //            rules. This is true for newImportMapScopes, as well as to newImportMapImports below.

    // 2. Let oldImportMap be global's import map.
    auto& old_import_map = global.import_map();

    // 3. Let newImportMapImports be a deep copy of newImportMap's imports.
    auto new_import_map_imports = new_import_map.imports();

    // 4. For each scopePrefix → scopeImports of newImportMapScopes:
    for (auto& [scope_prefix, scope_imports] : new_import_map_scopes) {
        // 1. For each record of global's resolved module set:
        for (auto const& record : global.resolved_module_set()) {
            // 1. If scopePrefix is record's serialized base URL, or if scopePrefix ends with U+002F (/) and scopePrefix is a code unit prefix of record's serialized base URL, then:
            if (scope_prefix.to_string() == record.serialized_base_url || (scope_prefix.to_string().ends_with('/') && record.serialized_base_url.has_value() && Infra::is_code_unit_prefix(scope_prefix.to_string(), *record.serialized_base_url))) {
                // 1. For each specifierKey → resolutionResult of scopeImports:
                scope_imports.remove_all_matching([&](String const& specifier_key, Optional<URL::URL> const&) {
                    // 1. If specifierKey is record's specifier, or if all of the following conditions are true:
                    //      * specifierKey ends with U+002F (/);
                    //      * specifierKey is a code unit prefix of record's specifier;
                    //      * either record's specifier as a URL is null or is special,
                    //    then:
                    if (specifier_key.bytes_as_string_view() == record.specifier
                        || (specifier_key.ends_with('/')
                            && Infra::is_code_unit_prefix(specifier_key, record.specifier)
                            && record.specifier_is_null_or_url_like_that_is_special)) {
                        // 1. The user agent may report a warning to the console indicating the ignored rule. They
                        //    may choose to avoid reporting if the rule is identical to an existing one.
                        auto& console = realm.intrinsics().console_object()->console();
                        console.output_debug_message(JS::Console::LogLevel::Warn,
                            MUST(String::formatted("An import map rule for specifier '{}' was ignored as one was already present in the existing import map", specifier_key)));

                        // 2. Remove scopeImports[specifierKey].
                        return true;
                    }

                    return false;
                });
            }
        }

        // 2. If scopePrefix exists in oldImportMap's scopes, then set oldImportMap's scopes[scopePrefix] to the result
        //    of merging module specifier maps, given scopeImports and oldImportMap's scopes[scopePrefix].
        if (auto it = old_import_map.scopes().find(scope_prefix); it != old_import_map.scopes().end()) {
            it->value = merge_module_specifier_maps(realm, scope_imports, it->value);
        }
        // 3. Otherwise, set oldImportMap's scopes[scopePrefix] to scopeImports.
        else {
            old_import_map.scopes().set(scope_prefix, scope_imports);
        }
    }

    // 5. For each url → integrity of newImportMap's integrity:
    for (auto const& [url, integrity] : new_import_map.integrity()) {
        // 1. If url exists in oldImportMap's integrity, then:
        if (old_import_map.integrity().contains(url)) {
            // 1. The user agent may report a warning to the console indicating the ignored rule. They may choose to
            //    avoid reporting if the rule is identical to an existing one.
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                MUST(String::formatted("An import map integrity rule for url '{}' was ignored as one was already present in the existing import map", url)));

            // 2. Continue.
            continue;
        }

        // 2. Set oldImportMap's integrity[url] to integrity.
        old_import_map.integrity().set(url, integrity);
    }

    // 6. For each record of global's resolved module set:
    for (auto const& record : global.resolved_module_set()) {
        // 1. For each specifier → url of newImportMapImports:
        new_import_map_imports.remove_all_matching([&](String const& specifier, Optional<URL::URL> const&) {
            // 1. If specifier starts with record's specifier, then:
            if (specifier.bytes_as_string_view().starts_with(record.specifier)) {
                // 1. The user agent may report a warning to the console indicating the ignored rule. They may choose to
                //    avoid reporting if the rule is identical to an existing one.
                auto& console = realm.intrinsics().console_object()->console();
                console.output_debug_message(JS::Console::LogLevel::Warn,
                    MUST(String::formatted("An import map rule for specifier '{}' was ignored as one was already present in the existing import map", specifier)));

                // 2. Remove newImportMapImports[specifier].
                return true;
            }

            return false;
        });
    }

    // 7. Set oldImportMap's imports to the result of merge module specifier maps, given newImportMapImports and oldImportMap's imports.
    old_import_map.set_imports(merge_module_specifier_maps(realm, new_import_map_imports, old_import_map.imports()));
}

}
