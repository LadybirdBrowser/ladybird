/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/DefaultSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveFactory.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(DefaultSourceDirective);

DefaultSourceDirective::DefaultSourceDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

// https://w3c.github.io/webappsec-csp/#default-src-pre-request
Directive::Result DefaultSourceDirective::pre_request_check(GC::Heap& heap, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Policy const> policy) const
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, default-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::DefaultSrc, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. Return the result of executing the pre-request check for the directive whose name is name on request and
    //    policy, using this directive’s value for the comparison.
    auto directive = create_directive(heap, name->to_string(), value());
    return directive->pre_request_check(heap, request, policy);
}

// https://w3c.github.io/webappsec-csp/#default-src-post-request
Directive::Result DefaultSourceDirective::post_request_check(GC::Heap& heap, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ref<Policy const> policy) const
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, default-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::DefaultSrc, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. Return the result of executing the post-request check for the directive whose name is name on request,
    //    response, and policy, using this directive’s value for the comparison.
    auto directive = create_directive(heap, name->to_string(), value());
    return directive->post_request_check(heap, request, response, policy);
}

// https://w3c.github.io/webappsec-csp/#default-src-inline
Directive::Result DefaultSourceDirective::inline_check(GC::Heap& heap, GC::Ptr<DOM::Element const> element, InlineType type, GC::Ref<Policy const> policy, String const& source) const
{
    // 1. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, default-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::DefaultSrc, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. Otherwise, return the result of executing the inline check for the directive whose name is name on element,
    //    type, policy and source, using this directive’s value for the comparison.
    auto directive = create_directive(heap, name.to_string(), value());
    return directive->inline_check(heap, element, type, policy, source);
}

}
