/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/StructuredField.h>
#include <LibJS/Runtime/Realm.h>
#include <LibURL/URL.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/SerializedPolicyContainer.h>
#include <LibWeb/ReferrerPolicy/AbstractOperations.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PolicyContainer);

PolicyContainer::PolicyContainer(GC::Heap& heap)
    : csp_list(heap.allocate<ContentSecurityPolicy::PolicyList>())
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

// https://w3c.github.io/webappsec-subresource-integrity/#processing-an-integrity-policy
static IntegrityPolicy process_an_integrity_policy(AK::NonnullRefPtr<HTTP::HeaderList> const& headers, StringView header_name)
{
    // 1. Let integrityPolicy be a new integrity policy.
    IntegrityPolicy integrity_policy {};

    // 2. Let dictionary be the result of getting a structured field value from headers given headerName and "dictionary".
    auto parsed_dictionary = headers->get_a_structured_field_value(header_name, HTTP::StructuredFieldType::Dictionary);
    if (!parsed_dictionary.has_value()) {
        return integrity_policy;
    }
    auto dictionary = parsed_dictionary.value().get<HTTP::StructuredFieldDictionary>();

    // 3. If dictionary["sources"] does not exist or if its value contains "inline", append "inline" to integrityPolicy’s sources.
    auto sources = dictionary.members.get("sources"sv);
    if (!sources.has_value() || (sources->has<HTTP::StructuredFieldInnerList>() && any_of(sources->get<HTTP::StructuredFieldInnerList>().members, [](auto const& member) {
            return member.item.template has<HTTP::StructuredFieldToken>() && member.item.template get<HTTP::StructuredFieldToken>().value == "inline"sv;
        }))) {
        integrity_policy.sources.append("inline"_string);
    }

    // 4. If dictionary["blocked-destinations"] exists:
    auto blocked_destinations = dictionary.members.get("blocked-destinations"sv);
    if (blocked_destinations.has_value() && blocked_destinations->has<HTTP::StructuredFieldInnerList>()) {
        auto blocked_destinations_list = blocked_destinations->get<HTTP::StructuredFieldInnerList>();
        // 1. If its value contains "script", append "script" to integrityPolicy’s blocked destinations.
        if (any_of(blocked_destinations_list.members, [](auto const& member) {
                return member.item.template has<HTTP::StructuredFieldToken>() && member.item.template get<HTTP::StructuredFieldToken>().value == "script"sv;
            })) {
            integrity_policy.blocked_destinations.append(Fetch::Infrastructure::Request::Destination::Script);
        }

        // 2. If its value contains "style", append "style" to integrityPolicy’s blocked destinations.
        if (any_of(blocked_destinations_list.members, [](auto const& member) {
                return member.item.template has<HTTP::StructuredFieldToken>() && member.item.template get<HTTP::StructuredFieldToken>().value == "style"sv;
            })) {
            integrity_policy.blocked_destinations.append(Fetch::Infrastructure::Request::Destination::Style);
        }
    }

    // 5. If dictionary["endpoints"] exists:
    auto endpoints = dictionary.members.get("endpoints"sv);
    if (endpoints.has_value() && endpoints->has<HTTP::StructuredFieldInnerList>()) {
        // 1. Set integrityPolicy’s endpoints to dictionary['endpoints'].
        auto endpoints_list = endpoints->get<HTTP::StructuredFieldInnerList>();
        for (auto const& member : endpoints_list.members) {
            if (member.item.has<HTTP::StructuredFieldToken>()) {
                integrity_policy.endpoints.append(member.item.get<HTTP::StructuredFieldToken>().value);
            }
        }
    }

    // 6. Return integrityPolicy.
    return integrity_policy;
}

