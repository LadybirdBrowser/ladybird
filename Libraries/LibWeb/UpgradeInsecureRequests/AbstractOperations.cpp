/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/UpgradeInsecureRequests/AbstractOperations.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>

namespace Web::UpgradeInsecureRequests {

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#nesting
void inherit_the_insecure_requests_policy_into_a_nested_browsing_context(HTML::BrowsingContext& context, InsecureRequestsPolicy embedding_document_policy, UpgradeInsecureNavigationsSet const& embedding_document_set)
{
    // 1. When a nested browsing context context is created:
    // NB: The caller passes context's embedding document's insecure requests policy and upgrade insecure navigations
    //     set, which the process holding that document reports when context is created in another one.

    // 1. If context's embedding document's insecure requests policy is Upgrade, then:
    if (embedding_document_policy != InsecureRequestsPolicy::Upgrade)
        return;

    // 1. Set context's insecure requests policy to Upgrade.
    context.set_insecure_requests_policy(InsecureRequestsPolicy::Upgrade);

    // 2. For each value in context's embedding document's upgrade insecure navigations set, add value to context's
    //    upgrade insecure navigations set.
    for (auto const& value : embedding_document_set.tuples())
        context.add_to_upgrade_insecure_navigations_set(value);
}

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#nesting
void inherit_the_insecure_requests_policy_into_a_new_document(HTML::EnvironmentSettingsObject& document_settings, HTML::BrowsingContext const& context)
{
    // 2. When creating a new Document object document in a browsing context context:

    // 1. If context's insecure requests policy is Upgrade, then:
    if (context.insecure_requests_policy() != InsecureRequestsPolicy::Upgrade)
        return;

    // 1. Let settings be document's incumbent settings object.
    // NB: That is document's relevant settings object, which the caller passes. A document that reuses its browsing
    //     context's initial about:blank document's window shares its settings object, which inherited the same.
    auto& settings = document_settings;

    // 2. Set settings' insecure requests policy to Upgrade.
    settings.set_insecure_requests_policy(InsecureRequestsPolicy::Upgrade);

    // 3. For each value in context's upgrade insecure navigations set, add value to settings's upgrade insecure
    //    navigations set.
    for (auto const& value : context.upgrade_insecure_navigations_set().tuples())
        settings.add_to_upgrade_insecure_navigations_set(value);
}

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#nesting
void inherit_the_insecure_requests_policy_into_a_worker(HTML::EnvironmentSettingsObject& settings_object, InsecureRequestsPolicy outside_settings_policy, UpgradeInsecureNavigationsSet const& outside_settings_set)
{
    // When executing the set up a worker environment settings object algorithm, perform the following steps after the
    // current step #4:

    // 1. If inherited responsible browsing context's insecure requests policy is Upgrade, then:
    // AD-HOC: HTML no longer has an inherited responsible browsing context, which was outside settings' responsible
    //         browsing context. We inherit outside settings' insecure requests policy and upgrade insecure navigations
    //         set instead, which the caller passes. Those include what outside settings inherited from its browsing
    //         context, and the policies its own CSP list enforces, so a top-level document's workers inherit too.
    if (outside_settings_policy != InsecureRequestsPolicy::Upgrade)
        return;

    // 1. Set settings object's insecure requests policy to Upgrade.
    settings_object.set_insecure_requests_policy(InsecureRequestsPolicy::Upgrade);

    // 2. For each value in inherited responsible browsing context's upgrade insecure navigations set, add value to
    //    settings object's upgrade insecure navigations set.
    for (auto const& value : outside_settings_set.tuples())
        settings_object.add_to_upgrade_insecure_navigations_set(value);
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
            auto const& host = url.host();
            if (!request.client() || !host.has_value() || !request.client()->upgrade_insecure_navigations_set().contains({ *host, url.port() })) {
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
    // NB: A client's responsible document's insecure requests policy is the client's own, which includes what it
    //     inherited from its responsible browsing context. See EnvironmentSettingsObject::insecure_requests_policy().
    return client->insecure_requests_policy();
}

}
