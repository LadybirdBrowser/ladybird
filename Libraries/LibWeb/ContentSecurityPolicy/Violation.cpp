/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/SecurityPolicyViolationEvent.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/EnvironmentSettingsSnapshot.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Infra/JSON.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>

namespace Web::ContentSecurityPolicy {

GC_DEFINE_ALLOCATOR(Violation);

Violation::Violation(GC::Ptr<DOM::EventTarget> global_scope, GC::Ref<Policy const> policy, String directive)
    : m_global_scope(global_scope)
    , m_policy(policy)
    , m_effective_directive(Utf16String::from_utf8(directive))
{
}

static GC::Ptr<DOM::EventTarget> global_scope_from_global_object(GC::Ptr<JS::Object> global_object)
{
    if (!global_object)
        return nullptr;

    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(*global_object);
    if (!global_scope)
        return nullptr;
    return global_scope->this_impl();
}

// https://w3c.github.io/webappsec-csp/#create-violation-for-global
GC::Ref<Violation> Violation::create_a_violation_object_for_global_policy_and_directive(GC::Ptr<JS::Object> global_object, GC::Ref<Policy const> policy, String directive)
{
    // 1. Let violation be a new violation whose global object is global, policy is policy, effective directive is
    //    directive, and resource is null.
    auto violation = GC::Heap::the().allocate<Violation>(global_scope_from_global_object(global_object), policy, move(directive));

    // 2. If the user agent is currently executing script, and can extract a source file’s URL, line number,
    //    and column number from the global, set violation’s source file, line number, and column number
    //    accordingly.
    // SPEC ISSUE 1:  Is this kind of thing specified anywhere? I didn’t see anything that looked useful in [ECMA262].
    // NB: We use the innermost stack frame that has a source location.
    for (auto const& frame : JS::VM::the().stack_trace()) {
        if (!frame.source_range.has_value())
            continue;
        auto const& source_range = *frame.source_range;
        auto source_file = URL::Parser::basic_parse(source_range.filename().utf16_view());
        if (!source_file.has_value())
            break;

        // NB: The URL of a classic script is its response URL, which can reveal where a cross-origin request was
        //     redirected to. Per the note in the spec, we strip such URLs down to their origin. Redirects only lead
        //     to HTTP(S) URLs, so URLs with an opaque origin, such as file: URLs, are kept as they are.
        if (global_object) {
            auto source_origin = source_file->origin();
            if (!source_origin.is_opaque() && !source_origin.is_same_origin(HTML::relevant_settings_object(*global_object).origin())) {
                source_file = URL::Parser::basic_parse(source_origin.serialize());
                if (!source_file.has_value())
                    break;
            }
        }

        violation->m_source_file = source_file.release_value();
        violation->m_line_number = source_range.start.line;
        violation->m_column_number = source_range.start.column;
        break;
    }

    // 3. If global is a Window object, set violation’s referrer to global’s document's referrer.
    if (auto* global = global_object.ptr()) {
        if (auto* window = HTML::window_from_global_object(*global))
            violation->m_referrer = URL::Parser::basic_parse(window->associated_document().referrer());
    }

    // FIXME: 4. Set violation’s status to the HTTP status code for the resource associated with violation’s global object.
    // SPEC ISSUE 2: How, exactly, do we get the status code? We don’t actually store it anywhere.

    // 5. Return violation.
    return violation;
}

// https://w3c.github.io/webappsec-csp/#create-violation-for-request
GC::Ref<Violation> Violation::create_a_violation_object_for_request_and_policy(GC::Ref<Fetch::Infrastructure::Request> request, GC::Ref<Policy const> policy)
{
    // 1. Let directive be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto directive = Directives::get_the_effective_directive_for_request(request);

    // NOTE: The spec assumes that the effective directive of a Violation is a non-empty string.
    //       See the definition of m_effective_directive.
    VERIFY(directive.has_value());

    // 2. Let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive
    //      on request’s client’s global object, policy, and directive.
    auto violation = create_a_violation_object_for_global_policy_and_directive(request->client()->global_object(), policy, String { directive->view().to_utf8_but_should_be_ported_to_utf16() });

    // 3. Set violation’s resource to request’s url.
    // Spec Note: We use request’s url, and not its current url, as the latter might contain information about redirect
    //            targets to which the page MUST NOT be given access.
    violation->m_resource = request->url();

    // AD-HOC: A request whose client is a snapshot of an environment in another process, such as a worker's outside
    //         settings when the worker fetches its script, violates that environment's policies. Its global object is
    //         in that process, so the violation is reported there.
    if (auto const* snapshot = as_if<HTML::EnvironmentSettingsSnapshot>(*request->client()); snapshot && snapshot->report_violation()) {
        // NB: Request's policy container is a clone of the snapshot's, which lists the policies in the same order.
        auto const& policies = request->policy_container().get<GC::Ref<HTML::PolicyContainer>>()->csp_list->policies();
        for (size_t policy_index = 0; policy_index < policies.size(); ++policy_index) {
            if (policies[policy_index].ptr() != policy.ptr())
                continue;
            violation->m_report_in_other_process = GC::create_function(GC::Heap::the(), [report_violation = GC::Ref { *snapshot->report_violation() }, policy_index, effective_directive = String { violation->m_effective_directive.to_utf8() }, resource = request->url()] {
                report_violation->function()(policy_index, effective_directive, resource);
            });
            break;
        }
    }

    // 4. Return violation.
    return violation;
}

void Violation::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_scope);
    visitor.visit(m_policy);
    visitor.visit(m_element);
    visitor.visit(m_report_in_other_process);
}

