/*
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Origin.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/HTMLIntegration.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>
#include <LibWebCommon/HTML/SandboxingFlagSet.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#default-src-inline
static Directive::Result default_src_inline_check(Directive const& directive, GC::Ptr<DOM::Element const> element, Directive::InlineType type, Policy const& policy, Utf16View source)
{
    // 1. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, default-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::DefaultSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. Otherwise, return the result of executing the inline check for the directive whose name is name on element,
    //    type, policy and source, using this directive’s value for the comparison.
    return inline_check(Directive::create(name, directive.value()), element, type, policy, source);
}

// https://w3c.github.io/webappsec-csp/#script-src-attr-inline
static Directive::Result script_src_attr_inline_check(Directive const& directive, GC::Ptr<DOM::Element const> element, Directive::InlineType type, Policy const& policy, Utf16View source)
{
    // 1. Assert: element is not null or type is "navigation".
    VERIFY(element || type == Directive::InlineType::Navigation);

    // 2. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 3. If the result of executing § 6.8.4 Should fetch directive execute on name, script-src-attr and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ScriptSrcAttr, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 4. If the result of executing § 6.7.3.3 Does element match source list for type and source? on element, this
    //    directive’s value, type, and source is "Does Not Match", return "Blocked".
    if (does_element_match_source_list_for_type_and_source(element, directive.value(), type, source) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 5. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#script-src-inline
static Directive::Result script_src_inline_check(Directive const& directive, GC::Ptr<DOM::Element const> element, Directive::InlineType type, Policy const& policy, Utf16View source)
{
    // 1. Assert: element is not null or type is "navigation".
    VERIFY(element || type == Directive::InlineType::Navigation);

    // 2. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 3. If the result of executing § 6.8.4 Should fetch directive execute on name, script-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ScriptSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 4. If the result of executing § 6.7.3.3 Does element match source list for type and source? on element, this
    //    directive’s value, type, and source, is "Does Not Match", return "Blocked".
    if (does_element_match_source_list_for_type_and_source(element, directive.value(), type, source) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 5. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#script-src-elem-inline
static Directive::Result script_src_elem_inline_check(Directive const& directive, GC::Ptr<DOM::Element const> element, Directive::InlineType type, Policy const& policy, Utf16View source)
{
    // 1. Assert: element is not null or type is "navigation".
    VERIFY(element || type == Directive::InlineType::Navigation);

    // 2. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 3. If the result of executing § 6.8.4 Should fetch directive execute on name, script-src-elem and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ScriptSrcElem, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 4. If the result of executing § 6.7.3.3 Does element match source list for type and source? on element, this
    //    directive’s value, type, and source is "Does Not Match", return "Blocked".
    if (does_element_match_source_list_for_type_and_source(element, directive.value(), type, source) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 5. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#style-src-attr-inline
static Directive::Result style_src_attr_inline_check(Directive const& directive, GC::Ptr<DOM::Element const> element, Directive::InlineType type, Policy const& policy, Utf16View source)
{
    // 1. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src-attr and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrcAttr, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.3.3 Does element match source list for type and source? on element, this
    //    directive’s value, type, and source, is "Does Not Match", return "Blocked".
    if (does_element_match_source_list_for_type_and_source(element, directive.value(), type, source) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#style-src-inline
static Directive::Result style_src_inline_check(Directive const& directive, GC::Ptr<DOM::Element const> element, Directive::InlineType type, Policy const& policy, Utf16View source)
{
    // 1. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.3.3 Does element match source list for type and source? on element, this
    //    directive’s value, type, and source, is "Does Not Match", return "Blocked".
    if (does_element_match_source_list_for_type_and_source(element, directive.value(), type, source) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#style-src-elem-inline
static Directive::Result style_src_elem_inline_check(Directive const& directive, GC::Ptr<DOM::Element const> element, Directive::InlineType type, Policy const& policy, Utf16View source)
{
    // 1. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src-elem and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrcElem, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.3.3 Does element match source list for type and source? on element, this
    //    directive’s value, type, and source, is "Does Not Match", return "Blocked".
    if (does_element_match_source_list_for_type_and_source(element, directive.value(), type, source) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#sandbox-init
static Directive::Result sandbox_initialization(Directive const& directive, Variant<GC::Ref<DOM::Document const>, GC::Ref<HTML::WorkerGlobalScope const>> context, Policy const& policy)
{
    // 1. If policy’s disposition is not "enforce", or context is not a WorkerGlobalScope, then abort this algorithm.
    // FIXME: File spec issue that this step doesn't specify the return value. It must be allowed, because Document
    //        asserts that the result of this algorithm is Allowed.
    if (policy.disposition() != Policy::Disposition::Enforce || !context.has<GC::Ref<HTML::WorkerGlobalScope const>>())
        return Directive::Result::Allowed;

    // 2. Let sandboxing flag set be a new sandboxing flag set.
    // 3. Parse a sandboxing directive using this directive’s value as the input, and sandboxing flag set as the output.
    // FIXME: File spec issue that "parse a sandboxing directive" does not accept a set of tokens.
    auto sandboxing_flag_set = HTML::parse_a_sandboxing_directive(directive.value());

    // 4. If sandboxing flag set contains either the sandboxed scripts browsing context flag or the sandboxed origin
    //    browsing context flag flags, return "Blocked".
    // Spec Note: This will need to change if we allow Workers to be sandboxed into unique origins, which seems like a
    //            pretty reasonable thing to do.
    if (has_flag(sandboxing_flag_set, HTML::SandboxingFlagSet::SandboxedScripts) || has_flag(sandboxing_flag_set, HTML::SandboxingFlagSet::SandboxedOrigin))
        return Directive::Result::Blocked;

    // 5. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#form-action-pre-navigate
static Directive::Result form_action_pre_navigation_check(Directive const& directive, Fetch::Infrastructure::Request& request, Directive::NavigationType navigation_type, Policy const& policy)
{
    // 1. Assert: policy is unused in this algorithm.
    // FIXME: File spec issue, because this is not the case. The policy is required to resolve 'self'.

    // 2. If navigation type is "form-submission":
    if (navigation_type == Directive::NavigationType::FormSubmission) {
        // 1. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
        //    and a policy, is "Does Not Match", return "Blocked".
        if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
            return Directive::Result::Blocked;
    }

    // 3. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#frame-ancestors-navigation-response
static Directive::Result frame_ancestors_navigation_response_check(Directive const& directive, Fetch::Infrastructure::Response const& navigation_response, ReadonlySpan<URL::Origin> target_container_document_origins, Directive::CheckType check_type, Policy const& policy)
{
    // 1. If navigation response’s URL is local, return "Allowed".
    VERIFY(navigation_response.url().has_value());
    if (Fetch::Infrastructure::is_local_url(navigation_response.url().value()))
        return Directive::Result::Allowed;

    // 2. Assert: request, navigation response, and navigation type, are unused from this point forward in this
    //    algorithm, as frame-ancestors is concerned only with navigation response’s frame-ancestors directive.

    // 3. If check type is "source", return "Allowed".
    // Spec Note: The 'frame-ancestors' directive is relevant only to the target navigable and it has no impact on the
    //            request’s context.
    if (check_type == Directive::CheckType::Source)
        return Directive::Result::Allowed;

    // 4. If target is not a child navigable, return "Allowed".
    if (target_container_document_origins.is_empty())
        return Directive::Result::Allowed;

    // 5. Let current be target.
    // 6. While current is a child navigable:
    //     1. Let document be current’s container document.
    //     4. Set current to document’s node navigable.
    for (auto const& document_origin : target_container_document_origins) {
        // 2. Let origin be the result of executing the URL parser on the ASCII serialization of document’s origin.
        auto serialized_origin = document_origin.serialize();
        auto origin = DOMURL::parse_from_byte_string(serialized_origin.bytes_as_string_view());

        // AD-HOC: If the origin is opaque, serialization produces "null" which fails URL parsing.
        //         All major engines block in this case, as an opaque origin can never match any source expression.
        if (!origin.has_value())
            return Directive::Result::Blocked;

        // 3. If § 6.7.2.7 Does url match source list in origin with redirect count? returns Does Not Match when
        //    executed upon origin, this directive’s value, policy’s self-origin, and 0, return "Blocked".
        if (does_url_match_source_list_in_origin_with_redirect_count(origin.value(), directive.value(), policy.self_origin(), 0) == MatchResult::DoesNotMatch)
            return Directive::Result::Blocked;
    }

    // 7. Return "Allowed".
    return Directive::Result::Allowed;
}

Directive::Result inline_check(Directive const& directive, GC::Ptr<DOM::Element const> element, Directive::InlineType type, Policy const& policy, Utf16View source)
{
    switch (directive.kind()) {
    case Directive::Kind::DefaultSrc:
        return default_src_inline_check(directive, element, type, policy, source);
    case Directive::Kind::ScriptSrc:
        return script_src_inline_check(directive, element, type, policy, source);
    case Directive::Kind::ScriptSrcAttr:
        return script_src_attr_inline_check(directive, element, type, policy, source);
    case Directive::Kind::ScriptSrcElem:
        return script_src_elem_inline_check(directive, element, type, policy, source);
    case Directive::Kind::StyleSrc:
        return style_src_inline_check(directive, element, type, policy, source);
    case Directive::Kind::StyleSrcAttr:
        return style_src_attr_inline_check(directive, element, type, policy, source);
    case Directive::Kind::StyleSrcElem:
        return style_src_elem_inline_check(directive, element, type, policy, source);
    default:
        return Directive::Result::Allowed;
    }
}

Directive::Result initialization(Directive const& directive, Variant<GC::Ref<DOM::Document const>, GC::Ref<HTML::WorkerGlobalScope const>> context, Policy const& policy)
{
    if (directive.kind() == Directive::Kind::Sandbox)
        return sandbox_initialization(directive, context, policy);
    return Directive::Result::Allowed;
}

Directive::Result pre_navigation_check(Directive const& directive, Fetch::Infrastructure::Request& request, Directive::NavigationType navigation_type, Policy const& policy)
{
    switch (directive.kind()) {
    case Directive::Kind::FormAction:
        return form_action_pre_navigation_check(directive, request, navigation_type, policy);
    case Directive::Kind::RequireTrustedTypesFor:
        return TrustedTypes::require_trusted_types_for_pre_navigation_check(request);
    default:
        return Directive::Result::Allowed;
    }
}

Directive::Result navigation_response_check(Directive const& directive, Fetch::Infrastructure::Request const&, Directive::NavigationType, Fetch::Infrastructure::Response const& navigation_response, ReadonlySpan<URL::Origin> target_container_document_origins, Directive::CheckType check_type, Policy const& policy)
{
    if (directive.kind() == Directive::Kind::FrameAncestors)
        return frame_ancestors_navigation_response_check(directive, navigation_response, target_container_document_origins, check_type, policy);
    return Directive::Result::Allowed;
}

}
