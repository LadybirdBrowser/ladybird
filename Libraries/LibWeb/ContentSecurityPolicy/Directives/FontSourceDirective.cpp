/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/FontSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(FontSourceDirective);

FontSourceDirective::FontSourceDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

// https://w3c.github.io/webappsec-csp/#font-src-pre-request
Directive::Result FontSourceDirective::pre_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Policy const> policy) const
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, font-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::FontSrc, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
    //    and policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, value(), policy) == MatchResult::DoesNotMatch)
        return Result::Blocked;

    // 4. Return "Allowed".
    return Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#font-src-post-request
Directive::Result FontSourceDirective::post_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ref<Policy const> policy) const
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, font-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::FontSrc, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, value(), policy) == MatchResult::DoesNotMatch)
        return Result::Blocked;

    // 4. Return "Allowed".
    return Result::Allowed;
}

}
