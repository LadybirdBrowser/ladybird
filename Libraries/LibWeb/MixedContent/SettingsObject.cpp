/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MixedContent/SettingsObject.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>

namespace Web::MixedContent {

// https://w3c.github.io/webappsec-mixed-content/#categorize-settings-object
ProhibitsMixedSecurityContexts does_settings_prohibit_mixed_security_contexts(HTML::EnvironmentSettingsObject& settings)
{
    // 1. If settings’ origin is a potentially trustworthy origin, then return "Prohibits Mixed Security Contexts".
    if (SecureContexts::is_origin_potentially_trustworthy(settings.origin()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy)
        return ProhibitsMixedSecurityContexts::ProhibitsMixedSecurityContexts;

    // 2. If settings’ global object is a window, then:
    if (auto* window = HTML::window_from_global_object(settings.global_object())) {
        // 1. Set document to settings’ global object's associated Document.
        auto document = window->document();

        // 2. For each navigable navigable in document’s ancestor navigables:
        for (auto const& navigable : document->ancestor_navigables()) {
            // 1. If navigable’s active document's origin is a potentially trustworthy origin, then return "Prohibits Mixed Security Contexts".
            if (SecureContexts::is_origin_potentially_trustworthy(*navigable->active_document_origin()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy)
                return ProhibitsMixedSecurityContexts::ProhibitsMixedSecurityContexts;
        }
    }

    // 3. Return "Does Not Restrict Mixed Security Contexts".
    return ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts;
}

}
