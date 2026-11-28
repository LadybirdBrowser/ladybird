/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>
#include <LibWebCommon/HTML/SerializedPolicyContainer.h>

namespace Web::HTML {

NonnullRefPtr<PolicyContainer> PolicyContainer::create()
{
    return adopt_ref(*new PolicyContainer);
}

// https://html.spec.whatwg.org/multipage/browsers.html#requires-storing-the-policy-container-in-history
bool url_requires_storing_the_policy_container_in_history(URL::URL const& url)
{
    // 1. If url's scheme is "blob", then return false.
    if (url.scheme() == "blob"sv)
        return false;

    // 2. If url is local, then return true.
    // 3. Return false.
    return Fetch::Infrastructure::is_local_url(url);
}

NonnullRefPtr<PolicyContainer> create_a_policy_container_from_serialized_policy_container(SerializedPolicyContainer const& serialized_policy_container)
{
    auto result = PolicyContainer::create();
    result->csp_list = ContentSecurityPolicy::PolicyList { serialized_policy_container.csp_list };
    result->embedder_policy = serialized_policy_container.embedder_policy;
    result->referrer_policy = serialized_policy_container.referrer_policy;
    result->force_load_at_top = serialized_policy_container.force_load_at_top;
    // NB: A serialized policy container holds its effective insecure requests policy and set, which the result inherits.
    result->inherit_insecure_requests_policy(serialized_policy_container.insecure_requests_policy, serialized_policy_container.upgrade_insecure_navigations_set);
    return result;
}

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#delivery
// AD-HOC: Enforcing the upgrade-insecure-requests directive sets the protected resource's settings' insecure requests
//         policy to "Upgrade", and inserts a tuple of the protected resource's URL's host and port into settings'
//         upgrade insecure navigations set. We instead derive both from the policies enforced in the policy
//         container's CSP list, whether they were delivered with a response or by a meta element, and use each
//         policy's self-origin as the protected resource's URL. Monitoring the directive has no effect, so only
//         enforced policies count. The stored values hold what the policy container inherited.
template<typename Callback>
static void for_each_enforced_policy_with_upgrade_insecure_requests(PolicyContainer const& policy_container, Callback callback)
{
    for (auto const& policy : policy_container.csp_list.policies()) {
        if (policy->disposition() != ContentSecurityPolicy::Policy::Disposition::Enforce)
            continue;
        if (!policy->contains_directive_with_name(ContentSecurityPolicy::Directives::Names::UpgradeInsecureRequests))
            continue;
        if (callback(*policy) == IterationDecision::Break)
            return;
    }
}

UpgradeInsecureRequests::InsecureRequestsPolicy PolicyContainer::insecure_requests_policy() const
{
    auto result = m_insecure_requests_policy;
    if (result == UpgradeInsecureRequests::InsecureRequestsPolicy::Upgrade)
        return result;
    for_each_enforced_policy_with_upgrade_insecure_requests(*this, [&](auto const&) {
        result = UpgradeInsecureRequests::InsecureRequestsPolicy::Upgrade;
        return IterationDecision::Break;
    });
    return result;
}

UpgradeInsecureRequests::UpgradeInsecureNavigationsSet PolicyContainer::upgrade_insecure_navigations_set() const
{
    auto result = m_upgrade_insecure_navigations_set;
    for_each_enforced_policy_with_upgrade_insecure_requests(*this, [&](ContentSecurityPolicy::Policy const& policy) {
        auto const& self_origin = policy.self_origin();
        if (!self_origin.is_opaque())
            result.insert({ self_origin.host(), self_origin.port() });
        return IterationDecision::Continue;
    });
    return result;
}

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#nesting
// NB: These are the steps for a new Document. Callers pass the policy and set this policy container inherits, from its
//     owner's context or from a serialized policy container.
void PolicyContainer::inherit_insecure_requests_policy(UpgradeInsecureRequests::InsecureRequestsPolicy policy, UpgradeInsecureRequests::UpgradeInsecureNavigationsSet const& upgrade_insecure_navigations_set)
{
    // 1. If context's insecure requests policy is Upgrade, then:
    if (policy != UpgradeInsecureRequests::InsecureRequestsPolicy::Upgrade)
        return;

    // 1. Let settings be document's incumbent settings object.
    // AD-HOC: Document's own policy container takes the policy, rather than its incumbent settings object. See
    //         https://github.com/w3c/webappsec-upgrade-insecure-requests/issues/17.
    // 2. Set settings' insecure requests policy to Upgrade.
    m_insecure_requests_policy = UpgradeInsecureRequests::InsecureRequestsPolicy::Upgrade;

    // 3. For each value in context's upgrade insecure navigations set, add value to settings's upgrade insecure
    //    navigations set.
    for (auto const& value : upgrade_insecure_navigations_set.tuples())
        m_upgrade_insecure_navigations_set.insert(value);
}

// https://html.spec.whatwg.org/multipage/browsers.html#clone-a-policy-container
NonnullRefPtr<PolicyContainer> PolicyContainer::clone() const
{
    // 1. Let clone be a new policy container.
    auto clone = create();

    // 2. For each policy in policyContainer's CSP list, append a copy of policy into clone's CSP list.
    // NB: A policy in a CSP list is immutable and shared by its clones.
    clone->csp_list = csp_list;

    // 3. Set clone's embedder policy to a copy of policyContainer's embedder policy.
    // NOTE: This is a C++ copy.
    clone->embedder_policy = embedder_policy;

    // 4. Set clone's referrer policy to policyContainer's referrer policy.
    clone->referrer_policy = referrer_policy;

    // 5. Set clone's integrity policy to a copy of policyContainer's integrity policy.
    clone->integrity_policy = integrity_policy;

    // https://wicg.github.io/scroll-to-text-fragment/#document-policy-integration
    // This specification defines a configuration point in Document Policy with name
    // "force-load-at-top".
    clone->force_load_at_top = force_load_at_top;

    // AD-HOC: The clone holds the insecure requests policy and upgrade insecure navigations set that policyContainer
    //         inherited, as policy containers hold them instead of settings objects.
    clone->m_insecure_requests_policy = m_insecure_requests_policy;
    clone->m_upgrade_insecure_navigations_set = m_upgrade_insecure_navigations_set;

    // 6. Return clone.
    return clone;
}

SerializedPolicyContainer PolicyContainer::serialize() const
{
    return SerializedPolicyContainer {
        .csp_list = csp_list.serialize(),
        .embedder_policy = embedder_policy,
        .referrer_policy = referrer_policy,
        .force_load_at_top = force_load_at_top,
        .insecure_requests_policy = insecure_requests_policy(),
        .upgrade_insecure_navigations_set = upgrade_insecure_navigations_set(),
    };
}

}
