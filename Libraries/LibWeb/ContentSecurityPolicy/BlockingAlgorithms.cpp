/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/HTMLIntegration.h>
#include <LibWeb/ContentSecurityPolicy/Directives/KeywordSources.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>
#include <LibWebCommon/Infra/Strings.h>

namespace Web::ContentSecurityPolicy {

static Optional<RemoteViolationDirective> remote_violation_directive(Utf16FlyString const& directive)
{
    if (directive == Directives::Names::FrameSrc)
        return RemoteViolationDirective::FrameSrc;
    if (directive == Directives::Names::ObjectSrc)
        return RemoteViolationDirective::ObjectSrc;
    if (directive == Directives::Names::FormAction)
        return RemoteViolationDirective::FormAction;
    return {};
}

static Utf16FlyString const& directive_name(RemoteViolationDirective directive)
{
    switch (directive) {
    case RemoteViolationDirective::FrameSrc:
        return Directives::Names::FrameSrc;
    case RemoteViolationDirective::ObjectSrc:
        return Directives::Names::ObjectSrc;
    case RemoteViolationDirective::FormAction:
        return Directives::Names::FormAction;
    }
    VERIFY_NOT_REACHED();
}

// AD-HOC: A client another process hosts has its global object there, so its page reports the violation to that
//         process, by the violated policy's index in the client's CSP list.
static void report_a_violation_of_a_remote_client(Page& page, Fetch::Infrastructure::Request const& request, Policy const& policy, Utf16FlyString const& directive)
{
    // NB: Request's policy container is a copy of the client's, which lists the policies in the same order.
    auto const& policies = request.policy_container().get<NonnullRefPtr<HTML::PolicyContainer const>>()->csp_list.policies();
    auto policy_index = policies.find_first_index_if([&](auto const& candidate) { return candidate.ptr() == &policy; });
    if (!policy_index.has_value())
        return;

    if (auto const& navigation = request.remote_client_navigation(); navigation.has_value()) {
        // NB: The UI process checks a navigation's report against the directives the navigation can violate, and
        //     reports the URL the navigation started with, upgraded to https when the request's URL is.
        auto url_is_https = request.url().scheme() == "https"sv;
        if (auto remote_directive = remote_violation_directive(directive); remote_directive.has_value())
            page.client().page_did_report_navigation_violation_of_remote_client(*navigation, *policy_index, *remote_directive, url_is_https);
        return;
    }
    page.client().page_did_report_violation_of_remote_client(*policy_index, directive.view().to_utf8_but_should_be_ported_to_utf16(), request.url());
}

ViolationReporter violation_reporter_for_request(JS::Realm& realm, NonnullRefPtr<Fetch::Infrastructure::Request> request)
{
    return [&realm, request](NonnullRefPtr<Policy const> policy) {
        if (Fetch::Fetching::remote_client(request)) {
            auto effective_directive = Directives::get_the_effective_directive_for_request(request);
            VERIFY(effective_directive.has_value());
            report_a_violation_of_a_remote_client(Bindings::principal_host_defined_page(realm), request, policy, *effective_directive);
            return;
        }
        auto violation = Violation::create_a_violation_object_for_request_and_policy(request, move(policy));
        violation->report_a_violation(realm);
    };
}

static bool policy_governs_directive(Policy const& policy, Utf16FlyString const& directive)
{
    auto governing_directives = Directives::get_fetch_directive_fallback_list(directive);
    if (governing_directives.is_empty())
        governing_directives.append(directive);
    return any_of(governing_directives, [&](auto const& governing_directive) {
        return policy.contains_directive_with_name(governing_directive);
    });
}

// AD-HOC: A fetch in another process found the violation of a policy of a client this process hosts. The report is
//         that process's claim, so it is reported only for a policy of the client governing its directive.
void report_a_violation_found_in_another_process(HTML::EnvironmentSettingsObject& settings, u64 policy_index, Utf16FlyString const& directive, URL::URL resource)
{
    // NB: The other process's copy of the client's CSP list lists its policies in the same order, and the list only
    //     grows while the client keeps its document, so while it does, policy_index names the violated policy.
    auto const& policies = settings.policy_container()->csp_list.policies();
    if (policy_index >= policies.size() || !policy_governs_directive(policies[policy_index], directive))
        return;

    auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(settings.global_object(), policies[policy_index], directive.view().to_utf8_but_should_be_ported_to_utf16());
    violation->set_resource(move(resource));
    violation->report_a_violation(settings.realm());
}

void report_a_violation_found_in_another_process(HTML::EnvironmentSettingsObject& settings, u64 policy_index, RemoteViolationDirective directive, URL::URL resource)
{
    report_a_violation_found_in_another_process(settings, policy_index, directive_name(directive), move(resource));
}

// https://w3c.github.io/webappsec-csp/#should-block-navigation-request
Directives::Directive::Result should_navigation_request_of_type_be_blocked_by_content_security_policy(NonnullRefPtr<Fetch::Infrastructure::Request> navigation_request, Directives::Directive::NavigationType navigation_type, Page& page)
{
    // 1. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    auto client = Fetch::Fetching::resolve_client(navigation_request);

    // 2. For each policy of navigation request’s policy container’s CSP list:
    auto policy_container = navigation_request->policy_container().get<NonnullRefPtr<HTML::PolicyContainer const>>();
    for (auto const& policy : policy_container->csp_list.policies()) {
        // 1. For each directive of policy:
        for (auto const& directive : policy->directives()) {
            // 1. If directive’s pre-navigation check returns "Allowed" when executed upon navigation request, type, and policy skip to the next directive.
            auto directive_result = Directives::pre_navigation_check(directive, navigation_request, navigation_type, policy);
            if (directive_result == Directives::Directive::Result::Allowed)
                continue;

            if (Fetch::Fetching::remote_client(*navigation_request)) {
                report_a_violation_of_a_remote_client(page, *navigation_request, policy, directive.name());
            } else {
                // 2. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on navigation request’s
                //    client’s global object, policy, and directive’s name.
                auto& realm = client->realm();
                auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(client->global_object(), policy, directive.name().view().to_utf8_but_should_be_ported_to_utf16());

                // 3. Set violation’s resource to navigation request’s URL.
                violation->set_resource(navigation_request->url());

                // 4. Execute § 5.5 Report a violation on violation.
                violation->report_a_violation(realm);
            }

            // 5. If policy’s disposition is "enforce", then set result to "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce)
                result = Directives::Directive::Result::Blocked;
        }
    }

    // 3. If result is "Allowed", and if navigation request’s current URL’s scheme is javascript:
    if (result == Directives::Directive::Result::Allowed && navigation_request->current_url().scheme() == "javascript"sv) {
        // 1. For each policy of navigation request’s policy container’s CSP list:
        VERIFY(navigation_request->policy_container().has<NonnullRefPtr<HTML::PolicyContainer const>>());
        auto const& csp_list = navigation_request->policy_container().get<NonnullRefPtr<HTML::PolicyContainer const>>()->csp_list;

        for (auto const& policy : csp_list.policies()) {
            // 1. For each directive of policy:
            for (auto const& directive : policy->directives()) {
                // 1. Let directive-name be the result of executing § 6.8.2 Get the effective directive for inline
                //    checks on type.
                // FIXME: File spec issue that the type should probably always be "navigation", as NavigationType would
                //        cause this algorithm to return null, making directive-name null, then piping directive-name
                //        into a Violation object where the directive name is defined to be a non-empty string.
                //        Other parts of the spec seem to refer to the "navigation" inline type as being for
                //        javascript: URLs. Additionally, this doesn't have an impact on the security decision here,
                //        just which directive is reported to have been violated.
                auto directive_name = Directives::get_the_effective_directive_for_inline_checks(Directives::Directive::InlineType::Navigation);

                // 2. If directive’s inline check returns "Allowed" when executed upon null, "navigation", policy, and
                //    navigation request’s current URL, skip to the next directive.
                // FIXME: File spec issue that current URL is a URL object and not a string, therefore they must use a
                //        spec operation to serialize the URL.
                auto serialized_url = navigation_request->current_url().to_string();
                auto serialized_url_utf16 = Utf16String::from_utf8(serialized_url);
                if (Directives::inline_check(directive, nullptr, Directives::Directive::InlineType::Navigation, policy, serialized_url_utf16.utf16_view()) == Directives::Directive::Result::Allowed)
                    continue;

                if (Fetch::Fetching::remote_client(*navigation_request)) {
                    report_a_violation_of_a_remote_client(page, *navigation_request, policy, directive_name);
                } else {
                    // 3. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global,
                    //    policy, and directive on navigation request’s client’s global object, policy, and directive-name.
                    auto& realm = client->realm();
                    auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(client->global_object(), policy, directive_name.view().to_utf8_but_should_be_ported_to_utf16());

                    // 4. Set violation’s resource to "inline".
                    violation->set_resource(Violation::Resource::Inline);

                    // 5. Execute § 5.5 Report a violation on violation.
                    violation->report_a_violation(realm);
                }

                // 6. If policy’s disposition is "enforce", then set result to "Blocked".
                if (policy->disposition() == Policy::Disposition::Enforce)
                    result = Directives::Directive::Result::Blocked;
            }
        }
    }

    // 4. Return result.
    return result;
}

// https://w3c.github.io/webappsec-csp/#should-block-navigation-response
Directives::Directive::Result should_navigation_response_to_navigation_request_of_type_in_target_be_blocked_by_content_security_policy(
    RefPtr<Fetch::Infrastructure::Request> navigation_request,
    GC::Ref<Fetch::Infrastructure::ResponseView> navigation_response,
    PolicyList const& response_csp_list,
    Directives::Directive::NavigationType navigation_type,
    GC::Ref<HTML::LocalNavigable> target)
{
    // 1. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // FIXME: File spec issue stating that the request can be null (e.g. from a srcdoc resource).
    if (!navigation_request)
        return result;

    auto client = Fetch::Fetching::resolve_client(*navigation_request);

    // NB: Only frame-ancestors reads the target's container documents. Each is the next parent navigable's active
    //     document, and a parent navigable answers for a document hosted in another process.
    auto has_frame_ancestors = any_of(response_csp_list.policies(), [](auto const& policy) {
        return policy->contains_directive_with_name(Directives::Names::FrameAncestors);
    });
    Vector<URL::Origin> target_container_document_origins;
    if (has_frame_ancestors) {
        for (auto ancestor = target->parent(); ancestor; ancestor = ancestor->parent()) {
            // AD-HOC: An ancestor whose document was destroyed has no active document, so its origin counts as opaque.
            auto origin = ancestor->active_document_origin();
            target_container_document_origins.append(origin.has_value() ? origin.release_value() : URL::Origin::create_opaque());
        }
    }

    // 2. For each policy of response CSP list:
    for (auto const& policy : response_csp_list.policies()) {
        // Spec Note: Some directives (like frame-ancestors) allow a response’s Content Security Policy to act on the navigation.
        // 1. For each directive of policy:
        for (auto const& directive : policy->directives()) {
            // 1. If directive’s navigation response check returns "Allowed" when executed upon navigation request, type, navigation response, target, "response", and policy skip to the next directive.
            auto directive_result = Directives::navigation_response_check(directive, *navigation_request, navigation_type, navigation_response->record(), target_container_document_origins, Directives::Directive::CheckType::Response, policy);
            if (directive_result == Directives::Directive::Result::Allowed)
                continue;

            // 2. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on null, policy, and directive’s name.
            // Spec Note: We use null for the global object, as no global exists: we haven’t processed the navigation to create a Document yet.
            // FIXME: What should the realm be here?
            // AD-HOC: Without a client of this process, target's active window's realm reports the violation.
            auto& realm = client ? client->realm() : HTML::relevant_realm(*target->active_window());
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(nullptr, policy, directive.name().view().to_utf8_but_should_be_ported_to_utf16());

            // 3. Set violation’s resource to navigation response’s URL.
            if (navigation_response->url().has_value()) {
                violation->set_resource(navigation_response->url().value());
            } else {
                violation->set_resource(Empty {});
            }

            // 4. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(realm);

            // 5. If policy’s disposition is "enforce", then set result to "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce)
                result = Directives::Directive::Result::Blocked;
        }
    }

    // 3. For each policy of navigation request’s policy container’s CSP list:
    auto request_policy_container = navigation_request->policy_container().get<NonnullRefPtr<HTML::PolicyContainer const>>();
    for (auto const& policy : request_policy_container->csp_list.policies()) {
        // Spec Note: NOTE: Some directives in the navigation request’s context (like frame-ancestors) need the response before acting on the navigation.
        // 1. For each directive of policy:
        for (auto const& directive : policy->directives()) {
            // 1. If directive’s navigation response check returns "Allowed" when executed upon navigation request, type, navigation response, target, "source", and policy skip to the next directive.
            auto directive_result = Directives::navigation_response_check(directive, *navigation_request, navigation_type, navigation_response->record(), target_container_document_origins, Directives::Directive::CheckType::Source, policy);
            if (directive_result == Directives::Directive::Result::Allowed)
                continue;

            // 2. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on navigation request’s client’s global object, policy, and directive’s name.
            // NB: No directive's navigation response check acts on the navigation request's own policies.
            auto& realm = client->realm();
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(client->global_object(), policy, directive.name().view().to_utf8_but_should_be_ported_to_utf16());

            // 3. Set violation’s resource to navigation request’s URL.
            violation->set_resource(navigation_request->url());

            // 4. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(realm);

            // 5. If policy’s disposition is "enforce", then set result to "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce)
                result = Directives::Directive::Result::Blocked;
        }
    }

    // 4. Return result.
    return result;
}

// https://w3c.github.io/webappsec-csp/#should-block-inline
Directives::Directive::Result should_elements_inline_type_behavior_be_blocked_by_content_security_policy(GC::Ref<DOM::Element> element, Directives::Directive::InlineType type, Utf16View source)
{
    // Spec Note: The valid values for type are "script", "script attribute", "style", and "style attribute".
    VERIFY(type == Directives::Directive::InlineType::Script || type == Directives::Directive::InlineType::ScriptAttribute || type == Directives::Directive::InlineType::Style || type == Directives::Directive::InlineType::StyleAttribute);

    // 1. Assert: element is not null.
    // NOTE: Already done by only accepting a GC::Ref.

    // 2. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 3. For each policy of element’s Document's global object’s CSP list:
    auto& settings = element->document().relevant_settings_object();
    auto& global_object = settings.global_object();
    auto const& csp_list = settings.policy_container()->csp_list;

    for (auto const& policy : csp_list.policies()) {
        // 1. For each directive of policy’s directive set:
        for (auto const& directive : policy->directives()) {
            // 1. If directive’s inline check returns "Allowed" when executed upon element, type, policy and source,
            //    skip to the next directive.
            if (Directives::inline_check(directive, element, type, policy, source) == Directives::Directive::Result::Allowed)
                continue;

            // 2. Let directive-name be the result of executing § 6.8.2 Get the effective directive for inline checks
            //    on type.
            auto directive_name = Directives::get_the_effective_directive_for_inline_checks(type);

            // 3. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global,
            //   policy, and directive on the current settings object’s global object, policy, and directive-name.
            // FIXME: File spec issue about using "current settings object" here, as it can run outside of a script
            //        context (for example, a just parsed inline script being prepared)
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(global_object, policy, directive_name.view().to_utf8_but_should_be_ported_to_utf16());

            // 4. Set violation’s resource to "inline".
            violation->set_resource(Violation::Resource::Inline);

            // 5. Set violation’s element to element.
            violation->set_element(element);

            // 6. If directive’s value contains the expression "'report-sample'", then set violation’s sample to the
            //    substring of source containing its first 40 characters.
            // FIXME: Should this be case insensitive?
            auto maybe_report_sample = directive.value().find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case(Directives::KeywordSources::ReportSample.view());
            });

            if (!maybe_report_sample.is_end()) {
                auto sample = source.unicode_substring_view(0, min(source.length_in_code_points(), 40));
                violation->set_sample(Utf16String::from_utf16(sample));
            }

            // 7. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(HTML::relevant_realm(element->document()));

            // 8. If policy’s disposition is "enforce", then set result to "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce) {
                result = Directives::Directive::Result::Blocked;
            }
        }
    }

    // 4. Return result.
    return result;
}

