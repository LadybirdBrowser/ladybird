/*
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-pre-request-check
// A pre-request check, which takes a request and a policy as an argument, and is executed during § 4.1.2 Should
// request be blocked by Content Security Policy?. This algorithm returns "Allowed" unless otherwise specified.
Directive::Result pre_request_check(Directive const&, Fetch::Infrastructure::Request const&, Policy const&);

// https://w3c.github.io/webappsec-csp/#directive-post-request-check
// A post-request check, which takes a request, a response, and a policy as arguments, and is executed during § 4.1.3
// Should response to request be blocked by Content Security Policy?. This algorithm returns "Allowed" unless otherwise
// specified.
Directive::Result post_request_check(Directive const&, Fetch::Infrastructure::Request const&, Fetch::Infrastructure::Response const&, Policy const&);

}
