/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/UpgradeInsecureRequests/AbstractOperations.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>

namespace Web::UpgradeInsecureRequests {

// AD-HOC: Upgrade Insecure Requests reads request's client's target browsing context, but means the browsing context
//         being navigated, which request's reserved client names.
static bool target_browsing_context_is_nested(Fetch::Infrastructure::Request const& request)
{
    using Context = Fetch::Infrastructure::ReservedClientContext;
    auto const& reserved_client = request.reserved_client_snapshot();
    auto const* environment = reserved_client ? reserved_client->kind.get_pointer<Context::Environment>() : nullptr;
    return environment && environment->target_browsing_context == Context::TargetBrowsingContext::Nested;
}

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#upgrade-request
void upgrade_request_to_a_potentially_trustworthy_url_if_appropriate(Fetch::Infrastructure::Request& request)
{
    // AD-HOC: This algorithm predates request's URL list. It notes that it upgrades insecurely-redirected requests as
    //         well as insecure initial requests, which needs it to operate on request's current URL.
    auto& url = request.current_url();

    // AD-HOC: Request's policy container holds the insecure requests policy and upgrade insecure navigations set of
    //         request's client as captured for this fetch, which the steps below read in place of client's.
    auto const& policy_container = *request.policy_container().get<NonnullRefPtr<HTML::PolicyContainer const>>();

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
        if (!request.is_form_submission() && !target_browsing_context_is_nested(request)) {
            // 3. Let tuple be a tuple of request's URL's host and port.
            // 4. If tuple is contained in client's upgrade insecure navigations set, then skip the remaining
            //    substeps, and continue upgrading request.
            auto const& host = url.host();
            if (!host.has_value() || !policy_container.upgrade_insecure_navigations_set().contains({ *host, url.port() })) {
                // 5. Return without further modifying request.
                return;
            }
        }
    }

    // 3. Let upgrade state be the result of executing § 4.2 Should insecure requests be upgraded for client? upon
    //    request's client.
    // AD-HOC: § 4.2 reads client's responsible document or browsing context, which HTML no longer has. Request's policy
    //         container holds what client inherited, and its CSP list the policies client enforces. See
    //         https://github.com/w3c/webappsec-upgrade-insecure-requests/issues/27.
    auto upgrade_state = policy_container.insecure_requests_policy();

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

}
