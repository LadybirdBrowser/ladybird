/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>

#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::TrustedTypes {

#define __ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR(name, value) \
    FlyString name = value##_fly_string;
ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR
#undef __ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR

GC_DEFINE_ALLOCATOR(RequireTrustedTypesForDirective);

RequireTrustedTypesForDirective::RequireTrustedTypesForDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

// https://www.w3.org/TR/trusted-types/#require-trusted-types-for-pre-navigation-check
ContentSecurityPolicy::Directives::Directive::Result RequireTrustedTypesForDirective::pre_navigation_check(GC::Ref<Fetch::Infrastructure::Request> request, NavigationType, GC::Ref<ContentSecurityPolicy::Policy const>) const
{
    // 1. If request’s url’s scheme is not "javascript", return "Allowed" and abort further steps.
    if (request->url().scheme() != "javascript"sv)
        return Result::Allowed;

    // 2. Let urlString be the result of running the URL serializer on request’s url.
    auto url_string = request->url().serialize();

    // 3. Let encodedScriptSource be the result of removing the leading "javascript:" from urlString.
    auto const encoded_script_source = MUST(url_string.substring_from_byte_offset("javascript:"sv.length()));

    // 4. Let convertedScriptSource be the result of executing Process value with a default policy algorithm, with the following arguments:
    //    expectedType:
    //      TrustedScript
    //    global:
    //      request’s clients’s global object:
    //    input:
    //      encodedScriptSource:
    //    sink:
    //      "Location href":
    auto converted_script_source = process_value_with_a_default_policy(
        TrustedTypeName::TrustedScript,
        request->client()->global_object(),
        Utf16String::from_utf8(encoded_script_source),
        InjectionSink::Locationhref);

    // If that algorithm threw an error or convertedScriptSource is not a TrustedScript object, return "Blocked" and abort further steps.
    if (converted_script_source.is_error() || !converted_script_source.value().has_value())
        return Result::Blocked;

    auto const* converted_script_source_value = converted_script_source.value().value().get_pointer<GC::Root<TrustedScript>>();

    if (!converted_script_source_value)
        return Result::Blocked;

    // 5. Set urlString to be the result of prepending "javascript:" to stringified convertedScriptSource.
    url_string = MUST(String::formatted("javascript:{}", (*converted_script_source_value)->to_string()));

    // 6. Let newURL be the result of running the URL parser on urlString. If the parser returns a failure, return "Blocked" and abort further steps.
    auto const new_url = DOMURL::parse(url_string);
    if (!new_url.has_value())
        return Result::Blocked;

    // 7. Set request’s url to newURL.
    request->set_url(new_url.value());

    // 8. Return "Allowed".
    return Result::Allowed;
}

// https://w3c.github.io/trusted-types/dist/spec/#does-sink-require-trusted-types
bool does_sink_require_trusted_types(JS::Object& global, String sink_group, IncludeReportOnlyPolicies include_report_only_policies)
{
    // 1. For each policy in global’s CSP list:
    for (auto const policy : ContentSecurityPolicy::PolicyList::from_object(global)->policies()) {
        // 1. If policy’s directive set does not contain a directive whose name is "require-trusted-types-for", skip to the next policy.
        if (!policy->contains_directive_with_name(ContentSecurityPolicy::Directives::Names::RequireTrustedTypesFor))
            continue;

        // 2. Let directive be the policy’s directive set’s directive whose name is "require-trusted-types-for"
        auto const directive = policy->get_directive_by_name(ContentSecurityPolicy::Directives::Names::RequireTrustedTypesFor);

        // 3. If directive’s value does not contain a trusted-types-sink-group which is a match for sinkGroup, skip to the next policy.
        auto const maybe_sink_group = directive->value().find_if([&sink_group](auto const& directive_value) {
            return directive_value.equals_ignoring_ascii_case(sink_group);
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
ContentSecurityPolicy::Directives::Directive::Result should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(JS::Object& global, TrustedTypes::InjectionSink sink, String sink_group, Utf16String const& source)
{
    auto& realm = HTML::relevant_realm(global);

    // 1. Let result be "Allowed".
    auto result = ContentSecurityPolicy::Directives::Directive::Result::Allowed;

    // 2. Let sample be source.
    auto sample = source.substring_view(0);

    // 3. If sink is "Function", then:
    if (sink == TrustedTypes::InjectionSink::Function) {
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
    for (auto const policy : ContentSecurityPolicy::PolicyList::from_object(global)->policies()) {
        // 1. If policy’s directive set does not contain a directive whose name is "require-trusted-types-for", skip to the next policy.
        if (!policy->contains_directive_with_name(ContentSecurityPolicy::Directives::Names::RequireTrustedTypesFor))
            continue;

        // 2. Let directive be the policy’s directive set’s directive whose name is "require-trusted-types-for"
        auto const directive = policy->get_directive_by_name(ContentSecurityPolicy::Directives::Names::RequireTrustedTypesFor);

        // 3. If directive’s value does not contain a trusted-types-sink-group which is a match for sinkGroup, skip to the next policy.
        auto const maybe_sink_group = directive->value().find_if([&sink_group](auto const& directive_value) {
            return directive_value.equals_ignoring_ascii_case(sink_group);
        });
        if (maybe_sink_group.is_end())
            continue;

        // 4. Let violation be the result of executing Create a violation object for global, policy, and directive on global, policy and "require-trusted-types-for"
        auto violation = ContentSecurityPolicy::Violation::create_a_violation_object_for_global_policy_and_directive(realm, global, policy, ContentSecurityPolicy::Directives::Names::RequireTrustedTypesFor.to_string());

        // 5. Set violation’s resource to "trusted-types-sink".
        violation->set_resource(ContentSecurityPolicy::Violation::Resource::TrustedTypesSink);

        // 6. Let trimmedSample be the substring of sample, containing its first 40 characters.
        auto const trimmed_sample = sample.substring_view(0, min(sample.length_in_code_points(), 40));

        // 7. Set violation’s sample to be the result of concatenating the list « sink, trimmedSample « using "|" as a separator.
        violation->set_sample(MUST(String::formatted("{}|{}", to_string(sink), trimmed_sample)));

        // 8. Execute Report a violation on violation.
        violation->report_a_violation(realm);

        // 9. If policy’s disposition is "enforce", then set result to "Blocked".
        if (policy->disposition() == ContentSecurityPolicy::Policy::Disposition::Enforce)
            result = ContentSecurityPolicy::Directives::Directive::Result::Blocked;
    }

    // 5. Return result.
    return result;
}

}
