/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/RequestViolation.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/PolicyContainers.h>

namespace Web::ContentSecurityPolicy {

u64 index_of_policy_in_requests_csp_list(Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // NB: Request's policy container is a copy of the client's, which lists the policies in the same order.
    return request.policy_container().get<NonnullRefPtr<HTML::PolicyContainer const>>()->csp_list.index_of(policy);
}

RequestViolation violation_of_request(Fetch::Infrastructure::Request const& request, NonnullRefPtr<Policy const> policy)
{
    // https://w3c.github.io/webappsec-csp/#create-violation-for-request
    // 1. Let directive be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto effective_directive = Directives::get_the_effective_directive_for_request(request);

    // NOTE: The spec assumes that the effective directive of a Violation is a non-empty string.
    //       See the definition of m_effective_directive.
    VERIFY(effective_directive.has_value());

    auto reported_to = [&] -> RequestViolation::ReportedTo {
        if (auto const& navigation = request.child_navigable_navigation(); navigation.has_value()) {
            // NB: The request's copy of the container document's CSP list lists the policies in the same order.
            auto policy_index = navigation->container_document_csp_list.index_of(policy);
            return RequestViolation::ContainerDocument { navigation->navigable_id, navigation->navigation_id, policy_index };
        }
        if (auto const& client = request.client(); client.has_value() && client->has<NonnullRefPtr<HTML::RemoteEnvironmentSettings const>>())
            return RequestViolation::RemoteClient { index_of_policy_in_requests_csp_list(request, policy), request.remote_client_navigation() };
        return RequestViolation::LocalClient { request.client()->get<HTML::EnvironmentSettingsObjectAddress>() };
    }();

    return RequestViolation {
        .policy = move(policy),
        .effective_directive = effective_directive.release_value(),
        .resource = request.url(),
        .reported_to = move(reported_to),
    };
}

}
