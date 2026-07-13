/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <LibWeb/Bindings/HTMLMetaElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHeadElement.h>
#include <LibWeb/HTML/HTMLMetaElement.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLMetaElement);

HTMLMetaElement::HTMLMetaElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLMetaElement::~HTMLMetaElement() = default;

void HTMLMetaElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLMetaElement);
    Base::initialize(realm);
}

Optional<HTMLMetaElement::HttpEquivAttributeState> HTMLMetaElement::http_equiv_state() const
{
    auto value = get_attribute_value_view(HTML::AttributeNames::http_equiv).value_or({});

#define __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE(keyword, state) \
    if (value.equals_ignoring_ascii_case(keyword##sv))             \
        return HTMLMetaElement::HttpEquivAttributeState::state;
    ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTES
#undef __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE

    return OptionalNone {};
}

void HTMLMetaElement::update_metadata(Optional<Utf16String> const& old_name)
{
    if (auto name = get_attribute_value_view(AttributeNames::name); name.has_value()) {
        if (name->equals_ignoring_ascii_case(u"theme-color"sv)) {
            document().obtain_theme_color();
        } else if (name->equals_ignoring_ascii_case(u"color-scheme"sv)) {
            document().obtain_supported_color_schemes();
        } else if (name->equals_ignoring_ascii_case(u"referrer"sv)) {
            // 2. If element does not have a name attribute whose value is an ASCII case-insensitive match for "referrer", then return.
            update_referrer_policy();
        }
    }

    if (old_name.has_value()) {
        if (old_name->equals_ignoring_ascii_case(u"theme-color"sv)) {
            document().obtain_theme_color();
        } else if (old_name->equals_ignoring_ascii_case(u"color-scheme"sv)) {
            document().obtain_supported_color_schemes();
        }

        // NOTE: For historical reasons, unlike other standard metadata names, the processing model for referrer is not
        //       responsive to element removals, and does not use tree order. Only the most-recently-inserted or
        //       most-recently-modified meta element in this state has an effect.
    }
}

// https://html.spec.whatwg.org/multipage/semantics.html#meta-referrer
void HTMLMetaElement::update_referrer_policy()
{
    // 1. If element is not in a document tree, then return.
    if (!in_a_document_tree())
        return;

    // 3. If element does not have a content attribute, or that attribute's value is the empty string, then return.
    auto content = attribute(AttributeNames::content);
    if (!content.has_value() || content->is_empty())
        return;

    // 4. Let value be the value of element's content attribute, converted to ASCII lowercase.
    auto value = content->utf16_view();

    // 5. If value is one of the values given in the first column of the following table, then set value to the value given in the second column:
    ReferrerPolicy::ReferrerPolicy policy;
    if (value.equals_ignoring_ascii_case(u"never"sv))
        policy = ReferrerPolicy::ReferrerPolicy::NoReferrer;
    else if (value.equals_ignoring_ascii_case(u"default"sv))
        policy = ReferrerPolicy::DEFAULT_REFERRER_POLICY;
    else if (value.equals_ignoring_ascii_case(u"always"sv))
        policy = ReferrerPolicy::ReferrerPolicy::UnsafeURL;
    else if (value.equals_ignoring_ascii_case(u"origin-when-crossorigin"sv))
        policy = ReferrerPolicy::ReferrerPolicy::OriginWhenCrossOrigin;
    // 6. If value is a referrer policy, then...
    else if (auto parsed_policy = ReferrerPolicy::from_string(value); parsed_policy.has_value())
        policy = *parsed_policy;
    else
        return;

    // 6. ...set element's node document's policy container's referrer policy to policy.
    document().policy_container()->referrer_policy = policy;
}

void HTMLMetaElement::inserted()
{
    Base::inserted();

    update_metadata();

    // https://html.spec.whatwg.org/multipage/semantics.html#pragma-directives
    // When a meta element is inserted into the document, if its http-equiv attribute is present and represents one of
    // the above states, then the user agent must run the algorithm appropriate for that state, as described in the
    // following list:
    if (!in_a_document_tree())
        return;

    auto http_equiv = http_equiv_state();
    if (http_equiv.has_value()) {
        switch (http_equiv.value()) {
        case HttpEquivAttributeState::EncodingDeclaration:
            // https://html.spec.whatwg.org/multipage/semantics.html#attr-meta-http-equiv-content-type
            // The Encoding declaration state is just an alternative form of setting the charset attribute: it is a character encoding declaration.
            // This state's user agent requirements are all handled by the parsing section of the specification.
            break;
        case HttpEquivAttributeState::Refresh: {
            // https://html.spec.whatwg.org/multipage/semantics.html#attr-meta-http-equiv-refresh
            // 1. If the meta element has no content attribute, or if that attribute's value is the empty string, then return.
            // 2. Let input be the value of the element's content attribute.
            if (!has_attribute(AttributeNames::content))
                break;

            auto input = get_attribute_value_view(AttributeNames::content).value_or({});
            if (input.is_empty())
                break;

            // 3. Run the shared declarative refresh steps with the meta element's node document, input, and the meta element.
            document().shared_declarative_refresh_steps(input, this);
            break;
        }
        case HttpEquivAttributeState::SetCookie:
            // https://html.spec.whatwg.org/multipage/semantics.html#attr-meta-http-equiv-set-cookie
            // This pragma is non-conforming and has no effect.
            // User agents are required to ignore this pragma.
            break;
        case HttpEquivAttributeState::XUACompatible:
            // https://html.spec.whatwg.org/multipage/semantics.html#attr-meta-http-equiv-x-ua-compatible
            // In practice, this pragma encourages Internet Explorer to more closely follow the specifications.
            // For meta elements with an http-equiv attribute in the X-UA-Compatible state, the content attribute must have a value that is an ASCII case-insensitive match for the string "IE=edge".
            // User agents are required to ignore this pragma.
            break;
        case HttpEquivAttributeState::ContentLanguage: {
            // https://html.spec.whatwg.org/multipage/semantics.html#attr-meta-http-equiv-content-language
            // 1. If the meta element has no content attribute, then return.
            if (!has_attribute(AttributeNames::content))
                break;

            // 2. If the element's content attribute contains a U+002C COMMA character (,), then return.
            auto content = get_attribute_value_view(AttributeNames::content).value_or({});
            if (content.contains(u","sv))
                break;

            // 3. Let input be the value of the element's content attribute.
            // 4. Let position point at the first character of input.
            auto input = content;
            size_t position = 0;

            // 5. Skip ASCII whitespace within input given position.
            while (position < input.length_in_code_units() && Web::Infra::is_ascii_whitespace(input.code_unit_at(position)))
                ++position;

            // 6. Collect a sequence of code points that are not ASCII whitespace from input given position.
            // 7. Let candidate be the string that resulted from the previous step.
            auto candidate_start = position;
            while (position < input.length_in_code_units() && !Web::Infra::is_ascii_whitespace(input.code_unit_at(position)))
                ++position;
            auto candidate = input.substring_view(candidate_start, position - candidate_start);

            // 8. If candidate is the empty string, return.
            if (candidate.is_empty())
                break;

            // 9. Set the pragma-set default language to candidate.
            document().set_pragma_set_default_language(Utf16String::from_utf16(candidate));
            document().document_element()->invalidate_lang_value();
            break;
        }
        case HttpEquivAttributeState::ContentSecurityPolicy: {
            // https://html.spec.whatwg.org/multipage/semantics.html#attr-meta-http-equiv-content-security-policy
            // This pragma enforces a Content Security Policy on a Document. [CSP]
            // 1. If the meta element is not a child of a head element, return.
            if (!is<HTMLHeadElement>(parent()))
                break;

            // 2. If the meta element has no content attribute, or if that attribute's value is the empty string, then return.
            auto input = get_attribute_value_view(AttributeNames::content).value_or({});
            if (input.is_empty())
                break;

            // 3. Let policy be the result of executing Content Security Policy's parse a serialized Content Security
            //    Policy algorithm on the meta element's content attribute's value, with a source of "meta", and a
            //    disposition of "enforce".
            auto& realm = this->realm();
            auto policy = ContentSecurityPolicy::Policy::parse_a_serialized_csp(realm.heap(), input, ContentSecurityPolicy::Policy::Source::Meta, ContentSecurityPolicy::Policy::Disposition::Enforce);

            // 4. Remove all occurrences of the report-uri, frame-ancestors, and sandbox directives from policy.
            policy->remove_directive({}, ContentSecurityPolicy::Directives::Names::ReportUri);
            policy->remove_directive({}, ContentSecurityPolicy::Directives::Names::FrameAncestors);
            policy->remove_directive({}, ContentSecurityPolicy::Directives::Names::Sandbox);

            // FIXME: File spec issue stating the policy's self origin isn't set here.
            policy->set_self_origin({}, document().origin());

            // 5. Enforce the policy policy.
            auto policy_list = ContentSecurityPolicy::PolicyList::from_object(realm.global_object());
            VERIFY(policy_list);
            policy_list->enforce_policy(policy);
            break;
        }
        default:
            dbgln("FIXME: Implement '{}' http-equiv state", get_attribute_value(AttributeNames::http_equiv));
            break;
        }
    }
}

void HTMLMetaElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    update_metadata();
}

void HTMLMetaElement::attribute_changed(Utf16FlyString const& local_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);
    if (local_name == HTML::AttributeNames::name) {
        update_metadata(old_value);
    } else {
        update_metadata();
    }
}

}