// https://w3c.github.io/webappsec-csp/#violation-url
URL::URL Violation::url() const
{
    // Each violation has a url which is its global object’s URL.
    if (!m_global_scope) {
        // FIXME: What do we return here?
        dbgln("FIXME: Figure out URL for violation with null global object.");
        return URL::URL {};
    }

    if (auto* window = as_if<HTML::Window>(*m_global_scope)) {
        return window->associated_document().url();
    }

    if (auto* worker = as_if<HTML::WorkerGlobalScope>(*m_global_scope)) {
        return worker->url();
    }

    TODO();
}

// https://w3c.github.io/webappsec-csp/#strip-url-for-use-in-reports
[[nodiscard]] static String strip_url_for_use_in_reports(URL::URL url)
{
    // 1. If url’s scheme is not an HTTP(S) scheme, then return url’s scheme.
    if (!Fetch::Infrastructure::is_http_or_https_scheme(url.scheme()))
        return MUST(String::from_utf8(url.scheme()));

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
    body.value.set("effective-directive"_string, Infra::JSONValue { m_effective_directive.to_utf8() });

    // "violated-directive"
    //    violation's effective directive
    body.value.set("violated-directive"_string, Infra::JSONValue { m_effective_directive.to_utf8() });

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
    body.value.set("script-sample"_string, Infra::JSONValue { m_sample.to_utf8() });

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
    // FIXME: This is not a valid assertion, since Eval, TrustedTypesSink and TrustedTypesPolicy provide a sample. https://github.com/w3c/webappsec-csp/issues/788
    if (auto* maybe_resource = m_resource.get_pointer<Resource>();
        !maybe_resource || (*maybe_resource != Resource::Inline && *maybe_resource != Resource::Eval && *maybe_resource != Resource::TrustedTypesSink && *maybe_resource != Resource::TrustedTypesPolicy)) {
        VERIFY(m_sample.is_empty());
    }

    // 4. Return the result of serialize an infra value to JSON bytes given «[ "csp-report" → body ]».
    Infra::JSONObject csp_report;
    csp_report.value.set("csp-report"_string, Infra::JSONObject { move(body) });

    HTML::TemporaryExecutionContext execution_context { realm };
    return Infra::serialize_an_infra_value_to_json_bytes(realm, move(csp_report));
}

