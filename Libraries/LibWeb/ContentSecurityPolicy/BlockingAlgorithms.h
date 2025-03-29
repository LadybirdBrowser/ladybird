/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy {

void report_content_security_policy_violations_for_request(JS::Realm&, GC::Ref<Fetch::Infrastructure::Request>);
[[nodiscard]] Directives::Directive::Result should_request_be_blocked_by_content_security_policy(JS::Realm&, GC::Ref<Fetch::Infrastructure::Request>);
[[nodiscard]] Directives::Directive::Result should_response_to_request_be_blocked_by_content_security_policy(JS::Realm&, GC::Ref<Fetch::Infrastructure::Response>, GC::Ref<Fetch::Infrastructure::Request>);

[[nodiscard]] Directives::Directive::Result should_navigation_request_of_type_be_blocked_by_content_security_policy(GC::Ref<Fetch::Infrastructure::Request> navigation_request, Directives::Directive::NavigationType navigation_type);
[[nodiscard]] Directives::Directive::Result should_navigation_response_to_navigation_request_of_type_in_target_be_blocked_by_content_security_policy(
    GC::Ptr<Fetch::Infrastructure::Request> navigation_request,
    GC::Ref<Fetch::Infrastructure::Response> navigation_response,
    GC::Ref<PolicyList> response_csp_list,
    Directives::Directive::NavigationType navigation_type,
    GC::Ref<HTML::Navigable> target);

}
