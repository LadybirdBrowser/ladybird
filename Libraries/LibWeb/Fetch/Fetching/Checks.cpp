/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Checks.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/EmbedderPolicy.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::Fetch::Fetching {

// https://fetch.spec.whatwg.org/#concept-cors-check
bool cors_check(Infrastructure::Request const& request, Infrastructure::Response const& response)
{
    // 1. Let origin be the result of getting `Access-Control-Allow-Origin` from response’s header list.
    auto origin = response.header_list()->get("Access-Control-Allow-Origin"sv);

    // 2. If origin is null, then return failure.
    // NOTE: Null is not `null`.
    if (!origin.has_value())
        return false;

    // 3. If request’s credentials mode is not "include" and origin is `*`, then return success.
    if (request.credentials_mode() != Infrastructure::Request::CredentialsMode::Include && *origin == "*"sv)
        return true;

    // 4. If the result of byte-serializing a request origin with request is not origin, then return failure.
    if (request.byte_serialize_origin() != *origin)
        return false;

    // 5. If request’s credentials mode is not "include", then return success.
    if (request.credentials_mode() != Infrastructure::Request::CredentialsMode::Include)
        return true;

    // 6. Let credentials be the result of getting `Access-Control-Allow-Credentials` from response’s header list.
    auto credentials = response.header_list()->get("Access-Control-Allow-Credentials"sv);

    // 7. If credentials is `true`, then return success.
    if (credentials == "true"sv)
        return true;

    // 8. Return failure.
    return false;
}

// https://fetch.spec.whatwg.org/#concept-tao-check
bool tao_check(Infrastructure::Request const& request, Infrastructure::Response const& response)
{
    // 1. If request’s timing allow failed flag is set, then return failure.
    if (request.timing_allow_failed())
        return false;

    // 2. Let values be the result of getting, decoding, and splitting `Timing-Allow-Origin` from response’s header list.
    auto values = response.header_list()->get_decode_and_split("Timing-Allow-Origin"sv);

    // 3. If values contains "*", then return success.
    if (values.has_value() && values->contains_slow("*"sv))
        return true;

    // 4. If values contains the result of serializing a request origin with request, then return success.
    if (values.has_value() && values->contains_slow(request.serialize_origin()))
        return true;

    // 5. If request’s mode is "navigate" and request’s current URL’s origin is not same origin with request’s origin, then return failure.
    // NOTE: This is necessary for navigations of a nested browsing context. There, request’s origin would be the
    //       container document’s origin and the TAO check would return failure. Since navigation timing never
    //       validates the results of the TAO check, the nested document would still have access to the full timing
    //       information, but the container document would not.
    if (request.mode() == Infrastructure::Request::Mode::Navigate
        && request.origin().has<URL::Origin>()
        && !request.current_url().origin().is_same_origin(request.origin().get<URL::Origin>())) {
        return false;
    }

    // 6. If request’s response tainting is "basic", then return success.
    if (request.response_tainting() == Infrastructure::Request::ResponseTainting::Basic)
        return true;

    // 7. Return failure.
    return false;
}

// https://fetch.spec.whatwg.org/#navigation-tao-check
bool navigation_tao_check(Infrastructure::Response const& response, URL::Origin const& destination_origin)
{
    // 1. For each taoValues of response's navigation timing allow values list:
    for (auto const& tao_values : response.navigation_timing_allow_values_list()) {
        // 1. If taoValues contains "*", then continue.
        if (tao_values.contains_slow("*"sv))
            continue;

        // 2. If taoValues contains destinationOrigin, serialized, then continue.
        if (tao_values.contains_slow(destination_origin.serialize()))
            continue;

        // 3. Return failure.
        return false;
    }

    // 2. Return success.
    return true;
}

// https://html.spec.whatwg.org/multipage/browsers.html#schemelessly-same-site
static bool is_schemelessly_same_site(URL::Origin const& a, URL::Origin const& b)
{
    // Two origins, A and B, are said to be schemelessly same site if the following algorithm returns true:
    // 1. If A and B are both tuple origins, then:
    if (!a.is_opaque() && !b.is_opaque()) {
        // 1. Let hostA be A's host, and let hostB be B's host.
        auto const& host_a = a.host();
        auto const& host_b = b.host();

        // 2. If hostA equals hostB and hostA's registrable domain is null, then return true.
        auto registrable_domain_a = host_a.registrable_domain();
        if (host_a == host_b && !registrable_domain_a.has_value())
            return true;

        // 3. If hostA's registrable domain equals hostB's registrable domain and hostA's registrable domain is non-null, then return true.
        if (registrable_domain_a.has_value() && registrable_domain_a == host_b.registrable_domain())
            return true;
    }

    // 2. Return false.
    return false;
}

// https://fetch.spec.whatwg.org/#cross-origin-resource-policy-internal-check
static bool cross_origin_resource_policy_internal_check(URL::Origin const& origin, HTML::EmbedderPolicyValue embedder_policy_value, Infrastructure::Response const& response, ForNavigation for_navigation)
{
    // 1. If forNavigation is true and embedderPolicyValue is "unsafe-none", then return allowed.
    if (for_navigation == ForNavigation::Yes && embedder_policy_value == HTML::EmbedderPolicyValue::UnsafeNone)
        return true;

    // 2. Let policy be the result of getting `Cross-Origin-Resource-Policy` from response's header list.
    auto policy = response.header_list()->get("Cross-Origin-Resource-Policy"sv);

    // 3. If policy is neither `same-origin`, `same-site`, nor `cross-origin`, then set policy to null.
    if (policy.has_value() && !policy->is_one_of("same-origin"sv, "same-site"sv, "cross-origin"sv))
        policy = {};

    // 4. If policy is null, then switch on embedderPolicyValue:
    if (!policy.has_value()) {
        switch (embedder_policy_value) {
        // -> "unsafe-none"
        case HTML::EmbedderPolicyValue::UnsafeNone:
            // Do nothing.
            break;
        // -> "credentialless"
        case HTML::EmbedderPolicyValue::Credentialless:
            // Set policy to `same-origin` if:
            // - response's request-includes-credentials is true, or
            // - forNavigation is true.
            if (response.request_includes_credentials() || for_navigation == ForNavigation::Yes)
                policy = "same-origin"sv;
            break;
        // -> "require-corp"
        case HTML::EmbedderPolicyValue::RequireCorp:
            // Set policy to `same-origin`.
            policy = "same-origin"sv;
            break;
        }
    }

    // 5. Switch on policy:
    // -> null
    // -> `cross-origin`
    if (!policy.has_value() || *policy == "cross-origin"sv) {
        // Return allowed.
        return true;
    }

    auto response_url = response.url();
    if (!response_url.has_value())
        return true;
    auto response_origin = response_url->origin();

    // -> `same-origin`
    if (*policy == "same-origin"sv) {
        // If origin is same origin with response's URL's origin, then return allowed.
        // Otherwise, return blocked.
        return origin.is_same_origin(response_origin);
    }

    // -> `same-site`
    // If all of the following are true
    // - origin is schemelessly same site with response's URL's origin
    // - origin's scheme is "https" or response's HTTPS state is "none"
    // then return allowed.
    // Otherwise, return blocked.
    // NOTE: We don't track the HTTPS state of responses, so the scheme of the response's URL stands in for it.
    return is_schemelessly_same_site(origin, response_origin)
        && (origin.scheme() == "https"sv || response_url->scheme() != "https"sv);
}

// https://fetch.spec.whatwg.org/#cross-origin-resource-policy-check
bool cross_origin_resource_policy_check(URL::Origin const& origin, GC::Ptr<HTML::EnvironmentSettingsObject const> environment, [[maybe_unused]] Optional<Infrastructure::Request::Destination> destination, Infrastructure::Response const& response, ForNavigation for_navigation)
{
    // 1. Let embedderPolicy be "unsafe-none".
    HTML::EmbedderPolicy embedder_policy;

    // 2. If environment is non-null, then set embedderPolicy to environment's policy container's embedder policy.
    if (environment)
        embedder_policy = environment->policy_container()->embedder_policy;

    // 3. If the cross-origin resource policy internal check with origin, embedderPolicy's value, response, and
    //    forNavigation returns blocked, then return blocked.
    if (!cross_origin_resource_policy_internal_check(origin, embedder_policy.value, response, for_navigation))
        return false;

    // FIXME: 4. If the cross-origin resource policy internal check with origin, embedderPolicy's report-only value, response,
    //           and forNavigation returns blocked, then queue a cross-origin embedder policy CORP violation report with
    //           response, environment, embedderPolicy's report only reporting endpoint, destination, and true.
    // FIXME: Queue the violation report.

    // 5. Return allowed.
    return true;
}

}
