/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/WebRTCIntegration.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#webrtc-pre-connect
static Directive::Result webrtc_directive_pre_connect_check(Directive const& directive, Policy const&)
{
    // 1. If this directive’s value contains a single item which is an ASCII case-insensitive match for the string
    //    "'allow'", return "Allowed".
    if (directive.value().size() == 1 && directive.value().first().equals_ignoring_ascii_case("'allow'"sv))
        return Directive::Result::Allowed;

    // 2. Return "Blocked".
    return Directive::Result::Blocked;
}

Directive::Result webrtc_pre_connect_check(Directive const& directive, Policy const& policy)
{
    if (directive.kind() == Directive::Kind::WebRTC)
        return webrtc_directive_pre_connect_check(directive, policy);
    return Directive::Result::Allowed;
}

}
