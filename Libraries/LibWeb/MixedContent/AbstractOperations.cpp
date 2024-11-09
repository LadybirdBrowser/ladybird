/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MixedContent/AbstractOperations.h>
#include <LibWeb/SecureContexts/AbstractOperations.h>

namespace Web::MixedContent {

// https://w3c.github.io/webappsec-mixed-content/#upgrade-algorithm
void upgrade_a_mixed_content_request_to_a_potentially_trustworthy_url_if_appropriate(Fetch::Infrastructure::Request& request)
{
    // 1. If one or more of the following conditions is met, return without modifying request:
    if (
        // 1. request’s URL is a potentially trustworthy URL.
        SecureContexts::is_url_potentially_trustworthy(request.url()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy

        // 2. request’s URL’s host is an IP address.
        || request.url().host().has<URL::IPv4Address>() || request.url().host().has<URL::IPv6Address>()

        // 3. § 4.3 Does settings prohibit mixed security contexts? returns "Does Not Restrict Mixed Security Contents" when applied to request’s client.
        || does_settings_prohibit_mixed_security_contexts(request.client()) == ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts

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
    if (request.url().scheme() == "http")
        request.url().set_scheme("https"_string);
}

// https://w3c.github.io/webappsec-mixed-content/#categorize-settings-object
ProhibitsMixedSecurityContexts does_settings_prohibit_mixed_security_contexts(JS::GCPtr<HTML::EnvironmentSettingsObject> settings)
{
    // 1. If settings’ origin is a potentially trustworthy origin, then return "Prohibits Mixed Security Contexts".
    if (SecureContexts::is_origin_potentially_trustworthy(settings->origin()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy)
        return ProhibitsMixedSecurityContexts::ProhibitsMixedSecurityContexts;

    // 2. If settings’ global object is a window, then:
    if (is<HTML::Window>(settings->global_object())) {
        // 1. Set document to settings’ global object's associated Document.
        auto document = verify_cast<HTML::Window>(settings->global_object()).document();

        // 2. For each navigable navigable in document’s ancestor navigables:
        for (auto const& navigable : document->ancestor_navigables()) {
            // 1. If navigable’s active document's origin is a potentially trustworthy origin, then return "Prohibits Mixed Security Contexts".
            if (SecureContexts::is_origin_potentially_trustworthy(navigable->active_document()->origin()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy)
                return ProhibitsMixedSecurityContexts::ProhibitsMixedSecurityContexts;
        }
    }

    // 3. Return "Does Not Restrict Mixed Security Contexts".
    return ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts;
}

// https://w3c.github.io/webappsec-mixed-content/#should-block-fetch
Fetch::Infrastructure::RequestOrResponseBlocking should_fetching_request_be_blocked_as_mixed_content(Fetch::Infrastructure::Request& request)
{
    // 1. Return allowed if one or more of the following conditions are met:
    if (
        // 1. § 4.3 Does settings prohibit mixed security contexts? returns "Does Not Restrict Mixed Security Contexts" when applied to request’s client.
        does_settings_prohibit_mixed_security_contexts(request.client()) == ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts

        // 2. request’s URL is a potentially trustworthy URL.
        || SecureContexts::is_url_potentially_trustworthy(request.url()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy

        // FIXME: 3. The user agent has been instructed to allow mixed content, as described in § 7.2 User Controls).
        || false

        // 4. request’s destination is "document", and request’s target browsing context has no parent browsing context.
        // TODO: "parent browsing context" doesn't exist anymore and is a spec bug, seems like it should be `is_top_level`
        || (request.destination() == Fetch::Infrastructure::Request::Destination::Document && request.client()->target_browsing_context && request.client()->target_browsing_context->is_top_level())) {
        return Fetch::Infrastructure::RequestOrResponseBlocking::Allowed;
    }

    // 2. Return blocked.
    dbgln("MixedContent: Blocked '{}' (request)", MUST(request.url().to_string()));
    return Fetch::Infrastructure::RequestOrResponseBlocking::Blocked;
}

// https://w3c.github.io/webappsec-mixed-content/#should-block-response
Web::Fetch::Infrastructure::RequestOrResponseBlocking should_response_to_request_be_blocked_as_mixed_content(Fetch::Infrastructure::Request& request, JS::NonnullGCPtr<Fetch::Infrastructure::Response>& response)
{
    // 1. Return allowed if one or more of the following conditions are met:
    if (
        // 1. § 4.3 Does settings prohibit mixed security contexts? returns Does Not Restrict Mixed Content when applied to request’s client.
        does_settings_prohibit_mixed_security_contexts(request.client()) == ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts

        // 2. response’s url is a potentially trustworthy URL.
        || (response->url().has_value() && SecureContexts::is_url_potentially_trustworthy(response->url().value()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy)

        // FIXME: 3. The user agent has been instructed to allow mixed content, as described in § 7.2 User Controls).
        || false

        // 4. request’s destination is "document", and request’s target browsing context has no parent browsing context.
        // TODO: "parent browsing context" doesn't exist anymore and is a spec bug, seems like it should be `is_top_level`
        || (request.destination() == Fetch::Infrastructure::Request::Destination::Document && request.client()->target_browsing_context && request.client()->target_browsing_context->is_top_level())) {
        return Fetch::Infrastructure::RequestOrResponseBlocking::Allowed;
    }

    // 2. Return blocked.
    dbgln("MixedContent: Blocked '{}' (response to request)", MUST(request.url().to_string()));
    return Fetch::Infrastructure::RequestOrResponseBlocking::Blocked;
}

}