// https://w3c.github.io/webappsec-csp/#report-violation
void Violation::report_a_violation(JS::Realm& realm)
{
    dbgln("Content Security Policy violation{}: Refusing access to resource '{}' because it does not appear in the '{}' directive.",
        disposition() == Policy::Disposition::Report ? " (report only)"sv : ""sv,
        obtain_the_blocked_uri_of_resource(),
        m_effective_directive);

    // AD-HOC: See create_a_violation_object_for_request_and_policy().
    if (m_report_in_other_process) {
        m_report_in_other_process->function()();
        return;
    }

    // 1. Let global be violation’s global object.
    auto global_scope = m_global_scope;

    // 2. Let target be violation’s element.
    auto target = m_element;

    // 3. Queue a task to run the following steps:
    // Spec Note: We "queue a task" here to ensure that the event targeting and dispatch happens after JavaScript
    //            completes execution of the task responsible for a given violation (which might manipulate the DOM).
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(GC::Heap::the(), [this, global_scope, target, &realm] {
        GC::Ptr<DOM::EventTarget> event_target = target;

        // 1. If target is not null, and global is a Window, and target’s shadow-including root is not global’s
        //    associated Document, set target to null.
        // Spec Note: This ensures that we fire events only at elements connected to violation’s policy’s Document.
        //            If a violation is caused by an element which isn’t connected to that document, we’ll fire the
        //            event at the document rather than the element in order to ensure that the violation is visible
        //            to the document’s listeners.
        if (target && global_scope) {
            auto const* window = as_if<HTML::Window>(*global_scope);
            if (window && &target->shadow_including_root() != &window->associated_document())
                event_target = nullptr;
        }

        // 2. If target is null:
        if (!event_target) {
            // 1. Set target to violation’s global object.
            if (global_scope) {
                event_target = [&]() -> GC::Ptr<DOM::EventTarget> {
                    // 2. If target is a Window, set target to target’s associated Document.
                    if (auto* window = as_if<HTML::Window>(*global_scope))
                        return window->associated_document();

                    return global_scope;
                }();
            }
        }

        // 3. If target implements EventTarget, fire an event named securitypolicyviolation that uses the
        //    SecurityPolicyViolationEvent interface at target with its attributes initialized as follows:
        if (event_target) {
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
            event_init.document_uri = Utf16String::from_utf8(strip_url_for_use_in_reports(url()));

            // referrer
            //    The result of executing § 5.4 Strip URL for use in reports on violation's referrer.
            // FIXME: File spec issue for referrer being potentially null.
            event_init.referrer = m_referrer.has_value() ? Utf16String::from_utf8(strip_url_for_use_in_reports(m_referrer.value())) : Utf16String {};

            // blockedURI
            //    The result of executing § 5.2 Obtain the blockedURI of a violation's resource on violation’s
            //    resource.
            event_init.blocked_uri = Utf16String::from_utf8(obtain_the_blocked_uri_of_resource());

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
            event_init.original_policy = Utf16String::from_utf8(m_policy->pre_parsed_policy_string({}));

            // disposition
            //    violation's disposition
            event_init.disposition = disposition();

            // sourceFile
            //    The result of executing § 5.4 Strip URL for use in reports on violation’s source file, if
            //    violation's source file is not null, or null otherwise.
            event_init.source_file = m_source_file.has_value() ? Utf16String::from_utf8(strip_url_for_use_in_reports(m_source_file.value())) : Utf16String {};

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

            auto event = SecurityPolicyViolationEvent::create(HTML::EventNames::securitypolicyviolation, event_init, HighResolutionTime::current_high_resolution_time(realm.global_object()));
            event->set_is_trusted(true);
            event_target->dispatch_event(event);
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
                        auto request = Fetch::Infrastructure::Request::create();

                        // method
                        //    "POST"
                        request->set_method("POST"sv);

                        // url
                        //    violation’s url
                        // FIXME: File spec issue that this is incorrect, it should be `endpoint` instead.
                        request->set_url(endpoint.value());

                        // origin
                        //    violation's global object's relevant settings object's origin
                        // FIXME: File spec issue that global object can be null, so we use the realm to get the ESO instead.
                        auto& environment_settings_object = Bindings::principal_host_defined_environment_settings_object(realm);
                        request->set_origin(environment_settings_object.origin());

                        // traversable for user prompts
                        //    "no-traversable"
                        request->set_traversable_for_user_prompts(Fetch::Infrastructure::Request::TraversableForUserPrompts::NoTraversable);

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
                        auto header_list = HTTP::HeaderList::create();
                        header_list->append({ "Content-Type"sv, "application/csp-report"sv });
                        request->set_header_list(move(header_list));

                        // body
                        //    The result of executing § 5.3 Obtain the deprecated serialization of violation on
                        //    violation
                        request->set_body(obtain_the_deprecated_serialization(realm));

                        // redirect mode
                        //    "error"
                        request->set_redirect_mode(Fetch::Infrastructure::Request::RedirectMode::Error);

                        // 4. Fetch request. The result will be ignored.
                        (void)Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(Fetch::Infrastructure::FetchAlgorithms::Input { Fetch::Engine::BodyIntent::DrainAndDiscard }));
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
