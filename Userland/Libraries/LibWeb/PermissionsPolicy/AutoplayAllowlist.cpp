/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/PermissionsPolicy/AutoplayAllowlist.h>

// FIXME: This is an ad-hoc implementation of the "autoplay" policy-controlled feature:
// https://w3c.github.io/webappsec-permissions-policy/#policy-controlled-feature

namespace Web::PermissionsPolicy {

AutoplayAllowlist& AutoplayAllowlist::the()
{
    static AutoplayAllowlist filter;
    return filter;
}

AutoplayAllowlist::AutoplayAllowlist() = default;
AutoplayAllowlist::~AutoplayAllowlist() = default;

// https://w3c.github.io/webappsec-permissions-policy/#is-feature-enabled
Decision AutoplayAllowlist::is_allowed_for_origin(DOM::Document const& document, URL::Origin const& origin) const
{
    // FIXME: 1. Let policy be document’s Permissions Policy
    // FIXME: 2. If policy’s inherited policy for feature is Disabled, return "Disabled".

    // 3. If feature is present in policy’s declared policy:
    if (m_allowlist.has_value()) {
        // 1. If the allowlist for feature in policy’s declared policy matches origin, then return "Enabled".
        // 2. Otherwise return "Disabled".
        return m_allowlist->visit(
            [](Global) {
                return Decision::Enabled;
            },
            [&](auto const& patterns) {
                for (auto const& pattern : patterns) {
                    if (pattern.is_same_origin_domain(origin))
                        return Decision::Enabled;
                }

                return Decision::Disabled;
            });
    }

    // 4. If feature’s default allowlist is *, return "Enabled".
    // 5. If feature’s default allowlist is 'self', and origin is same origin with document’s origin, return "Enabled".
    // NOTE: The "autoplay" feature's default allowlist is 'self'.
    //       https://html.spec.whatwg.org/multipage/infrastructure.html#autoplay-feature
    if (origin.is_same_origin(document.origin()))
        return Decision::Enabled;

    // 6. Return "Disabled".
    return Decision::Disabled;
}

void AutoplayAllowlist::enable_globally()
{
    m_allowlist = Global {};
}

ErrorOr<void> AutoplayAllowlist::enable_for_origins(ReadonlySpan<String> origins)
{
    m_allowlist = Patterns {};

    auto& allowlist = m_allowlist->get<Patterns>();
    TRY(allowlist.try_ensure_capacity(origins.size()));

    for (auto const& origin : origins) {
        URL::URL url { origin };

        if (!url.is_valid())
            url = TRY(String::formatted("https://{}", origin));
        if (!url.is_valid()) {
            dbgln("Invalid origin for autoplay allowlist: {}", origin);
            continue;
        }

        TRY(allowlist.try_append(url.origin()));
    }

    return {};
}

}
