/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>

#include <AK/Utf16StringBuilder.h>
#include <LibJS/HostClassBuilder.h>
#include <LibWeb/ContentSecurityPolicy/Directives/KeywordTrustedTypes.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::TrustedTypes {

#define __ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR(name, value) \
    Utf16FlyString const& name = *new Utf16FlyString(value##_utf16_fly_string);
ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR
#undef __ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR

static bool sink_group_matches(Utf16String const& directive_value, Utf16View sink_group)
{
    // INTEROP: Match the complete quoted token case-sensitively, as Chromium and Firefox do.
    //          The Trusted Types draft uses case-insensitive ABNF for this keyword.
    return directive_value == sink_group;
}

static Utf16View first_40_characters(Utf16View sample)
{
    // INTEROP: Count UTF-16 code units, but don't split a surrogate pair, as Firefox and WPT do.
    //          https://github.com/w3c/webappsec-csp/issues/704
    auto length = min(sample.length_in_code_units(), 40uz);
    if (length < sample.length_in_code_units() && AK::UnicodeUtils::is_utf16_high_surrogate(sample.code_unit_at(length - 1)) && AK::UnicodeUtils::is_utf16_low_surrogate(sample.code_unit_at(length)))
        ++length;
    return sample.substring_view(0, length);
}

// https://w3c.github.io/trusted-types/dist/spec/#tt-policy-name
static bool is_tt_policy_name(Utf16View token)
{
    // tt-policy-name = 1*( ALPHA / DIGIT / "-" / "#" / "=" / "_" / "/" / "@" / "." / "%")
    if (token.is_empty())
        return false;
    for (size_t i = 0; i < token.length_in_code_units(); ++i) {
        auto code_unit = token.code_unit_at(i);
        if (!is_ascii_alphanumeric(code_unit) && !AK::first_is_one_of(code_unit, '-', '#', '=', '_', '/', '@', '.', '%'))
            return false;
    }
    return true;
}

static ContentSecurityPolicy::PolicyList const* csp_list_of_global(JS::Object& global)
{
    if (auto* window = Bindings::window_from_global_object(global))
        return &window->associated_document().policy_container()->csp_list;

    if (auto* worker = Bindings::worker_global_scope_from_global_object(global))
        return &worker->policy_container()->csp_list;

    return nullptr;
}

// https://www.w3.org/TR/trusted-types/#require-trusted-types-for-pre-navigation-check
ContentSecurityPolicy::Directives::Directive::Result require_trusted_types_for_pre_navigation_check(Fetch::Infrastructure::Request& request)
{
    using Result = ContentSecurityPolicy::Directives::Directive::Result;

    // 1. If request’s url’s scheme is not "javascript", return "Allowed" and abort further steps.
    if (request.url().scheme() != "javascript"sv)
        return Result::Allowed;

    // 2. Let urlString be the result of running the URL serializer on request’s url.
    auto url_string = request.url().serialize();

    // 3. Let encodedScriptSource be the result of removing the leading "javascript:" from urlString.
    auto const encoded_script_source = MUST(url_string.substring_from_byte_offset("javascript:"sv.length()));

    // AD-HOC: The global object of a client another process hosts is in that process, so its default policy cannot
    //         convert the script source here.
    if (Fetch::Fetching::remote_client(request))
        return Result::Blocked;

    // 4. Let convertedScriptSource be the result of executing Process value with a default policy algorithm, with the following arguments:
    //    expectedType:
    //      TrustedScript
    //    global:
    //      request’s clients’s global object:
    //    input:
    //      encodedScriptSource:
    //    sink:
    //      "Location href":
    auto client = Fetch::Fetching::resolve_client(request);
    auto converted_script_source = process_value_with_a_default_policy(
        TrustedTypeName::TrustedScript,
        client->global_object(),
        Utf16String::from_utf8(encoded_script_source),
        InjectionSink::Location_href);

    // If that algorithm threw an error or convertedScriptSource is not a TrustedScript object, return "Blocked" and abort further steps.
    if (converted_script_source.is_error() || !converted_script_source.value().has_value())
        return Result::Blocked;

    auto const* converted_script_source_value = converted_script_source.value().value().get_pointer<GC::Ref<TrustedScript>>();

    if (!converted_script_source_value)
        return Result::Blocked;

    // 5. Set urlString to be the result of prepending "javascript:" to stringified convertedScriptSource.
    auto new_url_string = Utf16String::formatted("javascript:{}", (*converted_script_source_value)->to_string());

    // 6. Let newURL be the result of running the URL parser on urlString. If the parser returns a failure, return "Blocked" and abort further steps.
    auto const new_url = DOMURL::parse(new_url_string);
    if (!new_url.has_value())
        return Result::Blocked;

    // 7. Set request’s url to newURL.
    request.set_url(new_url.value());

    // 8. Return "Allowed".
    return Result::Allowed;
}

// https://w3c.github.io/trusted-types/dist/spec/#does-sink-require-trusted-types
bool does_sink_require_trusted_types(JS::Object& global, Utf16View sink_group, IncludeReportOnlyPolicies include_report_only_policies)
{
    // 1. For each policy in global’s CSP list:
    auto const* csp_list = csp_list_of_global(global);
    if (!csp_list)
        return false;
    for (auto const& policy : csp_list->policies()) {
        // 1. If policy’s directive set does not contain a directive whose name is "require-trusted-types-for", skip to the next policy.
        if (!policy->contains_directive_with_name(ContentSecurityPolicy::Directives::Names::RequireTrustedTypesFor))
            continue;

        // 2. Let directive be the policy’s directive set’s directive whose name is "require-trusted-types-for"
        auto const directive = policy->get_directive_by_name(ContentSecurityPolicy::Directives::Names::RequireTrustedTypesFor);

        // 3. If directive’s value does not contain a trusted-types-sink-group which is a match for sinkGroup, skip to the next policy.
        auto const maybe_sink_group = directive->value().find_if([sink_group](auto const& directive_value) {
            return sink_group_matches(directive_value, sink_group);
        });
        if (maybe_sink_group.is_end())
            continue;

        // 4. Let enforced be true if policy’s disposition is "enforce", and false otherwise.
        auto const enforced = policy->disposition() == ContentSecurityPolicy::Policy::Disposition::Enforce;

        // 5. If enforced is true, return true.
        if (enforced)
            return true;

        // 6. If includeReportOnlyPolicies is true, return true.
        if (include_report_only_policies == IncludeReportOnlyPolicies::Yes)
            return true;
    }

    // 2. Return false.
    return false;
}

// https://w3c.github.io/trusted-types/dist/spec/#should-block-sink-type-mismatch
ContentSecurityPolicy::Directives::Directive::Result should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(JS::Object& global, Utf16View sink, Utf16View sink_group, Utf16String source)
{
    auto& realm = HTML::relevant_realm(global);

    // 1. Let result be "Allowed".
    auto result = ContentSecurityPolicy::Directives::Directive::Result::Allowed;

    // 2. Let sample be source.
    auto sample = source.substring_view(0);

    // 3. If sink is "Function", then:
    if (sink == "Function"sv) {
        // 1. If sample starts with "function anonymous", strip that from sample.
        if (sample.starts_with("function anonymous"sv)) {
            sample = sample.substring_view("function anonymous"sv.length());
        }

        // 2. Otherwise if sample starts with "async function anonymous", strip that from sample.
        else if (sample.starts_with("async function anonymous"sv)) {
            sample = sample.substring_view("async function anonymous"sv.length());
        }

        // 3. Otherwise if sample starts with "function* anonymous", strip that from sample.
        else if (sample.starts_with("function* anonymous"sv)) {
            sample = sample.substring_view("function* anonymous"sv.length());
        }

        // 4. Otherwise if sample starts with "async function* anonymous", strip that from sample.
        else if (sample.starts_with("async function* anonymous"sv)) {
            sample = sample.substring_view("async function* anonymous"sv.length());
        }
    }

    // 4. For each policy in global’s CSP list:
    auto const* csp_list = csp_list_of_global(global);
    if (!csp_list)
        return result;
    for (auto const& policy : csp_list->policies()) {
        // 1. If policy’s directive set does not contain a directive whose name is "require-trusted-types-for", skip to the next policy.
        if (!policy->contains_directive_with_name(ContentSecurityPolicy::Directives::Names::RequireTrustedTypesFor))
            continue;

        // 2. Let directive be the policy’s directive set’s directive whose name is "require-trusted-types-for"
        auto const directive = policy->get_directive_by_name(ContentSecurityPolicy::Directives::Names::RequireTrustedTypesFor);

        // 3. If directive’s value does not contain a trusted-types-sink-group which is a match for sinkGroup, skip to the next policy.
        auto const maybe_sink_group = directive->value().find_if([sink_group](auto const& directive_value) {
            return sink_group_matches(directive_value, sink_group);
        });
        if (maybe_sink_group.is_end())
            continue;

        // 4. Let violation be the result of executing Create a violation object for global, policy, and directive on global, policy and "require-trusted-types-for"
        auto violation = ContentSecurityPolicy::Violation::create_a_violation_object_for_global_policy_and_directive(global, policy, ContentSecurityPolicy::Directives::Names::RequireTrustedTypesFor.view().to_utf8_but_should_be_ported_to_utf16());

        // 5. Set violation’s resource to "trusted-types-sink".
        violation->set_resource(ContentSecurityPolicy::Violation::Resource::TrustedTypesSink);

        // 6. Let trimmedSample be the substring of sample, containing its first 40 characters.
        auto const trimmed_sample = first_40_characters(sample);

        // 7. Set violation’s sample to be the result of concatenating the list « sink, trimmedSample « using "|" as a separator.
        Utf16StringBuilder sample_builder;
        sample_builder.append(sink);
        sample_builder.append_ascii('|');
        sample_builder.append(trimmed_sample);
        violation->set_sample(sample_builder.to_string());

        // 8. Execute Report a violation on violation.
        violation->report_a_violation(realm);

        // 9. If policy’s disposition is "enforce", then set result to "Blocked".
        if (policy->disposition() == ContentSecurityPolicy::Policy::Disposition::Enforce)
            result = ContentSecurityPolicy::Directives::Directive::Result::Blocked;
    }

    // 5. Return result.
    return result;
}

// https://w3c.github.io/trusted-types/dist/spec/#should-block-create-policy
ContentSecurityPolicy::Directives::Directive::Result should_trusted_type_policy_creation_be_blocked_by_content_security_policy(JS::Realm& realm, JS::Object& global, Utf16String const& policy_name, ReadonlySpan<Utf16String> created_policy_names)
{
    // 1. Let result be "Allowed".
    auto result = ContentSecurityPolicy::Directives::Directive::Result::Allowed;

    // 2. For each policy in global’s CSP list:
    auto const& csp_list = HTML::relevant_settings_object(global).policy_container()->csp_list;
    for (auto const& policy : csp_list.policies()) {
        // 1. Let createViolation be false.
        bool create_violation = false;

        // 2. If policy’s directive set does not contain a directive which name is "trusted-types", skip to the next policy.
        if (!policy->contains_directive_with_name(ContentSecurityPolicy::Directives::Names::TrustedTypes))
            continue;

        // 3. Let directive be the policy’s directive set’s directive which name is "trusted-types"
        auto const directive = policy->get_directive_by_name(ContentSecurityPolicy::Directives::Names::TrustedTypes);

        // 4. If directive’s value only contains a tt-keyword which is a match for a value 'none', set createViolation to true.
        if (directive->value().size() == 1 && directive->value().first().equals_ignoring_ascii_case(ContentSecurityPolicy::Directives::KeywordTrustedTypes::None.view()))
            create_violation = true;

        // 5. If createdPolicyNames contains policyName and directive’s value does not contain a tt-keyword which is a match for a value 'allow-duplicates', set createViolation to true.
        if (created_policy_names.contains_slow(policy_name)) {
            auto maybe_allow_duplicates = directive->value().find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case(ContentSecurityPolicy::Directives::KeywordTrustedTypes::AllowDuplicates.view());
            });
            if (maybe_allow_duplicates.is_end())
                create_violation = true;
        }

        // 6. If directive’s value does not contain a tt-policy-name, which value is policyName, and directive’s value does not contain a tt-wildcard, set createViolation to true.
        auto directive_value_iterator = directive->value().find_if([&](auto const& directive_value) {
            return directive_value == policy_name && is_tt_policy_name(directive_value);
        });
        if (directive_value_iterator.is_end()) {
            auto maybe_wild_card = directive->value().find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case(ContentSecurityPolicy::Directives::KeywordTrustedTypes::WildCard.view());
            });

            if (maybe_wild_card.is_end())
                create_violation = true;
        }

        // 7. If createViolation is false, skip to the next policy.
        if (!create_violation)
            continue;

        // 8. Let violation be the result of executing Create a violation object for global, policy, and directive on global, policy and "trusted-types"
        auto const violation = ContentSecurityPolicy::Violation::create_a_violation_object_for_global_policy_and_directive(global, policy, ContentSecurityPolicy::Directives::Names::TrustedTypes.view().to_utf8_but_should_be_ported_to_utf16());

        // 9. Set violation’s resource to "trusted-types-policy".
        violation->set_resource(ContentSecurityPolicy::Violation::Resource::TrustedTypesPolicy);

        // 10. Set violation’s sample to the substring of policyName, containing its first 40 characters.
        violation->set_sample(Utf16String::from_utf16(first_40_characters(policy_name)));

        // 11. Execute Report a violation on violation.
        violation->report_a_violation(realm);

        // 12. If policy’s disposition is "enforce", then set result to "Blocked".
        if (policy->disposition() == ContentSecurityPolicy::Policy::Disposition::Enforce)
            result = ContentSecurityPolicy::Directives::Directive::Result::Blocked;
    }

    // 3. Return result.
    return result;
}

