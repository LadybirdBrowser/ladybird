/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::ContentSecurityPolicy {

// https://w3c.github.io/webappsec-csp/#does-resource-hint-violate-policy
[[nodiscard]] static GC::Ptr<Directives::Directive> does_resource_hint_request_violate_policy(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Policy const> policy)
{
    // 1. Let defaultDirective be policy’s first directive whose name is "default-src".
    auto default_directive_iterator = policy->directives().find_if([](auto const& directive) {
        return directive->name() == Directives::Names::DefaultSrc;
    });

    // 2. If defaultDirective does not exist, return "Does Not Violate".
    if (default_directive_iterator.is_end())
        return {};

    // 3. For each directive of policy:
    for (auto directive : policy->directives()) {
        // 1. Let result be the result of executing directive’s pre-request check on request and policy.
        auto result = directive->pre_request_check(realm, request, policy);

        // 2. If result is "Allowed", then return "Does Not Violate".
        if (result == Directives::Directive::Result::Allowed) {
            return {};
        }
    }

    // 4. Return defaultDirective.
    return *default_directive_iterator;
}

// https://w3c.github.io/webappsec-csp/#does-request-violate-policy
[[nodiscard]] static GC::Ptr<Directives::Directive> does_request_violate_policy(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Policy const> policy)
{
    // 1. If request’s initiator is "prefetch", then return the result of executing § 6.7.2.2 Does resource hint
    //    request violate policy? on request and policy.
    if (request->initiator() == Fetch::Infrastructure::Request::Initiator::Prefetch)
        return does_resource_hint_request_violate_policy(realm, request, policy);

    // 2. Let violates be "Does Not Violate".
    GC::Ptr<Directives::Directive> violates;

    // 3. For each directive of policy:
    for (auto directive : policy->directives()) {
        // 1. Let result be the result of executing directive’s pre-request check on request and policy.
        auto result = directive->pre_request_check(realm, request, policy);

        // 2. If result is "Blocked", then let violates be directive.
        if (result == Directives::Directive::Result::Blocked) {
            violates = directive;
        }
    }

    // 4. Return violates.
    return violates;
}

// https://w3c.github.io/webappsec-csp/#report-for-request
void report_content_security_policy_violations_for_request(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request> request)
{
    // 1. Let CSP list be request’s policy container's CSP list.
    auto csp_list = request->policy_container().get<GC::Ref<HTML::PolicyContainer>>()->csp_list;

    // 2. For each policy of CSP list:
    for (auto policy : csp_list->policies()) {
        // 1. If policy’s disposition is "enforce", then skip to the next policy.
        if (policy->disposition() == Policy::Disposition::Enforce)
            continue;

        // 2. Let violates be the result of executing § 6.7.2.1 Does request violate policy? on request and policy.
        auto violates = does_request_violate_policy(realm, request, policy);

        // 3. If violates is not "Does Not Violate", then execute § 5.5 Report a violation on the result of executing
        //    § 2.4.2 Create a violation object for request, and policy. on request, and policy.
        if (violates) {
            auto violation = Violation::create_a_violation_object_for_request_and_policy(realm, request, policy);
            violation->report_a_violation(realm);
        }
    }
}

Directives::Directive::Result should_request_be_blocked_by_content_security_policy(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request> request)
{
    // 1. Let CSP list be request’s policy container's CSP list.
    auto csp_list = request->policy_container().get<GC::Ref<HTML::PolicyContainer>>()->csp_list;

    // 2. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 3. For each policy of CSP list:
    for (auto policy : csp_list->policies()) {
        // 1. If policy’s disposition is "report", then skip to the next policy.
        if (policy->disposition() == Policy::Disposition::Report)
            continue;

        // 2. Let violates be the result of executing § 6.7.2.1 Does request violate policy? on request and policy.
        auto violates = does_request_violate_policy(realm, request, policy);

        // 3. If violates is not "Does Not Violate", then:
        if (violates) {
            // 1. Execute § 5.5 Report a violation on the result of executing § 2.4.2 Create a violation object for
            //    request, and policy. on request, and policy.
            auto violation = Violation::create_a_violation_object_for_request_and_policy(realm, request, policy);
            violation->report_a_violation(realm);

            // 2. Set result to "Blocked".
            result = Directives::Directive::Result::Blocked;
        }
    }

    // 4. Return result.
    return result;
}

}
