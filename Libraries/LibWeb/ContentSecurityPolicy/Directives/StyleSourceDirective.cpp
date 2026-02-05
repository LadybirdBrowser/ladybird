/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Directives/StyleSourceDirective.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(StyleSourceDirective);

StyleSourceDirective::StyleSourceDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

// https://w3c.github.io/webappsec-csp/#style-src-pre-request
Directive::Result StyleSourceDirective::pre_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Policy const> policy) const
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrc, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. If the result of executing § 6.7.2.3 Does nonce match source list? on request’s cryptographic nonce metadata
    //    and this directive’s value is "Matches", return "Allowed".
    if (does_nonce_match_source_list(request->cryptographic_nonce_metadata(), value()) == MatchResult::Matches)
        return Result::Allowed;

    // 4. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value, and
    //    policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, value(), policy) == MatchResult::DoesNotMatch)
        return Result::Blocked;

    // 5. Return "Allowed".
    return Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#style-src-post-request
Directive::Result StyleSourceDirective::post_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ref<Policy const> policy) const
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrc, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. If the result of executing § 6.7.2.3 Does nonce match source list? on request’s cryptographic nonce metadata
    //    and this directive’s value is "Matches", return "Allowed".
    if (does_nonce_match_source_list(request->cryptographic_nonce_metadata(), value()) == MatchResult::Matches)
        return Result::Allowed;

    // 4. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, value(), policy) == MatchResult::DoesNotMatch)
        return Result::Blocked;

    // 5. Return "Allowed".
    return Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#style-src-inline
Directive::Result StyleSourceDirective::inline_check(GC::Heap&, GC::Ptr<DOM::Element const> element, InlineType type, GC::Ref<Policy const> policy, String const& source) const
{
    // 1. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrc, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. If the result of executing § 6.7.3.3 Does element match source list for type and source? on element, this
    //    directive’s value, type, and source, is "Does Not Match", return "Blocked".
    if (does_element_match_source_list_for_type_and_source(element, value(), type, source) == MatchResult::DoesNotMatch)
        return Result::Blocked;

    // 4. Return "Allowed".
    return Result::Allowed;
}

}
