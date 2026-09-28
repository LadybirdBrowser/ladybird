/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy {

// Reports the violation for the request being checked and the given policy.
using ViolationReporter = Function<void(NonnullRefPtr<Policy const>)>;

// https://w3c.github.io/webappsec-csp/#report-for-request
void report_content_security_policy_violations_for_request(Fetch::Infrastructure::Request const&, ViolationReporter const&);

// https://w3c.github.io/webappsec-csp/#should-block-request
Directives::Directive::Result should_request_be_blocked_by_content_security_policy(Fetch::Infrastructure::Request const&, ViolationReporter const&);

// https://w3c.github.io/webappsec-csp/#should-block-response
Directives::Directive::Result should_response_to_request_be_blocked_by_content_security_policy(Fetch::Infrastructure::Response const&, Fetch::Infrastructure::Request const&, ViolationReporter const&);

}
