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
#include <LibWebCommon/HTML/EmbedderPolicy.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

// https://w3c.github.io/webappsec-subresource-integrity/#integrity-policy
struct IntegrityPolicy {
    Vector<Utf16String> sources;
    Vector<Fetch::Infrastructure::Request::Destination> blocked_destinations;
    Vector<Utf16String> endpoints;

    bool is_empty() const { return sources.is_empty() && blocked_destinations.is_empty() && endpoints.is_empty(); }
};

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
    IntegrityPolicy integrity_policy {};

    // https://html.spec.whatwg.org/multipage/browsers.html#policy-container-report-only-integrity-policy
    // A report only integrity policy, which is an integrity policy, initially a new integrity policy.
    IntegrityPolicy report_only_integrity_policy {};

    // https://wicg.github.io/scroll-to-text-fragment/#document-policy-integration
    // This specification defines a configuration point in Document Policy with name
    // "force-load-at-top". Its type is boolean with default value false.
    bool force_load_at_top { false };

    [[nodiscard]] NonnullRefPtr<PolicyContainer> clone() const;
    [[nodiscard]] SerializedPolicyContainer serialize() const;

private:
    PolicyContainer() = default;
};

// https://html.spec.whatwg.org/multipage/browsers.html#requires-storing-the-policy-container-in-history
[[nodiscard]] bool url_requires_storing_the_policy_container_in_history(URL::URL const& url);

// https://html.spec.whatwg.org/multipage/browsers.html#creating-a-policy-container-from-a-fetch-response
[[nodiscard]] NonnullRefPtr<PolicyContainer> create_a_policy_container_from_a_fetch_response(GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ptr<Environment> environment);

[[nodiscard]] NonnullRefPtr<PolicyContainer> create_a_policy_container_from_serialized_policy_container(SerializedPolicyContainer const&);

}
