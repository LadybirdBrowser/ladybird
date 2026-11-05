/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLScriptElement);

HTMLScriptElement::HTMLScriptElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLScriptElement::~HTMLScriptElement() = default;

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#blocking-attribute
GC::Ref<DOM::DOMTokenList> HTMLScriptElement::blocking()
{
    if (!m_blocking)
        m_blocking = DOM::DOMTokenList::create(*this, AttributeNames::blocking);
    return *m_blocking;
}

void HTMLScriptElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_blocking);
    visit_script_element_edges(visitor);
}

void HTMLScriptElement::adopted_from(DOM::Document& old_document)
{
    Base::adopted_from(old_document);
    retarget_script_load_event_delayer(document());
}

void HTMLScriptElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (!namespace_.has_value() && name == AttributeNames::blocking && m_blocking)
        m_blocking->associated_attribute_changed(value.has_value() ? value->utf16_view() : u""sv);

    if (!namespace_.has_value() && name == HTML::AttributeNames::crossorigin) {
        m_crossorigin = cors_setting_attribute_from_keyword(value.map([](auto const& value) { return value.utf16_view(); }));
    } else if (!namespace_.has_value() && name == HTML::AttributeNames::referrerpolicy) {
        m_referrer_policy = ReferrerPolicy::from_string(value.has_value() ? value->utf16_view() : u""sv).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);
    } else if (name == HTML::AttributeNames::src) {
        // https://html.spec.whatwg.org/multipage/scripting.html#script-processing-model:concept-element-attributes-change-ext
        // 1. If namespace is not null, then return.
        if (namespace_.has_value())
            return;

        // 2. If localName is src, value is not null, and element is connected, then run the script HTML element
        //    post-connection steps, given element.
        if (value.has_value() && is_connected())
            post_connection();
    }

    script_element_attribute_changed(name);
}

void HTMLScriptElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);
    script_element_children_changed(metadata);
}

void HTMLScriptElement::post_connection()
{
    script_element_post_connection();
}

// https://github.com/w3c/trusted-types/blob/0f01aee35c3684e8cb20e7d83445ad65938f545d/spec/index.bs#L1341
TrustedTypes::TrustedScriptURLOrString HTMLScriptElement::src() const
{
    // 1. Let element be the result of running this's get the element.

    // 2. Let contentAttributeValue be the result of running this's get the content attribute.
    auto maybe_src = get_attribute(AttributeNames::src);

    // 3. If contentAttributeValue is null, then return the empty string.
    if (!maybe_src.has_value())
        return Utf16String {};

    auto const& raw = *maybe_src;

    // 4. Let urlString be the result of encoding-parsing-and-serializing a URL given contentAttributeValue, relative to element's node document.
    auto url_string = document().encoding_parse_and_serialize_url(raw);

    // 5. If urlString is not failure, then return urlString.
    if (url_string.has_value())
        return *url_string;

    // 6. Return contentAttributeValue, converted to a scalar value string.
    return Utf16String {};
}

// https://www.w3.org/TR/trusted-types/#the-text-idl-attribute
WebIDL::ExceptionOr<void> HTMLScriptElement::set_text(TrustedTypes::TrustedScriptOrString text)
{
    // 1. Let value be the result of calling Get Trusted Type compliant string with
    //    TrustedScript, this’s relevant global object, the given value, HTMLScriptElement text, and script.
    auto const value = TRY(get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedScript,
        HTML::relevant_global_object(*this),
        text,
        TrustedTypes::InjectionSink::HTMLScriptElement_text,
        TrustedTypes::Script.view()));

    // 2. Set this’s script text value to the given value.
    set_string_text(value);

    // 3. String replace all with the given value within this.
    string_replace_all(value);
    return {};
}

// https://www.w3.org/TR/trusted-types/#the-src-idl-attribute
WebIDL::ExceptionOr<void> HTMLScriptElement::set_src(TrustedTypes::TrustedScriptURLOrString text)
{
    // 1. Let value be the result of calling Get Trusted Type compliant string with
    //    TrustedScriptURL, this’s relevant global object, the given value, HTMLScriptElement src, and script.
    auto const value = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedScriptURL,
        HTML::relevant_global_object(*this),
        text,
        TrustedTypes::InjectionSink::HTMLScriptElement_src,
        TrustedTypes::Script.view()));

    // 2. Set this’s src content attribute to value.
    set_attribute_value(AttributeNames::src, value);
    return {};
}

// https://w3c.github.io/trusted-types/dist/spec/#the-textContent-idl-attribute
Variant<GC::Ref<TrustedTypes::TrustedScript>, Utf16String, Empty> HTMLScriptElement::text_content() const
{
    // 1. Return the result of running get text content with this.
    return descendant_text_content();
}

