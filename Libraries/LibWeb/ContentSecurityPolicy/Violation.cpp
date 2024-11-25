/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/SecurityPolicyViolationEvent.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Infra/JSON.h>

namespace Web::ContentSecurityPolicy {

GC_DEFINE_ALLOCATOR(Violation);

Violation::Violation(GC::Ptr<JS::Object> global_object, GC::Ref<Policy const> policy, String directive)
    : m_global_object(global_object)
    , m_policy(policy)
    , m_effective_directive(directive)
{
}

// https://w3c.github.io/webappsec-csp/#create-violation-for-global
GC::Ref<Violation> Violation::create_a_violation_object_for_global_policy_and_directive(JS::Realm& realm, GC::Ptr<JS::Object> global_object, GC::Ref<Policy const> policy, String directive)
{
    // 1. Let violation be a new violation whose global object is global, policy is policy, effective directive is
    //    directive, and resource is null.
    auto violation = realm.create<Violation>(global_object, policy, directive);

    // FIXME: 2. If the user agent is currently executing script, and can extract a source file’s URL, line number,
    //           and column number from the global, set violation’s source file, line number, and column number
    //           accordingly.
    // SPEC ISSUE 1:  Is this kind of thing specified anywhere? I didn’t see anything that looked useful in [ECMA262].

    // 3. If global is a Window object, set violation’s referrer to global’s document's referrer.
    if (global_object) {
        if (auto* window = dynamic_cast<HTML::Window*>(global_object.ptr())) {
            violation->m_referrer = URL::Parser::basic_parse(window->associated_document().referrer());
        }
    }

    // FIXME: 4. Set violation’s status to the HTTP status code for the resource associated with violation’s global object.
    // SPEC ISSUE 2: How, exactly, do we get the status code? We don’t actually store it anywhere.

    // 5. Return violation.
    return violation;
}

// https://w3c.github.io/webappsec-csp/#create-violation-for-request
GC::Ref<Violation> Violation::create_a_violation_object_for_request_and_policy(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request> request, GC::Ref<Policy const> policy)
{
    // 1. Let directive be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto directive = Directives::get_the_effective_directive_for_request(request);

    // NOTE: The spec assumes that the effective directive of a Violation is a non-empty string.
    //       See the definition of m_effective_directive.
    VERIFY(directive.has_value());

    // 2. Let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive
    //      on request’s client’s global object, policy, and directive.
    auto violation = create_a_violation_object_for_global_policy_and_directive(realm, request->client()->global_object(), policy, directive->to_string());

    // 3. Set violation’s resource to request’s url.
    // Spec Note: We use request’s url, and not its current url, as the latter might contain information about redirect
    //            targets to which the page MUST NOT be given access.
    violation->m_resource = request->url();

    // 4. Return violation.
    return violation;
}

void Violation::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_object);
    visitor.visit(m_policy);
    visitor.visit(m_element);
}

// https://w3c.github.io/webappsec-csp/#violation-url
URL::URL Violation::url() const
{
    // Each violation has a url which is its global object’s URL.
    if (!m_global_object) {
        // FIXME: What do we return here?
        dbgln("FIXME: Figure out URL for violation with null global object.");
        return URL::URL {};
    }

    // FIXME: File a spec issue about what to do for ShadowRealms here.
    auto* universal_scope = dynamic_cast<HTML::UniversalGlobalScopeMixin*>(m_global_object.ptr());
    VERIFY(universal_scope);
    auto& principal_global = HTML::relevant_principal_global_object(universal_scope->this_impl());

    if (auto* window = dynamic_cast<HTML::Window*>(&principal_global)) {
        return window->associated_document().url();
    }

    if (auto* worker = dynamic_cast<HTML::WorkerGlobalScope*>(&principal_global)) {
        return worker->url();
    }

    TODO();
}

// https://w3c.github.io/webappsec-csp/#strip-url-for-use-in-reports
[[nodiscard]] static String strip_url_for_use_in_reports(URL::URL url)
{
    // 1. If url’s scheme is not an HTTP(S) scheme, then return url’s scheme.
    if (!Fetch::Infrastructure::is_http_or_https_scheme(url.scheme()))
        return url.scheme();

    // 2. Set url’s fragment to the empty string.
    // FIXME: File spec issue about potentially meaning `null` here, as using empty string leaves a stray # at the end.
    url.set_fragment(OptionalNone {});

    // 3. Set url’s username to the empty string.
    url.set_username(String {});

    // 4. Set url’s password to the empty string.
    url.set_password(String {});

    // 5. Return the result of executing the URL serializer on url.
    return url.serialize();
}