// https://w3c.github.io/webappsec-csp/#can-compile-strings
JS::ThrowCompletionOr<void> ensure_csp_does_not_block_string_compilation(JS::Realm& realm, ReadonlySpan<Utf16String> parameter_strings, Utf16View body_string, Utf16View code_string, JS::CompilationType compilation_type, ReadonlySpan<JS::Value> parameter_args, JS::Value body_arg)
{
    // 1. Let sourceString be codeString.
    auto source_string = Utf16String::from_utf16(code_string);

    // 2. If compilationType is not "TIMER", then:
    if (compilation_type != JS::CompilationType::Timer) {
        // 1. Let compilationSink be "Function" if compilationType is "FUNCTION", and "eval" otherwise.
        auto const compilation_sink = compilation_type == JS::CompilationType::Function ? TrustedTypes::InjectionSink::Function : TrustedTypes::InjectionSink::Eval;

        // 2. Let isTrusted be true if bodyArg implements TrustedScript, and false otherwise.
        auto is_trusted = TrustedTypes::trusted_script_value_matches(body_arg, body_string.to_utf8_but_should_be_ported_to_utf16());

        // 3. If isTrusted is true, then:
        if (is_trusted) {
            // 1. Assert: bodyString is equal to bodyArg’s data.
            // NB: trusted_script_value_matches() also performs the data equality check from this step.

            // 2. Assert: parameterArgs’ size is equal to parameterStrings’ size.
            VERIFY(parameter_args.size() == parameter_strings.size());

            // 3. For each index of the range 0 to parameterArgs’ size:
            //    1. Let arg be parameterArgs[index].
            //    2. If arg implements TrustedScript, then:
            //       1. Assert: parameterStrings[index] is equal to arg’s data.
            //    3. Otherwise, set isTrusted to false.
            Vector<String> utf8_parameter_strings;
            utf8_parameter_strings.ensure_capacity(parameter_strings.size());
            for (auto const& parameter_string : parameter_strings)
                utf8_parameter_strings.append(parameter_string.to_utf8());
            is_trusted = TrustedTypes::trusted_script_values_match(parameter_args, utf8_parameter_strings);
        }

        // 4. If isTrusted is false, then:
        if (!is_trusted) {
            // 1. Set sourceString to the result of executing the get trusted type compliant string algorithm,
            //    with TrustedScript, realm, codeString, compilationSink, and 'script'.
            auto maybe_source_string = TrustedTypes::get_trusted_type_compliant_string(
                TrustedTypes::TrustedTypeName::TrustedScript,
                realm.global_object(),
                source_string,
                compilation_sink,
                TrustedTypes::Script.view());

            // 2. If the algorithm throws an error, throw an EvalError.
            if (maybe_source_string.is_error())
                return realm.vm().throw_completion<JS::EvalError>("Blocked by Content Security Policy"_utf16);
            source_string = maybe_source_string.release_value();

            // 3. If sourceString is not equal to codeString, throw an EvalError.
            if (source_string != code_string)
                return realm.vm().throw_completion<JS::EvalError>("Blocked by Content Security Policy"_utf16);
        }
    }

    // 3. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 4. Let global be realm’s global object.
    auto& global = realm.global_object();

    // 5. For each policy of global’s CSP list:
    auto const& csp_list = HTML::relevant_settings_object(global).policy_container()->csp_list;
    for (auto const& policy : csp_list.policies()) {
        // 1. Let source-list be null.
        Optional<Vector<Utf16String>> maybe_source_list;

        // 2. If policy contains a directive whose name is "script-src", then set source-list to that directive's value.
        auto maybe_script_src = policy->directives().find_if([](auto const& directive) {
            return directive.kind() == Directives::Directive::Kind::ScriptSrc;
        });

        if (!maybe_script_src.is_end()) {
            maybe_source_list = maybe_script_src->value();
        } else {
            //   Otherwise if policy contains a directive whose name is "default-src", then set source-list to that
            //   directive’s value.
            auto maybe_default_src = policy->directives().find_if([](auto const& directive) {
                return directive.kind() == Directives::Directive::Kind::DefaultSrc;
            });

            if (!maybe_default_src.is_end())
                maybe_source_list = maybe_default_src->value();
        }

        // 3. If source-list is not null:
        if (maybe_source_list.has_value()) {
            auto const& source_list = maybe_source_list.value();

            // 1. Let trustedTypesRequired be the result of executing does sink type require trusted types?, with realm, 'script', and false.
            auto const trusted_types_required = TrustedTypes::does_sink_require_trusted_types(global, TrustedTypes::Script.view(), TrustedTypes::IncludeReportOnlyPolicies::No);

            // 2. If trustedTypesRequired is true and source-list contains a source expression which is an ASCII case-insensitive match for the string "'trusted-types-eval'", then skip the following steps.
            if (trusted_types_required && any_of(source_list, [](auto const& value) {
                    return value.equals_ignoring_ascii_case(Directives::KeywordSources::TrustedTypesEval.view());
                }))
                continue;

            // 3. If source-list contains a source expression which is an ASCII case-insensitive match for the string "'unsafe-eval'", then skip the following steps.
            if (any_of(source_list, [](auto const& value) {
                    return value.equals_ignoring_ascii_case(Directives::KeywordSources::UnsafeEval.view());
                }))
                continue;

            // 4. Let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on global, policy, and "script-src".
            auto script_src_string = Directives::Names::ScriptSrc.view().to_utf8_but_should_be_ported_to_utf16();
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(global, policy, script_src_string);

            // 5. Set violation’s resource to "eval".
            violation->set_resource(Violation::Resource::Eval);

            // 6. If source-list contains the expression "'report-sample'", then set violation’s sample to the substring of sourceString containing its first 40 characters.
            // FIXME: Should this be case insensitive?
            auto maybe_report_sample = source_list.find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case(Directives::KeywordSources::ReportSample.view());
            });

            if (!maybe_report_sample.is_end()) {
                auto source_view = source_string.substring_view(0, min(source_string.length_in_code_units(), 40));
                violation->set_sample(Utf16String::from_utf16(source_view));
            }

            // 7. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(realm);

            // 8. If policy’s disposition is "enforce", then set result to "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce)
                result = Directives::Directive::Result::Blocked;
        }
    }

    // 6. If result is "Blocked", throw an EvalError exception.
    if (result == Directives::Directive::Result::Blocked) {
        return realm.vm().throw_completion<JS::EvalError>("Blocked by Content Security Policy"_utf16);
    }

    return {};
}