// https://w3c.github.io/trusted-types/dist/spec/#the-textContent-idl-attribute
WebIDL::ExceptionOr<void> HTMLScriptElement::set_text_content(TrustedTypes::NullableTrustedScriptOrString text)
{
    // NOTE: We still require this from the base implementation.
    // https://dom.spec.whatwg.org/#dom-node-textcontent
    // The textContent setter steps are to, if the given value is null, act as if it was the empty string instead, and then run set text content with this and the given value.
    TrustedTypes::TrustedScriptOrString non_null_text = text.has<Empty>()
        ? ""_utf16
        : text.downcast<TrustedTypes::TrustedScriptOrString>();

    // 1. Let value be the result of calling Get Trusted Type compliant string with
    //    TrustedScript, this's relevant global object, the given value, HTMLScriptElement textContent, and script.
    auto const value = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedScript,
        HTML::relevant_global_object(*this),
        non_null_text,
        TrustedTypes::InjectionSink::HTMLScriptElement_textContent,
        TrustedTypes::Script.view()));

    // 2. Set this’s script text value to value.
    set_string_text(value);

    // 3. Run set text content with this and value.
    string_replace_all(value);
    return {};
}

// https://w3c.github.io/trusted-types/dist/spec/#the-innerText-idl-attribute
TrustedTypes::TrustedScriptOrString HTMLScriptElement::inner_text()
{
    // 1. Return the result of running get text content with this.
    return get_the_text_steps();
}

// https://w3c.github.io/trusted-types/dist/spec/#the-innerText-idl-attribute
WebIDL::ExceptionOr<void> HTMLScriptElement::set_inner_text(TrustedTypes::TrustedScriptOrString text)
{
    // 1. Let value be the result of calling Get Trusted Type compliant string with
    //    TrustedScript, this’s relevant global object, the given value, HTMLScriptElement innerText, and script.
    auto const value = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedScript,
        HTML::relevant_global_object(*this),
        text,
        TrustedTypes::InjectionSink::HTMLScriptElement_innerText,
        TrustedTypes::Script.view()));

    // 2. Set this’s script text value to value.
    set_string_text(value);

    // 3. Run set the inner text steps with this and value.
    HTMLElement::set_inner_text(value);
    return {};
}

// https://html.spec.whatwg.org/multipage/scripting.html#dom-script-async
bool HTMLScriptElement::async() const
{
    // 1. If this's force async is true, then return true.
    if (is_force_async())
        return true;

    // 2. If this's async content attribute is present, then return true.
    if (has_attribute(HTML::AttributeNames::async))
        return true;

    // 3. Return false.
    return false;
}

void HTMLScriptElement::set_async(bool async)
{
    // 1. Set this's force async to false.
    set_force_async(false);

    // 2. If the given value is true, then set this's async content attribute to the empty string.
    if (async) {
        set_attribute_value(HTML::AttributeNames::async, ""_utf16);
    }
    // 3. Otherwise, remove this's async content attribute.
    else {
        remove_attribute(HTML::AttributeNames::async);
    }
}

WebIDL::ExceptionOr<void> HTMLScriptElement::cloned(Node& copy, bool subtree) const
{
    TRY(Base::cloned(copy, subtree));
    script_element_cloned(as<HTMLScriptElement>(copy));
    return {};
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#implicitly-potentially-render-blocking
bool HTMLScriptElement::is_implicitly_potentially_render_blocking() const
{
    // A script element el is implicitly potentially render-blocking if el's type is "classic", el is parser-inserted, and el does not have an async or defer attribute.
    return script_type() == ScriptType::Classic && is_parser_inserted() && !has_attribute(AttributeNames::async) && !has_attribute(AttributeNames::defer);
}

bool HTMLScriptElement::has_source_attribute() const
{
    return has_attribute_ns({}, AttributeNames::src);
}

Utf16String HTMLScriptElement::source_attribute_value()
{
    return get_attribute_ns({}, AttributeNames::src).value_or({});
}

Optional<Utf16String> HTMLScriptElement::language_attribute_value() const
{
    return attribute(AttributeNames::language);
}

Optional<Utf16String> HTMLScriptElement::charset_attribute_value() const
{
    return attribute(AttributeNames::charset);
}

Optional<Utf16String> HTMLScriptElement::event_attribute_value() const
{
    return attribute(AttributeNames::event);
}

Optional<Utf16String> HTMLScriptElement::for_attribute_value() const
{
    return attribute(AttributeNames::for_);
}

bool HTMLScriptElement::has_defer_attribute() const
{
    return has_attribute(AttributeNames::defer);
}

bool HTMLScriptElement::has_nomodule_attribute() const
{
    return has_attribute(AttributeNames::nomodule);
}

ScriptElementFetchAttributes HTMLScriptElement::fetch_attributes() const
{
    return {
        .crossorigin = m_crossorigin,
        .cryptographic_nonce = nonce(),
        .integrity = get_attribute_ns({}, AttributeNames::integrity),
        .referrer_policy = m_referrer_policy,
        .fetch_priority = Fetch::Infrastructure::request_priority_from_string(attribute(AttributeNames::fetchpriority).value_or({})).value_or(Fetch::Infrastructure::Request::Priority::Auto),
        .parser_inserted = is_parser_inserted(),
    };
}

}
