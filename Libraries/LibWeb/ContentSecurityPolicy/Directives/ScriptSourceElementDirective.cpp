/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ScriptSourceElementDirective.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(ScriptSourceElementDirective);

ScriptSourceElementDirective::ScriptSourceElementDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

// https://w3c.github.io/webappsec-csp/#script-src-elem-pre-request
Directive::Result ScriptSourceElementDirective::pre_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Policy const> policy) const
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, script-src-elem and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ScriptSrcElem, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. Return the result of executing § 6.7.1.1 Script directives pre-request check on request, this directive,
    //    and policy.
    return script_directives_pre_request_check(request, *this, policy);
}

// https://w3c.github.io/webappsec-csp/#script-src-elem-post-request
Directive::Result ScriptSourceElementDirective::post_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ref<Policy const> policy) const
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, script-src-elem and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ScriptSrcElem, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. Return the result of executing § 6.7.1.2 Script directives post-request check on request, response, this
    //    directive, and policy.
    return script_directives_post_request_check(request, response, *this, policy);
}

// https://w3c.github.io/webappsec-csp/#script-src-elem-inline
Directive::Result ScriptSourceElementDirective::inline_check(GC::Heap&, GC::Ptr<DOM::Element const> element, InlineType type, GC::Ref<Policy const> policy, String const& source) const
{
    // 1. Assert: element is not null or type is "navigation".
    VERIFY(element || type == InlineType::Navigation);

    // 2. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 3. If the result of executing § 6.8.4 Should fetch directive execute on name, script-src-elem and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ScriptSrcElem, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 4. If the result of executing § 6.7.3.3 Does element match source list for type and source? on element, this
    //    directive’s value, type, and source is "Does Not Match", return "Blocked".
    if (does_element_match_source_list_for_type_and_source(element, value(), type, source) == MatchResult::DoesNotMatch)
        return Result::Blocked;

    // 5. Return "Allowed".
    return Result::Allowed;
}

}