// https://w3c.github.io/webappsec-csp/#can-compile-wasm-bytes
JS::ThrowCompletionOr<void> ensure_csp_does_not_block_wasm_byte_compilation(JS::Realm& realm)
{
    // 1. Let global be realm’s global object.
    auto& global = realm.global_object();

    // 2. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 3. For each policy of global’s CSP list:
    auto const& csp_list = HTML::relevant_settings_object(global).policy_container()->csp_list;
    for (auto const& policy : csp_list.policies()) {
        // 1. Let source-list be null.
        Optional<Vector<Utf16String>> maybe_source_list;

        // 2. If policy contains a directive whose name is "script-src", then set source-list to that directive's value.
        auto maybe_script_src = policy->directives().find_if([](auto const& directive) {
            return directive.kind() == Directives::Directive::Kind::ScriptSrc;
        });

        if (!maybe_script_src.is_end()) {
            maybe_source_list = maybe_script_src->value();
        } else {
            //   Otherwise if policy contains a directive whose name is "default-src", then set source-list to that
            //   directive’s value.
            auto maybe_default_src = policy->directives().find_if([](auto const& directive) {
                return directive.kind() == Directives::Directive::Kind::DefaultSrc;
            });

            if (!maybe_default_src.is_end())
                maybe_source_list = maybe_default_src->value();
        }

        // 3. If source-list is non-null, and does not contain a source expression which is an ASCII case-insensitive
        //    match for the string "'unsafe-eval'", and does not contain a source expression which is an ASCII
        //    case-insensitive match for the string "'wasm-unsafe-eval'", then:
        if (maybe_source_list.has_value()) {
            auto const& source_list = maybe_source_list.value();

            auto maybe_unsafe_eval = source_list.find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case(Directives::KeywordSources::UnsafeEval.view())
                    || directive_value.equals_ignoring_ascii_case(Directives::KeywordSources::WasmUnsafeEval.view());
            });

            if (maybe_unsafe_eval.is_end()) {
                // 1. Let violation be the result of executing § 2.4.1 Create a violation object for global, policy,
                //    and directive on global, policy, and "script-src".
                auto script_src_string = Directives::Names::ScriptSrc.view().to_utf8_but_should_be_ported_to_utf16();
                auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(global, policy, script_src_string);

                // 2. Set violation’s resource to "wasm-eval".
                violation->set_resource(Violation::Resource::WasmEval);

                // 3. Execute § 5.5 Report a violation on violation.
                violation->report_a_violation(realm);

                // 4. If policy’s disposition is "enforce", then set result to "Blocked".
                if (policy->disposition() == Policy::Disposition::Enforce)
                    result = Directives::Directive::Result::Blocked;
            }
        }
    }

    // 4. If result is "Blocked", throw a WebAssembly.CompileError exception.
    if (result == Directives::Directive::Result::Blocked) {
        return JS::throw_completion(WebAssembly::create_compile_error(realm, "Blocked by Content Security Policy"_utf16));
    }

    return {};
}

