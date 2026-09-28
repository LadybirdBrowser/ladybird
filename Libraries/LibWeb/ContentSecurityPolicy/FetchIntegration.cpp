/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/FetchIntegration.h>
#include <LibWeb/ContentSecurityPolicy/FetchIntegration.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/PolicyContainers.h>

namespace Web::ContentSecurityPolicy {

// https://w3c.github.io/webappsec-csp/#does-resource-hint-violate-policy
[[nodiscard]] static Directives::Directive const* does_resource_hint_request_violate_policy(Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let defaultDirective be policy’s first directive whose name is "default-src".
    auto default_directive_iterator = policy.directives().find_if([](auto const& directive) {
        return directive.kind() == Directives::Directive::Kind::DefaultSrc;
    });

    // 2. If defaultDirective does not exist, return "Does Not Violate".
    if (default_directive_iterator.is_end())
        return nullptr;

    // 3. For each directive of policy:
    for (auto const& directive : policy.directives()) {
        // 1. Let result be the result of executing directive’s pre-request check on request and policy.
        auto result = Directives::pre_request_check(directive, request, policy);

        // 2. If result is "Allowed", then return "Does Not Violate".
        if (result == Directives::Directive::Result::Allowed) {
            return nullptr;
        }
    }

    // 4. Return defaultDirective.
    return &*default_directive_iterator;
}

// https://w3c.github.io/webappsec-csp/#does-request-violate-policy
[[nodiscard]] static Directives::Directive const* does_request_violate_policy(Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. If request’s initiator is "prefetch", then return the result of executing § 6.7.2.2 Does resource hint
    //    request violate policy? on request and policy.
    if (request.initiator() == Fetch::Infrastructure::Request::Initiator::Prefetch)
        return does_resource_hint_request_violate_policy(request, policy);

    // 2. Let violates be "Does Not Violate".
    Directives::Directive const* violates = nullptr;

    // 3. For each directive of policy:
    for (auto const& directive : policy.directives()) {
        // 1. Let result be the result of executing directive’s pre-request check on request and policy.
        auto result = Directives::pre_request_check(directive, request, policy);

        // 2. If result is "Blocked", then let violates be directive.
        if (result == Directives::Directive::Result::Blocked) {
            violates = &directive;
        }
    }

    // 4. Return violates.
    return violates;
}

// https://w3c.github.io/webappsec-csp/#report-for-request
void report_content_security_policy_violations_for_request(Fetch::Infrastructure::Request const& request, ViolationReporter const& report_violation)
{
    // 1. Let CSP list be request’s policy container's CSP list.
    auto const& csp_list = request.policy_container().get<NonnullRefPtr<HTML::PolicyContainer const>>()->csp_list;

    // 2. For each policy of CSP list:
    for (auto const& policy : csp_list.policies()) {
        // 1. If policy’s disposition is "enforce", then skip to the next policy.
        if (policy->disposition() == Policy::Disposition::Enforce)
            continue;

        // 2. Let violates be the result of executing § 6.7.2.1 Does request violate policy? on request and policy.
        auto violates = does_request_violate_policy(request, policy);

        // 3. If violates is not "Does Not Violate", then execute § 5.5 Report a violation on the result of executing
        //    § 2.4.2 Create a violation object for request, and policy. on request, and policy.
        if (violates)
            report_violation(policy);
    }
}

// https://w3c.github.io/webappsec-csp/#should-block-request
Directives::Directive::Result should_request_be_blocked_by_content_security_policy(Fetch::Infrastructure::Request const& request, ViolationReporter const& report_violation)
{
    // 1. Let CSP list be request’s policy container's CSP list.
    auto const& csp_list = request.policy_container().get<NonnullRefPtr<HTML::PolicyContainer const>>()->csp_list;

    // 2. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 3. For each policy of CSP list:
    for (auto const& policy : csp_list.policies()) {
        // 1. If policy’s disposition is "report", then skip to the next policy.
        if (policy->disposition() == Policy::Disposition::Report)
            continue;

        // 2. Let violates be the result of executing § 6.7.2.1 Does request violate policy? on request and policy.
        auto violates = does_request_violate_policy(request, policy);

        // 3. If violates is not "Does Not Violate", then:
        if (violates) {
            // 1. Execute § 5.5 Report a violation on the result of executing § 2.4.2 Create a violation object for
            //    request, and policy. on request, and policy.
            report_violation(policy);

            // 2. Set result to "Blocked".
            result = Directives::Directive::Result::Blocked;
        }
    }

    // 4. Return result.
    return result;
}

// https://w3c.github.io/webappsec-csp/#should-block-response
Directives::Directive::Result should_response_to_request_be_blocked_by_content_security_policy(Fetch::Infrastructure::Response const& response, Fetch::Infrastructure::Request const& request, ViolationReporter const& report_violation)
{
    // 1. Let CSP list be request’s policy container's CSP list.
    auto const& csp_list = request.policy_container().get<NonnullRefPtr<HTML::PolicyContainer const>>()->csp_list;

    // 2. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 3. For each policy of CSP list:
    // Spec Note: This portion of the check verifies that the page can load the response. That is, that a Service
    //            Worker hasn't substituted a file which would violate the page’s CSP.
    for (auto const& policy : csp_list.policies()) {
        // 1. For each directive of policy:
        for (auto const& directive : policy->directives()) {
            // 1. If the result of executing directive’s post-request check is "Blocked", then:
            if (Directives::post_request_check(directive, request, response, policy) == Directives::Directive::Result::Blocked) {
                // 1. Execute § 5.5 Report a violation on the result of executing § 2.4.2 Create a violation object for
                //    request, and policy. on request, and policy.
                report_violation(policy);

                // 2. If policy’s disposition is "enforce", then set result to "Blocked".
                if (policy->disposition() == Policy::Disposition::Enforce) {
                    result = Directives::Directive::Result::Blocked;
                }
            }
        }
    }

    // 4. Return result.
    return result;
}

}
