/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/KeywordSources.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/SRI/SRI.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::ContentSecurityPolicy {

// https://w3c.github.io/webappsec-csp/#does-resource-hint-violate-policy
[[nodiscard]] static GC::Ptr<Directives::Directive> does_resource_hint_request_violate_policy(GC::Heap& heap, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Policy const> policy)
{
    // 1. Let defaultDirective be policy’s first directive whose name is "default-src".
    auto default_directive_iterator = policy->directives().find_if([](auto const& directive) {
        return directive->name() == Directives::Names::DefaultSrc;
    });

    // 2. If defaultDirective does not exist, return "Does Not Violate".
    if (default_directive_iterator.is_end())
        return {};

    // 3. For each directive of policy:
    for (auto directive : policy->directives()) {
        // 1. Let result be the result of executing directive’s pre-request check on request and policy.
        auto result = directive->pre_request_check(heap, request, policy);

        // 2. If result is "Allowed", then return "Does Not Violate".
        if (result == Directives::Directive::Result::Allowed) {
            return {};
        }
    }

    // 4. Return defaultDirective.
    return *default_directive_iterator;
}

// https://w3c.github.io/webappsec-csp/#does-request-violate-policy
[[nodiscard]] static GC::Ptr<Directives::Directive> does_request_violate_policy(GC::Heap& heap, GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Policy const> policy)
{
    // 1. If request’s initiator is "prefetch", then return the result of executing § 6.7.2.2 Does resource hint
    //    request violate policy? on request and policy.
    if (request->initiator() == Fetch::Infrastructure::Request::Initiator::Prefetch)
        return does_resource_hint_request_violate_policy(heap, request, policy);

    // 2. Let violates be "Does Not Violate".
    GC::Ptr<Directives::Directive> violates;

    // 3. For each directive of policy:
    for (auto directive : policy->directives()) {
        // 1. Let result be the result of executing directive’s pre-request check on request and policy.
        auto result = directive->pre_request_check(heap, request, policy);

        // 2. If result is "Blocked", then let violates be directive.
        if (result == Directives::Directive::Result::Blocked) {
            violates = directive;
        }
    }

    // 4. Return violates.
    return violates;
}

// https://w3c.github.io/webappsec-csp/#report-for-request
void report_content_security_policy_violations_for_request(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request> request)
{
    // 1. Let CSP list be request’s policy container's CSP list.
    auto csp_list = request->policy_container().get<GC::Ref<HTML::PolicyContainer>>()->csp_list;

    // 2. For each policy of CSP list:
    for (auto policy : csp_list->policies()) {
        // 1. If policy’s disposition is "enforce", then skip to the next policy.
        if (policy->disposition() == Policy::Disposition::Enforce)
            continue;

        // 2. Let violates be the result of executing § 6.7.2.1 Does request violate policy? on request and policy.
        auto violates = does_request_violate_policy(realm.heap(), request, policy);

        // 3. If violates is not "Does Not Violate", then execute § 5.5 Report a violation on the result of executing
        //    § 2.4.2 Create a violation object for request, and policy. on request, and policy.
        if (violates) {
            auto violation = Violation::create_a_violation_object_for_request_and_policy(realm, request, policy);
            violation->report_a_violation(realm);
        }
    }
}

// https://w3c.github.io/webappsec-csp/#should-block-request
Directives::Directive::Result should_request_be_blocked_by_content_security_policy(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request> request)
{
    // 1. Let CSP list be request’s policy container's CSP list.
    auto csp_list = request->policy_container().get<GC::Ref<HTML::PolicyContainer>>()->csp_list;

    // 2. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 3. For each policy of CSP list:
    for (auto policy : csp_list->policies()) {
        // 1. If policy’s disposition is "report", then skip to the next policy.
        if (policy->disposition() == Policy::Disposition::Report)
            continue;

        // 2. Let violates be the result of executing § 6.7.2.1 Does request violate policy? on request and policy.
        auto violates = does_request_violate_policy(realm.heap(), request, policy);

        // 3. If violates is not "Does Not Violate", then:
        if (violates) {
            // 1. Execute § 5.5 Report a violation on the result of executing § 2.4.2 Create a violation object for
            //    request, and policy. on request, and policy.
            auto violation = Violation::create_a_violation_object_for_request_and_policy(realm, request, policy);
            violation->report_a_violation(realm);

            // 2. Set result to "Blocked".
            result = Directives::Directive::Result::Blocked;
        }
    }

    // 4. Return result.
    return result;
}

// https://w3c.github.io/webappsec-subresource-integrity/#should-request-be-blocked-by-integrity-policy
Directives::Directive::Result should_request_be_blocked_by_integrity_policy(GC::Ref<Fetch::Infrastructure::Request> request)
{
    VERIFY(request->policy_container().has<GC::Ref<HTML::PolicyContainer>>());

    // 1. Let policyContainer be request’s policy container.
    auto const& policy_container = request->policy_container().get<GC::Ref<HTML::PolicyContainer>>();

    // 2. Let parsedMetadata be the result of calling parse metadata with request’s integrity metadata.
    auto parsed_metadata = MUST(SRI::parse_metadata(request->integrity_metadata()));

    // 3. If parsedMetadata is not the empty set and request’s mode is either "cors" or "same-origin", return "Allowed".
    if (!parsed_metadata.is_empty() && (request->mode() == Fetch::Infrastructure::Request::Mode::CORS || request->mode() == Fetch::Infrastructure::Request::Mode::SameOrigin))
        return Directives::Directive::Result::Allowed;

    // 4. If request’s url is local, return "Allowed".
    if (Fetch::Infrastructure::is_local_url(request->url()))
        return Directives::Directive::Result::Allowed;

    // 5. Let policy be policyContainer’s integrity policy.
    auto const& policy = policy_container->integrity_policy;

    // 6. Let reportPolicy be policyContainer’s report only integrity policy.
    auto const& report_policy = policy_container->report_only_integrity_policy;

    // 7. If both policy and reportPolicy are empty integrity policys, return "Allowed".
    if (policy.is_empty() && report_policy.is_empty())
        return Directives::Directive::Result::Allowed;

    // 8. Let global be request’s client’s global object.
    auto& global = request->client()->global_object();

    // 9. If global is not a Window nor a WorkerGlobalScope, return "Allowed".
    if (!is<HTML::Window>(global) && !is<HTML::WorkerGlobalScope>(global))
        return Directives::Directive::Result::Allowed;

    // 10. Let block be a boolean, initially false.
    bool block = false;

    // FIXME: 11. Let reportBlock be a boolean, initially false.
    [[maybe_unused]] auto report_block = false;

    // 12. If policy’s sources contains "inline" and policy’s blocked destinations contains request’s destination, set block to true.
    if (policy.sources.contains_slow("inline"sv)
        && request->destination().has_value()
        && policy.blocked_destinations.contains_slow(request->destination().value()))
        block = true;

    // 13. If reportPolicy’s sources contains "inline" and reportPolicy’s blocked destinations contains request’s destination, set reportBlock to true.
    if (report_policy.sources.contains_slow("inline"sv)
        && request->destination().has_value()
        && report_policy.blocked_destinations.contains_slow(request->destination().value()))
        report_block = true;

    // FIXME: 14. If block is true or reportBlock is true, then report violation with request, block, reportBlock, policy and reportPolicy.

    // 15. If block is true, then return "Blocked"; otherwise "Allowed".
    return block ? Directives::Directive::Result::Blocked : Directives::Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#should-block-response
Directives::Directive::Result should_response_to_request_be_blocked_by_content_security_policy(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Response> response, GC::Ref<Fetch::Infrastructure::Request> request)
{
    // 1. Let CSP list be request’s policy container's CSP list.
    auto csp_list = request->policy_container().get<GC::Ref<HTML::PolicyContainer>>()->csp_list;

    // 2. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 3. For each policy of CSP list:
    // Spec Note: This portion of the check verifies that the page can load the response. That is, that a Service
    //            Worker hasn't substituted a file which would violate the page’s CSP.
    for (auto policy : csp_list->policies()) {
        // 1. For each directive of policy:
        for (auto directive : policy->directives()) {
            // 1. If the result of executing directive’s post-request check is "Blocked", then:
            if (directive->post_request_check(realm.heap(), request, response, policy) == Directives::Directive::Result::Blocked) {
                // 1. Execute § 5.5 Report a violation on the result of executing § 2.4.2 Create a violation object for
                //    request, and policy. on request, and policy.
                auto violation = Violation::create_a_violation_object_for_request_and_policy(realm, request, policy);
                violation->report_a_violation(realm);

                // 2. If policy’s disposition is "enforce", then set result to "Blocked".
                if (policy->disposition() == Policy::Disposition::Enforce) {
                    result = Directives::Directive::Result::Blocked;
                }
            }
        }
    }

    // 4. Return result.
    return result;
}

// https://w3c.github.io/webappsec-csp/#should-block-navigation-request
Directives::Directive::Result should_navigation_request_of_type_be_blocked_by_content_security_policy(GC::Ref<Fetch::Infrastructure::Request> navigation_request, Directives::Directive::NavigationType navigation_type)
{
    // 1. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 2. For each policy of navigation request’s policy container’s CSP list:
    auto policy_container = navigation_request->policy_container().get<GC::Ref<HTML::PolicyContainer>>();
    for (auto policy : policy_container->csp_list->policies()) {
        // 1. For each directive of policy:
        for (auto directive : policy->directives()) {
            // 1. If directive’s pre-navigation check returns "Allowed" when executed upon navigation request, type, and policy skip to the next directive.
            auto directive_result = directive->pre_navigation_check(navigation_request, navigation_type, policy);
            if (directive_result == Directives::Directive::Result::Allowed)
                continue;

            // 2. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on navigation request’s
            //    client’s global object, policy, and directive’s name.
            auto& realm = navigation_request->client()->realm();
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, navigation_request->client()->global_object(), policy, directive->name());

            // 3. Set violation’s resource to navigation request’s URL.
            violation->set_resource(navigation_request->url());

            // 4. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(realm);

            // 5. If policy’s disposition is "enforce", then set result to "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce)
                result = Directives::Directive::Result::Blocked;
        }
    }

    // 3. If result is "Allowed", and if navigation request’s current URL’s scheme is javascript:
    if (result == Directives::Directive::Result::Allowed && navigation_request->current_url().scheme() == "javascript"sv) {
        // 1. For each policy of navigation request’s policy container’s CSP list:
        VERIFY(navigation_request->policy_container().has<GC::Ref<HTML::PolicyContainer>>());
        auto csp_list = navigation_request->policy_container().get<GC::Ref<HTML::PolicyContainer>>()->csp_list;

        for (auto policy : csp_list->policies()) {
            // 1. For each directive of policy:
            for (auto directive : policy->directives()) {
                // 1. Let directive-name be the result of executing § 6.8.2 Get the effective directive for inline
                //    checks on type.
                // FIXME: File spec issue that the type should probably always be "navigation", as NavigationType would
                //        cause this algorithm to return null, making directive-name null, then piping directive-name
                //        into a Violation object where the directive name is defined to be a non-empty string.
                //        Other parts of the spec seem to refer to the "navigation" inline type as being for
                //        javascript: URLs. Additionally, this doesn't have an impact on the security decision here,
                //        just which directive is reported to have been violated.
                auto directive_name = Directives::get_the_effective_directive_for_inline_checks(Directives::Directive::InlineType::Navigation);

                // 2. If directive’s inline check returns "Allowed" when executed upon null, "navigation" and
                //    navigation request’s current URL, skip to the next directive.
                // FIXME: File spec issue that they forgot to pass in "policy" here.
                // FIXME: File spec issue that current URL is a URL object and not a string, therefore they must use a
                //        spec operation to serialize the URL.
                auto& realm = navigation_request->client()->realm();
                auto serialized_url = navigation_request->current_url().to_string();
                if (directive->inline_check(realm.heap(), nullptr, Directives::Directive::InlineType::Navigation, policy, serialized_url) == Directives::Directive::Result::Allowed)
                    continue;

                // 3. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global,
                //    policy, and directive on navigation request’s client’s global object, policy, and directive-name.
                auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, navigation_request->client()->global_object(), policy, directive_name.to_string());

                // 4. Set violation’s resource to navigation request’s URL.
                violation->set_resource(navigation_request->url());

                // 5. Execute § 5.5 Report a violation on violation.
                violation->report_a_violation(realm);

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
    GC::Ptr<Fetch::Infrastructure::Request> navigation_request,
    GC::Ref<Fetch::Infrastructure::Response> navigation_response,
    GC::Ref<PolicyList> response_csp_list,
    Directives::Directive::NavigationType navigation_type,
    GC::Ref<HTML::Navigable> target)
{
    // 1. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // FIXME: File spec issue stating that the request can be null (e.g. from a srcdoc resource).
    if (!navigation_request) {
        dbgln("FIXME: Handle null navigation_request in navigation response Content Security Policy check.");
        return result;
    }

    // 2. For each policy of response CSP list:
    for (auto policy : response_csp_list->policies()) {
        // Spec Note: Some directives (like frame-ancestors) allow a response’s Content Security Policy to act on the navigation.
        // 1. For each directive of policy:
        for (auto directive : policy->directives()) {
            // 1. If directive’s navigation response check returns "Allowed" when executed upon navigation request, type, navigation response, target, "response", and policy skip to the next directive.
            auto directive_result = directive->navigation_response_check(*navigation_request, navigation_type, navigation_response, target, Directives::Directive::CheckType::Response, policy);
            if (directive_result == Directives::Directive::Result::Allowed)
                continue;

            // 2. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on null, policy, and directive’s name.
            // Spec Note: We use null for the global object, as no global exists: we haven’t processed the navigation to create a Document yet.
            // FIXME: What should the realm be here?
            auto& realm = navigation_request->client()->realm();
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, nullptr, policy, directive->name());

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
    auto request_policy_container = navigation_request->policy_container().get<GC::Ref<HTML::PolicyContainer>>();
    for (auto policy : request_policy_container->csp_list->policies()) {
        // Spec Note: NOTE: Some directives in the navigation request’s context (like frame-ancestors) need the response before acting on the navigation.
        // 1. For each directive of policy:
        for (auto directive : policy->directives()) {
            // 1. If directive’s navigation response check returns "Allowed" when executed upon navigation request, type, navigation response, target, "source", and policy skip to the next directive.
            auto directive_result = directive->navigation_response_check(*navigation_request, navigation_type, navigation_response, target, Directives::Directive::CheckType::Source, policy);
            if (directive_result == Directives::Directive::Result::Allowed)
                continue;

            // 2. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on navigation request’s client’s global object, policy, and directive’s name.
            auto& realm = navigation_request->client()->realm();
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, navigation_request->client()->global_object(), policy, directive->name());

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
Directives::Directive::Result should_elements_inline_type_behavior_be_blocked_by_content_security_policy(JS::Realm& realm, GC::Ref<DOM::Element> element, Directives::Directive::InlineType type, String const& source)
{
    // Spec Note: The valid values for type are "script", "script attribute", "style", and "style attribute".
    VERIFY(type == Directives::Directive::InlineType::Script || type == Directives::Directive::InlineType::ScriptAttribute || type == Directives::Directive::InlineType::Style || type == Directives::Directive::InlineType::StyleAttribute);

    // 1. Assert: element is not null.
    // NOTE: Already done by only accepting a GC::Ref.

    // 2. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 3. For each policy of element’s Document's global object’s CSP list:
    auto& global_object = element->document().realm().global_object();
    auto csp_list = PolicyList::from_object(global_object);
    VERIFY(csp_list);

    for (auto const policy : csp_list->policies()) {
        // 1. For each directive of policy’s directive set:
        for (auto const directive : policy->directives()) {
            // 1. If directive’s inline check returns "Allowed" when executed upon element, type, policy and source,
            //    skip to the next directive.
            if (directive->inline_check(realm.heap(), element, type, policy, source) == Directives::Directive::Result::Allowed)
                continue;

            // 2. Let directive-name be the result of executing § 6.8.2 Get the effective directive for inline checks
            //    on type.
            auto directive_name = Directives::get_the_effective_directive_for_inline_checks(type);

            // 3. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global,
            //   policy, and directive on the current settings object’s global object, policy, and directive-name.
            // FIXME: File spec issue about using "current settings object" here, as it can run outside of a script
            //        context (for example, a just parsed inline script being prepared)
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, global_object, policy, directive_name.to_string());

            // 4. Set violation’s resource to "inline".
            violation->set_resource(Violation::Resource::Inline);

            // 5. Set violation’s element to element.
            violation->set_element(element);

            // 6. If directive’s value contains the expression "'report-sample'", then set violation’s sample to the
            //    substring of source containing its first 40 characters.
            // FIXME: Should this be case insensitive?
            auto maybe_report_sample = directive->value().find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case(Directives::KeywordSources::ReportSample);
            });

            if (!maybe_report_sample.is_end()) {
                Utf8View source_view { source };
                auto sample = source_view.unicode_substring_view(0, min(source_view.length(), 40));
                violation->set_sample(String::from_utf8_without_validation(sample.as_string().bytes()));
            }

            // 7. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(realm);

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
JS::ThrowCompletionOr<void> ensure_csp_does_not_block_string_compilation(JS::Realm& realm, ReadonlySpan<String> parameter_strings, StringView body_string, StringView code_string, JS::CompilationType compilation_type, ReadonlySpan<JS::Value> parameter_args, JS::Value body_arg)
{
    Utf16String source_string;

    // 1. If compilationType is "TIMER", then:
    if (compilation_type == JS::CompilationType::Timer) {
        // 1. Let sourceString be codeString.
        source_string = Utf16String::from_utf8(code_string);
    }
    // 2. Else:
    else {
        // 1. Let compilationSink be "Function" if compilationType is "FUNCTION", and "eval" otherwise.
        auto const compilation_sink = compilation_type == JS::CompilationType::Function ? TrustedTypes::InjectionSink::Function : TrustedTypes::InjectionSink::Eval;

        // 2. Let isTrusted be true if bodyArg implements TrustedScript, and false otherwise.
        auto is_trusted = body_arg.is<TrustedTypes::TrustedScript>();

        // 3. If isTrusted is true then:
        if (is_trusted) {
            // 1. If bodyString is not equal to bodyArg’s data, set isTrusted to false.
            if (body_string != as<TrustedTypes::TrustedScript>(body_arg.as_object()).to_string())
                is_trusted = false;
        }

        // 4. If isTrusted is true, then:
        if (is_trusted) {
            // 1. Assert: parameterArgs’ [list/size=] is equal to [parameterStrings]' size.
            VERIFY(parameter_args.size() == parameter_strings.size());

            // 2. For each index of the range 0 to |parameterArgs]' [list/size=]:
            for (size_t i = 0; i < parameter_args.size(); i++) {
                // 1. Let arg be parameterArgs[index].
                auto const& arg = parameter_args[i];

                // 2. If arg implements TrustedScript, then:
                if (auto trusted_script = arg.as_if<TrustedTypes::TrustedScript>()) {
                    // 1. if parameterStrings[index] is not equal to arg’s data, set isTrusted to false.
                    if (parameter_strings[i] != trusted_script->to_string()) {
                        is_trusted = false;
                        break;
                    }
                }
                // 3. Otherwise, set isTrusted to false.
                else {
                    is_trusted = false;
                    break;
                }
            }
        }

        // 5. Let sourceToValidate be a new TrustedScript object created in realm whose data is set to codeString
        //    if isTrusted is true, and codeString otherwise.
        auto const source_to_validate = is_trusted
            ? TrustedTypes::TrustedScriptOrString(realm.create<TrustedTypes::TrustedScript>(realm, Utf16String::from_utf8(code_string)))
            : Utf16String::from_utf8(code_string);

        // 6. Let sourceString be the result of executing the Get Trusted Type compliant string algorithm,
        //    with TrustedScript, realm, sourceToValidate, compilationSink, and 'script'.
        auto maybe_source_string = TrustedTypes::get_trusted_type_compliant_string(
            TrustedTypes::TrustedTypeName::TrustedScript,
            realm.global_object(),
            source_to_validate,
            compilation_sink,
            TrustedTypes::Script.to_string());

        // 7. If the algorithm throws an error, throw an EvalError.
        if (maybe_source_string.is_error()) {
            return realm.vm().throw_completion<JS::EvalError>("Blocked by Content Security Policy"sv);
        }
        source_string = maybe_source_string.release_value();

        // 8. If sourceString is not equal to codeString, throw an EvalError.
        if (source_string != code_string)
            return realm.vm().throw_completion<JS::EvalError>("Blocked by Content Security Policy"sv);
    }

    // 3. Let result be "Allowed".
    auto result = Directives::Directive::Result::Allowed;

    // 4. Let global be realm’s global object.
    auto& global = realm.global_object();

    // 5. For each policy of global’s CSP list:
    auto csp_list = PolicyList::from_object(global);
    VERIFY(csp_list);
    for (auto const policy : csp_list->policies()) {
        // 1. Let source-list be null.
        Optional<Vector<String>> maybe_source_list;

        // 2. If policy contains a directive whose name is "script-src", then set source-list to that directive's value.
        auto maybe_script_src = policy->directives().find_if([](auto const& directive) {
            return directive->name() == Directives::Names::ScriptSrc;
        });

        if (!maybe_script_src.is_end()) {
            maybe_source_list = (*maybe_script_src)->value();
        } else {
            //   Otherwise if policy contains a directive whose name is "default-src", then set source-list to that
            //   directive’s value.
            auto maybe_default_src = policy->directives().find_if([](auto const& directive) {
                return directive->name() == Directives::Names::DefaultSrc;
            });

            if (!maybe_default_src.is_end())
                maybe_source_list = (*maybe_default_src)->value();
        }

        // 3. If source-list is not null, and does not contain a source expression which is an ASCII case-insensitive
        //    match for the string "'unsafe-eval'", then:
        if (maybe_source_list.has_value()) {
            auto const& source_list = maybe_source_list.value();

            auto maybe_unsafe_eval = source_list.find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case(Directives::KeywordSources::UnsafeEval);
            });

            if (maybe_unsafe_eval.is_end()) {
                // 1. Let violation be the result of executing § 2.4.1 Create a violation object for global, policy,
                //    and directive on global, policy, and "script-src".
                auto script_src_string = Directives::Names::ScriptSrc.to_string();
                auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, global, policy, script_src_string);

                // 2. Set violation’s resource to "eval".
                violation->set_resource(Violation::Resource::Eval);

                // 3. If source-list contains the expression "'report-sample'", then set violation’s sample to the
                //    substring of sourceString containing its first 40 characters.
                // FIXME: Should this be case insensitive?
                auto maybe_report_sample = source_list.find_if([](auto const& directive_value) {
                    return directive_value.equals_ignoring_ascii_case(Directives::KeywordSources::ReportSample);
                });

                if (!maybe_report_sample.is_end()) {
                    auto source_view = source_string.substring_view(0, min(source_string.length_in_code_units(), 40));
                    violation->set_sample(source_view.to_utf8_but_should_be_ported_to_utf16());
                }

                // 4. Execute § 5.5 Report a violation on violation.
                violation->report_a_violation(realm);

                // 5. If policy’s disposition is "enforce", then set result to "Blocked".
                if (policy->disposition() == Policy::Disposition::Enforce)
                    result = Directives::Directive::Result::Blocked;
            }
        }
    }

    // 6. If result is "Blocked", throw an EvalError exception.
    if (result == Directives::Directive::Result::Blocked) {
        return realm.vm().throw_completion<JS::EvalError>("Blocked by Content Security Policy"sv);
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
    auto csp_list = PolicyList::from_object(global);
    VERIFY(csp_list);
    for (auto const policy : csp_list->policies()) {
        // 1. Let source-list be null.
        Optional<Vector<String>> maybe_source_list;

        // 2. If policy contains a directive whose name is "script-src", then set source-list to that directive's value.
        auto maybe_script_src = policy->directives().find_if([](auto const& directive) {
            return directive->name() == Directives::Names::ScriptSrc;
        });

        if (!maybe_script_src.is_end()) {
            maybe_source_list = (*maybe_script_src)->value();
        } else {
            //   Otherwise if policy contains a directive whose name is "default-src", then set source-list to that
            //   directive’s value.
            auto maybe_default_src = policy->directives().find_if([](auto const& directive) {
                return directive->name() == Directives::Names::DefaultSrc;
            });

            if (!maybe_default_src.is_end())
                maybe_source_list = (*maybe_default_src)->value();
        }

        // 3. If source-list is non-null, and does not contain a source expression which is an ASCII case-insensitive
        //    match for the string "'unsafe-eval'", and does not contain a source expression which is an ASCII
        //    case-insensitive match for the string "'wasm-unsafe-eval'", then:
        if (maybe_source_list.has_value()) {
            auto const& source_list = maybe_source_list.value();

            auto maybe_unsafe_eval = source_list.find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case(Directives::KeywordSources::UnsafeEval)
                    || directive_value.equals_ignoring_ascii_case(Directives::KeywordSources::WasmUnsafeEval);
            });

            if (maybe_unsafe_eval.is_end()) {
                // 1. Let violation be the result of executing § 2.4.1 Create a violation object for global, policy,
                //    and directive on global, policy, and "script-src".
                auto script_src_string = Directives::Names::ScriptSrc.to_string();
                auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, global, policy, script_src_string);

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
        return realm.vm().throw_completion<WebAssembly::CompileError>("Blocked by Content Security Policy"sv);
    }

    return {};
}

// https://w3c.github.io/webappsec-csp/#allow-base-for-document
Directives::Directive::Result is_base_allowed_for_document(JS::Realm& realm, URL::URL const& base, GC::Ref<DOM::Document const> document)
{
    // 1. For each policy of document’s global object’s csp list:
    auto csp_list = PolicyList::from_object(document->realm().global_object());
    VERIFY(csp_list);
    for (auto const policy : csp_list->policies()) {
        // 1. Let source list be null.
        // NOTE: Not necessary.

        // 2. If a directive whose name is "base-uri" is present in policy’s directive set, set source list to that
        //    directive’s value.
        auto maybe_base_uri = policy->directives().find_if([](auto const& directive) {
            return directive->name() == Directives::Names::BaseUri;
        });

        // 3. If source list is null, skip to the next policy.
        if (maybe_base_uri.is_end())
            continue;

        auto const& source_list = (*maybe_base_uri)->value();

        // 4. If the result of executing § 6.7.2.7 Does url match source list in origin with redirect count? on base,
        //    source list, policy’s self-origin, and 0 is "Does Not Match":
        // Spec Note: We compare against the fallback base URL in order to deal correctly with things like an iframe
        //            srcdoc Document which has been sandboxed into an opaque origin.
        if (Directives::does_url_match_source_list_in_origin_with_redirect_count(base, source_list, policy->self_origin(), 0) == Directives::MatchResult::DoesNotMatch) {
            // 1. Let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and
            //    directive on document’s global object, policy, and "base-uri".
            auto base_uri_string = Directives::Names::BaseUri.to_string();
            auto violation = Violation::create_a_violation_object_for_global_policy_and_directive(realm, document->window(), policy, base_uri_string);

            // 2. Set violation’s resource to "inline".
            violation->set_resource(Violation::Resource::Inline);

            // 3. Execute § 5.5 Report a violation on violation.
            violation->report_a_violation(realm);

            // 4. If policy’s disposition is "enforce", return "Blocked".
            if (policy->disposition() == Policy::Disposition::Enforce)
                return Directives::Directive::Result::Blocked;
        }
    }

    // 2. Return "Allowed".
    return Directives::Directive::Result::Allowed;
}

}
