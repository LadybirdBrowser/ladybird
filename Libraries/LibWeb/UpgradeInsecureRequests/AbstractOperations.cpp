/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/UpgradeInsecureRequests/AbstractOperations.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>

namespace Web::UpgradeInsecureRequests {

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#delivery
// AD-HOC: Enforcing the upgrade-insecure-requests directive sets the protected resource's settings' insecure requests
//         policy to "Upgrade", and inserts a tuple of the protected resource's URL's host and port into settings'
//         upgrade insecure navigations set. The spec then copies both into nested browsing contexts, their documents
//         and workers. We instead derive both from settings' policy container's CSP list, which HTML already
//         inherits into documents and workers with local scheme URLs, and use each policy's self-origin as the
//         protected resource's URL. Monitoring the directive has no effect, so only enforced policies count.
// FIXME: Nested browsing contexts whose documents do not inherit their embedding document's policy container, and
//        workers that do not inherit their owner's, should also inherit its insecure requests policy and upgrade
//        insecure navigations set.
template<typename Callback>
static void for_each_enforced_policy_with_upgrade_insecure_requests(HTML::EnvironmentSettingsObject const& settings, Callback callback)
{
    for (auto const& policy : settings.policy_container()->csp_list->policies()) {
        if (policy->disposition() != ContentSecurityPolicy::Policy::Disposition::Enforce)
            continue;
        if (!policy->contains_directive_with_name(ContentSecurityPolicy::Directives::Names::UpgradeInsecureRequests))
            continue;
        if (callback(*policy) == IterationDecision::Break)
            return;
    }
}

static InsecureRequestsPolicy insecure_requests_policy(HTML::EnvironmentSettingsObject const& settings)
{
    auto result = InsecureRequestsPolicy::DoNotUpgrade;
    for_each_enforced_policy_with_upgrade_insecure_requests(settings, [&](auto const&) {
        result = InsecureRequestsPolicy::Upgrade;
        return IterationDecision::Break;
    });
    return result;
}

static bool upgrade_insecure_navigations_set_contains(HTML::EnvironmentSettingsObject const& settings, URL::URL const& url)
{
    auto const& host = url.host();
    if (!host.has_value())
        return false;

    bool result = false;
    for_each_enforced_policy_with_upgrade_insecure_requests(settings, [&](ContentSecurityPolicy::Policy const& policy) {
        auto const& self_origin = policy.self_origin();
        if (!self_origin.is_opaque() && self_origin.host() == *host && self_origin.port() == url.port()) {
            result = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return result;
}

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#upgrade-request
void upgrade_request_to_a_potentially_trustworthy_url_if_appropriate(Fetch::Infrastructure::Request& request)
{
    // AD-HOC: This algorithm predates request's URL list. It notes that it upgrades insecurely-redirected requests as
    //         well as insecure initial requests, which needs it to operate on request's current URL.
    auto& url = request.current_url();

    // 1. If request is a navigation request, append a header named Upgrade-Insecure-Requests with a value of 1 to
    //    request's header list if any of the following criteria are met:
    //    1. request's URL is not a potentially trustworthy URL
    //    2. request's URL's host is not a preloadable HSTS host
    // NB: We have no preloaded HSTS hosts, so the second criterion is always met.
    // AD-HOC: Main fetch runs this algorithm again for every redirect, so only append the header once.
    if (request.is_navigation_request() && !request.header_list()->contains("Upgrade-Insecure-Requests"sv))
        request.header_list()->append(HTTP::Header::isomorphic_encode("Upgrade-Insecure-Requests"sv, "1"sv));

    // 2. If request is a navigation request, then:
    if (request.is_navigation_request()) {
        // 1. If request is a form submission, skip the remaining substeps, and continue upgrading request.
        // 2. If request's client's target browsing context is a nested browsing context, skip the remaining substeps
        //    and continue upgrading request.
        // NB: A navigation request's target browsing context is its reserved client's.
        auto target_browsing_context = request.target_browsing_context();
        if (!request.is_form_submission() && (!target_browsing_context || target_browsing_context->is_top_level())) {
            // 3. Let tuple be a tuple of request's URL's host and port.
            // 4. If tuple is contained in client's upgrade insecure navigations set, then skip the remaining
            //    substeps, and continue upgrading request.
            if (!request.client() || !upgrade_insecure_navigations_set_contains(*request.client(), url)) {
                // 5. Return without further modifying request.
                return;
            }
        }
    }

    // 3. Let upgrade state be the result of executing § 4.2 Should insecure requests be upgraded for client? upon
    //    request's client.
    auto upgrade_state = should_insecure_requests_be_upgraded_for_client(request.client());

    // 4. If upgrade state is Do Not Upgrade, return without modifying request.
    if (upgrade_state == InsecureRequestsPolicy::DoNotUpgrade)
        return;

    // 5. If request's URL's scheme is "http", set request's URL's scheme to "https", and return.
    if (url.scheme() == "http"sv) {
        url.set_scheme("https"sv);

        // NB: Unlike the URL parser, setting the scheme does not remove a port that is the new scheme's default port.
        if (url.port() == 443)
            url.set_port({});
    }
}

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#should-upgrade-for-client
InsecureRequestsPolicy should_insecure_requests_be_upgraded_for_client(GC::Ptr<HTML::EnvironmentSettingsObject> client)
{
    // AD-HOC: Fetch requests initiated by the browser UI have no client. Their requests are never upgraded.
    if (!client)
        return InsecureRequestsPolicy::DoNotUpgrade;

    // 1. If client has a responsible document, return the value of its insecure requests policy.
    // 2. If client has a responsible browsing context, return the value of its insecure requests policy.
    // 3. Return Do Not Upgrade.
    // NB: See for_each_enforced_policy_with_upgrade_insecure_requests() for how we determine client's insecure
    //     requests policy.
    return insecure_requests_policy(*client);
}

}
