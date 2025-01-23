/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <LibWeb/Bindings/HTMLMetaElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/ParsingContext.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLMetaElement.h>
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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLMetaElement);
}

Optional<HTMLMetaElement::HttpEquivAttributeState> HTMLMetaElement::http_equiv_state() const
{
    auto value = get_attribute_value(HTML::AttributeNames::http_equiv);

#define __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE(keyword, state) \
    if (value.equals_ignoring_ascii_case(keyword##sv))             \
        return HTMLMetaElement::HttpEquivAttributeState::state;
    ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTES
#undef __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE

    return OptionalNone {};
}

void HTMLMetaElement::update_metadata(Optional<String> const& old_name)
{
    if (name().has_value()) {
        if (name()->equals_ignoring_ascii_case("theme-color"sv)) {
            document().obtain_theme_color();
        } else if (name()->equals_ignoring_ascii_case("color-scheme"sv)) {
            document().obtain_supported_color_schemes();
            return;
        }
    }

    if (old_name.has_value()) {
        if (old_name->equals_ignoring_ascii_case("theme-color"sv)) {
            document().obtain_theme_color();
        } else if (old_name->equals_ignoring_ascii_case("color-scheme"sv)) {
            document().obtain_supported_color_schemes();
            return;
        }
    }
}

void HTMLMetaElement::inserted()
{
    Base::inserted();

    update_metadata();

    // https://html.spec.whatwg.org/multipage/semantics.html#pragma-directives
    // When a meta element is inserted into the document, if its http-equiv attribute is present and represents one of
    // the above states, then the user agent must run the algorithm appropriate for that state, as described in the
    // following list:
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

            auto input = get_attribute_value(AttributeNames::content);
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

            // 2. If the element's content attribute contains a U+002C COMMA character (,) then return.
            auto content = get_attribute_value(AttributeNames::content);
            if (content.contains(","sv))
                break;

            // 3. Let input be the value of the element's content attribute.
            // 4. Let position point at the first character of input.
            GenericLexer lexer { content };

            // 5. Skip ASCII whitespace within input given position.
            lexer.ignore_while(Web::Infra::is_ascii_whitespace);

            // 6. Collect a sequence of code points that are not ASCII whitespace from input given position.
            // 7. Let candidate be the string that resulted from the previous step.
            auto candidate = lexer.consume_until(Web::Infra::is_ascii_whitespace);

            // 8. If candidate is the empty string, return.
            if (candidate.is_empty())
                break;

            // 9. Set the pragma-set default language to candidate.
            auto language = String::from_utf8_without_validation(candidate.bytes());
            document().set_pragma_set_default_language(language);
            break;
        }
        default:
            dbgln("FIXME: Implement '{}' http-equiv state", get_attribute_value(AttributeNames::http_equiv));
            break;
        }
    }
}

void HTMLMetaElement::removed_from(Node* old_parent, Node& old_root)
{
    Base::removed_from(old_parent, old_root);
    update_metadata();
}

void HTMLMetaElement::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);
    if (local_name == HTML::AttributeNames::name) {
        update_metadata(old_value);
    } else {
        update_metadata();
    }
}

}