// https://w3c.github.io/webappsec-csp/#obtain-violation-blocked-uri
String Violation::obtain_the_blocked_uri_of_resource() const
{
    // 1. Assert: resource is a URL or a string.
    VERIFY(m_resource.has<URL::URL>() || m_resource.has<Resource>());

    // 2. If resource is a URL, return the result of executing § 5.4 Strip URL for use in reports on resource.
    if (m_resource.has<URL::URL>()) {
        auto const& url = m_resource.get<URL::URL>();
        return strip_url_for_use_in_reports(url);
    }

    // 3. Return resource.
    auto resource = m_resource.get<Resource>();
    switch (resource) {
#define __ENUMERATE_RESOURCE_TYPE(type, value) \
    case Resource::type:                       \
        return value##_string;
        ENUMERATE_RESOURCE_TYPES
#undef __ENUMERATE_RESOURCE_TYPE
    default:
        VERIFY_NOT_REACHED();
    }
}

[[nodiscard]] static String original_disposition_to_string(Policy::Disposition disposition)
{
    switch (disposition) {
#define __ENUMERATE_DISPOSITION_TYPE(type, value) \
    case Policy::Disposition::type:               \
        return value##_string;
        ENUMERATE_DISPOSITION_TYPES
#undef __ENUMERATE_DISPOSITION_TYPE
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://w3c.github.io/webappsec-csp/#deprecated-serialize-violation
ByteBuffer Violation::obtain_the_deprecated_serialization(JS::Realm& realm) const
{
    // 1. Let body be a map with its keys initialized as follows:
    Infra::JSONObject body;

    // "document-uri"
    //    The result of executing § 5.4 Strip URL for use in reports on violation's url.
    body.value.set("document-uri"_string, Infra::JSONValue { strip_url_for_use_in_reports(url()) });

    // "referrer"
    //    The result of executing § 5.4 Strip URL for use in reports on violation's referrer.
    // FIXME: File spec issue that referrer can be null here.
    Infra::JSONValue referrer = m_referrer.has_value()
        ? Infra::JSONValue { strip_url_for_use_in_reports(m_referrer.value()) }
        : Infra::JSONValue { Empty {} };

    body.value.set("referrer"_string, referrer);

    // "blocked-uri"
    //    The result of executing § 5.2 Obtain the blockedURI of a violation’s resource on violation’s resource.
    body.value.set("blocked_uri"_string, Infra::JSONValue { obtain_the_blocked_uri_of_resource() });

    // "effective-directive"
    //    violation's effective directive
    body.value.set("effective-directive"_string, Infra::JSONValue { m_effective_directive });

    // "violated-directive"
    //    violation's effective directive
    body.value.set("violated-directive"_string, Infra::JSONValue { m_effective_directive });

    // "original-policy"
    //    The serialization of violation's policy
    body.value.set("original-policy"_string, Infra::JSONValue { m_policy->pre_parsed_policy_string({}) });

    // "disposition"
    //    The disposition of violation's policy
    body.value.set("disposition"_string, Infra::JSONValue { original_disposition_to_string(disposition()) });

    // "status-code"
    //    violation's status
    body.value.set("status-code"_string, Infra::JSONValue { m_status });

    // "script-sample"
    //    violation's sample
    // Spec Note: The name script-sample was chosen for compatibility with an earlier iteration of this feature which
    //            has shipped in Firefox since its initial implementation of CSP. Despite the name, this field will
    //            contain samples for non-script violations, like stylesheets. The data contained in a
    //            SecurityPolicyViolationEvent object, and in reports generated via the new report-to directive, is
    //            named in a more encompassing fashion: sample.
    body.value.set("script-sample"_string, Infra::JSONValue { m_sample });

    // 2. If violation’s source file is not null:
    if (m_source_file.has_value()) {
        // 1. Set body["source-file'] to the result of executing § 5.4 Strip URL for use in reports on violation’s
        //    source file.
        body.value.set("source-file"_string, Infra::JSONValue { strip_url_for_use_in_reports(m_source_file.value()) });

        // 2. Set body["line-number"] to violation’s line number.
        body.value.set("line-number"_string, Infra::JSONValue { m_line_number });

        // 3. Set body["column-number"] to violation’s column number.
        body.value.set("column-number"_string, Infra::JSONValue { m_column_number });
    }

    // 3. Assert: If body["blocked-uri"] is not "inline", then body["sample"] is the empty string.
    // FIXME: File spec issue that body["sample"] should be body["script-sample"]
    if (m_resource.has<Resource>() && m_resource.get<Resource>() != Resource::Inline) {
        VERIFY(m_sample.is_empty());
    }

    // 4. Return the result of serialize an infra value to JSON bytes given «[ "csp-report" → body ]».
    Infra::JSONObject csp_report;
    csp_report.value.set("csp-report"_string, Infra::JSONObject { move(body) });

    HTML::TemporaryExecutionContext execution_context { realm };
    return Infra::serialize_an_infra_value_to_json_bytes(realm, move(csp_report));
}

[[nodiscard]] static Bindings::SecurityPolicyViolationEventDisposition original_disposition_to_bindings_disposition(Policy::Disposition disposition)
{
    switch (disposition) {
#define __ENUMERATE_DISPOSITION_TYPE(type, _) \
    case Policy::Disposition::type:           \
        return Bindings::SecurityPolicyViolationEventDisposition::type;
        ENUMERATE_DISPOSITION_TYPES
#undef __ENUMERATE_DISPOSITION_TYPE
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://w3c.github.io/webappsec-csp/#report-violation
void Violation::report_a_violation(JS::Realm& realm)
{
    dbgln("Content Security Policy violation{}: Refusing access to resource '{}' because it does not appear in the '{}' directive.",
        disposition() == Policy::Disposition::Report ? " (report only)"sv : ""sv,
        obtain_the_blocked_uri_of_resource(),
        m_effective_directive);

    // 1. Let global be violation’s global object.
    auto global = m_global_object;

    // 2. Let target be violation’s element.
    auto target = m_element;

    // 3. Queue a task to run the following steps:
    // Spec Note: We "queue a task" here to ensure that the event targeting and dispatch happens after JavaScript
    //            completes execution of the task responsible for a given violation (which might manipulate the DOM).
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [this, global, target, &realm] {
        auto& vm = realm.vm();

        GC::Ptr<JS::Object> target_as_object = target;

        // 1. If target is not null, and global is a Window, and target’s shadow-including root is not global’s
        //    associated Document, set target to null.
        // Spec Note: This ensures that we fire events only at elements connected to violation’s policy’s Document.
        //            If a violation is caused by an element which isn’t connected to that document, we’ll fire the
        //            event at the document rather than the element in order to ensure that the violation is visible
        //            to the document’s listeners.
        if (target && is<HTML::Window>(global.ptr())) {
            auto const& window = static_cast<HTML::Window const&>(*global.ptr());
            if (&target->shadow_including_root() != &window.associated_document())
                target_as_object = nullptr;
        }

        // 2. If target is null:
        if (!target_as_object) {
            // 1. Set target to violation’s global object.
            target_as_object = m_global_object;

            // 2. If target is a Window, set target to target’s associated Document.
            if (is<HTML::Window>(target_as_object.ptr())) {
                auto& window = static_cast<HTML::Window&>(*target_as_object.ptr());
                target_as_object = window.associated_document();
            }
        }

        // 3. If target implements EventTarget, fire an event named securitypolicyviolation that uses the
        //    SecurityPolicyViolationEvent interface at target with its attributes initialized as follows:
        if (is<DOM::EventTarget>(target_as_object.ptr())) {
            auto& event_target = static_cast<DOM::EventTarget&>(*target_as_object.ptr());

            SecurityPolicyViolationEventInit event_init {};

            // bubbles
            //    true
            event_init.bubbles = true;

            // composed
            //    true
            // Spec Note: We set the composed attribute, which means that this event can be captured on its way
            //            into, and will bubble its way out of a shadow tree. target, et al will be automagically
            //            scoped correctly for the main tree.
            event_init.composed = true;

            // documentURI
            //    The result of executing § 5.4 Strip URL for use in reports on violation's url.
            event_init.document_uri = strip_url_for_use_in_reports(url());

            // referrer
            //    The result of executing § 5.4 Strip URL for use in reports on violation's referrer.
            // FIXME: File spec issue for referrer being potentially null.
            event_init.referrer = m_referrer.has_value() ? strip_url_for_use_in_reports(m_referrer.value()) : String {};

            // blockedURI
            //    The result of executing § 5.2 Obtain the blockedURI of a violation's resource on violation’s
            //    resource.
            event_init.blocked_uri = obtain_the_blocked_uri_of_resource();

            // effectiveDirective
            //    violation's effective directive
            event_init.effective_directive = m_effective_directive;

            // violatedDirective
            //    violation's effective directive
            // Spec Note: Both effectiveDirective and violatedDirective are the same value. This is intentional
            //            to maintain backwards compatibility.
            event_init.violated_directive = m_effective_directive;

            // originalPolicy
            //    The serialization of violation's policy
            event_init.original_policy = m_policy->pre_parsed_policy_string({});

            // disposition
            //    violation's disposition
            event_init.disposition = original_disposition_to_bindings_disposition(disposition());

            // sourceFile
            //    The result of executing § 5.4 Strip URL for use in reports on violation’s source file, if
            //    violation's source file is not null, or null otherwise.
            event_init.source_file = m_source_file.has_value() ? strip_url_for_use_in_reports(m_source_file.value()) : String {};

            // statusCode
            //    violation's status
            event_init.status_code = m_status;

            // lineNumber
            //    violation’s line number
            event_init.line_number = m_line_number;

            // columnNumber
            //    violation’s column number
            event_init.column_number = m_column_number;

            // sample
            //    violation's sample
            event_init.sample = m_sample;

            auto event = SecurityPolicyViolationEvent::create(realm, HTML::EventNames::securitypolicyviolation, event_init);
            event->set_is_trusted(true);
            event_target.dispatch_event(event);
        }

        // 4. If violation’s policy’s directive set contains a directive named "report-uri" directive:
        if (auto report_uri_directive = m_policy->get_directive_by_name(Directives::Names::ReportUri)) {
            // 1. If violation’s policy’s directive set contains a directive named "report-to", skip the remaining
            //    substeps.
            if (!m_policy->contains_directive_with_name(Directives::Names::ReportTo)) {
                // 1. For each token of directive’s value:
                for (auto const& token : report_uri_directive->value()) {
                    // 1. Let endpoint be the result of executing the URL parser with token as the input, and
                    //    violation’s url as the base URL.
                    auto endpoint = DOMURL::parse(token, url());

                    // 2. If endpoint is not a valid URL, skip the remaining substeps.
                    if (endpoint.has_value()) {
                        // 3. Let request be a new request, initialized as follows:
                        auto request = Fetch::Infrastructure::Request::create(vm);

                        // method
                        //    "POST"
                        request->set_method(MUST(ByteBuffer::copy("POST"sv.bytes())));

                        // url
                        //    violation’s url
                        // FIXME: File spec issue that this is incorrect, it should be `endpoint` instead.
                        request->set_url(endpoint.value());

                        // origin
                        //    violation's global object's relevant settings object's origin
                        // FIXME: File spec issue that global object can be null, so we use the realm to get the ESO
                        //        instead, and cross ShadowRealm boundaries with the principal realm.
                        auto& environment_settings_object = Bindings::principal_host_defined_environment_settings_object(HTML::principal_realm(realm));
                        request->set_origin(environment_settings_object.origin());

                        // window
                        //    "no-window"
                        request->set_window(Fetch::Infrastructure::Request::Window::NoWindow);

                        // client
                        //    violation's global object's relevant settings object
                        request->set_client(&environment_settings_object);

                        // destination
                        //    "report"
                        request->set_destination(Fetch::Infrastructure::Request::Destination::Report);

                        // initiator
                        //    ""
                        request->set_initiator(OptionalNone {});

                        // credentials mode
                        //    "same-origin"
                        request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::SameOrigin);

                        // keepalive
                        //    "true"
                        request->set_keepalive(true);

                        // header list
                        //    A header list containing a single header whose name is "Content-Type", and value is
                        //    "application/csp-report"
                        auto header_list = Fetch::Infrastructure::HeaderList::create(vm);
                        auto content_type_header = Fetch::Infrastructure::Header::from_string_pair("Content-Type"sv, "application/csp-report"sv);
                        header_list->append(move(content_type_header));
                        request->set_header_list(header_list);

                        // body
                        //    The result of executing § 5.3 Obtain the deprecated serialization of violation on
                        //    violation
                        request->set_body(obtain_the_deprecated_serialization(realm));

                        // redirect mode
                        //    "error"
                        request->set_redirect_mode(Fetch::Infrastructure::Request::RedirectMode::Error);

                        // 4. Fetch request. The result will be ignored.
                        (void)Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(vm, {}));
                    }
                }
            }

            // 5. If violation's policy's directive set contains a directive named "report-to" directive:
            if (auto report_to_directive = m_policy->get_directive_by_name(Directives::Names::ReportTo)) {
                (void)report_to_directive;
                dbgln("FIXME: Implement report-to directive in violation reporting");
            }
        }
    }));
}

}