// https://w3c.github.io/webappsec-subresource-integrity/#parse-integrity-policy-headers-section
static void parse_integrity_policy_headers(GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ref<PolicyContainer> container)
{
    // 1. Let headers be response’s header list.
    auto const& headers = response->header_list();

    // 2. If headers contains integrity-policy, set container’s integrity policy be the result of running processing an integrity policy with the corresponding header value.
    if (headers->contains("integrity-policy"sv)) {
        container->integrity_policy = process_an_integrity_policy(headers, "integrity-policy"sv);
    }

    // 3. If headers contains integrity-policy-report-only, set container’s report only integrity policy be the result of running processing an integrity policy with the corresponding header value.
    if (headers->contains("integrity-policy-report-only"sv)) {
        container->report_only_integrity_policy = process_an_integrity_policy(headers, "integrity-policy-report-only"sv);
    }
}

// https://html.spec.whatwg.org/multipage/browsers.html#creating-a-policy-container-from-a-fetch-response
GC::Ref<PolicyContainer> create_a_policy_container_from_a_fetch_response(GC::Heap& heap, GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ptr<Environment>)
{
    // FIXME: 1. If response's URL's scheme is "blob", then return a clone of response's URL's blob URL entry's
    //           environment's policy container.

    // 2. Let result be a new policy container.
    GC::Ref<PolicyContainer> result = heap.allocate<PolicyContainer>(heap);

    // 3. Set result's CSP list to the result of parsing a response's Content Security Policies given response.
    result->csp_list = ContentSecurityPolicy::Policy::parse_a_responses_content_security_policies(heap, response);

    // FIXME: 4. If environment is non-null, then set result's embedder policy to the result of obtaining an embedder
    //           policy given response and environment. Otherwise, set it to "unsafe-none".

    // 5. Set result's referrer policy to the result of parsing the `Referrer-Policy` header given response.
    //    [REFERRERPOLICY]
    auto parsed_referrer_policy = ReferrerPolicy::parse_a_referrer_policy_from_a_referrer_policy_header(response);
    if (parsed_referrer_policy != ReferrerPolicy::ReferrerPolicy::EmptyString)
        result->referrer_policy = parsed_referrer_policy;

    // 6. Parse Integrity-Policy headers with response and result.
    parse_integrity_policy_headers(response, result);

    // 7. Return result.
    return result;
}

GC::Ref<PolicyContainer> create_a_policy_container_from_serialized_policy_container(GC::Heap& heap, SerializedPolicyContainer const& serialized_policy_container)
{
    GC::Ref<PolicyContainer> result = heap.allocate<PolicyContainer>(heap);
    result->csp_list = ContentSecurityPolicy::PolicyList::create(heap, serialized_policy_container.csp_list);
    result->embedder_policy = serialized_policy_container.embedder_policy;
    result->referrer_policy = serialized_policy_container.referrer_policy;
    return result;
}

// https://html.spec.whatwg.org/multipage/browsers.html#clone-a-policy-container
GC::Ref<PolicyContainer> PolicyContainer::clone(GC::Heap& heap) const
{
    // 1. Let clone be a new policy container.
    auto clone = heap.allocate<PolicyContainer>(heap);

    // 2. For each policy in policyContainer's CSP list, append a copy of policy into clone's CSP list.
    clone->csp_list = csp_list->clone(heap);

    // 3. Set clone's embedder policy to a copy of policyContainer's embedder policy.
    // NOTE: This is a C++ copy.
    clone->embedder_policy = embedder_policy;

    // 4. Set clone's referrer policy to policyContainer's referrer policy.
    clone->referrer_policy = referrer_policy;

    // 5. Set clone's integrity policy to a copy of policyContainer's integrity policy.
    clone->integrity_policy = integrity_policy;

    // 6. Return clone.
    return clone;
}

SerializedPolicyContainer PolicyContainer::serialize() const
{
    return SerializedPolicyContainer {
        .csp_list = csp_list->serialize(),
        .embedder_policy = embedder_policy,
        .referrer_policy = referrer_policy,
    };
}

void PolicyContainer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(csp_list);
}

}
