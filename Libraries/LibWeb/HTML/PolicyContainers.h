/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibURL/Forward.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>
#include <LibWeb/SRI/IntegrityPolicy.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>
#include <LibWebCommon/UpgradeInsecureRequests/InsecureRequestsPolicy.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/origin.html#policy-container
// A policy container is a struct containing policies that apply to a Document, a WorkerGlobalScope, or a WorkletGlobalScope. It has the following items:
struct PolicyContainer final : public AtomicRefCounted<PolicyContainer> {
    [[nodiscard]] static NonnullRefPtr<PolicyContainer> create();

    // https://html.spec.whatwg.org/multipage/origin.html#policy-container-csp-list
    // A CSP list, which is a CSP list. It is initially empty.
    ContentSecurityPolicy::PolicyList csp_list;

    // https://html.spec.whatwg.org/multipage/origin.html#policy-container-embedder-policy
    // An embedder policy, which is an embedder policy. It is initially a new embedder policy.
    EmbedderPolicy embedder_policy {};

    // https://html.spec.whatwg.org/multipage/origin.html#policy-container-referrer-policy
    // A referrer policy, which is a referrer policy. It is initially the default referrer policy.
    ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::DEFAULT_REFERRER_POLICY };

    // https://html.spec.whatwg.org/multipage/browsers.html#policy-container-integrity-policy
    // An integrity policy, which is an integrity policy, initially a new integrity policy.
    SRI::IntegrityPolicy integrity_policy {};

    // https://html.spec.whatwg.org/multipage/browsers.html#policy-container-report-only-integrity-policy
    // A report only integrity policy, which is an integrity policy, initially a new integrity policy.
    SRI::IntegrityPolicy report_only_integrity_policy {};

    // https://wicg.github.io/scroll-to-text-fragment/#document-policy-integration
    // This specification defines a configuration point in Document Policy with name
    // "force-load-at-top". Its type is boolean with default value false.
    bool force_load_at_top { false };

    // https://w3c.github.io/webappsec-upgrade-insecure-requests/#insecure-requests-policy
    // https://w3c.github.io/webappsec-upgrade-insecure-requests/#upgrade-insecure-navigations-set
    // AD-HOC: Upgrade Insecure Requests defines these on settings objects and browsing contexts. A policy container
    //         holds them instead, so a request's clone of its client's policy container holds them for the whole fetch.
    //         See https://github.com/w3c/webappsec-upgrade-insecure-requests/issues/5.
    [[nodiscard]] UpgradeInsecureRequests::InsecureRequestsPolicy insecure_requests_policy() const;
    [[nodiscard]] UpgradeInsecureRequests::UpgradeInsecureNavigationsSet upgrade_insecure_navigations_set() const;
    void inherit_insecure_requests_policy(UpgradeInsecureRequests::InsecureRequestsPolicy, UpgradeInsecureRequests::UpgradeInsecureNavigationsSet const&);

    [[nodiscard]] NonnullRefPtr<PolicyContainer> clone() const;
    [[nodiscard]] SerializedPolicyContainer serialize() const;

private:
    PolicyContainer() = default;

    // https://w3c.github.io/webappsec-upgrade-insecure-requests/#insecure-requests-policy
    // It is set to Do Not Upgrade unless otherwise specified.
    // NB: These hold what the policy container inherited. Its CSP list holds its own policies.
    UpgradeInsecureRequests::InsecureRequestsPolicy m_insecure_requests_policy { UpgradeInsecureRequests::InsecureRequestsPolicy::DoNotUpgrade };

    // https://w3c.github.io/webappsec-upgrade-insecure-requests/#upgrade-insecure-navigations-set
    // Its value is the empty set unless otherwise specified.
    UpgradeInsecureRequests::UpgradeInsecureNavigationsSet m_upgrade_insecure_navigations_set;
};

// https://html.spec.whatwg.org/multipage/browsers.html#requires-storing-the-policy-container-in-history
[[nodiscard]] bool url_requires_storing_the_policy_container_in_history(URL::URL const& url);

// https://html.spec.whatwg.org/multipage/browsers.html#creating-a-policy-container-from-a-fetch-response
[[nodiscard]] NonnullRefPtr<PolicyContainer> create_a_policy_container_from_a_fetch_response(GC::Ref<Fetch::Infrastructure::ResponseView const> response, GC::Ptr<Environment> environment);

[[nodiscard]] NonnullRefPtr<PolicyContainer> create_a_policy_container_from_serialized_policy_container(SerializedPolicyContainer const&);

}
