/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

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

// https://w3c.github.io/webappsec-csp/#should-block-request
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

// https://w3c.github.io/webappsec-csp/#should-block-response
Directives::Directive::Result should_response_to_request_be_blocked_by_content_security_policy(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Response> response, GC::Ref<Fetch::Infrastructure::Request> request)
{
    // 1. Let CSP list be request’s policy container's CSP list.
    auto csp_list = request->policy_container().get<GC::Ref<HTML::PolicyContainer>>()->csp_list;

    // 2. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 3. For each policy of CSP list:
    // Spec Note: This portion of the check verifies that the page can load the response. That is, that a Service
    //            Worker hasn't substituted a file which would violate the page’s CSP.
    for (auto policy : csp_list->policies()) {
        // 1. For each directive of policy:
        for (auto directive : policy->directives()) {
            // 1. If the result of executing directive’s post-request check is "Blocked", then:
            if (directive->post_request_check(realm, request, response, policy) == Directives::Directive::Result::Blocked) {
                // 1. Execute § 5.5 Report a violation on the result of executing § 2.4.2 Create a violation object for
                //    request, and policy. on request, and policy.
                auto violation = Violation::create_a_violation_object_for_request_and_policy(realm, request, policy);
                violation->report_a_violation(realm);

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

// https://w3c.github.io/webappsec-csp/#should-block-navigation-request
Directives::Directive::Result should_navigation_request_of_type_be_blocked_by_content_security_policy(GC::Ref<Fetch::Infrastructure::Request> navigation_request, Directives::Directive::NavigationType navigation_type)
{
    // 1. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 2. For each policy of navigation request’s policy container’s CSP list:
    auto policy_container = navigation_request->policy_container().get<GC::Ref<HTML::PolicyContainer>>();
    for (auto policy : policy_container->csp_list->policies()) {
        // 1. For each directive of policy:
        for (auto directive : policy->directives()) {
            // 1. If directive’s pre-navigation check returns "Allowed" when executed upon navigation request, type, and policy skip to the next directive.
            auto directive_result = directive->pre_navigation_check(navigation_request, navigation_type, policy);
            if (directive_result == Directives::Directive::Result::Allowed)
                continue;

            // 2. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on navigation request’s
            //    client’s global object, policy, and directive’s name.
            auto& realm = navigation_request->client()->realm();
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, navigation_request->client()->global_object(), policy, directive->name());

            // 3. Set violation’s resource to navigation request’s URL.
            violation->set_resource(navigation_request->url());

            // 4. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(realm);

            // 5. If policy’s disposition is "enforce", then set result to "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce)
                result = Directives::Directive::Result::Blocked;
        }
    }

    // 3. If result is "Allowed", and if navigation request’s current URL’s scheme is javascript:
    if (result == Directives::Directive::Result::Allowed && navigation_request->current_url().scheme() == "javascript"sv) {
        // 1. For each policy of navigation request’s policy container’s CSP list:
        VERIFY(navigation_request->policy_container().has<GC::Ref<HTML::PolicyContainer>>());
        auto csp_list = navigation_request->policy_container().get<GC::Ref<HTML::PolicyContainer>>()->csp_list;

        for (auto policy : csp_list->policies()) {
            // 1. For each directive of policy:
            for (auto directive : policy->directives()) {
                // 1. Let directive-name be the result of executing § 6.8.2 Get the effective directive for inline
                //    checks on type.
                // FIXME: File spec issue that the type should probably always be "navigation", as NavigationType would
                //        cause this algorithm to return null, making directive-name null, then piping directive-name
                //        into a Violation object where the directive name is defined to be a non-empty string.
                //        Other parts of the spec seem to refer to the "navigation" inline type as being for
                //        javascript: URLs. Additionally, this doesn't have an impact on the security decision here,
                //        just which directive is reported to have been violated.
                auto directive_name = Directives::get_the_effective_directive_for_inline_checks(Directives::Directive::InlineType::Navigation);

                // 2. If directive’s inline check returns "Allowed" when executed upon null, "navigation" and
                //    navigation request’s current URL, skip to the next directive.
                // FIXME: File spec issue that they forgot to pass in "policy" here.
                // FIXME: File spec issue that current URL is a URL object and not a string, therefore they must use a
                //        spec operation to serialize the URL.
                auto& realm = navigation_request->client()->realm();
                auto serialized_url = navigation_request->current_url().to_string();
                if (directive->inline_check(realm, nullptr, Directives::Directive::InlineType::Navigation, policy, serialized_url) == Directives::Directive::Result::Allowed)
                    continue;

                // 3. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global,
                //    policy, and directive on navigation request’s client’s global object, policy, and directive-name.
                auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, navigation_request->client()->global_object(), policy, directive_name.to_string());

                // 4. Set violation’s resource to navigation request’s URL.
                violation->set_resource(navigation_request->url());

                // 5. Execute § 5.5 Report a violation on violation.
                violation->report_a_violation(realm);

                // 6. If policy’s disposition is "enforce", then set result to "Blocked".
                if (policy->disposition() == Policy::Disposition::Enforce)
                    result = Directives::Directive::Result::Blocked;
            }
        }
    }

    // 4. Return result.
    return result;
}

// https://w3c.github.io/webappsec-csp/#should-block-navigation-response
Directives::Directive::Result should_navigation_response_to_navigation_request_of_type_in_target_be_blocked_by_content_security_policy(
    GC::Ptr<Fetch::Infrastructure::Request> navigation_request,
    GC::Ref<Fetch::Infrastructure::Response> navigation_response,
    GC::Ref<PolicyList> response_csp_list,
    Directives::Directive::NavigationType navigation_type,
    GC::Ref<HTML::Navigable> target)
{
    // 1. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // FIXME: File spec issue stating that the request can be null (e.g. from a srcdoc resource).
    if (!navigation_request) {
        dbgln("FIXME: Handle null navigation_request in navigation response Content Security Policy check.");
        return result;
    }

    // 2. For each policy of response CSP list:
    for (auto policy : response_csp_list->policies()) {
        // Spec Note: Some directives (like frame-ancestors) allow a response’s Content Security Policy to act on the navigation.
        // 1. For each directive of policy:
        for (auto directive : policy->directives()) {
            // 1. If directive’s navigation response check returns "Allowed" when executed upon navigation request, type, navigation response, target, "response", and policy skip to the next directive.
            auto directive_result = directive->navigation_response_check(*navigation_request, navigation_type, navigation_response, target, Directives::Directive::CheckType::Response, policy);
            if (directive_result == Directives::Directive::Result::Allowed)
                continue;

            // 2. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on null, policy, and directive’s name.
            // Spec Note: We use null for the global object, as no global exists: we haven’t processed the navigation to create a Document yet.
            // FIXME: What should the realm be here?
            auto& realm = navigation_request->client()->realm();
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, nullptr, policy, directive->name());

            // 3. Set violation’s resource to navigation response’s URL.
            if (navigation_response->url().has_value()) {
                violation->set_resource(navigation_response->url().value());
            } else {
                violation->set_resource(Empty {});
            }

            // 4. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(realm);

            // 5. If policy’s disposition is "enforce", then set result to "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce)
                result = Directives::Directive::Result::Blocked;
        }
    }

    // 3. For each policy of navigation request’s policy container’s CSP list:
    auto request_policy_container = navigation_request->policy_container().get<GC::Ref<HTML::PolicyContainer>>();
    for (auto policy : request_policy_container->csp_list->policies()) {
        // Spec Note: NOTE: Some directives in the navigation request’s context (like frame-ancestors) need the response before acting on the navigation.
        // 1. For each directive of policy:
        for (auto directive : policy->directives()) {
            // 1. If directive’s navigation response check returns "Allowed" when executed upon navigation request, type, navigation response, target, "source", and policy skip to the next directive.
            auto directive_result = directive->navigation_response_check(*navigation_request, navigation_type, navigation_response, target, Directives::Directive::CheckType::Source, policy);
            if (directive_result == Directives::Directive::Result::Allowed)
                continue;

            // 2. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on navigation request’s client’s global object, policy, and directive’s name.
            auto& realm = navigation_request->client()->realm();
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, navigation_request->client()->global_object(), policy, directive->name());

            // 3. Set violation’s resource to navigation request’s URL.
            violation->set_resource(navigation_request->url());

            // 4. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(realm);

            // 5. If policy’s disposition is "enforce", then set result to "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce)
                result = Directives::Directive::Result::Blocked;
        }
    }

    // 4. Return result.
    return result;
}

}
