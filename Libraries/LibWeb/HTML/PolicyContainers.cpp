/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/SerializedPolicyContainer.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PolicyContainer);

PolicyContainer::PolicyContainer(JS::Realm&)
{
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

GC::Ref<PolicyContainer> create_a_policy_container_from_serialized_policy_container(JS::Realm& realm, SerializedPolicyContainer const& serialized_policy_container)
{
    GC::Ref<PolicyContainer> result = realm.create<PolicyContainer>(realm);
    result->embedder_policy = serialized_policy_container.embedder_policy;
    result->referrer_policy = serialized_policy_container.referrer_policy;
    return result;
}

// https://html.spec.whatwg.org/multipage/browsers.html#clone-a-policy-container
GC::Ref<PolicyContainer> PolicyContainer::clone(JS::Realm& realm) const
{
    // 1. Let clone be a new policy container.
    auto clone = realm.create<PolicyContainer>(realm);

    // FIXME: 2. For each policy in policyContainer's CSP list, append a copy of policy into clone's CSP list.

    // 3. Set clone's embedder policy to a copy of policyContainer's embedder policy.
    // NOTE: This is a C++ copy.
    clone->embedder_policy = embedder_policy;

    // 4. Set clone's referrer policy to policyContainer's referrer policy.
    clone->referrer_policy = referrer_policy;

    // 5. Return clone.
    return clone;
}

SerializedPolicyContainer PolicyContainer::serialize() const
{
    return SerializedPolicyContainer {
        .embedder_policy = embedder_policy,
        .referrer_policy = referrer_policy,
    };
}

}
