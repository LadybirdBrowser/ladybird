/*
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibURL/Forward.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-inline-check
// An inline check, which takes an Element, a type string, a policy, and a source string as arguments, and is executed
// during § 4.2.3 Should element’s inline type behavior be blocked by Content Security Policy? and during § 4.2.4 Should
// navigation request of type be blocked by Content Security Policy? for javascript: requests. This algorithm returns
// "Allowed" unless otherwise specified.
Directive::Result inline_check(Directive const&, GC::Ptr<DOM::Element const>, Directive::InlineType, Policy const&, Utf16View source);

// https://w3c.github.io/webappsec-csp/#directive-initialization
// An initialization, which takes a Document or global object and a policy as arguments. This algorithm is executed
// during § 4.2.1 Run CSP initialization for a Document and § 4.2.6 Run CSP initialization for a global object. Unless
// otherwise specified, it has no effect and it returns "Allowed".
Directive::Result initialization(Directive const&, Variant<GC::Ref<DOM::Document const>, GC::Ref<HTML::WorkerGlobalScope const>>, Policy const&);

// https://w3c.github.io/webappsec-csp/#directive-pre-navigation-check
// A pre-navigation check, which takes a request, a navigation type string ("form-submission" or "other") and a policy
// as arguments, and is executed during § 4.2.4 Should navigation request of type be blocked by Content Security
// Policy?. It returns "Allowed" unless otherwise specified.
Directive::Result pre_navigation_check(Directive const&, Fetch::Infrastructure::Request&, Directive::NavigationType, Policy const&);

// https://w3c.github.io/webappsec-csp/#directive-navigation-response-check
// A navigation response check, which takes a request, a navigation type string ("form-submission" or "other"), a
// response, a navigable, a check type string ("source" or "response"), and a policy as arguments, and is executed
// during § 4.2.5 Should navigation response to navigation request of type in target be blocked by Content Security
// Policy?. It returns "Allowed" unless otherwise specified.
// NB: The navigable is given as the origins of its container documents, nearest first.
Directive::Result navigation_response_check(Directive const&, Fetch::Infrastructure::Request const&, Directive::NavigationType, Fetch::Infrastructure::Response const&, ReadonlySpan<URL::Origin> target_container_document_origins, Directive::CheckType, Policy const&);

}
