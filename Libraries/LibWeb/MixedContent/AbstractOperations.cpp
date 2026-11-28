/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/MixedContent/AbstractOperations.h>
#include <LibWebCommon/MixedContent/ProhibitsMixedSecurityContexts.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>

namespace Web::MixedContent {

// NB: The client's snapshot holds the result of § 4.3 Does settings prohibit mixed security contexts? for it.
static ProhibitsMixedSecurityContexts does_client_prohibit_mixed_security_contexts(Fetch::Infrastructure::Request const& request)
{
    // AD-HOC: Fetch requests initiated by the browser UI have no client. Treat that as not restricting mixed content.
    if (auto const& client = request.client_snapshot())
        return client->prohibits_mixed_security_contexts;
    return ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts;
}

// https://w3c.github.io/webappsec-mixed-content/#upgrade-algorithm
void upgrade_a_mixed_content_request_to_a_potentially_trustworthy_url_if_appropriate(Fetch::Infrastructure::Request& request)
{
    // INTEROP: Main fetch runs this algorithm again for every redirect. We upgrade request's current URL rather than
    //          its URL, so that a redirect to an insecure URL is upgraded as well.
    auto& url = request.current_url();

    // 1. If one or more of the following conditions is met, return without modifying request:
    if (
        // 1. request’s URL is a potentially trustworthy URL.
        SecureContexts::is_url_potentially_trustworthy(url) == SecureContexts::Trustworthiness::PotentiallyTrustworthy

        // 2. request’s URL’s host is an IP address.
        || (url.host().has_value() && (url.host()->has<IPv4Address>() || url.host()->has<IPv6Address>()))

        // 3. § 4.3 Does settings prohibit mixed security contexts? returns "Does Not Restrict Mixed Security Contents" when applied to request’s client.
        || does_client_prohibit_mixed_security_contexts(request) == ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts

        // 4. request’s destination is not "image", "audio", or "video".
        || (request.destination() != Fetch::Infrastructure::Request::Destination::Image
            && request.destination() != Fetch::Infrastructure::Request::Destination::Audio
            && request.destination() != Fetch::Infrastructure::Request::Destination::Video)

        // 5. request’s destination is "image" and request’s initiator is "imageset".
        || (request.destination() == Fetch::Infrastructure::Request::Destination::Image
            && request.initiator() == Fetch::Infrastructure::Request::Initiator::ImageSet)) {
        return;
    }

    // 2. If request’s URL’s scheme is http, set request’s URL’s scheme to https, and return.
    if (url.scheme() == "http") {
        url.set_scheme("https"sv);

        // NB: Unlike the URL parser, setting the scheme does not remove a port that is the new scheme's default port.
        if (url.port() == 443)
            url.set_port({});
    }
}

// NB: A navigation request's target browsing context is the one its reserved client names.
static bool target_browsing_context_is_top_level(Fetch::Infrastructure::Request const& request)
{
    using Context = Fetch::Infrastructure::ReservedClientContext;
    auto const& reserved_client = request.reserved_client_snapshot();
    auto const* environment = reserved_client ? reserved_client->kind.get_pointer<Context::Environment>() : nullptr;
    return environment && environment->target_browsing_context == Context::TargetBrowsingContext::TopLevel;
}

static bool is_blob_url(URL::URL const& url)
{
    return url.scheme() == "blob"sv;
}

// https://w3c.github.io/webappsec-mixed-content/#should-block-fetch
Fetch::Infrastructure::RequestOrResponseBlocking should_fetching_request_be_blocked_as_mixed_content(Fetch::Infrastructure::Request& request)
{
    // 1. Return allowed if one or more of the following conditions are met:
    if (
        // 1. § 4.3 Does settings prohibit mixed security contexts? returns "Does Not Restrict Mixed Security Contexts" when applied to request’s client.
        does_client_prohibit_mixed_security_contexts(request) == ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts

        // 2. request’s URL is a potentially trustworthy URL.
        // INTEROP: Main fetch runs this algorithm again for every redirect. We check request's current URL rather than
        //          its URL, so that a redirect to an insecure URL is blocked as well.
        || SecureContexts::is_url_potentially_trustworthy(request.current_url()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy

        // INTEROP: A blob URL is resolved from the blob URL store and never reaches the network. A blob URL created in
        //          a context with an opaque origin, such as a sandboxed iframe, is not potentially trustworthy, but
        //          is still never treated as mixed content.
        || is_blob_url(request.current_url())

        // FIXME: 3. The user agent has been instructed to allow mixed content, as described in § 7.2 User Controls).
        || false

        // 4. request’s destination is "document", and request’s target browsing context has no parent browsing context.
        // TODO: "parent browsing context" doesn't exist anymore and is a spec bug, seems like it should be `is_top_level`
        || (request.destination() == Fetch::Infrastructure::Request::Destination::Document && target_browsing_context_is_top_level(request))) {
        return Fetch::Infrastructure::RequestOrResponseBlocking::Allowed;
    }

    // 2. Return blocked.
    dbgln("MixedContent: Blocked '{}' (request)", request.current_url().to_string());
    return Fetch::Infrastructure::RequestOrResponseBlocking::Blocked;
}

// https://w3c.github.io/webappsec-mixed-content/#should-block-response
Web::Fetch::Infrastructure::RequestOrResponseBlocking should_response_to_request_be_blocked_as_mixed_content(Fetch::Infrastructure::Request& request, Fetch::Infrastructure::Response const& response)
{
    // 1. Return allowed if one or more of the following conditions are met:
    if (
        // 1. § 4.3 Does settings prohibit mixed security contexts? returns Does Not Restrict Mixed Content when applied to request’s client.
        does_client_prohibit_mixed_security_contexts(request) == ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts

        // 2. response’s url is a potentially trustworthy URL.
        || (response.url().has_value() && SecureContexts::is_url_potentially_trustworthy(response.url().value()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy)

        // INTEROP: A blob URL is never treated as mixed content. See should_fetching_request_be_blocked_as_mixed_content().
        || (response.url().has_value() && is_blob_url(response.url().value()))

        // FIXME: 3. The user agent has been instructed to allow mixed content, as described in § 7.2 User Controls).
        || false

        // 4. request’s destination is "document", and request’s target browsing context has no parent browsing context.
        // TODO: "parent browsing context" doesn't exist anymore and is a spec bug, seems like it should be `is_top_level`
        || (request.destination() == Fetch::Infrastructure::Request::Destination::Document && target_browsing_context_is_top_level(request))) {
        return Fetch::Infrastructure::RequestOrResponseBlocking::Allowed;
    }

    // 2. Return blocked.
    dbgln("MixedContent: Blocked '{}' (response to request)", request.url().to_string());
    return Fetch::Infrastructure::RequestOrResponseBlocking::Blocked;
}

}