extern "C" {

// Called by Bindings/Rust/src/trusted_types/require_trusted_types_for_directive.rs.
bool web_trusted_types_does_sink_require_trusted_types(JSObject* global, FlatPtr sink_group, bool include_report_only_policies);
bool web_trusted_types_should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(JSObject* global, FlatPtr sink, FlatPtr sink_group, FlatPtr source);
bool web_trusted_types_should_trusted_type_policy_creation_be_blocked_by_content_security_policy(JS::Realm*, JSObject* global, FlatPtr policy_name, ReadonlySpan<FlatPtr> created_policy_names);

bool web_trusted_types_does_sink_require_trusted_types(JSObject* global, FlatPtr sink_group, bool include_report_only_policies)
{
    return does_sink_require_trusted_types(*JS::HostABI::object_from_abi(global), Utf16String::from_raw(sink_group), include_report_only_policies ? IncludeReportOnlyPolicies::Yes : IncludeReportOnlyPolicies::No);
}

bool web_trusted_types_should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(JSObject* global, FlatPtr sink, FlatPtr sink_group, FlatPtr source)
{
    return should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(*JS::HostABI::object_from_abi(global), Utf16String::from_raw(sink), Utf16String::from_raw(sink_group), Utf16String::from_raw(source))
        == ContentSecurityPolicy::Directives::Directive::Result::Blocked;
}

bool web_trusted_types_should_trusted_type_policy_creation_be_blocked_by_content_security_policy(JS::Realm* realm, JSObject* global, FlatPtr policy_name, ReadonlySpan<FlatPtr> created_policy_names)
{
    Vector<Utf16String> names;
    names.ensure_capacity(created_policy_names.size());
    for (auto name : created_policy_names)
        names.unchecked_append(Utf16String::from_raw(name));
    return should_trusted_type_policy_creation_be_blocked_by_content_security_policy(*realm, *JS::HostABI::object_from_abi(global), Utf16String::from_raw(policy_name), names)
        == ContentSecurityPolicy::Directives::Directive::Result::Blocked;
}
}

}
