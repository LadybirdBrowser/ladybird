/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-webrtc-pre-connect-check
// A webrtc pre-connect check, which takes a policy, and is executed during § 4.3.1 Should RTC connections be blocked
// for global?. It returns "Allowed" unless otherwise specified.
Directive::Result webrtc_pre_connect_check(Directive const&, Policy const&);

}