// https://w3c.github.io/webappsec-csp/#allow-base-for-document
Directives::Directive::Result is_base_allowed_for_document(URL::URL const& base, GC::Ref<DOM::Document const> document)
{
    // 1. For each policy of document’s global object’s csp list:
    auto const& csp_list = document->relevant_settings_object().policy_container()->csp_list;
    for (auto const& policy : csp_list.policies()) {
        // 1. Let source list be null.
        // NOTE: Not necessary.

        // 2. If a directive whose name is "base-uri" is present in policy’s directive set, set source list to that
        //    directive’s value.
        auto maybe_base_uri = policy->directives().find_if([](auto const& directive) {
            return directive.kind() == Directives::Directive::Kind::BaseUri;
        });

        // 3. If source list is null, skip to the next policy.
        if (maybe_base_uri.is_end())
            continue;

        auto const& source_list = maybe_base_uri->value();

        // 4. If the result of executing § 6.7.2.7 Does url match source list in origin with redirect count? on base,
        //    source list, policy’s self-origin, and 0 is "Does Not Match":
        // Spec Note: We compare against the fallback base URL in order to deal correctly with things like an iframe
        //            srcdoc Document which has been sandboxed into an opaque origin.
        if (Directives::does_url_match_source_list_in_origin_with_redirect_count(base, source_list, policy->self_origin(), 0) == Directives::MatchResult::DoesNotMatch) {
            // 1. Let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and
            //    directive on document’s global object, policy, and "base-uri".
            auto base_uri_string = Directives::Names::BaseUri.view().to_utf8_but_should_be_ported_to_utf16();
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(document->relevant_settings_object().global_object(), policy, base_uri_string);

            // 2. Set violation’s resource to "inline".
            violation->set_resource(Violation::Resource::Inline);

            // 3. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(document->relevant_settings_object().realm());

            // 4. If policy’s disposition is "enforce", return "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce)
                return Directives::Directive::Result::Blocked;
        }
    }

    // 2. Return "Allowed".
    return Directives::Directive::Result::Allowed;
}

}
