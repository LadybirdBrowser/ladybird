/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/ReferrerPolicy/AbstractOperations.h>
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

// https://html.spec.whatwg.org/multipage/browsers.html#creating-a-policy-container-from-a-fetch-response
NonnullRefPtr<PolicyContainer> create_a_policy_container_from_a_fetch_response(GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ptr<Environment> environment)
{
    // 1. If response's URL's scheme is "blob", then return a clone of response's URL's blob URL entry's environment's
    //    policy container.
    // NB: The entry is in this process's store if this process added it, and otherwise in the user agent's, which has
    //     the environment's policy container serialized. An entry that is gone has no policy container to clone.
    if (auto const& response_url = response->url(); response_url.has_value() && response_url->scheme() == "blob"sv) {
        if (auto entry = FileAPI::local_blob_url_entry(*response_url); entry.has_value())
            return entry->environment->policy_container()->clone();
        if (auto* settings = as_if<EnvironmentSettingsObject>(environment.ptr())) {
            if (auto entry = FileAPI::blob_url_entry_in_the_user_agent_store(Bindings::principal_host_defined_page(settings->realm()), *response_url); entry.has_value())
                return create_a_policy_container_from_serialized_policy_container(entry->environment_policy_container);
        }
    }

    // 2. Let result be a new policy container.
    auto result = PolicyContainer::create();

    // 3. Set result's CSP list to the result of parsing a response's Content Security Policies given response.
    result->csp_list = ContentSecurityPolicy::Policy::parse_a_responses_content_security_policies(response);

    // 4. If environment is non-null, then set result's embedder policy to the result of obtaining an embedder policy
    //    given response and environment. Otherwise, set it to "unsafe-none".
    if (environment)
        result->embedder_policy = obtain_an_embedder_policy(*response->header_list(), is_non_secure_context(*environment));

    // 5. Set result's referrer policy to the result of parsing the `Referrer-Policy` header given response.
    //    [REFERRERPOLICY]
    auto parsed_referrer_policy = ReferrerPolicy::parse_a_referrer_policy_from_a_referrer_policy_header(response);
    if (parsed_referrer_policy != ReferrerPolicy::ReferrerPolicy::EmptyString)
        result->referrer_policy = parsed_referrer_policy;

    // https://wicg.github.io/scroll-to-text-fragment/#document-policy-integration
    // This specification defines a configuration point in Document Policy with name
    // "force-load-at-top". Its type is boolean with default value false.
    if (auto document_policy = response->header_list()->get_structured_field_value<HTTP::StructuredFields::Dictionary>("Document-Policy"sv); document_policy.has_value()) {
        if (auto force_load_at_top = document_policy->get("force-load-at-top"sv); force_load_at_top.has_value()) {
            if (auto const* item = force_load_at_top->get_pointer<HTTP::StructuredFields::Item>()) {
                if (auto const* value = item->bare_item.get_pointer<bool>())
                    result->force_load_at_top = *value;
            }
        }
    }

    // FIXME: 6. Parse Integrity-Policy headers with response and result.

    // 7. Return result.
    return result;
}

NonnullRefPtr<PolicyContainer> create_a_policy_container_from_serialized_policy_container(SerializedPolicyContainer const& serialized_policy_container)
{
    auto result = PolicyContainer::create();
    result->csp_list = ContentSecurityPolicy::PolicyList { serialized_policy_container.csp_list };
    result->embedder_policy = serialized_policy_container.embedder_policy;
    result->referrer_policy = serialized_policy_container.referrer_policy;
    result->force_load_at_top = serialized_policy_container.force_load_at_top;
    return result;
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
    };
}

}
