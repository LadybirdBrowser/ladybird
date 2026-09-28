/*
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibURL/Origin.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/HTMLIntegration.h>
#include <LibWeb/ContentSecurityPolicy/Directives/KeywordSources.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Directives/SourceExpression.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HTMLStyleElement.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGScriptElement.h>
#include <LibWeb/SVG/SVGStyleElement.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>
#include <LibWebCommon/HTML/SandboxingFlagSet.h>
#include <LibWebCommon/Infra/Strings.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#effective-directive-for-inline-check
Utf16FlyString get_the_effective_directive_for_inline_checks(Directive::InlineType type)
{
    // Spec Note: While the effective directive is only defined for requests, in this algorithm it is used similarly to
    //            mean the directive that is most relevant to a particular type of inline check.

    // Switch on type:
    switch (type) {
        // "script"
        // "navigation"
        //    Return script-src-elem.
    case Directive::InlineType::Script:
    case Directive::InlineType::Navigation:
        return Names::ScriptSrcElem;
        // "script attribute"
        //    Return script-src-attr.
    case Directive::InlineType::ScriptAttribute:
        return Names::ScriptSrcAttr;
        // "style"
        //    Return style-src-elem.
    case Directive::InlineType::Style:
        return Names::StyleSrcElem;
        // "style attribute"
        //    Return style-src-attr.
    case Directive::InlineType::StyleAttribute:
        return Names::StyleSrcAttr;
    }

    // 2. Return null.
    // FIXME: File spec issue that this should be invalid, as the result of this algorithm ends up being piped into
    //        Violation's effective directive, which is defined to be a non-empty string.
    VERIFY_NOT_REACHED();
}

enum class [[nodiscard]] AllowsResult {
    DoesNotAllow,
    Allows,
};

static AllowsResult does_a_source_list_allow_all_inline_behavior_for_type(Vector<Utf16String> const& source_list, Directive::InlineType type)
{
    // 1. Let allow all inline be false.
    bool allow_all_inline = false;

    // 2. For each expression of list:
    for (auto const& expression : source_list) {
        // 1. If expression matches the nonce-source or hash-source grammar, return "Does Not Allow".
        auto nonce_source_parse_result = parse_source_expression(Production::NonceSource, expression);
        if (nonce_source_parse_result.has_value())
            return AllowsResult::DoesNotAllow;

        auto hash_source_parse_result = parse_source_expression(Production::HashSource, expression);
        if (hash_source_parse_result.has_value())
            return AllowsResult::DoesNotAllow;

        // 2. If type is "script", "script attribute" or "navigation" and expression matches the keyword-source
        //    "'strict-dynamic'", return "Does Not Allow".
        if (type == Directive::InlineType::Script || type == Directive::InlineType::ScriptAttribute || type == Directive::InlineType::Navigation) {
            if (expression.equals_ignoring_ascii_case(KeywordSources::StrictDynamic.view()))
                return AllowsResult::DoesNotAllow;
        }

        // 3. If expression is an ASCII case-insensitive match for the keyword-source "'unsafe-inline'", set allow all
        //    inline to true.
        if (expression.equals_ignoring_ascii_case(KeywordSources::UnsafeInline.view()))
            allow_all_inline = true;
    }

    // 3. If allow all inline is true, return "Allows". Otherwise, return "Does Not Allow".
    return allow_all_inline ? AllowsResult::Allows : AllowsResult::DoesNotAllow;
}

enum class NonceableResult {
    NotNonceable,
    Nonceable,
};

// https://w3c.github.io/webappsec-csp/#is-element-nonceable
[[nodiscard]] static NonceableResult is_element_nonceable(GC::Ptr<DOM::Element const> element)
{
    // SPEC ISSUE 7: This processing is meant to mitigate the risk of dangling markup attacks that steal the nonce from
    //               an existing element in order to load injected script. It is fairly expensive, however, as it
    //               requires that we walk through all attributes and their values in order to determine whether the
    //               script should execute. Here, we try to minimize the impact by doing this check only for script
    //               elements when a nonce is present, but we should probably consider this algorithm as "at risk"
    //               until we know its impact. [Issue #w3c/webappsec-csp#98] (https://github.com/w3c/webappsec-csp/issues/98)

    // FIXME: See FIXME in `does_element_match_source_list_for_type_and_source`
    if (!element)
        return NonceableResult::NotNonceable;

    // 1. If element does not have an attribute named "nonce", return "Not Nonceable".
    if (!is<HTML::HTMLElement>(element.ptr()) && !is<SVG::SVGElement>(element.ptr()))
        return NonceableResult::NotNonceable;

    // INTEROP: An element whose [[CryptographicNonce]] was set through the nonce IDL attribute, which intentionally
    //          does not update the content attribute, is treated as if it had a nonce attribute.
    auto const& internal_nonce = is<HTML::HTMLElement>(element.ptr()) ? as<HTML::HTMLElement>(*element).nonce() : as<SVG::SVGElement>(*element).nonce();
    if (!element->has_attribute(HTML::AttributeNames::nonce) && internal_nonce.is_empty())
        return NonceableResult::NotNonceable;

    // 2. If element is a script element, then for each attribute of element’s attribute list:
    // FIXME: File spec issue to ask if this should include SVGScriptElement.
    if (is<HTML::HTMLScriptElement>(element.ptr())) {
        for (size_t attribute_index = 0; attribute_index < element->attributes()->length(); ++attribute_index) {
            auto attribute = element->attributes()->item(attribute_index);
            VERIFY(attribute);

            auto contains_dangling_markup = [](Utf16View string) {
                return string.find_code_unit_offset_ignoring_case("<link"sv).has_value()
                    || string.find_code_unit_offset_ignoring_case("<script"sv).has_value()
                    || string.find_code_unit_offset_ignoring_case("<style"sv).has_value();
            };

            // 1. If attribute’s name contains an ASCII case-insensitive match for "<link", "<script", or "<style",
            //    return "Not Nonceable".
            if (contains_dangling_markup(attribute->name().view()))
                return NonceableResult::NotNonceable;

            // 2. If attribute’s value contains an ASCII case-insensitive match for "<link", "<script", or "<style",
            //    return "Not Nonceable".
            if (contains_dangling_markup(attribute->value()))
                return NonceableResult::NotNonceable;
        }
    }

    // 3. If element had a duplicate-attribute parse error during tokenization, return "Not Nonceable".
    // SPEC ISSUE 6: We need some sort of hook in HTML to record this error if we’re planning on using it here.
    //               [Issue #whatwg/html#3257] (https://github.com/whatwg/html/issues/3257)
    if (element->had_duplicate_attribute_during_tokenization())
        return NonceableResult::NotNonceable;

    // 4. Return "Nonceable".
    return NonceableResult::Nonceable;
}

// https://w3c.github.io/webappsec-csp/#match-element-to-source-list
static MatchResult does_element_match_source_list_for_type_and_source(GC::Ptr<DOM::Element const> element, Vector<Utf16String> const& source_list, Directive::InlineType type, Utf16View source)
{
    // Spec Note: Regardless of the encoding of the document, source will be converted to UTF-8 before applying any
    //            hashing algorithms.

    // 1. If § 6.7.3.2 Does a source list allow all inline behavior for type? returns "Allows" given list and type,
    //    return "Matches".
    if (does_a_source_list_allow_all_inline_behavior_for_type(source_list, type) == AllowsResult::Allows)
        return MatchResult::Matches;

    // 2. If type is "script" or "style", and § 6.7.3.1 Is element nonceable? returns "Nonceable" when executed upon
    //    element:
    // Spec Note: Nonces only apply to inline script and inline style, not to attributes of either element or to
    //            javascript: navigations.
    // FIXME: File spec issue that this algorithm doesn't handle `element` being null, which is it when doing a
    //        javascript: URL navigation. For now, we say that the element is not nonceable if it's null, because
    //        we simply can't pull a nonce attribute value from a null element.
    // AD-HOC: For interoperability, match a style element's internal nonce without requiring a nonce content
    // attribute. This allows styles whose nonce was set through the IDL attribute, which intentionally does not update
    // the content attribute. Keep applying the nonceability checks above to script elements to prevent dangling markup
    // attacks.
    auto is_nonceable = (type == Directive::InlineType::Style && element && (is<HTML::HTMLStyleElement>(element.ptr()) || is<SVG::SVGStyleElement>(element.ptr())))
        || (type == Directive::InlineType::Script && is_element_nonceable(element) == NonceableResult::Nonceable);
    if (is_nonceable) {
        // 1. For each expression of list:
        for (auto const& expression : source_list) {
            // 1. If expression matches the nonce-source grammar, and element has a nonce attribute whose value is
            //    expression's base64-value part, return "Matches".
            auto nonce_source_parse_result = parse_source_expression(Production::NonceSource, expression);
            if (nonce_source_parse_result.has_value()) {
                VERIFY(element);
                VERIFY(is<HTML::HTMLElement>(element.ptr()) || is<SVG::SVGElement>(element.ptr()));

                Utf16String element_nonce;
                if (auto* html_element = as_if<HTML::HTMLElement>(element.ptr())) {
                    element_nonce = html_element->nonce();
                } else {
                    auto const& svg_element = as<SVG::SVGElement>(*element);
                    element_nonce = svg_element.nonce();
                }

                if (element_nonce == nonce_source_parse_result->base64_value.value())
                    return MatchResult::Matches;
            }
        }
    }

    // 3. Let unsafe-hashes flag be false.
    bool unsafe_hashes_flag = false;

    // 4. For each expression of list:
    for (auto const& expression : source_list) {
        // 1. If expression is an ASCII case-insensitive match for the keyword-source "'unsafe-hashes'", set
        //    unsafe-hashes flag to true. Break out of the loop.
        if (expression.equals_ignoring_ascii_case(KeywordSources::UnsafeHashes.view())) {
            unsafe_hashes_flag = true;
            break;
        }
    }

    // 5. If type is "script" or "style", or unsafe-hashes flag is true:
    // NOTE: Hashes apply to inline script and inline style. If the "'unsafe-hashes'" source expression is present,
    //       they will also apply to event handlers, style attributes and javascript: navigations.
    if (type == Directive::InlineType::Script || type == Directive::InlineType::Style || unsafe_hashes_flag) {
        // 1. Set source to the result of executing UTF-8 encode on the result of executing JavaScript string
        //    converting on source.
        auto converted_source = MUST(Infra::convert_to_scalar_value_string(source));
        auto converted_source_utf8 = converted_source.to_utf8(AllowLonelySurrogates::No);

        // NOTE: converted_source_utf8 is already UTF-8 encoded.
        auto converted_source_bytes = converted_source_utf8.bytes();

        // 2. For each expression of list:
        for (auto const& expression : source_list) {
            // 1. If expression is the "'strict-dynamic'" keyword-source:
            if (expression.equals_ignoring_ascii_case(KeywordSources::StrictDynamic.view())) {
                // 1. If type is "script", and element is not parser-inserted, return "Matches".
                if (type == Directive::InlineType::Script && element) {
                    if (auto const* html_script_element = as_if<HTML::HTMLScriptElement>(element.ptr())) {
                        if (!html_script_element->is_parser_inserted())
                            return MatchResult::Matches;
                    } else if (auto const* svg_script_element = as_if<SVG::SVGScriptElement>(element.ptr())) {
                        if (!svg_script_element->is_parser_inserted())
                            return MatchResult::Matches;
                    }
                }
            }

            // 2. If expression matches the hash-source grammar:
            auto hash_source_parse_result = parse_source_expression(Production::HashSource, expression);
            if (hash_source_parse_result.has_value()) {
                // 1. Let algorithm be null.
                Optional<StringView> algorithm;

                // 2. If expression’s hash-algorithm part is an ASCII case-insensitive match for "sha256", set
                //    algorithm to SHA-256.
                VERIFY(hash_source_parse_result->hash_algorithm.has_value());
                auto hash_algorithm_from_expression = hash_source_parse_result->hash_algorithm.value();

                if (hash_algorithm_from_expression.equals_ignoring_ascii_case(u"sha256"sv))
                    algorithm = "SHA-256"sv;

                // 3. If expression’s hash-algorithm part is an ASCII case-insensitive match for "sha384", set
                //    algorithm to SHA-384.
                if (hash_algorithm_from_expression.equals_ignoring_ascii_case(u"sha384"sv))
                    algorithm = "SHA-384"sv;

                // 4. If expression’s hash-algorithm part is an ASCII case-insensitive match for "sha512", set
                //    algorithm to SHA-512.
                if (hash_algorithm_from_expression.equals_ignoring_ascii_case(u"sha512"sv))
                    algorithm = "SHA-512"sv;

                // 5. If algorithm is not null:
                if (algorithm.has_value()) {
                    // 1. Let actual be the result of base64 encoding the result of applying algorithm to source.
                    auto apply_algorithm_to_source = [&] {
                        if (*algorithm == "SHA-256"sv) {
                            auto result = ::Crypto::Hash::SHA256::hash(converted_source_bytes);
                            return MUST(encode_base64(result.bytes()));
                        }

                        if (*algorithm == "SHA-384"sv) {
                            auto result = ::Crypto::Hash::SHA384::hash(converted_source_bytes);
                            return MUST(encode_base64(result.bytes()));
                        }

                        if (*algorithm == "SHA-512"sv) {
                            auto result = ::Crypto::Hash::SHA512::hash(converted_source_bytes);
                            return MUST(encode_base64(result.bytes()));
                        }

                        VERIFY_NOT_REACHED();
                    };

                    auto actual = apply_algorithm_to_source();

                    // 2. Let expected be expression’s base64-value part, with all '-' characters replaced with '+',
                    //    and all '_' characters replaced with '/'.
                    // Spec Note: This replacement normalizes hashes expressed in base64url encoding into base64
                    //            encoding for matching.
                    VERIFY(hash_source_parse_result->base64_value.has_value());
                    auto base64_value_string = MUST(hash_source_parse_result->base64_value->to_utf8());

                    auto expected = MUST(base64_value_string.replace("-"sv, "+"sv, ReplaceMode::All));
                    expected = MUST(expected.replace("_"sv, "/"sv, ReplaceMode::All));

                    // 3. If actual is identical to expected, return "Matches".
                    if (actual == expected)
                        return MatchResult::Matches;
                }
            }
        }
    }

    // 6. Return "Does Not Match".
    return MatchResult::DoesNotMatch;
}

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
