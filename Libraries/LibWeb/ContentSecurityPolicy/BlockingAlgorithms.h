/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Utf16View.h>
#include <LibJS/Runtime/VM.h>
#include <LibURL/Forward.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy {

// Reports the violation for the request being checked and the given policy.
using ViolationReporter = Function<void(NonnullRefPtr<Policy const>)>;

void report_content_security_policy_violations_for_request(Fetch::Infrastructure::Request const&, ViolationReporter const&);
Directives::Directive::Result should_request_be_blocked_by_content_security_policy(Fetch::Infrastructure::Request const&, ViolationReporter const&);
Directives::Directive::Result should_request_be_blocked_by_integrity_policy(GC::Ref<Fetch::Infrastructure::Request>);
Directives::Directive::Result should_response_to_request_be_blocked_by_content_security_policy(Fetch::Infrastructure::Response const&, Fetch::Infrastructure::Request const&, ViolationReporter const&);

[[nodiscard]] ViolationReporter violation_reporter_for_request(JS::Realm&, GC::Ref<Fetch::Infrastructure::Request>);

Directives::Directive::Result should_navigation_request_of_type_be_blocked_by_content_security_policy(GC::Ref<Fetch::Infrastructure::Request> navigation_request, Directives::Directive::NavigationType navigation_type);
Directives::Directive::Result should_navigation_response_to_navigation_request_of_type_in_target_be_blocked_by_content_security_policy(
    GC::Ptr<Fetch::Infrastructure::Request> navigation_request,
    GC::Ref<Fetch::Infrastructure::Response> navigation_response,
    ContentSecurityPolicy::PolicyList const& response_csp_list,
    Directives::Directive::NavigationType navigation_type,
    GC::Ref<HTML::LocalNavigable> target);

Directives::Directive::Result should_elements_inline_type_behavior_be_blocked_by_content_security_policy(GC::Ref<DOM::Element> element, Directives::Directive::InlineType type, Utf16View source);
JS::ThrowCompletionOr<void> ensure_csp_does_not_block_string_compilation(JS::Realm& realm, ReadonlySpan<Utf16String> parameter_strings, Utf16View body_string, Utf16View code_string, JS::CompilationType compilation_type, ReadonlySpan<JS::Value> parameter_args, JS::Value body_arg);
JS::ThrowCompletionOr<void> ensure_csp_does_not_block_wasm_byte_compilation(JS::Realm&);

[[nodiscard]] Directives::Directive::Result is_base_allowed_for_document(URL::URL const& base, GC::Ref<DOM::Document const> document);

}
