/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directives
// Each policy contains an ordered set of directives (its directive set), each of which controls a specific behavior.
// The directives defined in this document are described in detail in § 6 Content Security Policy Directives.
class Directive : public JS::Cell {
    GC_CELL(Directive, JS::Cell)
    GC_DECLARE_ALLOCATOR(Directive);

public:
    enum class Result {
        Blocked,
        Allowed,
    };

    enum class NavigationType {
        FormSubmission,
        Other,
    };

    enum class CheckType {
        Source,
        Response,
    };

    enum class InlineType {
        Navigation,
        Script,
        ScriptAttribute,
        Style,
        StyleAttribute,
    };

    virtual ~Directive() = default;

    // Directives have a number of associated algorithms:
    // https://w3c.github.io/webappsec-csp/#directive-pre-request-check
    // 1. A pre-request check, which takes a request and a policy as an argument, and is executed during
    //    § 4.1.2 Should request be blocked by Content Security Policy?. This algorithm returns "Allowed"
    //    unless otherwise specified.
    [[nodiscard]] virtual Result pre_request_check(JS::Realm&, GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Policy const>) const { return Result::Allowed; }

    // https://w3c.github.io/webappsec-csp/#directive-post-request-check
    // 2. A post-request check, which takes a request, a response, and a policy as arguments, and is executed during
    //    § 4.1.3 Should response to request be blocked by Content Security Policy?. This algorithm returns "Allowed"
    //    unless otherwise specified.
    [[nodiscard]] virtual Result post_request_check(JS::Realm&, GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Fetch::Infrastructure::Response const>, GC::Ref<Policy const>) const { return Result::Allowed; }

    // https://w3c.github.io/webappsec-csp/#directive-inline-check
    // 3. An inline check, which takes an Element, a type string, a policy, and a source string as arguments, and is
    //    executed during § 4.2.3 Should element’s inline type behavior be blocked by Content Security Policy? and
    //    during § 4.2.4 Should navigation request of type be blocked by Content Security Policy? for javascript:
    //    requests. This algorithm returns "Allowed" unless otherwise specified.
    [[nodiscard]] virtual Result inline_check(JS::Realm&, GC::Ptr<DOM::Element const>, InlineType, GC::Ref<Policy const>, String const&) const { return Result::Allowed; }

    // https://w3c.github.io/webappsec-csp/#directive-initialization
    // 4. An initialization, which takes a Document or global object and a policy as arguments. This algorithm is
    //    executed during § 4.2.1 Run CSP initialization for a Document and § 4.2.6 Run CSP initialization for
    //    a global object. Unless otherwise specified, it has no effect and it returns "Allowed".
    [[nodiscard]] virtual Result initialization(Variant<GC::Ref<DOM::Document const>, GC::Ref<HTML::WorkerGlobalScope const>>, GC::Ref<Policy const>) const { return Result::Allowed; }

    // https://w3c.github.io/webappsec-csp/#directive-pre-navigation-check
    // 5. A pre-navigation check, which takes a request, a navigation type string ("form-submission" or "other")
    //    and a policy as arguments, and is executed during § 4.2.4 Should navigation request of type be blocked by
    //    Content Security Policy?. It returns "Allowed" unless otherwise specified.
    [[nodiscard]] virtual Result pre_navigation_check(GC::Ref<Fetch::Infrastructure::Request const>, NavigationType, GC::Ref<Policy const>) const { return Result::Allowed; }

    // https://w3c.github.io/webappsec-csp/#directive-navigation-response-check
    // 6. A navigation response check, which takes a request, a navigation type string ("form-submission" or "other"),
    //    a response, a navigable, a check type string ("source" or "response"), and a policy as arguments, and is
    //    executed during § 4.2.5 Should navigation response to navigation request of type in target be blocked by
    //    Content Security Policy?. It returns "Allowed" unless otherwise specified.
    [[nodiscard]] virtual Result navigation_response_check(GC::Ref<Fetch::Infrastructure::Request const>, NavigationType, GC::Ref<Fetch::Infrastructure::Response const>, GC::Ref<HTML::Navigable const>, CheckType, GC::Ref<Policy const>) const { return Result::Allowed; }

    // https://w3c.github.io/webappsec-csp/#directive-webrtc-pre-connect-check
    // 7. A webrtc pre-connect check, which takes a policy, and is executed during § 4.3.1 Should RTC connections be
    //    blocked for global?. It returns "Allowed" unless otherwise specified.
    [[nodiscard]] virtual Result webrtc_pre_connect_check(GC::Ref<Policy const>) const { return Result::Allowed; }

    [[nodiscard]] String const& name() const { return m_name; }
    [[nodiscard]] Vector<String> const& value() const { return m_value; }

    [[nodiscard]] GC::Ref<Directive> clone(JS::Realm&) const;
    [[nodiscard]] SerializedDirective serialize() const;

protected:
    Directive(String name, Vector<String> value);

private:
    // https://w3c.github.io/webappsec-csp/#directive-name
    // https://w3c.github.io/webappsec-csp/#directive-value
    // Each directive is a name / value pair. The name is a non-empty string, and the value is a set of non-empty strings.
    // The value MAY be empty.
    String m_name;
    Vector<String> m_value;
};

}
