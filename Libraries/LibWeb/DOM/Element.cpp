/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Assertions.h>
#include <AK/Checked.h>
#include <AK/Debug.h>
#include <AK/IterationDecision.h>
#include <AK/JsonObjectSerializer.h>
#include <AK/NumericLimits.h>
#include <AK/StringBuilder.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibURL/Parser.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Locale.h>
#include <LibWeb/Bindings/ElementPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StylePropertyMap.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/RandomValueSharingStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectList.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementName.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactionNames.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLBaseElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLMenuElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLStyleElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/HTMLUListElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/XMLSerializer.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/IntersectionObserver/IntersectionObserver.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/ListItemBox.h>
#include <LibWeb/Layout/TreeBuilder.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/SVG/SVGAElement.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/XML/XMLFragmentParser.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(Element);

Element::Element(Document& document, DOM::QualifiedName qualified_name)
    : ParentNode(document, NodeType::ELEMENT_NODE)
    , m_qualified_name(move(qualified_name))
{
}

Element::~Element() = default;

void Element::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Element);
    Base::initialize(realm);
}

void Element::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SlottableMixin::visit_edges(visitor);
    Animatable::visit_edges(visitor);
    ARIAMixin::visit_edges(visitor);

    visitor.visit(m_attributes);
    visitor.visit(m_inline_style);
    visitor.visit(m_class_list);
    visitor.visit(m_shadow_root);
    visitor.visit(m_part_list);
    visitor.visit(m_custom_element_definition);
    visitor.visit(m_custom_state_set);
    visitor.visit(m_cascaded_properties);
    visitor.visit(m_computed_properties);
    visitor.visit(m_computed_style_map_cache);
    visitor.visit(m_attribute_style_map);
    if (m_pseudo_element_data) {
        for (auto& pseudo_element : *m_pseudo_element_data) {
            visitor.visit(pseudo_element.value);
        }
    }
    if (m_registered_intersection_observers) {
        for (auto& registered_intersection_observers : *m_registered_intersection_observers)
            visitor.visit(registered_intersection_observers.observer);
    }
    if (m_counters_set)
        m_counters_set->visit_edges(visitor);
}

// https://dom.spec.whatwg.org/#dom-element-getattribute
Optional<String> Element::get_attribute(FlyString const& name) const
{
    // 1. Let attr be the result of getting an attribute given qualifiedName and this.
    if (!m_attributes)
        return {};
    auto const* attribute = m_attributes->get_attribute(name);

    // 2. If attr is null, return null.
    if (!attribute)
        return {};

    // 3. Return attr’s value.
    return attribute->value();
}

// https://dom.spec.whatwg.org/#dom-element-getattributens
Optional<String> Element::get_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name) const
{
    // 1. Let attr be the result of getting an attribute given namespace, localName, and this.
    if (!m_attributes)
        return {};
    auto const* attribute = m_attributes->get_attribute_ns(namespace_, name);

    // 2. If attr is null, return null.
    if (!attribute)
        return {};

    // 3. Return attr’s value.
    return attribute->value();
}

// https://dom.spec.whatwg.org/#concept-element-attributes-get-value
String Element::get_attribute_value(FlyString const& local_name, Optional<FlyString> const& namespace_) const
{
    // 1. Let attr be the result of getting an attribute given namespace, localName, and element.
    if (!m_attributes)
        return {};
    auto const* attribute = m_attributes->get_attribute_ns(namespace_, local_name);

    // 2. If attr is null, then return the empty string.
    if (!attribute)
        return String {};

    // 3. Return attr’s value.
    return attribute->value();
}

// https://html.spec.whatwg.org/multipage/semantics.html#get-an-element's-target
String Element::get_an_elements_target(Optional<String> target) const
{
    // To get an element's target, given an a, area, or form element element, and an optional string-or-null target (default null), run these steps:

    // 1. If target is null, then:
    if (!target.has_value()) {
        // 1. If element has a target attribute, then set target to that attribute's value.
        if (auto maybe_target = attribute(HTML::AttributeNames::target); maybe_target.has_value()) {
            target = maybe_target.release_value();
        }
        // 2. Otherwise, if element's node document contains a base element with a target attribute,
        //    set target to the value of the target attribute of the first such base element.
        else if (auto base_element = document().first_base_element_with_target_in_tree_order()) {
            target = base_element->attribute(HTML::AttributeNames::target);
        }
    }

    // 2. If target is not null, and contains an ASCII tab or newline and a U+003C (<), then set target to "_blank".
    if (target.has_value() && target->bytes_as_string_view().contains("\t\n\r"sv) && target->contains('<'))
        target = "_blank"_string;

    // 3. Return target.
    return target.value_or({});
}

// https://html.spec.whatwg.org/multipage/links.html#get-an-element's-noopener
HTML::TokenizedFeature::NoOpener Element::get_an_elements_noopener(URL::URL const& url, StringView target) const
{
    // To get an element's noopener, given an a, area, or form element element, a URL record url, and a string target,
    // perform the following steps. They return a boolean.
    auto rel = MUST(get_attribute_value(HTML::AttributeNames::rel).to_lowercase());
    auto link_types = rel.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);

    // 1. If element's link types include the noopener or noreferrer keyword, then return true.
    if (link_types.contains_slow("noopener"sv) || link_types.contains_slow("noreferrer"sv))
        return HTML::TokenizedFeature::NoOpener::Yes;

    // 2. If element's link types do not include the opener keyword and
    //    target is an ASCII case-insensitive match for "_blank", then return true.
    if (!link_types.contains_slow("opener"sv) && target.equals_ignoring_ascii_case("_blank"sv))
        return HTML::TokenizedFeature::NoOpener::Yes;

    // 3. If url's blob URL entry is not null:
    if (url.blob_url_entry().has_value()) {
        // 1. Let blobOrigin be url's blob URL entry's environment's origin.
        auto const& blob_origin = url.blob_url_entry()->environment.origin;

        // 2. Let topLevelOrigin be element's relevant settings object's top-level origin.
        auto const& top_level_origin = HTML::relevant_settings_object(*this).top_level_origin;

        // 3. If blobOrigin is not same site with topLevelOrigin, then return true.
        if (!blob_origin.is_same_site(top_level_origin.value()))
            return HTML::TokenizedFeature::NoOpener::Yes;
    }

    // 4. Return false.
    return HTML::TokenizedFeature::NoOpener::No;
}

// https://html.spec.whatwg.org/multipage/links.html#cannot-navigate
bool Element::cannot_navigate() const
{
    // An element element cannot navigate if one of the following is true:

    // - element's node document is not fully active
    if (!document().is_fully_active())
        return true;

    // - element is not an a element and is not connected.
    return !(is_html_anchor_element() || is_svg_a_element()) && !is_connected();
}

// https://html.spec.whatwg.org/multipage/links.html#following-hyperlinks-2
void Element::follow_the_hyperlink(Optional<String> hyperlink_suffix, HTML::UserNavigationInvolvement user_involvement)
{
    // 1. If subject cannot navigate, then return.
    if (cannot_navigate())
        return;

    // 2. Let targetAttributeValue be the empty string.
    String target_attribute_value;

    // 3. If subject is an a or area element, then set targetAttributeValue to the result of getting an element's target given subject.
    if (is_html_anchor_element() || is_html_area_element() || is_svg_a_element())
        target_attribute_value = get_an_elements_target();

    // 4. Let urlRecord be the result of encoding-parsing a URL given subject's href attribute value, relative to subject's node document.
    auto url_record = document().encoding_parse_url(get_attribute_value(HTML::AttributeNames::href));

    // 5. If urlRecord is failure, then return.
    if (!url_record.has_value())
        return;

    // 6. Let noopener be the result of getting an element's noopener with subject, urlRecord, and targetAttributeValue.
    auto noopener = get_an_elements_noopener(*url_record, target_attribute_value);

    // 7. Let targetNavigable be the first return value of applying the rules for choosing a navigable given
    //    targetAttributeValue, subject's node navigable, and noopener.
    auto target_navigable = document().navigable()->choose_a_navigable(target_attribute_value, noopener).navigable;

    // 8. If targetNavigable is null, then return.
    if (!target_navigable)
        return;

    // 9. Let urlString be the result of applying the URL serializer to urlRecord.
    auto url_string = url_record->serialize();

    // 10. If hyperlinkSuffix is non-null, then append it to urlString.
    if (hyperlink_suffix.has_value())
        url_string = MUST(String::formatted("{}{}", url_string, *hyperlink_suffix));

    // 11. Let referrerPolicy be the current state of subject's referrerpolicy content attribute.
    auto referrer_policy = ReferrerPolicy::from_string(attribute(HTML::AttributeNames::referrerpolicy).value_or({})).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);

    // FIXME: 12. If subject's link types includes the noreferrer keyword, then set referrerPolicy to "no-referrer".

    // 13. Navigate targetNavigable to urlString using subject's node document, with referrerPolicy set to referrerPolicy and userInvolvement set to userInvolvement.
    auto url = URL::Parser::basic_parse(url_string);
    VERIFY(url.has_value());
    MUST(target_navigable->navigate({ .url = url.release_value(), .source_document = document(), .referrer_policy = referrer_policy, .user_involvement = user_involvement }));
}

// https://dom.spec.whatwg.org/#dom-element-getattributenode
GC::Ptr<Attr> Element::get_attribute_node(FlyString const& name) const
{
    // The getAttributeNode(qualifiedName) method steps are to return the result of getting an attribute given qualifiedName and this.
    if (!m_attributes)
        return {};
    return m_attributes->get_attribute(name);
}

// https://dom.spec.whatwg.org/#dom-element-getattributenodens
GC::Ptr<Attr> Element::get_attribute_node_ns(Optional<FlyString> const& namespace_, FlyString const& name) const
{
    // The getAttributeNodeNS(namespace, localName) method steps are to return the result of getting an attribute given namespace, localName, and this.
    if (!m_attributes)
        return {};
    return m_attributes->get_attribute_ns(namespace_, name);
}

// https://dom.spec.whatwg.org/#dom-element-setattribute
WebIDL::ExceptionOr<void> Element::set_attribute_for_bindings(FlyString qualified_name, Variant<GC::Root<TrustedTypes::TrustedHTML>, GC::Root<TrustedTypes::TrustedScript>, GC::Root<TrustedTypes::TrustedScriptURL>, Utf16String> const& value)
{
    // 1. If qualifiedName is not a valid attribute local name, then throw an "InvalidCharacterError" DOMException.
    if (!is_valid_attribute_local_name(qualified_name))
        return WebIDL::InvalidCharacterError::create(realm(), "Attribute name must not be empty or contain invalid characters"_utf16);

    // 2. If this is in the HTML namespace and its node document is an HTML document, then set qualifiedName to
    //    qualifiedName in ASCII lowercase.
    if (namespace_uri() == Namespace::HTML && document().document_type() == Document::Type::HTML)
        qualified_name = qualified_name.to_ascii_lowercase();

    // 3. Let verifiedValue be the result of calling get Trusted Types-compliant attribute value
    //    with qualifiedName, null, this, and value.
    auto const verified_value = TRY(TrustedTypes::get_trusted_types_compliant_attribute_value(qualified_name, {}, *this, value));

    // 4. Let attribute be the first attribute in this’s attribute list whose qualified name is qualifiedName, and null otherwise.
    auto* attribute = attributes()->get_attribute(qualified_name);

    // 5. If attribute is non-null, then change attribute to verifiedValue and return.
    if (attribute) {
        attribute->change_attribute(verified_value.to_utf8_but_should_be_ported_to_utf16());
        return {};
    }

    // 6. Set attribute to a new attribute whose local name is qualifiedName, value is verifiedValue,
    //    and node document is this’s node document.
    attribute = Attr::create(document(), qualified_name, verified_value.to_utf8_but_should_be_ported_to_utf16());

    // 7. Append attribute to this.
    m_attributes->append_attribute(*attribute);

    return {};
}

// https://dom.spec.whatwg.org/#dom-element-setattribute
WebIDL::ExceptionOr<void> Element::set_attribute_for_bindings(FlyString qualified_name, Variant<GC::Root<TrustedTypes::TrustedHTML>, GC::Root<TrustedTypes::TrustedScript>, GC::Root<TrustedTypes::TrustedScriptURL>, String> const& value)
{
    return set_attribute_for_bindings(move(qualified_name),
        value.visit(
            [](auto const& trusted_type) -> Variant<GC::Root<TrustedTypes::TrustedHTML>, GC::Root<TrustedTypes::TrustedScript>, GC::Root<TrustedTypes::TrustedScriptURL>, Utf16String> { return trusted_type; },
            [](String const& string) -> Variant<GC::Root<TrustedTypes::TrustedHTML>, GC::Root<TrustedTypes::TrustedScript>, GC::Root<TrustedTypes::TrustedScriptURL>, Utf16String> { return Utf16String::from_utf8(string); }));
}

// https://dom.spec.whatwg.org/#valid-namespace-prefix
bool is_valid_namespace_prefix(FlyString const& prefix)
{
    // A string is a valid namespace prefix if its length is at least 1 and it does not contain ASCII whitespace, U+0000 NULL, U+002F (/), or U+003E (>).
    constexpr Array<u32, 8> INVALID_NAMESPACE_PREFIX_CHARACTERS { '\t', '\n', '\f', '\r', ' ', '\0', '/', '>' };
    return !prefix.is_empty() && !prefix.code_points().contains_any_of(INVALID_NAMESPACE_PREFIX_CHARACTERS);
}

// https://dom.spec.whatwg.org/#valid-attribute-local-name
bool is_valid_attribute_local_name(FlyString const& local_name)
{
    // A string is a valid attribute local name if its length is at least 1 and it does not contain ASCII whitespace, U+0000 NULL, U+002F (/), U+003D (=), or U+003E (>).
    constexpr Array<u32, 9> INVALID_ATTRIBUTE_LOCAL_NAME_CHARACTERS { '\t', '\n', '\f', '\r', ' ', '\0', '/', '=', '>' };
    return !local_name.is_empty() && !local_name.code_points().contains_any_of(INVALID_ATTRIBUTE_LOCAL_NAME_CHARACTERS);
}

// https://dom.spec.whatwg.org/#valid-element-local-name
bool is_valid_element_local_name(FlyString const& name)
{
    // 1. If name’s length is 0, then return false.
    if (name.is_empty())
        return false;

    // 2. If name’s 0th code point is an ASCII alpha, then:
    auto first_code_point = *name.code_points().begin().peek();
    if (is_ascii_alpha(first_code_point)) {
        // 1. If name contains ASCII whitespace, U+0000 NULL, U+002F (/), or U+003E (>), then return false.
        constexpr Array<u32, 8> INVALID_CHARACTERS { '\t', '\n', '\f', '\r', ' ', '\0', '/', '>' };
        if (name.code_points().contains_any_of(INVALID_CHARACTERS))
            return false;

        // 2. Return true.
        return true;
    }

    // 3. If name’s 0th code point is not U+003A (:), U+005F (_), or in the range U+0080 to U+10FFFF, inclusive, then return false.
    if (!first_is_one_of(first_code_point, 0x003Au, 0x005Fu) && (first_code_point < 0x0080 || first_code_point > 0x10FFFF))
        return false;

    // 4. If name’s subsequent code points, if any, are not ASCII alphas, ASCII digits, U+002D (-), U+002E (.), U+003A (:), U+005F (_), or in the range U+0080 to U+10FFFF, inclusive, then return false.
    for (auto code_point : name.code_points().unicode_substring_view(1)) {
        if (!is_ascii_alpha(code_point) && !is_ascii_digit(code_point) && !first_is_one_of(code_point, 0X002Du, 0X002Eu, 0X003Au, 0X005Fu) && (code_point < 0x0080 || code_point > 0x10FFFF))
            return false;
    }

    // 5. Return true.
    return true;
}

// https://dom.spec.whatwg.org/#validate-and-extract
WebIDL::ExceptionOr<QualifiedName> validate_and_extract(JS::Realm& realm, Optional<FlyString> namespace_, FlyString const& qualified_name, ValidationContext context)
{
    // To validate and extract a namespace and qualifiedName, run these steps:

    // 1. If namespace is the empty string, then set it to null.
    if (namespace_.has_value() && namespace_.value().is_empty())
        namespace_ = {};

    // 2. Let prefix be null.
    Optional<FlyString> prefix = {};

    // 3. Let localName be qualifiedName.
    auto local_name = qualified_name;

    // 4. If qualifiedName contains a U+003A (:):
    auto split_result = qualified_name.bytes_as_string_view().split_view(':', SplitBehavior::KeepEmpty);
    if (split_result.size() > 1) {
        // 1. Let splitResult be the result of running strictly split given qualifiedName and U+003A (:).
        // 2. Set prefix to splitResult[0].
        prefix = MUST(FlyString::from_utf8(split_result[0]));

        // 3. Set localName to splitResult[1].
        local_name = MUST(FlyString::from_utf8(split_result[1]));

        // 4. If prefix is not a valid namespace prefix, then throw an "InvalidCharacterError" DOMException.
        if (!is_valid_namespace_prefix(*prefix))
            return WebIDL::InvalidCharacterError::create(realm, "Prefix not a valid namespace prefix."_utf16);
    }

    // 5. Assert: prefix is either null or a valid namespace prefix.
    ASSERT(!prefix.has_value() || is_valid_namespace_prefix(*prefix));

    // 6. If context is "attribute" and localName is not a valid attribute local name, then throw an "InvalidCharacterError" DOMException.
    if (context == ValidationContext::Attribute && !is_valid_attribute_local_name(local_name))
        return WebIDL::InvalidCharacterError::create(realm, "Local name not a valid attribute local name."_utf16);

    // 7. If context is "element" and localName is not a valid element local name, then throw an "InvalidCharacterError" DOMException.
    if (context == ValidationContext::Element && !is_valid_element_local_name(local_name))
        return WebIDL::InvalidCharacterError::create(realm, "Local name not a valid element local name."_utf16);

    // 8. If prefix is non-null and namespace is null, then throw a "NamespaceError" DOMException.
    if (prefix.has_value() && !namespace_.has_value())
        return WebIDL::NamespaceError::create(realm, "Prefix is non-null and namespace is null."_utf16);

    // 9. If prefix is "xml" and namespace is not the XML namespace, then throw a "NamespaceError" DOMException.
    if (prefix == "xml"sv && namespace_ != Namespace::XML)
        return WebIDL::NamespaceError::create(realm, "Prefix is 'xml' and namespace is not the XML namespace."_utf16);

    // 10. If either qualifiedName or prefix is "xmlns" and namespace is not the XMLNS namespace, then throw a "NamespaceError" DOMException.
    if ((qualified_name == "xmlns"sv || prefix == "xmlns"sv) && namespace_ != Namespace::XMLNS)
        return WebIDL::NamespaceError::create(realm, "Either qualifiedName or prefix is 'xmlns' and namespace is not the XMLNS namespace."_utf16);

    // 11. If namespace is the XMLNS namespace and neither qualifiedName nor prefix is "xmlns", then throw a "NamespaceError" DOMException.
    if (namespace_ == Namespace::XMLNS && !(qualified_name == "xmlns"sv || prefix == "xmlns"sv))
        return WebIDL::NamespaceError::create(realm, "Namespace is the XMLNS namespace and neither qualifiedName nor prefix is 'xmlns'."_utf16);

    // 12. Return (namespace, prefix, localName).
    return QualifiedName { local_name, prefix, namespace_ };
}

// https://dom.spec.whatwg.org/#dom-element-setattributens
WebIDL::ExceptionOr<void> Element::set_attribute_ns_for_bindings(Optional<FlyString> const& namespace_, FlyString const& qualified_name, Variant<GC::Root<TrustedTypes::TrustedHTML>, GC::Root<TrustedTypes::TrustedScript>, GC::Root<TrustedTypes::TrustedScriptURL>, Utf16String> const& value)
{
    // 1. Let (namespace, prefix, localName) be the result of validating and extracting namespace and qualifiedName given "attribute".
    auto extracted_qualified_name = TRY(validate_and_extract(realm(), namespace_, qualified_name, ValidationContext::Attribute));

    // 2. Let verifiedValue be the result of calling get Trusted Types-compliant attribute value
    //    with localName, namespace, this, and value.
    auto const verified_value = TRY(TrustedTypes::get_trusted_types_compliant_attribute_value(
        extracted_qualified_name.local_name(),
        extracted_qualified_name.namespace_().has_value() ? Utf16String::from_utf8(extracted_qualified_name.namespace_().value()) : Optional<Utf16String>(),
        *this,
        value));

    // 3. Set an attribute value for this using localName, verifiedValue, and also prefix and namespace.
    set_attribute_value(extracted_qualified_name.local_name(), verified_value.to_utf8_but_should_be_ported_to_utf16(), extracted_qualified_name.prefix(), extracted_qualified_name.namespace_());

    return {};
}

// https://dom.spec.whatwg.org/#concept-element-attributes-append
void Element::append_attribute(FlyString const& name, String const& value)
{
    attributes()->append_attribute(Attr::create(document(), name, value));
}

// https://dom.spec.whatwg.org/#concept-element-attributes-append
void Element::append_attribute(Attr& attribute)
{
    attributes()->append_attribute(attribute);
}

// https://dom.spec.whatwg.org/#concept-element-attributes-set-value
void Element::set_attribute_value(FlyString const& local_name, String const& value, Optional<FlyString> const& prefix, Optional<FlyString> const& namespace_)
{
    // 1. Let attribute be the result of getting an attribute given namespace, localName, and element.
    auto* attribute = attributes()->get_attribute_ns(namespace_, local_name);

    // 2. If attribute is null, create an attribute whose namespace is namespace, namespace prefix is prefix, local name
    //    is localName, value is value, and node document is element’s node document, then append this attribute to element,
    //    and then return.
    if (!attribute) {
        QualifiedName name { local_name, prefix, namespace_ };

        auto new_attribute = Attr::create(document(), move(name), value);
        m_attributes->append_attribute(new_attribute);

        return;
    }

    // 3. Change attribute to value.
    attribute->change_attribute(value);
}

// https://dom.spec.whatwg.org/#dom-element-setattributenode
WebIDL::ExceptionOr<GC::Ptr<Attr>> Element::set_attribute_node_for_bindings(Attr& attr)
{
    // The setAttributeNode(attr) and setAttributeNodeNS(attr) methods steps are to return the result of setting an attribute given attr and this.
    return attributes()->set_attribute(attr);
}

// https://dom.spec.whatwg.org/#dom-element-setattributenodens
WebIDL::ExceptionOr<GC::Ptr<Attr>> Element::set_attribute_node_ns_for_bindings(Attr& attr)
{
    // The setAttributeNode(attr) and setAttributeNodeNS(attr) methods steps are to return the result of setting an attribute given attr and this.
    return attributes()->set_attribute(attr);
}

// https://dom.spec.whatwg.org/#dom-element-removeattribute
void Element::remove_attribute(FlyString const& name)
{
    // The removeAttribute(qualifiedName) method steps are to remove an attribute given qualifiedName and this, and then return undefined.
    if (!m_attributes)
        return;
    m_attributes->remove_attribute(name);
}

// https://dom.spec.whatwg.org/#dom-element-removeattributens
void Element::remove_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name)
{
    // The removeAttributeNS(namespace, localName) method steps are to remove an attribute given namespace, localName, and this, and then return undefined.
    if (!m_attributes)
        return;
    m_attributes->remove_attribute_ns(namespace_, name);
}

// https://dom.spec.whatwg.org/#dom-element-removeattributenode
WebIDL::ExceptionOr<GC::Ref<Attr>> Element::remove_attribute_node(GC::Ref<Attr> attr)
{
    return attributes()->remove_attribute_node(attr);
}

// https://dom.spec.whatwg.org/#dom-element-hasattribute
bool Element::has_attribute(FlyString const& name) const
{
    if (!m_attributes)
        return false;
    return m_attributes->get_attribute(name) != nullptr;
}

// https://dom.spec.whatwg.org/#dom-element-hasattributens
bool Element::has_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name) const
{
    if (!m_attributes)
        return false;

    // 1. If namespace is the empty string, then set it to null.
    // 2. Return true if this has an attribute whose namespace is namespace and local name is localName; otherwise false.
    if (namespace_ == FlyString {})
        return m_attributes->get_attribute_ns(OptionalNone {}, name) != nullptr;

    return m_attributes->get_attribute_ns(namespace_, name) != nullptr;
}

// https://dom.spec.whatwg.org/#dom-element-toggleattribute
WebIDL::ExceptionOr<bool> Element::toggle_attribute(FlyString const& name, Optional<bool> force)
{
    // 1. If qualifiedName is not a valid attribute local name, then throw an "InvalidCharacterError" DOMException.
    if (!is_valid_attribute_local_name(name))
        return WebIDL::InvalidCharacterError::create(realm(), "Attribute name must not be empty or contain invalid characters"_utf16);

    // 2. If this is in the HTML namespace and its node document is an HTML document, then set qualifiedName to qualifiedName in ASCII lowercase.
    bool insert_as_lowercase = namespace_uri() == Namespace::HTML && document().document_type() == Document::Type::HTML;

    // 3. Let attribute be the first attribute in this’s attribute list whose qualified name is qualifiedName, and null otherwise.
    auto* attribute = attributes()->get_attribute(name);

    // 4. If attribute is null, then:
    if (!attribute) {
        // 1. If force is not given or is true, create an attribute whose local name is qualifiedName, value is the empty
        //    string, and node document is this’s node document, then append this attribute to this, and then return true.
        if (!force.has_value() || force.value()) {
            auto new_attribute = Attr::create(document(), insert_as_lowercase ? name.to_ascii_lowercase() : name.to_string(), String {});
            m_attributes->append_attribute(new_attribute);

            return true;
        }

        // 2. Return false.
        return false;
    }

    // 5. Otherwise, if force is not given or is false, remove an attribute given qualifiedName and this, and then return false.
    if (!force.has_value() || !force.value()) {
        m_attributes->remove_attribute(name);
        return false;
    }

    // 6. Return true.
    return true;
}

// https://dom.spec.whatwg.org/#dom-element-getattributenames
Vector<String> Element::get_attribute_names() const
{
    // The getAttributeNames() method steps are to return the qualified names of the attributes in this’s attribute list, in order; otherwise a new list.
    if (!m_attributes)
        return {};
    Vector<String> names;
    for (size_t i = 0; i < m_attributes->length(); ++i) {
        auto const* attribute = m_attributes->item(i);
        names.append(attribute->name().to_string());
    }
    return names;
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#attr-associated-element
GC::Ptr<DOM::Element> Element::get_the_attribute_associated_element(FlyString const& content_attribute, GC::Ptr<DOM::Element> explicitly_set_attribute_element) const
{
    // 1. Let element be the result of running reflectedTarget's get the element.
    auto const& element = *this;

    // 2. Let contentAttributeValue be the result of running reflectedTarget's get the content attribute.
    auto content_attribute_value = element.get_attribute(content_attribute);

    // 3. If reflectedTarget's explicitly set attr-element is not null:
    if (explicitly_set_attribute_element) {
        // 1. If reflectedTarget's explicitly set attr-element is a descendant of any of element's shadow-including
        //    ancestors, then return reflectedTarget's explicitly set attr-element.
        if (&explicitly_set_attribute_element->root() == &element.shadow_including_root())
            return *explicitly_set_attribute_element;

        // 2. Return null.
        return {};
    }

    // 4. Otherwise, if contentAttributeValue is not null, return the first element candidate, in tree order, that meets
    //    the following criteria:
    //     * candidate's root is the same as element's root;
    //     * candidate's ID is contentAttributeValue; and
    //     * candidate implements T.
    if (content_attribute_value.has_value())
        return element.document().get_element_by_id(*content_attribute_value);

    // 5. If no such element exists, then return null.
    // 6. Return null.
    return {};
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#attr-associated-elements
Optional<GC::RootVector<GC::Ref<DOM::Element>>> Element::get_the_attribute_associated_elements(FlyString const& content_attribute, Optional<Vector<GC::Weak<DOM::Element>> const&> explicitly_set_attribute_elements) const
{
    // 1. Let elements be an empty list.
    GC::RootVector<GC::Ref<DOM::Element>> elements(heap());

    // 2. Let element be the result of running reflectedTarget's get the element.
    auto const& element = *this;

    // 3. If reflectedTarget's explicitly set attr-elements is not null:
    if (explicitly_set_attribute_elements.has_value()) {
        // 1. For each attrElement in reflectedTarget's explicitly set attr-elements:
        for (auto const& attribute_element : *explicitly_set_attribute_elements) {
            // 1. If attrElement is not a descendant of any of element's shadow-including ancestors, then continue.
            if (!attribute_element || &attribute_element->root() != &element.shadow_including_root())
                continue;

            // 2. Append attrElement to elements.
            elements.append(*attribute_element);
        }
    }
    // 4. Otherwise:
    else {
        // 1. Let contentAttributeValue be the result of running reflectedTarget's get the content attribute.
        auto content_attribute_value = element.get_attribute(content_attribute);

        // 2. If contentAttributeValue is null, then return null.
        if (!content_attribute_value.has_value())
            return {};

        // 3. Let tokens be contentAttributeValue, split on ASCII whitespace.
        auto tokens = content_attribute_value->bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);

        // 4. For each id of tokens:
        for (auto id : tokens) {
            // 1. Let candidate be the first element, in tree order, that meets the following criteria:
            //     * candidate's root is the same as element's root;
            //     * candidate's ID is id; and
            //     * candidate implements T.
            auto candidate = element.document().get_element_by_id(MUST(FlyString::from_utf8(id)));

            // 2. If no such element exists, then continue.
            if (!candidate)
                continue;

            // 3. Append candidate to elements.
            elements.append(*candidate);
        }
    }

    // 5. Return elements.
    return elements;
}

GC::Ptr<Layout::Node> Element::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    if (local_name() == "noscript" && document().is_scripting_enabled())
        return nullptr;

    auto display = style->display();
    return create_layout_node_for_display_type(document(), display, style, this);
}

GC::Ptr<Layout::NodeWithStyle> Element::create_layout_node_for_display_type(DOM::Document& document, CSS::Display const& display, GC::Ref<CSS::ComputedProperties> style, Element* element)
{
    if (display.is_none())
        return {};

    if (display.is_table_inside() || display.is_table_row_group() || display.is_table_header_group() || display.is_table_footer_group() || display.is_table_row())
        return document.heap().allocate<Layout::Box>(document, element, move(style));

    if (display.is_list_item())
        return document.heap().allocate<Layout::ListItemBox>(document, element, move(style));

    if (display.is_table_cell())
        return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));

    if (display.is_table_column() || display.is_table_column_group() || display.is_table_caption()) {
        // FIXME: This is just an incorrect placeholder until we improve table layout support.
        return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));
    }

    if (display.is_math_inside()) {
        // https://w3c.github.io/mathml-core/#new-display-math-value
        // MathML elements with a computed display value equal to block math or inline math control box generation
        // and layout according to their tag name, as described in the relevant sections.
        // FIXME: Figure out what kind of node we should make for them. For now, we'll stick with a generic Box.
        return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));
    }

    if (display.is_inline_outside()) {
        if (display.is_flow_root_inside())
            return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));
        if (display.is_flow_inside())
            return document.heap().allocate<Layout::InlineNode>(document, element, move(style));
        if (display.is_flex_inside())
            return document.heap().allocate<Layout::Box>(document, element, move(style));
        if (display.is_grid_inside())
            return document.heap().allocate<Layout::Box>(document, element, move(style));
        dbgln_if(LIBWEB_CSS_DEBUG, "FIXME: Support display: {}", display.to_string());
        return document.heap().allocate<Layout::InlineNode>(document, element, move(style));
    }

    if (display.is_flex_inside() || display.is_grid_inside())
        return document.heap().allocate<Layout::Box>(document, element, move(style));

    if (display.is_flow_inside() || display.is_flow_root_inside() || display.is_contents())
        return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));

    dbgln("FIXME: CSS display '{}' not implemented yet.", display.to_string());

    // FIXME: We don't actually support `display: block ruby`, this is just a hack to prevent a crash
    if (display.is_ruby_inside())
        return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));

    return document.heap().allocate<Layout::InlineNode>(document, element, move(style));
}

void Element::run_attribute_change_steps(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    attribute_changed(local_name, old_value, value, namespace_);

    if (old_value != value) {
        invalidate_style_after_attribute_change(local_name, old_value, value);
        document().bump_dom_tree_version();
    }
}

static CSS::RequiredInvalidationAfterStyleChange compute_required_invalidation(CSS::ComputedProperties const& old_style, CSS::ComputedProperties const& new_style, CSS::FontComputer const& font_computer)
{
    CSS::RequiredInvalidationAfterStyleChange invalidation;

    if (old_style.computed_font_list(font_computer) != new_style.computed_font_list(font_computer))
        invalidation.relayout = true;

    for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
        auto property_id = static_cast<CSS::PropertyID>(i);

        invalidation |= CSS::compute_property_invalidation(property_id, old_style.property(property_id), new_style.property(property_id));
    }
    return invalidation;
}

CSS::RequiredInvalidationAfterStyleChange Element::recompute_style(bool& did_change_custom_properties)
{
    VERIFY(parent());

    m_style_uses_attr_css_function = false;
    m_style_uses_var_css_function = false;
    m_affected_by_has_pseudo_class_in_subject_position = false;
    m_affected_by_has_pseudo_class_in_non_subject_position = false;
    m_affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator = false;
    m_affected_by_direct_sibling_combinator = false;
    m_affected_by_indirect_sibling_combinator = false;
    m_affected_by_sibling_position_or_count_pseudo_class = false;
    m_affected_by_nth_child_pseudo_class = false;
    m_sibling_invalidation_distance = 0;

    auto& style_computer = document().style_computer();
    auto new_computed_properties = style_computer.compute_style({ *this }, did_change_custom_properties);

    // Tables must not inherit -libweb-* values for text-align.
    // FIXME: Find the spec for this.
    if (is<HTML::HTMLTableElement>(*this)) {
        auto text_align = new_computed_properties->text_align();
        if (text_align == CSS::TextAlign::LibwebLeft || text_align == CSS::TextAlign::LibwebCenter || text_align == CSS::TextAlign::LibwebRight)
            new_computed_properties->set_property(CSS::PropertyID::TextAlign, CSS::KeywordStyleValue::create(CSS::Keyword::Start));
    }

    bool had_list_marker = false;

    CSS::RequiredInvalidationAfterStyleChange invalidation;
    if (m_computed_properties) {
        invalidation = compute_required_invalidation(*m_computed_properties, new_computed_properties, document().font_computer());
        had_list_marker = m_computed_properties->display().is_list_item();
    } else {
        invalidation = CSS::RequiredInvalidationAfterStyleChange::full();
    }

    auto old_display_is_none = m_computed_properties ? m_computed_properties->display().is_none() : true;
    auto new_display_is_none = new_computed_properties->display().is_none();

    set_computed_properties({}, move(new_computed_properties));

    if (old_display_is_none != new_display_is_none) {
        for_each_shadow_including_inclusive_descendant([&](auto& node) {
            if (!node.is_element())
                return TraversalDecision::Continue;
            auto& element = static_cast<Element&>(node);
            element.play_or_cancel_animations_after_display_property_change();
            return TraversalDecision::Continue;
        });
    }

    // Any document change that can cause this element's style to change, could also affect its pseudo-elements.
    auto recompute_pseudo_element_style = [&](CSS::PseudoElement pseudo_element) {
        style_computer.push_ancestor(*this);

        auto pseudo_element_style = computed_properties(pseudo_element);
        auto new_pseudo_element_style = style_computer.compute_pseudo_element_style_if_needed({ *this, pseudo_element }, did_change_custom_properties);

        // TODO: Can we be smarter about invalidation?
        if (pseudo_element_style && new_pseudo_element_style) {
            invalidation |= compute_required_invalidation(*pseudo_element_style, *new_pseudo_element_style, document().font_computer());
        } else if (pseudo_element_style || new_pseudo_element_style) {
            invalidation = CSS::RequiredInvalidationAfterStyleChange::full();
        }

        set_computed_properties(pseudo_element, move(new_pseudo_element_style));
        style_computer.pop_ancestor(*this);
    };

    recompute_pseudo_element_style(CSS::PseudoElement::Before);
    recompute_pseudo_element_style(CSS::PseudoElement::After);
    recompute_pseudo_element_style(CSS::PseudoElement::Selection);
    if (m_rendered_in_top_layer)
        recompute_pseudo_element_style(CSS::PseudoElement::Backdrop);
    if (had_list_marker || m_computed_properties->display().is_list_item())
        recompute_pseudo_element_style(CSS::PseudoElement::Marker);

    if (invalidation.is_none())
        return invalidation;

    if (invalidation.repaint && paintable())
        paintable()->set_needs_paint_only_properties_update(true);

    if (!invalidation.rebuild_layout_tree && layout_node()) {
        // If we're keeping the layout tree, we can just apply the new style to the existing layout tree.
        layout_node()->apply_style(*m_computed_properties);
        if (invalidation.repaint && paintable()) {
            paintable()->set_needs_paint_only_properties_update(true);
            paintable()->set_needs_display();
        }

        // Do the same for pseudo-elements.
        for (auto i = 0; i < to_underlying(CSS::PseudoElement::KnownPseudoElementCount); i++) {
            auto pseudo_element_type = static_cast<CSS::PseudoElement>(i);
            auto pseudo_element = get_pseudo_element(pseudo_element_type);
            if (!pseudo_element.has_value() || !pseudo_element->layout_node())
                continue;

            auto pseudo_element_style = computed_properties(pseudo_element_type);
            if (!pseudo_element_style)
                continue;

            if (auto node_with_style = pseudo_element->layout_node()) {
                node_with_style->apply_style(*pseudo_element_style);
                if (invalidation.repaint && node_with_style->first_paintable()) {
                    node_with_style->first_paintable()->set_needs_paint_only_properties_update(true);
                    node_with_style->first_paintable()->set_needs_display();
                }
            }
        }
    }

    return invalidation;
}

CSS::RequiredInvalidationAfterStyleChange Element::recompute_inherited_style()
{
    auto computed_properties = this->computed_properties();
    if (!m_cascaded_properties || !computed_properties || !layout_node())
        return {};

    CSS::RequiredInvalidationAfterStyleChange invalidation;

    HashMap<size_t, RefPtr<CSS::StyleValue const>> property_values_affected_by_inherited_style;
    for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
        auto property_id = static_cast<CSS::PropertyID>(i);
        // FIXME: We should use the specified value rather than the cascaded value as the cascaded value may include
        //        unresolved CSS-wide keywords (e.g. 'initial' or 'inherit') rather than the resolved value.
        auto const& preabsolutized_value = m_cascaded_properties->property(property_id);
        RefPtr old_value = computed_properties->property(property_id);

        if (preabsolutized_value) {
            // A property needs updating if:
            // - It uses relative units as it might have been affected by a change in ancestor element style.
            //   FIXME: Consider other style values that rely on relative lengths (e.g. CalculatedStyleValue,
            //          StyleValues which contain lengths (e.g. StyleValueList))
            // - font-weight is `bolder` or `lighter`
            // - font-size is `larger` or `smaller`
            // FIXME: Consider any other properties that rely on inherited values for computation.
            auto needs_updating = (preabsolutized_value->is_length() && preabsolutized_value->as_length().length().is_font_relative())
                || (property_id == CSS::PropertyID::FontWeight && first_is_one_of(preabsolutized_value->to_keyword(), CSS::Keyword::Bolder, CSS::Keyword::Lighter))
                || (property_id == CSS::PropertyID::FontSize && first_is_one_of(preabsolutized_value->to_keyword(), CSS::Keyword::Larger, CSS::Keyword::Smaller));
            if (needs_updating) {
                computed_properties->set_property_without_modifying_flags(property_id, *preabsolutized_value);
                property_values_affected_by_inherited_style.set(i, old_value);
            }
        }

        if (!computed_properties->is_property_inherited(property_id))
            continue;

        if (computed_properties->is_animated_property_inherited(property_id) || !computed_properties->animated_property_values().contains(property_id)) {
            if (auto new_animated_value = CSS::StyleComputer::get_animated_inherit_value(property_id, { *this }); new_animated_value.has_value())
                computed_properties->set_animated_property(property_id, new_animated_value->value, new_animated_value->is_result_of_transition, CSS::ComputedProperties::Inherited::Yes);
            else if (computed_properties->animated_property_values().contains(property_id))
                computed_properties->remove_animated_property(property_id);
        }

        RefPtr new_value = CSS::StyleComputer::get_non_animated_inherit_value(property_id, { *this });
        computed_properties->set_property(property_id, *new_value, CSS::ComputedProperties::Inherited::Yes);
        invalidation |= CSS::compute_property_invalidation(property_id, old_value, computed_properties->property(property_id));
    }

    if (invalidation.is_none() && property_values_affected_by_inherited_style.is_empty())
        return invalidation;

    AbstractElement abstract_element { *this };

    document().style_computer().compute_property_values(*computed_properties, abstract_element);

    for (auto const& [property_id, old_value] : property_values_affected_by_inherited_style) {
        auto const& new_value = computed_properties->property(static_cast<CSS::PropertyID>(property_id));
        invalidation |= CSS::compute_property_invalidation(static_cast<CSS::PropertyID>(property_id), old_value, new_value);
    }

    if (invalidation.is_none())
        return invalidation;

    layout_node()->apply_style(*computed_properties);
    return invalidation;
}

GC::Ref<DOMTokenList> Element::class_list()
{
    if (!m_class_list)
        m_class_list = DOMTokenList::create(*this, HTML::AttributeNames::class_);
    return *m_class_list;
}

// https://drafts.csswg.org/css-shadow-1/#dom-element-part
GC::Ref<DOMTokenList> Element::part_list()
{
    // The part attribute’s getter must return a DOMTokenList object whose associated element is the context object and
    // whose associated attribute’s local name is part.
    if (!m_part_list)
        m_part_list = DOMTokenList::create(*this, HTML::AttributeNames::part);
    return *m_part_list;
}

// https://dom.spec.whatwg.org/#valid-shadow-host-name
static bool is_valid_shadow_host_name(FlyString const& name)
{
    // A valid shadow host name is:
    // - a valid custom element name
    // - "article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3", "h4", "h5", "h6", "header", "main", "nav", "p", "section", or "span"
    if (!HTML::is_valid_custom_element_name(name)
        && !name.is_one_of("article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3", "h4", "h5", "h6", "header", "main", "nav", "p", "section", "span")) {
        return false;
    }
    return true;
}

// https://dom.spec.whatwg.org/#concept-attach-a-shadow-root
WebIDL::ExceptionOr<void> Element::attach_a_shadow_root(Bindings::ShadowRootMode mode, bool clonable, bool serializable, bool delegates_focus, Bindings::SlotAssignmentMode slot_assignment)
{
    // 1. If element’s namespace is not the HTML namespace, then throw a "NotSupportedError" DOMException.
    if (namespace_uri() != Namespace::HTML)
        return WebIDL::NotSupportedError::create(realm(), "Element's namespace is not the HTML namespace"_utf16);

    // 2. If element’s local name is not a valid shadow host name, then throw a "NotSupportedError" DOMException.
    if (!is_valid_shadow_host_name(local_name()))
        return WebIDL::NotSupportedError::create(realm(), "Element's local name is not a valid shadow host name"_utf16);

    // 3. If element’s local name is a valid custom element name, or element’s is value is not null, then:
    if (HTML::is_valid_custom_element_name(local_name()) || m_is_value.has_value()) {
        // 1. Let definition be the result of looking up a custom element definition given element’s node document, its namespace, its local name, and its is value.
        auto definition = document().lookup_custom_element_definition(namespace_uri(), local_name(), m_is_value);

        // 2. If definition is not null and definition’s disable shadow is true, then throw a "NotSupportedError" DOMException.
        if (definition && definition->disable_shadow())
            return WebIDL::NotSupportedError::create(realm(), "Cannot attach a shadow root to a custom element that has disabled shadow roots"_utf16);
    }

    // 4. If element is a shadow host, then:
    if (is_shadow_host()) {
        // 1. Let currentShadowRoot be element’s shadow root.
        auto current_shadow_root = shadow_root();

        // 2. If any of the following are true:
        // - currentShadowRoot’s declarative is false; or
        // - currentShadowRoot’s mode is not mode,
        // then throw a "NotSupportedError" DOMException.
        if (!current_shadow_root->declarative() || current_shadow_root->mode() != mode) {
            return WebIDL::NotSupportedError::create(realm(), "Element already is a shadow host"_utf16);
        }

        // 3. Otherwise:
        //    1. Remove all of currentShadowRoot’s children, in tree order.
        current_shadow_root->remove_all_children();

        //    2. Set currentShadowRoot’s declarative to false.
        current_shadow_root->set_declarative(false);

        //    3. Return.
        return {};
    }

    // 5. Let shadow be a new shadow root whose node document is element’s node document, host is this, and mode is mode.
    auto shadow = realm().create<ShadowRoot>(document(), *this, mode);

    // 6. Set shadow’s delegates focus to delegatesFocus".
    shadow->set_delegates_focus(delegates_focus);

    // 7. If element’s custom element state is "precustomized" or "custom", then set shadow’s available to element internals to true.
    if (m_custom_element_state == CustomElementState::Precustomized || m_custom_element_state == CustomElementState::Custom)
        shadow->set_available_to_element_internals(true);

    // 8. Set shadow’s slot assignment to slotAssignment.
    shadow->set_slot_assignment(slot_assignment);

    // 9. Set shadow’s declarative to false.
    shadow->set_declarative(false);

    // 10. Set shadow’s clonable to clonable.
    shadow->set_clonable(clonable);

    // 11. Set shadow’s serializable to serializable.
    shadow->set_serializable(serializable);

    // 12. Set element’s shadow root to shadow.
    set_shadow_root(shadow);
    return {};
}

// https://dom.spec.whatwg.org/#dom-element-attachshadow
WebIDL::ExceptionOr<GC::Ref<ShadowRoot>> Element::attach_shadow(ShadowRootInit init)
{
    // 1. Run attach a shadow root with this, init["mode"], init["clonable"], init["serializable"], init["delegatesFocus"], and init["slotAssignment"].
    TRY(attach_a_shadow_root(init.mode, init.clonable, init.serializable, init.delegates_focus, init.slot_assignment));

    // 2. Return this’s shadow root.
    return GC::Ref { *shadow_root() };
}

// https://dom.spec.whatwg.org/#dom-element-shadowroot
GC::Ptr<ShadowRoot> Element::shadow_root_for_bindings() const
{
    // 1. Let shadow be this’s shadow root.
    auto shadow = m_shadow_root;

    // 2. If shadow is null or its mode is "closed", then return null.
    if (shadow == nullptr || shadow->mode() == Bindings::ShadowRootMode::Closed)
        return nullptr;

    // 3. Return shadow.
    return shadow;
}

// https://dom.spec.whatwg.org/#dom-element-matches
WebIDL::ExceptionOr<bool> Element::matches(StringView selectors) const
{
    // 1. Let s be the result of parse a selector from selectors.
    auto maybe_selectors = parse_selector(CSS::Parser::ParsingParams(document()), selectors);

    // 2. If s is failure, then throw a "SyntaxError" DOMException.
    if (!maybe_selectors.has_value())
        return WebIDL::SyntaxError::create(realm(), "Failed to parse selector"_utf16);

    // 3. If the result of match a selector against an element, using s, this, and scoping root this, returns success, then return true; otherwise, return false.
    auto sel = maybe_selectors.value();
    for (auto& s : sel) {
        SelectorEngine::MatchContext context;
        if (SelectorEngine::matches(s, *this, nullptr, context, {}, static_cast<ParentNode const*>(this)))
            return true;
    }
    return false;
}

// https://dom.spec.whatwg.org/#dom-element-closest
WebIDL::ExceptionOr<DOM::Element const*> Element::closest(StringView selectors) const
{
    // 1. Let s be the result of parse a selector from selectors.
    auto maybe_selectors = parse_selector(CSS::Parser::ParsingParams(document()), selectors);

    // 2. If s is failure, then throw a "SyntaxError" DOMException.
    if (!maybe_selectors.has_value())
        return WebIDL::SyntaxError::create(realm(), "Failed to parse selector"_utf16);

    auto matches_selectors = [this](CSS::SelectorList const& selector_list, Element const* element) {
        // 4. For each element in elements, if match a selector against an element, using s, element, and scoping root this, returns success, return element.
        for (auto const& selector : selector_list) {
            SelectorEngine::MatchContext context;
            if (SelectorEngine::matches(selector, *element, nullptr, context, {}, this))
                return true;
        }
        return false;
    };

    auto const selector_list = maybe_selectors.release_value();

    // 3. Let elements be this’s inclusive ancestors that are elements, in reverse tree order.
    for (auto* element = this; element; element = element->parent_element()) {
        if (!matches_selectors(selector_list, element))
            continue;

        return element;
    }

    // 5. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-innerhtml
WebIDL::ExceptionOr<void> Element::set_inner_html(TrustedTypes::TrustedHTMLOrString const& value)
{
    // 1. Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, the given value, "Element innerHTML", and "script".
    auto const compliant_string = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::relevant_global_object(*this),
        value,
        TrustedTypes::InjectionSink::Element_innerHTML,
        TrustedTypes::Script.to_string()));

    // 2. Let context be this.
    DOM::Node* context = this;

    // 3. Let fragment be the result of invoking the fragment parsing algorithm steps with context and compliantString.
    auto fragment = TRY(as<Element>(*context).parse_fragment(compliant_string.to_utf8_but_should_be_ported_to_utf16()));

    // 4. If context is a template element, then set context to the template element's template contents (a DocumentFragment).
    auto* template_element = as_if<HTML::HTMLTemplateElement>(*context);
    if (template_element)
        context = template_element->content();

    // 5. Replace all with fragment within context.
    context->replace_all(fragment);

    // NOTE: We don't invalidate style & layout for <template> elements since they don't affect rendering.
    if (!template_element) {
        context->set_needs_style_update(true);

        if (context->is_connected()) {
            // NOTE: Since the DOM has changed, we have to rebuild the layout tree.
            context->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::ElementSetInnerHTML);
        }
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-innerhtml
WebIDL::ExceptionOr<TrustedTypes::TrustedHTMLOrString> Element::inner_html() const
{
    return TRY(serialize_fragment(HTML::RequireWellFormed::Yes));
}

bool Element::is_focused() const
{
    return document().focused_area() == this;
}

bool Element::is_active() const
{
    return document().active_element() == this;
}

bool Element::is_target() const
{
    return document().target_element() == this;
}

// https://dom.spec.whatwg.org/#document-element
bool Element::is_document_element() const
{
    // The document element of a document is the element whose parent is that document, if it exists; otherwise null.
    return parent() == &document();
}

// https://dom.spec.whatwg.org/#element-shadow-host
bool Element::is_shadow_host() const
{
    // An element is a shadow host if its shadow root is non-null.
    return m_shadow_root != nullptr;
}

void Element::set_shadow_root(GC::Ptr<ShadowRoot> shadow_root)
{
    if (m_shadow_root == shadow_root)
        return;
    if (m_shadow_root)
        m_shadow_root->set_host(nullptr);
    m_shadow_root = move(shadow_root);
    if (m_shadow_root)
        m_shadow_root->set_host(this);
    invalidate_style(StyleInvalidationReason::ElementSetShadowRoot);
}

GC::Ref<CSS::CSSStyleProperties> Element::style_for_bindings()
{
    if (!m_inline_style)
        m_inline_style = CSS::CSSStyleProperties::create_element_inline_style({ *this }, {}, {});
    return *m_inline_style;
}

GC::Ref<CSS::StylePropertyMap> Element::attribute_style_map()
{
    if (!m_attribute_style_map)
        m_attribute_style_map = CSS::StylePropertyMap::create(realm(), style_for_bindings());
    return *m_attribute_style_map;
}

void Element::set_inline_style(GC::Ptr<CSS::CSSStyleProperties> style)
{
    m_inline_style = style;
    if (m_attribute_style_map)
        m_attribute_style_map = nullptr;
    set_needs_style_update(true);
}

// https://dom.spec.whatwg.org/#element-html-uppercased-qualified-name
FlyString Element::make_html_uppercased_qualified_name() const
{
    // This is allowed by the spec: "User agents could optimize qualified name and HTML-uppercased qualified name by storing them in internal slots."
    if (namespace_uri() == Namespace::HTML && document().document_type() == Document::Type::HTML)
        return qualified_name().to_ascii_uppercase();
    return qualified_name();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#queue-an-element-task
HTML::TaskID Element::queue_an_element_task(HTML::Task::Source source, Function<void()> steps)
{
    return queue_a_task(source, HTML::main_thread_event_loop(), document(), GC::create_function(heap(), move(steps)));
}

// https://html.spec.whatwg.org/multipage/syntax.html#void-elements
bool Element::is_void_element() const
{
    return local_name().is_one_of(HTML::TagNames::area, HTML::TagNames::base, HTML::TagNames::br, HTML::TagNames::col, HTML::TagNames::embed, HTML::TagNames::hr, HTML::TagNames::img, HTML::TagNames::input, HTML::TagNames::link, HTML::TagNames::meta, HTML::TagNames::param, HTML::TagNames::source, HTML::TagNames::track, HTML::TagNames::wbr);
}

// https://html.spec.whatwg.org/multipage/parsing.html#serializes-as-void
bool Element::serializes_as_void() const
{
    return is_void_element() || local_name().is_one_of(HTML::TagNames::basefont, HTML::TagNames::bgsound, HTML::TagNames::frame, HTML::TagNames::keygen);
}

// https://drafts.csswg.org/cssom-view/#dom-element-getboundingclientrect
GC::Ref<Geometry::DOMRect> Element::get_bounding_client_rect_for_bindings() const
{
    auto rect = get_bounding_client_rect();
    return MUST(Geometry::DOMRect::construct_impl(realm(), static_cast<double>(rect.x()), static_cast<double>(rect.y()), static_cast<double>(rect.width()), static_cast<double>(rect.height())));
}

// https://drafts.csswg.org/cssom-view/#dom-element-getboundingclientrect
CSSPixelRect Element::get_bounding_client_rect() const
{
    // 1. Let list be the result of invoking getClientRects() on element.
    auto list = get_client_rects();

    // 2. If the list is empty return a DOMRect object whose x, y, width and height members are zero.
    if (list.size() == 0)
        return { 0, 0, 0, 0 };

    // 3. If all rectangles in list have zero width or height, return the first rectangle in list.
    auto all_rectangle_has_zero_width_or_height = true;
    for (auto i = 0u; i < list.size(); ++i) {
        auto const& rect = list.at(i);
        if (rect.width() != 0 && rect.height() != 0) {
            all_rectangle_has_zero_width_or_height = false;
            break;
        }
    }
    if (all_rectangle_has_zero_width_or_height)
        return list.at(0);

    // 4. Otherwise, return a DOMRect object describing the smallest rectangle that includes all of the rectangles in
    //    list of which the height or width is not zero.
    auto bounding_rect = list.at(0);
    for (auto i = 1u; i < list.size(); ++i) {
        auto const& rect = list.at(i);
        if (rect.width() == 0 || rect.height() == 0)
            continue;
        bounding_rect.unite(rect);
    }
    return bounding_rect;
}

// https://drafts.csswg.org/cssom-view/#dom-element-getclientrects
GC::Ref<Geometry::DOMRectList> Element::get_client_rects_for_bindings() const
{
    Vector<GC::Root<Geometry::DOMRect>> rects;
    for (auto const& rect : get_client_rects()) {
        rects.append(MUST(Geometry::DOMRect::construct_impl(realm(), static_cast<double>(rect.x()), static_cast<double>(rect.y()), static_cast<double>(rect.width()), static_cast<double>(rect.height()))));
    }
    return Geometry::DOMRectList::create(realm(), move(rects));
}

// https://drafts.csswg.org/cssom-view/#dom-element-getclientrects
Vector<CSSPixelRect> Element::get_client_rects() const
{
    auto navigable = document().navigable();
    if (!navigable)
        return {};

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementGetClientRects);

    // 1. If the element on which it was invoked does not have an associated layout box return an empty DOMRectList
    //    object and stop this algorithm.
    if (!layout_node())
        return {};

    // FIXME: 2. If the element has an associated SVG layout box return a DOMRectList object containing a single
    //          DOMRect object that describes the bounding box of the element as defined by the SVG specification,
    //          applying the transforms that apply to the element and its ancestors.

    // 3. Return a DOMRectList object containing DOMRect objects in content order, one for each box fragment,
    // describing its border area (including those with a height or width of zero) with the following constraints:
    // - Apply the transforms that apply to the element and its ancestors.
    // FIXME: - If the element on which the method was invoked has a computed value for the display property of table
    //          or inline-table include both the table box and the caption box, if any, but not the anonymous container box.
    // FIXME: - Replace each anonymous block box with its child box(es) and repeat this until no anonymous block boxes
    //          are left in the final list.

    // NOTE: Make sure CSS transforms are resolved before it is used to calculate the rect position.
    const_cast<Document&>(document()).update_paint_and_hit_testing_properties_if_needed();

    Vector<CSSPixelRect> rects;
    if (auto const* paintable_box = this->paintable_box()) {
        auto absolute_rect = paintable_box->absolute_border_box_rect();

        if (auto const& accumulated_visual_context = paintable_box->accumulated_visual_context()) {
            auto const& viewport_paintable = *document().paintable();
            auto const& scroll_state = viewport_paintable.scroll_state_snapshot();
            auto transformed_rect = accumulated_visual_context->transform_rect_to_viewport(absolute_rect, scroll_state);
            rects.append(transformed_rect);
        } else {
            rects.append(absolute_rect);
        }
    } else if (paintable()) {
        dbgln("FIXME: Failed to get client rects for element ({})", debug_description());
    }

    return rects;
}

int Element::client_top() const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementClientTop);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    if (!paintable_box())
        return 0;

    // 2. Return the computed value of the border-top-width property
    //    plus the height of any scrollbar rendered between the top padding edge and the top border edge,
    //    ignoring any transforms that apply to the element and its ancestors.
    return paintable_box()->computed_values().border_top().width.to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-element-clientleft
int Element::client_left() const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementClientLeft);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    if (!paintable_box())
        return 0;

    // 2. Return the computed value of the border-left-width property
    //    plus the width of any scrollbar rendered between the left padding edge and the left border edge,
    //    ignoring any transforms that apply to the element and its ancestors.
    return paintable_box()->computed_values().border_left().width.to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-element-clientwidth
int Element::client_width() const
{
    // NOTE: We do step 2 before step 1 here since step 2 can exit early without needing to perform layout.

    // 2. If the element is the root element and the element’s node document is not in quirks mode,
    //    or if the element is the HTML body element and the element’s node document is in quirks mode,
    //    return the viewport width excluding the size of a rendered scroll bar (if any).
    if ((is<HTML::HTMLHtmlElement>(*this) && !document().in_quirks_mode())
        || (is<HTML::HTMLBodyElement>(*this) && document().in_quirks_mode())) {
        return document().viewport_rect().width().to_int();
    }

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementClientWidth);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    if (!paintable_box())
        return 0;

    // 3. Return the width of the padding edge excluding the width of any rendered scrollbar between the padding edge and the border edge,
    // ignoring any transforms that apply to the element and its ancestors.
    return paintable_box()->absolute_padding_box_rect().width().to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-element-clientheight
int Element::client_height() const
{
    // NOTE: We do step 2 before step 1 here since step 2 can exit early without needing to perform layout.

    // 2. If the element is the root element and the element’s node document is not in quirks mode,
    //    or if the element is the HTML body element and the element’s node document is in quirks mode,
    //    return the viewport height excluding the size of a rendered scroll bar (if any).
    if ((is<HTML::HTMLHtmlElement>(*this) && !document().in_quirks_mode())
        || (is<HTML::HTMLBodyElement>(*this) && document().in_quirks_mode())) {
        return document().viewport_rect().height().to_int();
    }

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementClientHeight);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    if (!paintable_box())
        return 0;

    // 3. Return the height of the padding edge excluding the height of any rendered scrollbar between the padding edge and the border edge,
    //    ignoring any transforms that apply to the element and its ancestors.
    return paintable_box()->absolute_padding_box_rect().height().to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-element-currentcsszoom
double Element::current_css_zoom() const
{
    dbgln("FIXME: Implement Element::current_css_zoom()");
    return 1.0;
}

void Element::inserted()
{
    Base::inserted();

    if (is_connected()) {
        if (m_id.has_value())
            document().element_with_id_was_added({}, *this);
        if (m_name.has_value())
            document().element_with_name_was_added({}, *this);
    }

    play_or_cancel_animations_after_display_property_change();
}

void Element::removed_from(Node* old_parent, Node& old_root)
{
    Base::removed_from(old_parent, old_root);

    if (old_root.is_connected()) {
        if (m_id.has_value())
            document().element_with_id_was_removed({}, *this);
        if (m_name.has_value())
            document().element_with_name_was_removed({}, *this);
    }

    play_or_cancel_animations_after_display_property_change();
}

void Element::moved_from(GC::Ptr<Node> old_parent)
{
    Base::moved_from(old_parent);
}

void Element::children_changed(ChildrenChangedMetadata const* metadata)
{
    Node::children_changed(metadata);
    set_needs_style_update(true);

    if (child_style_uses_tree_counting_function()) {
        for_each_child_of_type<Element>([&](Element& element) {
            element.set_needs_style_update(true);
            set_child_needs_style_update(true);

            return IterationDecision::Continue;
        });
    }
}

void Element::set_pseudo_element_node(Badge<Layout::TreeBuilder>, CSS::PseudoElement pseudo_element, GC::Ptr<Layout::NodeWithStyle> pseudo_element_node)
{
    auto existing_pseudo_element = get_pseudo_element(pseudo_element);
    if (!existing_pseudo_element.has_value() && !pseudo_element_node)
        return;

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element)) {
        return;
    }

    ensure_pseudo_element(pseudo_element).set_layout_node(move(pseudo_element_node));
}

GC::Ptr<Layout::NodeWithStyle> Element::get_pseudo_element_node(CSS::PseudoElement pseudo_element) const
{
    if (auto element_data = get_pseudo_element(pseudo_element); element_data.has_value())
        return element_data->layout_node();
    return nullptr;
}

bool Element::affected_by_pseudo_class(CSS::PseudoClass pseudo_class) const
{
    if (m_computed_properties && m_computed_properties->has_attempted_match_against_pseudo_class(pseudo_class)) {
        return true;
    }
    if (m_pseudo_element_data) {
        for (auto& pseudo_element : *m_pseudo_element_data) {
            if (!pseudo_element.value->computed_properties())
                continue;
            if (pseudo_element.value->computed_properties()->has_attempted_match_against_pseudo_class(pseudo_class))
                return true;
        }
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-enabled
bool Element::matches_enabled_pseudo_class() const
{
    // The :enabled pseudo-class must match any button, input, select, textarea, optgroup, option, fieldset element, or form-associated custom element that is not actually disabled.
    return (is<HTML::HTMLButtonElement>(*this) || is<HTML::HTMLInputElement>(*this) || is<HTML::HTMLSelectElement>(*this) || is<HTML::HTMLTextAreaElement>(*this) || is<HTML::HTMLOptGroupElement>(*this) || is<HTML::HTMLOptionElement>(*this) || is<HTML::HTMLFieldSetElement>(*this))
        && !is_actually_disabled();
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-disabled
bool Element::matches_disabled_pseudo_class() const
{
    // The :disabled pseudo-class must match any element that is actually disabled.
    return is_actually_disabled();
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-checked
bool Element::matches_checked_pseudo_class() const
{
    // The :checked pseudo-class must match any element falling into one of the following categories:
    // - input elements whose type attribute is in the Checkbox state and whose checkedness state is true
    // - input elements whose type attribute is in the Radio Button state and whose checkedness state is true
    if (auto* input_element = as_if<HTML::HTMLInputElement>(*this)) {
        switch (input_element->type_state()) {
        case HTML::HTMLInputElement::TypeAttributeState::Checkbox:
        case HTML::HTMLInputElement::TypeAttributeState::RadioButton:
            return static_cast<HTML::HTMLInputElement const&>(*this).checked();
        default:
            return false;
        }
    }

    // - option elements whose selectedness is true
    if (auto* option_element = as_if<HTML::HTMLOptionElement>(*this)) {
        return option_element->selected();
    }
    return false;
}

bool Element::matches_unchecked_pseudo_class() const
{
    // AD-HOC: There is no spec for this yet, so it's based on the spec for :checked, assuming that :unchecked applies to the same cases but with a `false` value.
    if (auto* input_element = as_if<HTML::HTMLInputElement>(*this)) {
        switch (input_element->type_state()) {
        case HTML::HTMLInputElement::TypeAttributeState::Checkbox:
        case HTML::HTMLInputElement::TypeAttributeState::RadioButton:
            return !static_cast<HTML::HTMLInputElement const&>(*this).checked();
        default:
            return false;
        }
    }

    if (auto* option_element = as_if<HTML::HTMLOptionElement>(*this)) {
        return !option_element->selected();
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-placeholder-shown
bool Element::matches_placeholder_shown_pseudo_class() const
{
    //  The :placeholder-shown pseudo-class must match any element falling into one of the following categories:
    // - input elements that have a placeholder attribute whose value is currently being presented to the user.
    if (is<HTML::HTMLInputElement>(*this) && has_attribute(HTML::AttributeNames::placeholder)) {
        auto const& input_element = static_cast<HTML::HTMLInputElement const&>(*this);
        return input_element.placeholder_element() && input_element.placeholder_value().has_value();
    }
    // - textarea elements that have a placeholder attribute whose value is currently being presented to the user.
    if (is<HTML::HTMLTextAreaElement>(*this) && has_attribute(HTML::AttributeNames::placeholder)) {
        auto const& textarea_element = static_cast<HTML::HTMLTextAreaElement const&>(*this);
        return textarea_element.placeholder_element() && textarea_element.placeholder_value().has_value();
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-link
bool Element::matches_link_pseudo_class() const
{
    // All a elements that have an href attribute, and all area elements that have an href attribute, must match one of :link and :visited.
    if (!is<HTML::HTMLAnchorElement>(*this) && !is<HTML::HTMLAreaElement>(*this) && !is<SVG::SVGAElement>(*this))
        return false;
    return has_attribute(HTML::AttributeNames::href);
}

bool Element::matches_local_link_pseudo_class() const
{
    // The :local-link pseudo-class allows authors to style hyperlinks based on the users current location
    // within a site. It represents an element that is the source anchor of a hyperlink whose target’s
    // absolute URL matches the element’s own document URL. If the hyperlink’s target includes a fragment
    // URL, then the fragment URL of the current URL must also match; if it does not, then the fragment
    // URL portion of the current URL is not taken into account in the comparison.
    if (!matches_link_pseudo_class())
        return false;
    auto document_url = document().url();
    auto maybe_href = attribute(HTML::AttributeNames::href);
    if (!maybe_href.has_value())
        return false;
    auto target_url = document().encoding_parse_url(*maybe_href);
    if (!target_url.has_value())
        return false;
    if (target_url->fragment().has_value())
        return document_url.equals(*target_url, URL::ExcludeFragment::No);
    return document_url.equals(*target_url, URL::ExcludeFragment::Yes);
}

bool Element::includes_properties_from_invalidation_set(CSS::InvalidationSet const& set) const
{
    auto includes_property = [&](CSS::InvalidationSet::Property const& property) {
        switch (property.type) {
        case CSS::InvalidationSet::Property::Type::Class:
            return m_classes.contains_slow(property.name());
        case CSS::InvalidationSet::Property::Type::Id:
            return m_id == property.name();
        case CSS::InvalidationSet::Property::Type::TagName:
            return local_name() == property.name();
        case CSS::InvalidationSet::Property::Type::Attribute: {
            if (property.name() == HTML::AttributeNames::id || property.name() == HTML::AttributeNames::class_)
                return true;
            return has_attribute(property.name());
        }
        case CSS::InvalidationSet::Property::Type::PseudoClass: {
            switch (property.value.get<CSS::PseudoClass>()) {
            case CSS::PseudoClass::Has:
                return true;
            case CSS::PseudoClass::Enabled: {
                return matches_enabled_pseudo_class();
            }
            case CSS::PseudoClass::Disabled: {
                return matches_disabled_pseudo_class();
            }
            case CSS::PseudoClass::Defined: {
                return is_defined();
            }
            case CSS::PseudoClass::Checked: {
                return matches_checked_pseudo_class();
            }
            case CSS::PseudoClass::PlaceholderShown: {
                return matches_placeholder_shown_pseudo_class();
            }
            case CSS::PseudoClass::AnyLink:
            case CSS::PseudoClass::Link:
                return matches_link_pseudo_class();
            case CSS::PseudoClass::LocalLink: {
                return matches_local_link_pseudo_class();
            }
            case CSS::PseudoClass::Root:
                return is<HTML::HTMLHtmlElement>(*this);
            case CSS::PseudoClass::Host:
                return is_shadow_host();
            case CSS::PseudoClass::Required:
            case CSS::PseudoClass::Optional:
                return is<HTML::HTMLInputElement>(*this) || is<HTML::HTMLSelectElement>(*this) || is<HTML::HTMLTextAreaElement>(*this);
            default:
                VERIFY_NOT_REACHED();
            }
        }
        case CSS::InvalidationSet::Property::Type::InvalidateSelf:
            return false;
        case CSS::InvalidationSet::Property::Type::InvalidateWholeSubtree:
            return true;
        default:
            VERIFY_NOT_REACHED();
        }
    };

    bool includes_any = false;
    set.for_each_property([&](auto const& property) {
        if (includes_property(property)) {
            includes_any = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return includes_any;
}

void Element::invalidate_style_if_affected_by_has()
{
    if (affected_by_has_pseudo_class_in_subject_position()) {
        set_needs_style_update(true);
    }
    if (affected_by_has_pseudo_class_in_non_subject_position()) {
        invalidate_style(StyleInvalidationReason::Other, { { CSS::InvalidationSet::Property::Type::PseudoClass, CSS::PseudoClass::Has } }, {});
    }
}

bool Element::has_pseudo_elements() const
{
    if (m_pseudo_element_data) {
        for (auto& pseudo_element : *m_pseudo_element_data) {
            if (pseudo_element.value->layout_node())
                return true;
        }
    }
    return false;
}

void Element::clear_pseudo_element_nodes(Badge<Layout::TreeBuilder>)
{
    if (m_pseudo_element_data) {
        for (auto& pseudo_element : *m_pseudo_element_data) {
            pseudo_element.value->set_layout_node(nullptr);
        }
    }
}

void Element::serialize_children_as_json(JsonObjectSerializer<StringBuilder>& element_object) const
{
    bool has_pseudo_elements = this->has_pseudo_elements();
    if (!is_shadow_host() && !has_child_nodes() && !has_pseudo_elements)
        return;

    auto children = MUST(element_object.add_array("children"sv));

    auto serialize_pseudo_element = [&](CSS::PseudoElement pseudo_element_type, auto const& pseudo_element) {
        // FIXME: Find a way to make these still inspectable? (eg, `::before { display: none }`)
        if (!pseudo_element->layout_node())
            return;
        auto object = MUST(children.add_object());
        MUST(object.add("name"sv, MUST(String::formatted("::{}", CSS::pseudo_element_name(pseudo_element_type)))));
        MUST(object.add("type"sv, "pseudo-element"));
        MUST(object.add("parent-id"sv, unique_id().value()));
        MUST(object.add("pseudo-element"sv, to_underlying(pseudo_element_type)));
        MUST(object.finish());
    };

    if (has_pseudo_elements) {
        if (auto backdrop = m_pseudo_element_data->get(CSS::PseudoElement::Backdrop); backdrop.has_value()) {
            serialize_pseudo_element(CSS::PseudoElement::Backdrop, backdrop.value());
        }
        if (auto marker = m_pseudo_element_data->get(CSS::PseudoElement::Marker); marker.has_value()) {
            serialize_pseudo_element(CSS::PseudoElement::Marker, marker.value());
        }
        if (auto before = m_pseudo_element_data->get(CSS::PseudoElement::Before); before.has_value()) {
            serialize_pseudo_element(CSS::PseudoElement::Before, before.value());
        }
    }

    if (is_shadow_host())
        serialize_child_as_json(children, *shadow_root());

    auto add_child = [this, &children](Node const& child) {
        return serialize_child_as_json(children, child);
    };
    for_each_child(add_child);

    if (has_pseudo_elements) {
        if (auto after = m_pseudo_element_data->get(CSS::PseudoElement::After); after.has_value()) {
            serialize_pseudo_element(CSS::PseudoElement::After, after.value());
        }

        // Any other pseudo-elements, as a catch-all.
        for (auto const& [type, pseudo_element] : *m_pseudo_element_data) {
            if (first_is_one_of(type, CSS::PseudoElement::After, CSS::PseudoElement::Backdrop, CSS::PseudoElement::Before, CSS::PseudoElement::Marker))
                continue;
            serialize_pseudo_element(type, pseudo_element);
        }
    }

    MUST(children.finish());
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 Element::default_tab_index_value() const
{
    // The default value is 0 if the element is an a, area, button, frame, iframe, input, object, select, textarea, or SVG a element, or is a summary element that is a summary for its parent details.
    // The default value is −1 otherwise.
    // Note: The varying default value based on element type is a historical artifact.
    return -1;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 Element::tab_index() const
{
    auto maybe_table_index = Web::HTML::parse_integer(get_attribute_value(HTML::AttributeNames::tabindex));

    if (!maybe_table_index.has_value())
        return default_tab_index_value();
    return maybe_table_index.value();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
void Element::set_tab_index(i32 tab_index)
{
    set_attribute_value(HTML::AttributeNames::tabindex, String::number(tab_index));
}

// https://drafts.csswg.org/cssom-view/#potentially-scrollable
bool Element::is_potentially_scrollable(TreatOverflowClipOnBodyParentAsOverflowHidden treat_overflow_clip_on_body_parent_as_overflow_hidden = TreatOverflowClipOnBodyParentAsOverflowHidden::No) const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementIsPotentiallyScrollable);
    const_cast<Document&>(document()).update_style();

    // NB: Since this should always be the body element, the body element must have a <html> element parent. See Document::body().
    VERIFY(parent_element());

    // An element body (which will be the body element) is potentially scrollable if all of the following conditions are true:
    VERIFY(is<HTML::HTMLBodyElement>(this) || is<HTML::HTMLFrameSetElement>(this));

    // - body has an associated box.
    if (!layout_node())
        return false;

    // - body’s parent element’s computed value of the overflow-x or overflow-y properties is neither visible nor clip.
    if (parent_element()->computed_properties()->overflow_x() == CSS::Overflow::Visible || parent_element()->computed_properties()->overflow_y() == CSS::Overflow::Visible)
        return false;
    // NOTE: When treating 'overflow:clip' as 'overflow:hidden', we can never fail this condition
    if (treat_overflow_clip_on_body_parent_as_overflow_hidden == TreatOverflowClipOnBodyParentAsOverflowHidden::No && (parent_element()->computed_properties()->overflow_x() == CSS::Overflow::Clip || parent_element()->computed_properties()->overflow_y() == CSS::Overflow::Clip))
        return false;

    // - body’s computed value of the overflow-x or overflow-y properties is neither visible nor clip.
    if (first_is_one_of(computed_properties()->overflow_x(), CSS::Overflow::Visible, CSS::Overflow::Clip) || first_is_one_of(computed_properties()->overflow_y(), CSS::Overflow::Visible, CSS::Overflow::Clip))
        return false;

    return true;
}

bool Element::is_scroll_container() const
{
    // NB: We should only call this if we know that computed_properties has already been computed
    VERIFY(computed_properties());

    if (is_document_element())
        return true;

    return Layout::overflow_value_makes_box_a_scroll_container(computed_properties()->overflow_x())
        || Layout::overflow_value_makes_box_a_scroll_container(computed_properties()->overflow_y());
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrolltop
double Element::scroll_top() const
{
    // 1. Let document be the element’s node document.
    auto& document = this->document();

    // 2. If document is not the active document, return zero and terminate these steps.
    if (!document.is_active())
        return 0.0;

    // 3. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 4. If window is null, return zero and terminate these steps.
    if (!window)
        return 0.0;

    // 5. If the element is the root element and document is in quirks mode, return zero and terminate these steps.
    if (document.document_element() == this && document.in_quirks_mode())
        return 0.0;

    // 6. If the element is the root element return the value of scrollY on window.
    if (document.document_element() == this)
        return window->scroll_y();

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementScrollTop);

    // 7. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, return the value of scrollY on window.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return window->scroll_y();

    // 8. If the element does not have any associated box, return zero and terminate these steps.
    if (!paintable_box())
        return 0.0;

    // 9. Return the y-coordinate of the scrolling area at the alignment point with the top of the padding edge of the element.
    // FIXME: Is this correct?
    return paintable_box()->scroll_offset().y().to_double();
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollleft
double Element::scroll_left() const
{
    // 1. Let document be the element’s node document.
    auto& document = this->document();

    // 2. If document is not the active document, return zero and terminate these steps.
    if (!document.is_active())
        return 0.0;

    // 3. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 4. If window is null, return zero and terminate these steps.
    if (!window)
        return 0.0;

    // 5. If the element is the root element and document is in quirks mode, return zero and terminate these steps.
    if (document.document_element() == this && document.in_quirks_mode())
        return 0.0;

    // 6. If the element is the root element return the value of scrollX on window.
    if (document.document_element() == this)
        return window->scroll_x();

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementScrollLeft);

    // 7. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, return the value of scrollX on window.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return window->scroll_x();

    // 8. If the element does not have any associated box, return zero and terminate these steps.
    if (!paintable_box())
        return 0.0;

    // 9. Return the x-coordinate of the scrolling area at the alignment point with the left of the padding edge of the element.
    // FIXME: Is this correct?
    return paintable_box()->scroll_offset().x().to_double();
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollleft
void Element::set_scroll_left(double x)
{
    // 1. Let x be the given value.

    // 2. Normalize non-finite values for x.
    x = HTML::normalize_non_finite_values(x);

    // 3. Let document be the element’s node document.
    auto& document = this->document();

    // 4. If document is not the active document, terminate these steps.
    if (!document.is_active())
        return;

    // 5. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 6. If window is null, terminate these steps.
    if (!window)
        return;

    // 7. If the element is the root element and document is in quirks mode, terminate these steps.
    if (document.document_element() == this && document.in_quirks_mode())
        return;

    // 8. If the element is the root element invoke scroll() on window with x as first argument and scrollY on window as second argument, and terminate these steps.
    if (document.document_element() == this) {
        window->scroll(x, window->scroll_y());
        return;
    }

    // NOTE: Ensure that layout is up-to-date before looking at metrics or scrolling the page.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementSetScrollLeft);

    // 9. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, invoke scroll() on window with x as first argument and scrollY on window as second argument, and terminate these steps.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable()) {
        window->scroll(x, window->scroll_y());
        return;
    }

    // 10. If the element does not have any associated box, the element has no associated scrolling box, or the element has no overflow, terminate these steps.
    if (!paintable_box())
        return;

    if (!paintable_box()->layout_node_with_style_and_box_metrics().is_scroll_container())
        return;

    // FIXME: or the element has no overflow.

    // 11. Scroll the element to x,scrollTop, with the scroll behavior being "auto".
    // FIXME: Implement this in terms of calling "scroll the element".
    auto scroll_offset = paintable_box()->scroll_offset();
    scroll_offset.set_x(CSSPixels::nearest_value_for(x));
    paintable_box()->set_scroll_offset(scroll_offset);
}

void Element::set_scroll_top(double y)
{
    // 1. Let y be the given value.

    // 2. Normalize non-finite values for y.
    y = HTML::normalize_non_finite_values(y);

    // 3. Let document be the element’s node document.
    auto& document = this->document();

    // 4. If document is not the active document, terminate these steps.
    if (!document.is_active())
        return;

    // 5. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 6. If window is null, terminate these steps.
    if (!window)
        return;

    // 7. If the element is the root element and document is in quirks mode, terminate these steps.
    if (document.document_element() == this && document.in_quirks_mode())
        return;

    // 8. If the element is the root element invoke scroll() on window with scrollX on window as first argument and y as second argument, and terminate these steps.
    if (document.document_element() == this) {
        window->scroll(window->scroll_x(), y);
        return;
    }

    // NOTE: Ensure that layout is up-to-date before looking at metrics or scrolling the page.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementSetScrollTop);

    // 9. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, invoke scroll() on window with scrollX as first argument and y as second argument, and terminate these steps.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable()) {
        window->scroll(window->scroll_x(), y);
        return;
    }

    // 10. If the element does not have any associated box, the element has no associated scrolling box, or the element has no overflow, terminate these steps.
    if (!paintable_box())
        return;

    if (!paintable_box()->layout_node_with_style_and_box_metrics().is_scroll_container())
        return;

    // FIXME: or the element has no overflow.

    // 11. Scroll the element to scrollLeft,y, with the scroll behavior being "auto".
    // FIXME: Implement this in terms of calling "scroll the element".
    auto scroll_offset = paintable_box()->scroll_offset();
    scroll_offset.set_y(CSSPixels::nearest_value_for(y));
    paintable_box()->set_scroll_offset(scroll_offset);
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollwidth
int Element::scroll_width()
{
    // 1. Let document be the element’s node document.
    auto& document = this->document();

    // 2. If document is not the active document, return zero and terminate these steps.
    if (!document.is_active())
        return 0;

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    document.update_layout(UpdateLayoutReason::ElementScrollWidth);
    VERIFY(document.paintable_box() && document.paintable()->scrollable_overflow_rect().has_value());

    // 3. Let viewport width be the width of the viewport excluding the width of the scroll bar, if any,
    //    or zero if there is no viewport.
    auto viewport_width = document.viewport_rect().width().to_int();
    auto viewport_scrolling_area_width = document.paintable()->scrollable_overflow_rect()->width().to_int();

    // 4. If the element is the root element and document is not in quirks mode
    //    return max(viewport scrolling area width, viewport width).
    if (document.document_element() == this && !document.in_quirks_mode())
        return max(viewport_scrolling_area_width, viewport_width);

    // 5. If the element is the body element, document is in quirks mode and the element is not potentially scrollable,
    //    return max(viewport scrolling area width, viewport width).
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return max(viewport_scrolling_area_width, viewport_width);

    // 6. If the element does not have any associated box return zero and terminate these steps.
    if (!paintable_box())
        return 0;

    // 7. Return the width of the element’s scrolling area.
    if (auto scrollable_overflow_rect = paintable_box()->scrollable_overflow_rect(); scrollable_overflow_rect.has_value())
        return scrollable_overflow_rect->width().to_int();

    return 0;
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollheight
int Element::scroll_height()
{
    // 1. Let document be the element’s node document.
    auto& document = this->document();

    // 2. If document is not the active document, return zero and terminate these steps.
    if (!document.is_active())
        return 0;

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    document.update_layout(UpdateLayoutReason::ElementScrollHeight);
    VERIFY(document.paintable_box() && document.paintable()->scrollable_overflow_rect().has_value());

    // 3. Let viewport height be the height of the viewport excluding the height of the scroll bar, if any,
    //    or zero if there is no viewport.
    auto viewport_height = document.viewport_rect().height().to_int();
    auto viewport_scrolling_area_height = document.paintable()->scrollable_overflow_rect()->height().to_int();

    // 4. If the element is the root element and document is not in quirks mode
    //    return max(viewport scrolling area height, viewport height).
    if (document.document_element() == this && !document.in_quirks_mode())
        return max(viewport_scrolling_area_height, viewport_height);

    // 5. If the element is the body element, document is in quirks mode and the element is not potentially scrollable,
    //    return max(viewport scrolling area height, viewport height).
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return max(viewport_scrolling_area_height, viewport_height);

    // 6. If the element does not have any associated box return zero and terminate these steps.
    if (!paintable_box())
        return 0;

    // 7. Return the height of the element’s scrolling area.
    if (auto scrollable_overflow_rect = paintable_box()->scrollable_overflow_rect(); scrollable_overflow_rect.has_value()) {
        return scrollable_overflow_rect->height().to_int();
    }
    return 0;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#concept-element-disabled
bool Element::is_actually_disabled() const
{
    // An element is said to be actually disabled if it is one of the following:
    // - a button element that is disabled
    // - an input element that is disabled
    // - a select element that is disabled
    // - a textarea element that is disabled
    if (is<HTML::HTMLButtonElement>(this) || is<HTML::HTMLInputElement>(this) || is<HTML::HTMLSelectElement>(this) || is<HTML::HTMLTextAreaElement>(this)) {
        auto const& form_associated_element = as<HTML::FormAssociatedElement>(*this);
        return !form_associated_element.enabled();
    }

    // - an optgroup element that has a disabled attribute
    if (is<HTML::HTMLOptGroupElement>(this))
        return has_attribute(HTML::AttributeNames::disabled);

    // - an option element that is disabled
    if (is<HTML::HTMLOptionElement>(this))
        return static_cast<HTML::HTMLOptionElement const&>(*this).disabled();

    // - a fieldset element that is a disabled fieldset
    if (is<HTML::HTMLFieldSetElement>(this))
        return static_cast<HTML::HTMLFieldSetElement const&>(*this).is_disabled();

    // FIXME: - a form-associated custom element that is disabled
    return false;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#fragment-parsing-algorithm-steps
WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> Element::parse_fragment(StringView markup)
{
    // 1. Let algorithm be the HTML fragment parsing algorithm.
    auto algorithm = HTML::HTMLParser::parse_html_fragment;

    // 2. If context's node document is an XML document, then set algorithm to the XML fragment parsing algorithm.
    if (document().is_xml_document()) {
        algorithm = XMLFragmentParser::parse_xml_fragment;
    }

    // 3. Let newChildren be the result of invoking algorithm given context and markup.
    auto new_children = TRY(algorithm(*this, markup, HTML::HTMLParser::AllowDeclarativeShadowRoots::No));

    // 4. Let fragment be a new DocumentFragment whose node document is context's node document.
    auto fragment = realm().create<DOM::DocumentFragment>(document());

    // 5. For each node of newChildren, in tree order: append node to fragment.
    for (auto& child : new_children)
        TRY(fragment->append_child(*child));

    // 6. Return fragment.
    return fragment;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-outerhtml
WebIDL::ExceptionOr<TrustedTypes::TrustedHTMLOrString> Element::outer_html() const
{
    return TRY(serialize_fragment(HTML::RequireWellFormed::Yes, FragmentSerializationMode::Outer));
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-outerhtml
WebIDL::ExceptionOr<void> Element::set_outer_html(TrustedTypes::TrustedHTMLOrString const& value)
{
    // 1. Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, the given value, "Element outerHTML", and "script".
    auto const compliant_string = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::relevant_global_object(*this),
        value,
        TrustedTypes::InjectionSink::Element_outerHTML,
        TrustedTypes::Script.to_string()));

    // 2. Let parent be this's parent.
    auto* parent = this->parent();

    // 3. If parent is null, return. There would be no way to obtain a reference to the nodes created even if the remaining steps were run.
    if (!parent)
        return {};

    // 4. If parent is a Document, throw a "NoModificationAllowedError" DOMException.
    if (parent->is_document())
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot set outer HTML on document"_utf16);

    // 5. If parent is a DocumentFragment, set parent to the result of creating an element given this's node document, "body", and the HTML namespace.
    if (parent->is_document_fragment())
        parent = TRY(create_element(document(), HTML::TagNames::body, Namespace::HTML));

    // 6. Let fragment be the result of invoking the fragment parsing algorithm steps given parent and compliantString.
    auto fragment = TRY(as<Element>(*parent).parse_fragment(compliant_string.to_utf8_but_should_be_ported_to_utf16()));

    // 6. Replace this with fragment within this's parent.
    TRY(parent->replace_child(fragment, *this));

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#the-insertadjacenthtml()-method
WebIDL::ExceptionOr<void> Element::insert_adjacent_html(String const& position, TrustedTypes::TrustedHTMLOrString const& string)
{
    // 1. Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, string, "Element insertAdjacentHTML", and "script".
    auto const compliant_string = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::relevant_global_object(*this),
        string,
        TrustedTypes::InjectionSink::Element_insertAdjacentHTML,
        TrustedTypes::Script.to_string()));

    // 2. Let context be null.
    GC::Ptr<Node> context;

    // 3. Use the first matching item from this list:
    // - If position is an ASCII case-insensitive match for the string "beforebegin"
    // - If position is an ASCII case-insensitive match for the string "afterend"
    if (position.equals_ignoring_ascii_case("beforebegin"sv)
        || position.equals_ignoring_ascii_case("afterend"sv)) {
        // 1. Set context to this's parent.
        context = this->parent();

        // 2. If context is null or a Document, throw a "NoModificationAllowedError" DOMException.
        if (!context || context->is_document())
            return WebIDL::NoModificationAllowedError::create(realm(), "insertAdjacentHTML: context is null or a Document"_utf16);
    }
    // - If position is an ASCII case-insensitive match for the string "afterbegin"
    // - If position is an ASCII case-insensitive match for the string "beforeend"
    else if (position.equals_ignoring_ascii_case("afterbegin"sv)
        || position.equals_ignoring_ascii_case("beforeend"sv)) {
        // Set context to this.
        context = this;
    }
    // Otherwise
    else {
        // Throw a "SyntaxError" DOMException.
        return WebIDL::SyntaxError::create(realm(), "insertAdjacentHTML: invalid position argument"_utf16);
    }

    // 4. If context is not an Element or the following are all true:
    //    - context's node document is an HTML document,
    //    - context's local name is "html", and
    //    - context's namespace is the HTML namespace;
    if (!is<Element>(*context)
        || (context->document().document_type() == Document::Type::HTML
            && static_cast<Element const&>(*context).local_name() == "html"sv
            && static_cast<Element const&>(*context).namespace_uri() == Namespace::HTML)) {
        // then set context to the result of creating an element given this's node document, "body", and the HTML namespace.
        context = TRY(create_element(document(), HTML::TagNames::body, Namespace::HTML));
    }

    // 5. Let fragment be the result of invoking the fragment parsing algorithm steps with context and compliantString.
    auto fragment = TRY(as<Element>(*context).parse_fragment(compliant_string.to_utf8_but_should_be_ported_to_utf16()));

    // 6. Use the first matching item from this list:

    // - If position is an ASCII case-insensitive match for the string "beforebegin"
    if (position.equals_ignoring_ascii_case("beforebegin"sv)) {
        // Insert fragment into this's parent before this.
        parent()->insert_before(fragment, this);
    }

    // - If position is an ASCII case-insensitive match for the string "afterbegin"
    else if (position.equals_ignoring_ascii_case("afterbegin"sv)) {
        // Insert fragment into this before its first child.
        insert_before(fragment, first_child());
    }

    // - If position is an ASCII case-insensitive match for the string "beforeend"
    else if (position.equals_ignoring_ascii_case("beforeend"sv)) {
        // Append fragment to this.
        TRY(append_child(fragment));
    }

    // - If position is an ASCII case-insensitive match for the string "afterend"
    else if (position.equals_ignoring_ascii_case("afterend"sv)) {
        // Insert fragment into this's parent before this's next sibling.
        parent()->insert_before(fragment, next_sibling());
    }
    return {};
}

// https://dom.spec.whatwg.org/#insert-adjacent
WebIDL::ExceptionOr<GC::Ptr<Node>> Element::insert_adjacent(StringView where, GC::Ref<Node> node)
{
    // To insert adjacent, given an element element, string where, and a node node, run the steps associated with the first ASCII case-insensitive match for where:
    if (where.equals_ignoring_ascii_case("beforebegin"sv)) {
        // -> "beforebegin"
        // If element’s parent is null, return null.
        if (!parent())
            return GC::Ptr<Node> { nullptr };

        // Return the result of pre-inserting node into element’s parent before element.
        return GC::Ptr<Node> { TRY(parent()->pre_insert(move(node), this)) };
    }

    if (where.equals_ignoring_ascii_case("afterbegin"sv)) {
        // -> "afterbegin"
        // Return the result of pre-inserting node into element before element’s first child.
        return GC::Ptr<Node> { TRY(pre_insert(move(node), first_child())) };
    }

    if (where.equals_ignoring_ascii_case("beforeend"sv)) {
        // -> "beforeend"
        // Return the result of pre-inserting node into element before null.
        return GC::Ptr<Node> { TRY(pre_insert(move(node), nullptr)) };
    }

    if (where.equals_ignoring_ascii_case("afterend"sv)) {
        // -> "afterend"
        // If element’s parent is null, return null.
        if (!parent())
            return GC::Ptr<Node> { nullptr };

        // Return the result of pre-inserting node into element’s parent before element’s next sibling.
        return GC::Ptr<Node> { TRY(parent()->pre_insert(move(node), next_sibling())) };
    }

    // -> Otherwise
    // Throw a "SyntaxError" DOMException.
    return WebIDL::SyntaxError::create(realm(), Utf16String::formatted("Unknown position '{}'. Must be one of 'beforebegin', 'afterbegin', 'beforeend' or 'afterend'", where));
}

// https://dom.spec.whatwg.org/#dom-element-insertadjacentelement
WebIDL::ExceptionOr<GC::Ptr<Element>> Element::insert_adjacent_element(String const& where, GC::Ref<Element> element)
{
    // The insertAdjacentElement(where, element) method steps are to return the result of running insert adjacent, give this, where, and element.
    auto returned_node = TRY(insert_adjacent(where, element));
    if (!returned_node)
        return GC::Ptr<Element> { nullptr };
    return GC::Ptr<Element> { as<Element>(*returned_node) };
}

// https://dom.spec.whatwg.org/#dom-element-insertadjacenttext
WebIDL::ExceptionOr<void> Element::insert_adjacent_text(String const& where, Utf16String const& data)
{
    // 1. Let text be a new Text node whose data is data and node document is this’s node document.
    auto text = realm().create<DOM::Text>(document(), data);

    // 2. Run insert adjacent, given this, where, and text.
    // Spec Note: This method returns nothing because it existed before we had a chance to design it.
    (void)TRY(insert_adjacent(where, text));
    return {};
}

// https://drafts.csswg.org/cssom-view-1/#determine-the-scroll-into-view-position
static CSSPixelPoint determine_the_scroll_into_view_position(Element& target, Bindings::ScrollLogicalPosition block, Bindings::ScrollLogicalPosition inline_, Node& scrolling_box)
{
    // To determine the scroll-into-view position of a target, which is an Element, pseudo-element, or Range, with a
    // block flow direction position block, an inline base direction position inline, and a scrolling box scrolling box,
    // run the following steps:

    if (!scrolling_box.is_document()) {
        // FIXME: Add support for scrolling boxes other than the viewport.
        return {};
    }
    // NOTE: For a viewport scrolling box is initial containing block
    CSSPixelRect scrolling_box_rect = scrolling_box.document().viewport_rect();

    // FIXME: All of this needs to support different block/inline directions.

    // 1. Let target bounding border box be the box represented by the return value of invoking Element’s
    //    getBoundingClientRect(), if target is an Element, or Range’s getBoundingClientRect(),
    //    if target is a Range.
    auto target_bounding_border_box = target.get_bounding_client_rect();

    // 2. Let scrolling box edge A be the beginning edge in the block flow direction of scrolling box, and
    //    let element edge A be target bounding border box’s edge on the same physical side as that of
    //    scrolling box edge A.
    CSSPixels element_edge_a = target_bounding_border_box.top();
    CSSPixels scrolling_box_edge_a = scrolling_box_rect.top();

    // 3. Let scrolling box edge B be the ending edge in the block flow direction of scrolling box, and let
    //    element edge B be target bounding border box’s edge on the same physical side as that of scrolling
    //    box edge B.
    CSSPixels element_edge_b = target_bounding_border_box.bottom();
    CSSPixels scrolling_box_edge_b = scrolling_box_rect.bottom();

    // 4. Let scrolling box edge C be the beginning edge in the inline base direction of scrolling box, and
    //    let element edge C be target bounding border box’s edge on the same physical side as that of scrolling
    //    box edge C.
    CSSPixels element_edge_c = target_bounding_border_box.left();
    CSSPixels scrolling_box_edge_c = scrolling_box_rect.left();

    // 5. Let scrolling box edge D be the ending edge in the inline base direction of scrolling box, and let element
    //    edge D be target bounding border box’s edge on the same physical side as that of scrolling box edge D.
    CSSPixels element_edge_d = target_bounding_border_box.right();
    CSSPixels scrolling_box_edge_d = scrolling_box_rect.right();

    // 6. Let element height be the distance between element edge A and element edge B.
    CSSPixels element_height = element_edge_b - element_edge_a;

    // 7. Let scrolling box height be the distance between scrolling box edge A and scrolling box edge B.
    CSSPixels scrolling_box_height = scrolling_box_edge_b - scrolling_box_edge_a;

    // 8. Let element width be the distance between element edge C and element edge D.
    CSSPixels element_width = element_edge_d - element_edge_c;

    // 9. Let scrolling box width be the distance between scrolling box edge C and scrolling box edge D.
    CSSPixels scrolling_box_width = scrolling_box_edge_d - scrolling_box_edge_c;

    // 10. Let position be the scroll position scrolling box would have by following these steps:
    auto position = [&]() -> CSSPixelPoint {
        CSSPixels x = 0;
        CSSPixels y = 0;

        // 1. If block is "start", then align element edge A with scrolling box edge A.
        if (block == Bindings::ScrollLogicalPosition::Start) {
            y = element_edge_a;
        }
        // 2. Otherwise, if block is "end", then align element edge B with scrolling box edge B.
        else if (block == Bindings::ScrollLogicalPosition::End) {
            y = element_edge_a + element_height - scrolling_box_height;
        }
        // 3. Otherwise, if block is "center", then align the center of target bounding border box with the center of
        //    scrolling box in scrolling box’s block flow direction.
        else if (block == Bindings::ScrollLogicalPosition::Center) {
            y = element_edge_a + (element_height / 2) - (scrolling_box_height / 2);
        }
        // 4. Otherwise, block is "nearest":
        else {
            // If element edge A and element edge B are both outside scrolling box edge A and scrolling box edge B
            if (element_edge_a <= 0 && element_edge_b >= scrolling_box_height) {
                // Do nothing.
            }
            // If element edge A is outside scrolling box edge A and element height is less than scrolling box height
            // If element edge B is outside scrolling box edge B and element height is greater than scrolling box height
            else if ((element_edge_a <= 0 && element_height < scrolling_box_height) || (element_edge_b >= scrolling_box_height && element_height > scrolling_box_height)) {
                // Align element edge A with scrolling box edge A.
                y = element_edge_a;
            }
            // If element edge A is outside scrolling box edge A and element height is greater than scrolling box height
            // If element edge B is outside scrolling box edge B and element height is less than scrolling box height
            else if ((element_edge_b >= scrolling_box_height && element_height < scrolling_box_height) || (element_edge_a <= 0 && element_height > scrolling_box_height)) {
                // Align element edge B with scrolling box edge B.
                y = element_edge_a + element_height - scrolling_box_height;
            }
        }

        // 5. If inline is "start", then align element edge C with scrolling box edge C.
        if (inline_ == Bindings::ScrollLogicalPosition::Start) {
            x = element_edge_c;
        }
        // 6. Otherwise, if inline is "end", then align element edge D with scrolling box edge D.
        else if (inline_ == Bindings::ScrollLogicalPosition::End) {
            x = element_edge_d + element_width - scrolling_box_width;
        }
        // 7. Otherwise, if inline is "center", then align the center of target bounding border box with the center of
        //    scrolling box in scrolling box’s inline base direction.
        else if (inline_ == Bindings::ScrollLogicalPosition::Center) {
            x = element_edge_c + (element_width / 2) - (scrolling_box_width / 2);
        }
        // 8. Otherwise, inline is "nearest":
        else {
            // If element edge C and element edge D are both outside scrolling box edge C and scrolling box edge D
            if (element_edge_c <= 0 && element_edge_d >= scrolling_box_width) {
                // Do nothing.
            }
            // If element edge C is outside scrolling box edge C and element width is less than scrolling box width
            // If element edge D is outside scrolling box edge D and element width is greater than scrolling box width
            else if ((element_edge_c <= 0 && element_width < scrolling_box_width) || (element_edge_d >= scrolling_box_width && element_width > scrolling_box_width)) {
                // Align element edge C with scrolling box edge C.
                x = element_edge_c;
            }
            // If element edge C is outside scrolling box edge C and element width is greater than scrolling box width
            // If element edge D is outside scrolling box edge D and element width is less than scrolling box width
            else if ((element_edge_d >= scrolling_box_width && element_width < scrolling_box_width) || (element_edge_c <= 0 && element_width > scrolling_box_width)) {
                // Align element edge D with scrolling box edge D.
                x = element_edge_d + element_width - scrolling_box_width;
            }
        }

        return CSSPixelPoint { x, y };
    }();

    // 11. Return position.
    return position;
}

// https://drafts.csswg.org/cssom-view-1/#scroll-a-target-into-view
static GC::Ref<WebIDL::Promise> scroll_an_element_into_view(Element& target, Bindings::ScrollBehavior behavior, Bindings::ScrollLogicalPosition block, Bindings::ScrollLogicalPosition inline_, GC::Ptr<Element> container)
{
    // FIXME: 1. Let ancestorPromises be an empty set of Promises.

    // 2. For each ancestor element or viewport that establishes a scrolling box scrolling box, in order of innermost
    //    to outermost scrolling box, run these substeps:
    auto* ancestor = target.parent();
    Vector<Node&> scrolling_boxes;
    while (ancestor) {
        if (ancestor->paintable_box() && ancestor->paintable_box()->has_scrollable_overflow())
            scrolling_boxes.append(*ancestor);
        ancestor = ancestor->parent();
    }

    for (auto& scrolling_box : scrolling_boxes) {
        // 1. If the Document associated with target is not same origin with the Document associated with the element
        //    or viewport associated with scrolling box, abort any remaining iteration of this loop.
        if (target.document().origin() != scrolling_box.document().origin())
            break;

        // 2. Let position be the scroll position resulting from running the steps to determine the scroll-into-view
        //    position of target with behavior as the scroll behavior, block as the block flow position, inline as the
        //    inline base direction position and scrolling box as the scrolling box.
        // FIXME: Pass in behavior.
        auto position = determine_the_scroll_into_view_position(target, block, inline_, scrolling_box);

        // 3. If position is not the same as scrolling box’s current scroll position, or scrolling box has an ongoing
        //    smooth scroll,
        // FIXME: Actually check this condition.
        if (true) {
            // -> If scrolling box is associated with an element
            if (scrolling_box.is_element()) {
                // FIXME: Perform a scroll of the element’s scrolling box to position, with the element as the associated element and behavior as the scroll behavior.
            }
            // -> If scrolling box is associated with a viewport
            else if (scrolling_box.is_document()) {
                // 1. Let document be the viewport’s associated Document.
                auto& document = static_cast<Document&>(scrolling_box);

                // FIXME: 2. Let root element be document’s root element, if there is one, or null otherwise.
                // FIXME: 3. Perform a scroll of the viewport to position, with root element as the associated element and behavior as the scroll behavior.
                //           Add the Promise returned from this step in the set ancestorPromises.
                (void)behavior;

                // AD-HOC:
                // NOTE: Since calculated position is relative to the viewport, we need to add the viewport's position to it
                //       before passing to perform_a_scroll_of_the_viewport() that expects a position relative to the page.
                position.set_y(position.y() + document.viewport_rect().y());
                document.navigable()->perform_a_scroll_of_the_viewport(position);
            }
        }

        // 4. If container is not null and either scrolling box is a shadow-including inclusive ancestor of container
        //    or is a viewport whose document is a shadow-including inclusive ancestor of container, abort any
        //    remaining iteration of this loop.
        // NB: Our viewports *are* Documents in the DOM, so both checks are equivalent.
        if (container != nullptr && scrolling_box.is_shadow_including_inclusive_ancestor_of(*container))
            break;
    }

    // 3. Let scrollPromise be a new Promise.
    auto scroll_promise = WebIDL::create_promise(target.realm());

    // 4. Return scrollPromise, and run the remaining steps in parallel.
    // 5. Resolve scrollPromise when all Promises in ancestorPromises have settled.
    // FIXME: Actually wait for those promises.
    WebIDL::resolve_promise(target.realm(), scroll_promise);

    return scroll_promise;
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollintoview
GC::Ref<WebIDL::Promise> Element::scroll_into_view(Optional<Variant<bool, ScrollIntoViewOptions>> arg)
{
    // 1. Let behavior be "auto".
    auto behavior = Bindings::ScrollBehavior::Auto;

    // 2. Let block be "start".
    auto block = Bindings::ScrollLogicalPosition::Start;

    // 3. Let inline be "nearest".
    auto inline_ = Bindings::ScrollLogicalPosition::Nearest;

    // 4. Let container be null.
    GC::Ptr<Element> container = nullptr;

    // 5. If arg is a ScrollIntoViewOptions dictionary, then:
    if (arg.has_value() && arg->has<ScrollIntoViewOptions>()) {
        auto options = arg->get<ScrollIntoViewOptions>();

        // 1. Set behavior to the behavior dictionary member of options.
        behavior = options.behavior;

        // 2. Set block to the block dictionary member of options.
        block = options.block;

        // 3. Set inline to the inline dictionary member of options.
        inline_ = options.inline_;

        // 4. If the container dictionary member of options is "nearest", set container to the element.
        if (options.container == Bindings::ScrollIntoViewContainer::Nearest)
            container = this;
    }
    // 6. Otherwise, if arg is false, then set block to "end".
    else if (arg.has_value() && arg->has<bool>() && arg->get<bool>() == false) {
        block = Bindings::ScrollLogicalPosition::End;
    }

    // 7. If the element does not have any associated box, or is not available to user-agent features, then return a
    //    resolved Promise and abort the remaining steps.
    document().update_layout(UpdateLayoutReason::ElementScrollIntoView);
    HTML::TemporaryExecutionContext temporary_execution_context { realm() };
    if (!layout_node())
        return WebIDL::create_resolved_promise(realm(), JS::js_undefined());

    // 8. Scroll the element into view with behavior, block, inline, and container. Let scrollPromise be the Promise
    //    returned from this step.
    auto scroll_promise = scroll_an_element_into_view(*this, behavior, block, inline_, container);

    // FIXME: 9. Optionally perform some other action that brings the element to the user’s attention.

    // 10. Return scrollPromise.
    return scroll_promise;
}

#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute)                  \
    Optional<String> Element::name() const                           \
    {                                                                \
        return get_attribute(ARIA::AttributeNames::name);            \
    }                                                                \
                                                                     \
    void Element::set_##name(Optional<String> const& value)          \
    {                                                                \
        if (value.has_value())                                       \
            set_attribute_value(ARIA::AttributeNames::name, *value); \
        else                                                         \
            remove_attribute(ARIA::AttributeNames::name);            \
    }
ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

void Element::invalidate_style_after_attribute_change(FlyString const& attribute_name, Optional<String> const& old_value, Optional<String> const& new_value)
{
    Vector<CSS::InvalidationSet::Property, 1> changed_properties;
    StyleInvalidationOptions style_invalidation_options;
    if (is_presentational_hint(attribute_name) || style_uses_attr_css_function()) {
        style_invalidation_options.invalidate_self = true;
    }

    if (attribute_name == HTML::AttributeNames::style) {
        style_invalidation_options.invalidate_self = true;
    } else if (attribute_name == HTML::AttributeNames::class_) {
        Vector<StringView> old_classes;
        Vector<StringView> new_classes;
        if (old_value.has_value())
            old_classes = old_value->bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
        if (new_value.has_value())
            new_classes = new_value->bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
        for (auto& old_class : old_classes) {
            if (!new_classes.contains_slow(old_class)) {
                changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::Class, .value = FlyString::from_utf8_without_validation(old_class.bytes()) });
            }
        }
        for (auto& new_class : new_classes) {
            if (!old_classes.contains_slow(new_class)) {
                changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::Class, .value = FlyString::from_utf8_without_validation(new_class.bytes()) });
            }
        }
    } else if (attribute_name == HTML::AttributeNames::id) {
        if (old_value.has_value())
            changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::Id, .value = FlyString(old_value.value()) });
        if (new_value.has_value())
            changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::Id, .value = FlyString(new_value.value()) });
    } else if (attribute_name == HTML::AttributeNames::disabled) {
        changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Disabled });
        changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Enabled });
    } else if (attribute_name == HTML::AttributeNames::placeholder) {
        changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::PlaceholderShown });
    } else if (attribute_name == HTML::AttributeNames::value) {
        changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Checked });
    } else if (attribute_name == HTML::AttributeNames::required) {
        changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Required });
        changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Optional });
    }

    changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::Attribute, .value = attribute_name });
    invalidate_style(StyleInvalidationReason::ElementAttributeChange, changed_properties, style_invalidation_options);
}

bool Element::is_hidden() const
{
    if (layout_node() == nullptr)
        return true;
    if (layout_node()->computed_values().visibility() == CSS::Visibility::Hidden || layout_node()->computed_values().visibility() == CSS::Visibility::Collapse || layout_node()->computed_values().content_visibility() == CSS::ContentVisibility::Hidden)
        return true;
    for (ParentNode const* self_or_ancestor = this; self_or_ancestor; self_or_ancestor = self_or_ancestor->parent_or_shadow_host()) {
        if (self_or_ancestor->is_element() && static_cast<DOM::Element const*>(self_or_ancestor)->aria_hidden() == "true")
            return true;
    }
    return false;
}

bool Element::has_hidden_ancestor() const
{
    for (ParentNode const* self_or_ancestor = this; self_or_ancestor; self_or_ancestor = self_or_ancestor->parent_or_shadow_host()) {
        if (self_or_ancestor->is_element() && static_cast<DOM::Element const*>(self_or_ancestor)->is_hidden())
            return true;
    }
    return false;
}

bool Element::is_referenced() const
{
    bool is_referenced = false;
    if (id().has_value()) {
        root().for_each_in_subtree_of_type<HTML::HTMLElement>([&](auto& element) {
            auto aria_data = MUST(Web::ARIA::AriaData::build_data(element));
            if (aria_data->aria_labelled_by_or_default().contains_slow(id().value())) {
                is_referenced = true;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
    }
    return is_referenced;
}

bool Element::has_referenced_and_hidden_ancestor() const
{
    for (auto const* ancestor = parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        if (ancestor->is_element())
            if (auto const* element = static_cast<DOM::Element const*>(ancestor); element->is_referenced() && element->is_hidden())
                return true;
    }
    return false;
}

// https://www.w3.org/TR/wai-aria-1.2/#tree_exclusion
bool Element::exclude_from_accessibility_tree() const
{
    // The following elements are not exposed via the accessibility API and user agents MUST NOT include them in the accessibility tree:

    // Elements, including their descendent elements, that have host language semantics specifying that the element is not displayed, such as CSS display:none, visibility:hidden, or the HTML hidden attribute.
    if (!layout_node())
        return true;

    // Elements with none or presentation as the first role in the role attribute. However, their exclusion is conditional. In addition, the element's descendants and text content are generally included. These exceptions and conditions are documented in the presentation (role) section.
    // FIXME: Handle exceptions to excluding presentation role
    auto role = role_or_default();
    if (role == ARIA::Role::none || role == ARIA::Role::presentation)
        return true;

    // TODO: If not already excluded from the accessibility tree per the above rules, user agents SHOULD NOT include the following elements in the accessibility tree:
    //    Elements, including their descendants, that have aria-hidden set to true. In other words, aria-hidden="true" on a parent overrides aria-hidden="false" on descendants.
    //    Any descendants of elements that have the characteristic "Children Presentational: True" unless the descendant is not allowed to be presentational because it meets one of the conditions for exception described in Presentational Roles Conflict Resolution. However, the text content of any excluded descendants is included.
    //    Elements with the following roles have the characteristic "Children Presentational: True":
    //      button
    //      checkbox
    //      img
    //      menuitemcheckbox
    //      menuitemradio
    //      meter
    //      option
    //      progressbar
    //      radio
    //      scrollbar
    //      separator
    //      slider
    //      switch
    //      tab
    return false;
}

// https://www.w3.org/TR/wai-aria-1.2/#tree_inclusion
bool Element::include_in_accessibility_tree() const
{
    // If not excluded from or marked as hidden in the accessibility tree per the rules above in Excluding Elements in the Accessibility Tree, user agents MUST provide an accessible object in the accessibility tree for DOM elements that meet any of the following criteria:
    if (exclude_from_accessibility_tree())
        return false;
    // Elements that are not hidden and may fire an accessibility API event, including:
    // Elements that are currently focused, even if the element or one of its ancestor elements has its aria-hidden attribute set to true.
    if (is_focused())
        return true;
    // TODO: Elements that are a valid target of an aria-activedescendant attribute.

    // Elements that have an explicit role or a global WAI-ARIA attribute and do not have aria-hidden set to true. (See Excluding Elements in the Accessibility Tree for additional guidance on aria-hidden.)
    // NOTE: The spec says only explicit roles count, but playing around in other browsers, this does not seem to be true in practice (for example button elements are always exposed with their implicit role if none is set)
    //       This issue https://github.com/w3c/aria/issues/1851 seeks clarification on this point
    if ((role_or_default().has_value() || has_global_aria_attribute()) && aria_hidden() != "true")
        return true;

    // TODO: Elements that are not hidden and have an ID that is referenced by another element via a WAI-ARIA property.

    return false;
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#enqueue-an-element-on-the-appropriate-element-queue
void Element::enqueue_an_element_on_the_appropriate_element_queue()
{
    // 1. Let reactionsStack be element's relevant agent's custom element reactions stack.
    auto& relevant_agent = HTML::relevant_similar_origin_window_agent(*this);
    auto& reactions_stack = relevant_agent.custom_element_reactions_stack;

    // 2. If reactionsStack is empty, then:
    if (reactions_stack.element_queue_stack.is_empty()) {
        // 1. Add element to reactionsStack's backup element queue.
        reactions_stack.backup_element_queue.append(*this);

        // 2. If reactionsStack's processing the backup element queue flag is set, then return.
        if (reactions_stack.processing_the_backup_element_queue)
            return;

        // 3. Set reactionsStack's processing the backup element queue flag.
        reactions_stack.processing_the_backup_element_queue = true;

        // 4. Queue a microtask to perform the following steps:
        // NOTE: `this` is protected by GC::Function
        HTML::queue_a_microtask(&document(), GC::create_function(heap(), [this]() {
            auto& reactions_stack = HTML::relevant_similar_origin_window_agent(*this).custom_element_reactions_stack;

            // 1. Invoke custom element reactions in reactionsStack's backup element queue.
            Bindings::invoke_custom_element_reactions(reactions_stack.backup_element_queue);

            // 2. Unset reactionsStack's processing the backup element queue flag.
            reactions_stack.processing_the_backup_element_queue = false;
        }));

        return;
    }

    // 3. Otherwise, add element to element's relevant agent's current element queue.
    relevant_agent.current_element_queue().append(*this);
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#enqueue-a-custom-element-upgrade-reaction
void Element::enqueue_a_custom_element_upgrade_reaction(HTML::CustomElementDefinition& custom_element_definition)
{
    // 1. Add a new upgrade reaction to element's custom element reaction queue, with custom element definition definition.
    ensure_custom_element_reaction_queue().append(CustomElementUpgradeReaction { .custom_element_definition = custom_element_definition });

    // 2. Enqueue an element on the appropriate element queue given element.
    enqueue_an_element_on_the_appropriate_element_queue();
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#enqueue-a-custom-element-callback-reaction
void Element::enqueue_a_custom_element_callback_reaction(FlyString const& callback_name, GC::RootVector<JS::Value> arguments)
{
    // 1. Let definition be element's custom element definition.
    auto& definition = m_custom_element_definition;

    // 2. Let callback be the value of the entry in definition's lifecycle callbacks with key callbackName.
    GC::Ptr<Web::WebIDL::CallbackType> callback;
    if (auto callback_iterator = definition->lifecycle_callbacks().find(callback_name); callback_iterator != definition->lifecycle_callbacks().end())
        callback = callback_iterator->value;

    // 3. If callbackName is "connectedMoveCallback" and callback is null:
    if (callback_name == HTML::CustomElementReactionNames::connectedMoveCallback && !callback) {
        // 1. Let disconnectedCallback be the value of the entry in definition's lifecycle callbacks with key "disconnectedCallback".
        GC::Ptr<WebIDL::CallbackType> disconnected_callback;
        if (auto it = definition->lifecycle_callbacks().find(HTML::CustomElementReactionNames::disconnectedCallback); it != definition->lifecycle_callbacks().end())
            disconnected_callback = it->value;

        // 2. Let connectedCallback be the value of the entry in definition's lifecycle callbacks with key "connectedCallback".
        GC::Ptr<WebIDL::CallbackType> connected_callback;
        if (auto it = definition->lifecycle_callbacks().find(HTML::CustomElementReactionNames::connectedCallback); it != definition->lifecycle_callbacks().end())
            connected_callback = it->value;

        // 3. If connectedCallback and disconnectedCallback are null, then return.
        if (!connected_callback && !disconnected_callback)
            return;

        // 4. Set callback to the following steps:
        auto steps = JS::NativeFunction::create(realm(), [this, disconnected_callback, connected_callback](JS::VM&) {
            GC::RootVector<JS::Value> no_arguments { heap() };

            // 1. If disconnectedCallback is not null, then call disconnectedCallback with no arguments.
            if (disconnected_callback)
                (void)WebIDL::invoke_callback(*disconnected_callback, this, WebIDL::ExceptionBehavior::Report, no_arguments);

            // 2. If connectedCallback is not null, then call connectedCallback with no arguments.
            if (connected_callback)
                (void)WebIDL::invoke_callback(*connected_callback, this, WebIDL::ExceptionBehavior::Report, no_arguments);

            return JS::js_undefined(); }, 0, Utf16FlyString {}, &realm());
        callback = realm().heap().allocate<WebIDL::CallbackType>(steps, realm());
    }

    // 3. If callback is null, then return.
    if (!callback)
        return;

    // 5. If callbackName is "attributeChangedCallback":
    if (callback_name == HTML::CustomElementReactionNames::attributeChangedCallback) {
        // 1. Let attributeName be the first element of args.
        VERIFY(!arguments.is_empty());
        auto& attribute_name_value = arguments.first();
        VERIFY(attribute_name_value.is_string());
        auto attribute_name = attribute_name_value.as_string().utf8_string();

        // 2. If definition's observed attributes does not contain attributeName, then return.
        if (!definition->observed_attributes().contains_slow(attribute_name))
            return;
    }

    // 6. Add a new callback reaction to element's custom element reaction queue, with callback function callback and arguments args.
    ensure_custom_element_reaction_queue().append(CustomElementCallbackReaction { .callback = callback, .arguments = move(arguments) });

    // 7. Enqueue an element on the appropriate element queue given element.
    enqueue_an_element_on_the_appropriate_element_queue();
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#concept-upgrade-an-element
JS::ThrowCompletionOr<void> Element::upgrade_element(GC::Ref<HTML::CustomElementDefinition> custom_element_definition)
{
    auto& realm = this->realm();
    auto& vm = this->vm();

    // 1. If element's custom element state is not "undefined" or "uncustomized", then return.
    if (m_custom_element_state != CustomElementState::Undefined && m_custom_element_state != CustomElementState::Uncustomized)
        return {};

    // 2. Set element's custom element definition to definition.
    m_custom_element_definition = custom_element_definition;

    // 3. Set element's custom element state to "failed".
    set_custom_element_state(CustomElementState::Failed);

    // 4. For each attribute in element's attribute list, in order, enqueue a custom element callback reaction with element, callback name "attributeChangedCallback",
    //    and « attribute's local name, null, attribute's value, attribute's namespace ».
    size_t attribute_count = m_attributes ? m_attributes->length() : 0;
    for (size_t attribute_index = 0; attribute_index < attribute_count; ++attribute_index) {
        auto const* attribute = m_attributes->item(attribute_index);
        VERIFY(attribute);

        GC::RootVector<JS::Value> arguments { vm.heap() };

        arguments.append(JS::PrimitiveString::create(vm, attribute->local_name()));
        arguments.append(JS::js_null());
        arguments.append(JS::PrimitiveString::create(vm, attribute->value()));
        arguments.append(attribute->namespace_uri().has_value() ? JS::PrimitiveString::create(vm, attribute->namespace_uri().value()) : JS::js_null());

        enqueue_a_custom_element_callback_reaction(HTML::CustomElementReactionNames::attributeChangedCallback, move(arguments));
    }

    // 5. If element is connected, then enqueue a custom element callback reaction with element, callback name "connectedCallback", and « ».
    if (is_connected()) {
        GC::RootVector<JS::Value> empty_arguments { vm.heap() };
        enqueue_a_custom_element_callback_reaction(HTML::CustomElementReactionNames::connectedCallback, move(empty_arguments));
    }

    // 6. Add element to the end of definition's construction stack.
    custom_element_definition->construction_stack().append(GC::Ref { *this });

    // 7. Let C be definition's constructor.
    auto& constructor = custom_element_definition->constructor();

    // 8. Run the following substeps while catching any exceptions:
    auto attempt_to_construct_custom_element = [&]() -> JS::ThrowCompletionOr<void> {
        // 1. If definition's disable shadow is true and element's shadow root is non-null, then throw a "NotSupportedError" DOMException.
        if (custom_element_definition->disable_shadow() && shadow_root())
            return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "Custom element definition disables shadow DOM and the custom element has a shadow root"_utf16));

        // 2. Set element's custom element state to "precustomized".
        set_custom_element_state(CustomElementState::Precustomized);

        // 3. Let constructResult be the result of constructing C, with no arguments.
        auto construct_result = TRY(WebIDL::construct(constructor, {}));

        // 4. If SameValue(constructResult, element) is false, then throw a TypeError.
        if (!JS::same_value(construct_result, this))
            return vm.throw_completion<JS::TypeError>("Constructing the custom element returned a different element from the custom element"sv);

        return {};
    };

    auto maybe_exception = attempt_to_construct_custom_element();

    // Then, perform the following substep, regardless of whether the above steps threw an exception or not:
    // 1. Remove the last entry from the end of definition's construction stack.
    (void)custom_element_definition->construction_stack().take_last();

    // Finally, if the above steps threw an exception, then:
    if (maybe_exception.is_throw_completion()) {
        // 1. Set element's custom element definition to null.
        m_custom_element_definition = nullptr;

        // 2. Empty element's custom element reaction queue.
        if (m_custom_element_reaction_queue)
            m_custom_element_reaction_queue->clear();

        // 3. Rethrow the exception (thus terminating this algorithm).
        return maybe_exception.release_error();
    }

    // FIXME: 9. If element is a form-associated custom element, then:
    //           1. Reset the form owner of element. If element is associated with a form element, then enqueue a custom element callback reaction with element, callback name "formAssociatedCallback", and « the associated form ».
    //           2. If element is disabled, then enqueue a custom element callback reaction with element, callback name "formDisabledCallback", and « true ».

    // 10. Set element's custom element state to "custom".
    set_custom_element_state(CustomElementState::Custom);

    return {};
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#concept-try-upgrade
void Element::try_to_upgrade()
{
    // 1. Let definition be the result of looking up a custom element definition given element's node document, element's namespace, element's local name, and element's is value.
    auto definition = document().lookup_custom_element_definition(namespace_uri(), local_name(), m_is_value);

    // 2. If definition is not null, then enqueue a custom element upgrade reaction given element and definition.
    if (definition)
        enqueue_a_custom_element_upgrade_reaction(*definition);
}

// https://dom.spec.whatwg.org/#concept-element-defined
bool Element::is_defined() const
{
    // An element whose custom element state is "uncustomized" or "custom" is said to be defined.
    return m_custom_element_state == CustomElementState::Uncustomized || m_custom_element_state == CustomElementState::Custom;
}

// https://dom.spec.whatwg.org/#concept-element-custom
bool Element::is_custom() const
{
    // An element whose custom element state is "custom" is said to be custom.
    return m_custom_element_state == CustomElementState::Custom;
}

void Element::set_custom_element_state(CustomElementState state)
{
    if (m_custom_element_state == state)
        return;
    m_custom_element_state = state;

    Vector<CSS::InvalidationSet::Property, 1> changed_properties;
    changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Defined });
    invalidate_style(StyleInvalidationReason::CustomElementStateChange, changed_properties, {});
}

// https://html.spec.whatwg.org/multipage/dom.html#html-element-constructors
void Element::setup_custom_element_from_constructor(HTML::CustomElementDefinition& custom_element_definition, Optional<String> const& is_value)
{
    // 7.6. Set element's custom element state to "custom".
    set_custom_element_state(CustomElementState::Custom);

    // 7.7. Set element's custom element definition to definition.
    m_custom_element_definition = custom_element_definition;

    // 7.8. Set element's is value to is value.
    m_is_value = is_value;
}

void Element::set_prefix(Optional<FlyString> value)
{
    m_qualified_name.set_prefix(move(value));
}

// https://dom.spec.whatwg.org/#locate-a-namespace-prefix
Optional<String> Element::locate_a_namespace_prefix(Optional<String> const& namespace_) const
{
    // 1. If element’s namespace is namespace and its namespace prefix is non-null, then return its namespace prefix.
    if (this->namespace_uri() == namespace_ && this->prefix().has_value())
        return this->prefix()->to_string();

    // 2. If element has an attribute whose namespace prefix is "xmlns" and value is namespace, then return element’s first such attribute’s local name.
    if (auto attributes = this->attributes()) {
        for (size_t i = 0; i < attributes->length(); ++i) {
            auto& attr = *attributes->item(i);
            if (attr.prefix() == "xmlns" && attr.value() == namespace_)
                return attr.local_name().to_string();
        }
    }

    // 3. If element’s parent element is not null, then return the result of running locate a namespace prefix on that element using namespace.
    if (auto parent = this->parent_element())
        return parent->locate_a_namespace_prefix(namespace_);

    // 4. Return null
    return {};
}

void Element::for_each_attribute(Function<void(Attr const&)> callback) const
{
    if (!m_attributes)
        return;
    for (size_t i = 0; i < m_attributes->length(); ++i)
        callback(*m_attributes->item(i));
}

void Element::for_each_attribute(Function<void(FlyString const&, String const&)> callback) const
{
    for_each_attribute([&callback](Attr const& attr) {
        callback(attr.name(), attr.value());
    });
}

GC::Ptr<Layout::NodeWithStyle> Element::layout_node()
{
    return static_cast<Layout::NodeWithStyle*>(Node::layout_node());
}

GC::Ptr<Layout::NodeWithStyle const> Element::layout_node() const
{
    return static_cast<Layout::NodeWithStyle const*>(Node::layout_node());
}

bool Element::has_attributes() const
{
    return m_attributes && !m_attributes->is_empty();
}

size_t Element::attribute_list_size() const
{
    return m_attributes ? m_attributes->length() : 0;
}

GC::Ptr<CSS::CascadedProperties> Element::cascaded_properties(Optional<CSS::PseudoElement> pseudo_element) const
{
    if (pseudo_element.has_value()) {
        auto pseudo_element_data = get_pseudo_element(pseudo_element.value());
        if (pseudo_element_data.has_value())
            return pseudo_element_data->cascaded_properties();
        return nullptr;
    }
    return m_cascaded_properties;
}

void Element::set_cascaded_properties(Optional<CSS::PseudoElement> pseudo_element, GC::Ptr<CSS::CascadedProperties> cascaded_properties)
{
    if (pseudo_element.has_value()) {
        if (pseudo_element.value() >= CSS::PseudoElement::KnownPseudoElementCount)
            return;
        ensure_pseudo_element(pseudo_element.value()).set_cascaded_properties(cascaded_properties);
    } else {
        m_cascaded_properties = cascaded_properties;
    }
}

GC::Ptr<CSS::ComputedProperties> Element::computed_properties(Optional<CSS::PseudoElement> pseudo_element_type)
{
    if (pseudo_element_type.has_value()) {
        if (auto pseudo_element = get_pseudo_element(*pseudo_element_type); pseudo_element.has_value())
            return pseudo_element->computed_properties();
        return {};
    }
    return m_computed_properties;
}

GC::Ptr<CSS::ComputedProperties const> Element::computed_properties(Optional<CSS::PseudoElement> pseudo_element_type) const
{
    if (pseudo_element_type.has_value()) {
        if (auto pseudo_element = get_pseudo_element(*pseudo_element_type); pseudo_element.has_value())
            return pseudo_element->computed_properties();
        return {};
    }
    return m_computed_properties;
}

void Element::set_computed_properties(Optional<CSS::PseudoElement> pseudo_element_type, GC::Ptr<CSS::ComputedProperties> style)
{
    if (pseudo_element_type.has_value()) {
        if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(*pseudo_element_type))
            return;
        ensure_pseudo_element(*pseudo_element_type).set_computed_properties(style);
        return;
    }
    m_computed_properties = style;
    computed_properties_changed();
}

Optional<PseudoElement&> Element::get_pseudo_element(CSS::PseudoElement type) const
{
    if (!m_pseudo_element_data)
        return {};

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(type)) {
        return {};
    }

    auto pseudo_element = m_pseudo_element_data->get(type);
    if (!pseudo_element.has_value())
        return {};

    return *(pseudo_element.value());
}

PseudoElement& Element::ensure_pseudo_element(CSS::PseudoElement type) const
{
    if (!m_pseudo_element_data)
        m_pseudo_element_data = make<PseudoElementData>();

    VERIFY(CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(type));

    if (!m_pseudo_element_data->get(type).has_value()) {
        if (is_pseudo_element_root(type)) {
            m_pseudo_element_data->set(type, heap().allocate<PseudoElementTreeNode>());
        } else {
            m_pseudo_element_data->set(type, heap().allocate<PseudoElement>());
        }
    }

    return *(m_pseudo_element_data->get(type).value());
}

void Element::set_custom_property_data(Optional<CSS::PseudoElement> pseudo_element, RefPtr<CSS::CustomPropertyData const> data)
{
    if (!pseudo_element.has_value()) {
        m_custom_property_data = move(data);
        return;
    }

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value())) {
        return;
    }

    ensure_pseudo_element(pseudo_element.value()).set_custom_property_data(move(data));
}

RefPtr<CSS::CustomPropertyData const> Element::custom_property_data(Optional<CSS::PseudoElement> pseudo_element) const
{
    if (!pseudo_element.has_value())
        return m_custom_property_data;

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value()))
        return nullptr;

    return ensure_pseudo_element(pseudo_element.value()).custom_property_data();
}

// https://drafts.csswg.org/cssom-view/#dom-element-scroll
GC::Ref<WebIDL::Promise> Element::scroll(double x, double y)
{
    // 1. If invoked with one argument, follow these substeps:
    //    NOTE: Not relevant here.
    // 2. If invoked with two arguments, follow these substeps:
    //     1. Let options be null converted to a ScrollToOptions dictionary. [WEBIDL]
    //     2. Let x and y be the arguments, respectively.
    //     3. Normalize non-finite values for x and y.
    //     4. Let the left dictionary member of options have the value x.
    //     5. Let the top dictionary member of options have the value y.
    x = HTML::normalize_non_finite_values(x);
    y = HTML::normalize_non_finite_values(y);

    // 3. Let document be the element’s node document.
    auto& document = this->document();

    // 4. If document is not the active document, return a resolved Promise and abort the remaining steps.
    if (!document.is_active())
        return WebIDL::create_resolved_promise(realm(), JS::js_undefined());

    // 5. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 6. If window is null, return a resolved Promise and abort the remaining steps.
    if (!window)
        return WebIDL::create_resolved_promise(realm(), JS::js_undefined());

    // 7. If the element is the root element and document is in quirks mode, return a resolved Promise and abort the
    //    remaining steps.
    if (document.document_element() == this && document.in_quirks_mode())
        return WebIDL::create_resolved_promise(realm(), JS::js_undefined());

    // OPTIMIZATION: Scrolling an unscrolled element to (0, 0) is a no-op as long
    //               as the element is not eligible to be the Document.scrollingElement.
    if (x == 0
        && y == 0
        && scroll_offset({}).is_zero()
        && this != document.body()
        && this != document.document_element()) {
        return WebIDL::create_resolved_promise(realm(), JS::js_undefined());
    }

    // NB: Ensure that layout is up-to-date before looking at metrics.
    document.update_layout(UpdateLayoutReason::ElementScroll);

    // 8. If the element is the root element, return the Promise returned by scroll() on window after the method is
    //    invoked with scrollX on window as first argument and y as second argument, and abort the remaining steps.
    if (document.document_element() == this)
        return window->scroll(window->scroll_x(), y);

    // 9. If the element is the body element, document is in quirks mode, and the element is not potentially
    //    scrollable, return the Promise returned by scroll() on window after the method is invoked with options as the
    //    only argument, and abort the remaining steps.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return window->scroll(x, y);

    // 10. If the element does not have any associated box, the element has no associated scrolling box, or the element
    //     has no overflow, return a resolved Promise and abort the remaining steps.
    // FIXME: or the element has no overflow
    if (!paintable_box())
        return WebIDL::create_resolved_promise(realm(), JS::js_undefined());

    // 11. Scroll the element to x,y, with the scroll behavior being the value of the behavior dictionary member of
    //     options. Let scrollPromise be the Promise returned from this step.
    // FIXME: Implement this in terms of calling "scroll the element".
    auto scroll_offset = paintable_box()->scroll_offset();
    scroll_offset.set_x(CSSPixels::nearest_value_for(x));
    scroll_offset.set_y(CSSPixels::nearest_value_for(y));
    paintable_box()->set_scroll_offset(scroll_offset);
    auto scroll_promise = WebIDL::create_resolved_promise(realm(), JS::js_undefined());

    // 12. Return scrollPromise.
    return scroll_promise;
}

// https://drafts.csswg.org/cssom-view/#dom-element-scroll
GC::Ref<WebIDL::Promise> Element::scroll(HTML::ScrollToOptions options)
{
    // 1. If invoked with one argument, follow these substeps:
    //     1. Let options be the argument.
    //     2. Normalize non-finite values for left and top dictionary members of options, if present.
    //     3. Let x be the value of the left dictionary member of options, if present, or the element’s current scroll position on the x axis otherwise.
    //     4. Let y be the value of the top dictionary member of options, if present, or the element’s current scroll position on the y axis otherwise.
    // NOTE: remaining steps performed by Element::scroll(double x, double y)
    auto x = options.left.has_value() ? HTML::normalize_non_finite_values(options.left.value()) : scroll_left();
    auto y = options.top.has_value() ? HTML::normalize_non_finite_values(options.top.value()) : scroll_top();
    return scroll(x, y);
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollby
GC::Ref<WebIDL::Promise> Element::scroll_by(double x, double y)
{
    // 2. If invoked with two arguments, follow these substeps:
    //    1. Let options be null converted to a ScrollToOptions dictionary. [WEBIDL]
    HTML::ScrollToOptions options;

    //    2. Let x and y be the arguments, respectively.
    //    3. Normalize non-finite values for x and y.
    //    4. Let the left dictionary member of options have the value x.
    //    5. Let the top dictionary member of options have the value y.
    // NOTE: Element::scroll_by(HTML::ScrollToOptions) performs the normalization and following steps.
    options.left = x;
    options.top = y;
    return scroll_by(options);
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollby
GC::Ref<WebIDL::Promise> Element::scroll_by(HTML::ScrollToOptions options)
{
    // 1. If invoked with one argument, follow these substeps:
    //    1. Let options be the argument.
    //    2. Normalize non-finite values for left and top dictionary members of options, if present.
    auto left = HTML::normalize_non_finite_values(options.left);
    auto top = HTML::normalize_non_finite_values(options.top);

    // NB: Step 2 is implemented by the other overload of scroll_by().

    // 3. Add the value of scrollLeft to the left dictionary member.
    options.left = scroll_left() + left;

    // 4. Add the value of scrollTop to the top dictionary member.
    options.top = scroll_top() + top;

    // 5. Return the Promise returned by scroll() after the method is invoked with options as the only argument.
    return scroll(options);
}

// https://drafts.csswg.org/cssom-view-1/#dom-element-checkvisibility
bool Element::check_visibility(Optional<CheckVisibilityOptions> options)
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    document().update_layout(UpdateLayoutReason::ElementCheckVisibility);

    // 1. If this does not have an associated box, return false.
    if (!paintable_box())
        return false;

    // 2. If an ancestor of this in the flat tree has content-visibility: hidden, return false.
    for (auto* element = flat_tree_parent_element(); element; element = element->flat_tree_parent_element()) {
        if (element->computed_properties()->content_visibility() == CSS::ContentVisibility::Hidden)
            return false;
    }

    // AD-HOC: Since the rest of the steps use the options, we can return early if we haven't been given any options.
    if (!options.has_value())
        return true;

    // 3. If either the opacityProperty or the checkOpacity dictionary members of options are true, and this, or an
    //    ancestor of this in the flat tree, has a computed opacity value of 0, return false.
    if (options->opacity_property || options->check_opacity) {
        for (auto* element = this; element; element = element->flat_tree_parent_element()) {
            if (element->computed_properties()->opacity() == 0.0f)
                return false;
        }
    }

    // 4. If either the visibilityProperty or the checkVisibilityCSS dictionary members of options are true, and this
    //    is invisible, return false.
    if (options->visibility_property || options->check_visibility_css) {
        if (computed_properties()->visibility() == CSS::Visibility::Hidden)
            return false;
    }

    // 5. If the contentVisibilityAuto dictionary member of options is true and an ancestor of this in the flat tree
    //    skips its contents due to content-visibility: auto, return false.
    // FIXME: Currently we do not skip any content if content-visibility is auto: https://drafts.csswg.org/css-contain-2/#proximity-to-the-viewport
    auto const skipped_contents_due_to_content_visibility_auto = false;
    if (options->content_visibility_auto && skipped_contents_due_to_content_visibility_auto) {
        for (auto* element = flat_tree_parent_element(); element; element = element->flat_tree_parent_element()) {
            if (element->computed_properties()->content_visibility() == CSS::ContentVisibility::Auto)
                return false;
        }
    }

    // 6. Return true.
    return true;
}

// https://drafts.csswg.org/css-contain/#proximity-to-the-viewport
void Element::determine_proximity_to_the_viewport()
{
    // An element that has content-visibility: auto is in one of three states when it comes to its proximity to the viewport:

    // - The element is close to the viewport: In this state, the element is considered "on-screen": its paint
    //   containment box's overflow clip edge intersects with the viewport, or a user-agent defined margin around the
    //   viewport.
    auto viewport_rect = document().viewport_rect();
    // NOTE: This margin is meant to allow the user agent to begin preparing for an element to be in the
    // viewport soon. A margin of 50% is suggested as a reasonable default.
    viewport_rect.inflate(viewport_rect.width(), viewport_rect.height());
    // FIXME: We don't have paint containment or the overflow clip edge yet, so this is just using the absolute rect for now.
    if (paintable_box()->absolute_rect().intersects(viewport_rect))
        m_proximity_to_the_viewport = ProximityToTheViewport::CloseToTheViewport;

    // FIXME: If a filter (see [FILTER-EFFECTS-1]) with non local effects includes the element as part of its input, the user
    //        agent should also treat the element as relevant to the user when the filter’s output can affect the rendering
    //        within the viewport (or within the user-agent defined margin around the viewport), even if the element itself is
    //        still off-screen.

    // - The element is far away from the viewport: In this state, the element’s proximity to the viewport has been
    //   computed and is not close to the viewport.
    m_proximity_to_the_viewport = ProximityToTheViewport::FarAwayFromTheViewport;

    // - The element’s proximity to the viewport is not determined: In this state, the computation to determine the
    //   element’s proximity to the viewport has not been done since the last time the element was connected.
    // NOTE: This function is what does the computation to determine the element’s proximity to the viewport, so this is not the case.
}

// https://drafts.csswg.org/css-contain/#relevant-to-the-user
bool Element::is_relevant_to_the_user()
{
    // An element is relevant to the user if any of the following conditions are true:

    // The element is close to the viewport.
    if (m_proximity_to_the_viewport == ProximityToTheViewport::CloseToTheViewport)
        return true;

    // Either the element or its contents are focused, as described in the focus section of the HTML spec.
    auto focused_area = document().focused_area();
    if (focused_area && is_inclusive_ancestor_of(*focused_area))
        return true;

    // Either the element or its contents are selected, where selection is described in the selection API.
    if (document().get_selection()->contains_node(*this, true))
        return true;

    bool has_relevant_contents = false;
    for_each_in_inclusive_subtree_of_type<Element>([&](auto& element) {
        // Either the element or its contents are placed in the top layer.
        if (element.in_top_layer()) {
            has_relevant_contents = true;
            return TraversalDecision::Break;
        }

        // The element has a flat tree descendant that is captured in a view transition.
        // FIXME: for_each_in_inclusive_subtree_of_type() doesn't walk the flat tree. For example, it doesn't walk from a slot to its assigned slottable.
        if (&element != this && element.captured_in_a_view_transition()) {
            has_relevant_contents = true;
            return TraversalDecision::Break;
        }

        return TraversalDecision::Continue;
    });
    if (has_relevant_contents)
        return true;

    // NOTE: none of the above conditions are true, so the element is not relevant to the user.
    return false;
}

// https://drafts.csswg.org/css-contain-2/#skips-its-contents
bool Element::skips_its_contents()
{
    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-hidden
    // The element skips its contents.
    if (computed_properties()->content_visibility() == CSS::ContentVisibility::Hidden)
        return true;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // If the element is not relevant to the user, it also skips its contents.
    if (computed_properties()->content_visibility() == CSS::ContentVisibility::Auto && !this->is_relevant_to_the_user()) {
        return true;
    }

    return false;
}

i32 Element::number_of_owned_list_items() const
{
    AK::Checked<i32> number_of_owned_li_elements = 0;
    for_each_numbered_item_owned_by_list_owner([&number_of_owned_li_elements]([[maybe_unused]] Element* item) {
        number_of_owned_li_elements++;
        return IterationDecision::Continue;
    });

    return number_of_owned_li_elements.value();
}

// https://html.spec.whatwg.org/multipage/grouping-content.html#list-owner
GC::Ptr<Element> Element::list_owner() const
{
    // Any element whose computed value of 'display' is 'list-item' has a list owner, which is determined as follows:
    if (!m_is_contained_in_list_subtree && (!computed_properties() || !computed_properties()->display().is_list_item()))
        return nullptr;

    // 1. If the element is not being rendered, return null; the element has no list owner.
    if (!layout_node())
        return nullptr;

    // 2. Let ancestor be the element's parent.
    auto ancestor = parent_element();

    // AC-HOC: There may not be any parent element in a shadow tree.
    if (!ancestor)
        return nullptr;

    // 3. If the element has an ol, ul, or menu ancestor, set ancestor to the closest such ancestor element.
    for_each_ancestor([&ancestor](GC::Ref<Node> node) {
        if (node->is_html_ol_ul_menu_element()) {
            ancestor = static_cast<Element*>(node.ptr());
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });

    // 4. Return the closest inclusive ancestor of ancestor that produces a CSS box.
    ancestor->for_each_inclusive_ancestor([&ancestor](GC::Ref<Node> node) {
        if (is<Element>(*node) && node->paintable_box()) {
            ancestor = static_cast<Element*>(node.ptr());
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return const_cast<Element*>(ancestor.ptr());
}

void Element::maybe_invalidate_ordinals_for_list_owner(Optional<Element*> skip_node)
{
    if (auto owner = list_owner())
        owner->for_each_numbered_item_owned_by_list_owner([&](Element* item) {
            if (skip_node.has_value() && item == skip_node.value())
                return IterationDecision::Continue;

            item->m_ordinal_value = {};

            // Invalidate just the first ordinal in the list of numbered items.
            // NOTE: This works since this item is the first accessed (preorder) when rendering the list.
            //       It will trigger a recalculation of all ordinals on the [first] call to ordinal_value().
            return IterationDecision::Break;
        });
}

// https://html.spec.whatwg.org/multipage/grouping-content.html#ordinal-value
i32 Element::ordinal_value()
{
    if (m_ordinal_value.has_value())
        return m_ordinal_value.value();

    auto owner = list_owner();
    if (!owner)
        return 1;

    // 1. Let i be 1. [Not necessary]
    // 2. If owner is an ol element, let numbering be owner's starting value. Otherwise, let numbering be 1.
    AK::Checked<i32> numbering = 1;
    auto reversed = false;

    if (auto* ol_element = as_if<HTML::HTMLOListElement>(owner.ptr())) {
        numbering = ol_element->starting_value().value();
        reversed = ol_element->has_attribute(HTML::AttributeNames::reversed);
    }

    // 3. Loop : If i is greater than the number of list items that owner owns, then return; all of owner's owned list items have been assigned ordinal values.
    // NOTE: We use `owner->for_each_numbered_item_in_list` to iterate through the owner's list of owned elements.
    //       As a result, we don't need `i` as counter (spec) in the list of children, with no material consequences.
    owner->for_each_numbered_item_owned_by_list_owner([&](Element* item) {
        // 4. Let item be the ith of owner's owned list items, in tree order. [Not necessary]
        // 5. If item is an li element that has a value attribute, then:
        auto value_attribute = item->get_attribute(HTML::AttributeNames::value);
        if (item->is_html_li_element() && value_attribute.has_value()) {
            // 1. Let parsed be the result of parsing the value of the attribute as an integer.
            auto parsed = HTML::parse_integer(value_attribute.value());

            // 2. If parsed is not an error, then set numbering to parsed.
            if (parsed.has_value())
                numbering = parsed.value();
        }

        // 6. The ordinal value of item is numbering.
        item->m_ordinal_value = numbering.value();

        // 7. If owner is an ol element, and owner has a reversed attribute, decrement numbering by 1; otherwise, increment numbering by 1.
        if (reversed) {
            numbering--;
        } else {
            numbering++;
        }

        // 8. Increment i by 1. [Not necessary]
        // 9. Go to the step labeled loop.
        return IterationDecision::Continue;
    });

    return m_ordinal_value.value_or(1);
}

bool Element::id_reference_exists(String const& id_reference) const
{
    return document().get_element_by_id(id_reference);
}

void Element::register_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, IntersectionObserver::IntersectionObserverRegistration registration)
{
    if (!m_registered_intersection_observers)
        m_registered_intersection_observers = make<Vector<IntersectionObserver::IntersectionObserverRegistration>>();
    m_registered_intersection_observers->append(move(registration));
}

void Element::unregister_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, GC::Ref<IntersectionObserver::IntersectionObserver> observer)
{
    if (!m_registered_intersection_observers)
        return;
    m_registered_intersection_observers->remove_first_matching([&observer](IntersectionObserver::IntersectionObserverRegistration const& entry) {
        return entry.observer == observer;
    });
}

IntersectionObserver::IntersectionObserverRegistration& Element::get_intersection_observer_registration(Badge<DOM::Document>, IntersectionObserver::IntersectionObserver const& observer)
{
    VERIFY(m_registered_intersection_observers);
    auto registration_iterator = m_registered_intersection_observers->find_if([&observer](IntersectionObserver::IntersectionObserverRegistration const& entry) {
        return entry.observer.ptr() == &observer;
    });
    VERIFY(!registration_iterator.is_end());
    return *registration_iterator;
}

CSSPixelPoint Element::scroll_offset(Optional<CSS::PseudoElement> pseudo_element_type) const
{
    if (pseudo_element_type.has_value()) {
        if (auto pseudo_element = get_pseudo_element(*pseudo_element_type); pseudo_element.has_value())
            return pseudo_element->scroll_offset();
        return {};
    }
    return m_scroll_offset;
}

void Element::set_scroll_offset(Optional<CSS::PseudoElement> pseudo_element_type, CSSPixelPoint offset)
{
    if (pseudo_element_type.has_value()) {
        if (auto pseudo_element = get_pseudo_element(*pseudo_element_type); pseudo_element.has_value())
            pseudo_element->set_scroll_offset(offset);
    } else {
        m_scroll_offset = offset;
    }
}

// https://html.spec.whatwg.org/multipage/dom.html#translation-mode
Element::TranslationMode Element::translation_mode() const
{
    // Each element (even non-HTML elements) has a translation mode, which is in either the translate-enabled state or
    // the no-translate state.

    // If an HTML element's translate attribute is in the Yes state, then the element's translation mode is in the
    // translate-enabled state;
    // NOTE: The attribute is in the Yes state if the attribute is present and its value is the empty string or is a
    //       ASCII-case-insensitive match for "yes".
    auto maybe_translate_attribute = attribute(HTML::AttributeNames::translate);
    if (maybe_translate_attribute.has_value() && (maybe_translate_attribute.value().is_empty() || maybe_translate_attribute.value().equals_ignoring_ascii_case("yes"sv)))
        return TranslationMode::TranslateEnabled;

    // otherwise, if the element's translate attribute is in the No state, then the element's translation mode is in
    // the no-translate state.
    if (maybe_translate_attribute.has_value() && maybe_translate_attribute.value().equals_ignoring_ascii_case("no"sv)) {
        return TranslationMode::NoTranslate;
    }

    // Otherwise, either the element's translate attribute is in the Inherit state, or the element is not an HTML
    // element and thus does not have a translate attribute; in either case, the element's translation mode is in the
    // same state as its parent element's, if any.
    if (auto parent = parent_element())
        return parent->translation_mode();

    // or in the translate-enabled state, if the element's parent element is null
    return TranslationMode::TranslateEnabled;
}

// https://html.spec.whatwg.org/multipage/dom.html#the-directionality
Element::Directionality Element::directionality() const
{
    // The directionality of an element (any element, not just an HTML element) is either 'ltr' or 'rtl'.
    // To compute the directionality given an element element, switch on element's dir attribute state:
    auto maybe_dir = this->dir();
    if (maybe_dir.has_value()) {
        auto dir = maybe_dir.release_value();
        switch (dir) {
        // -> ltr
        case Dir::Ltr:
            // Return 'ltr'.
            return Directionality::Ltr;
        // -> rtl
        case Dir::Rtl:
            // Return 'rtl'.
            return Directionality::Rtl;
        // -> auto
        case Dir::Auto:
            // 1. Let result be the auto directionality of element.
            auto result = auto_directionality();

            // 2. If result is null, then return 'ltr'.
            if (!result.has_value())
                return Directionality::Ltr;

            // 3. Return result.
            return result.release_value();
        }
    }
    // -> undefined
    VERIFY(!maybe_dir.has_value());

    // If element is a bdi element:
    if (local_name() == HTML::TagNames::bdi) {
        // 1. Let result be the auto directionality of element.
        auto result = auto_directionality();

        // 2. If result is null, then return 'ltr'.
        if (!result.has_value())
            return Directionality::Ltr;

        // 3. Return result.
        return result.release_value();
    }

    // If element is an input element whose type attribute is in the Telephone state:
    if (is<HTML::HTMLInputElement>(this) && static_cast<HTML::HTMLInputElement const&>(*this).type_state() == HTML::HTMLInputElement::TypeAttributeState::Telephone) {
        // Return 'ltr'.
        return Directionality::Ltr;
    }

    // Otherwise:
    // Return the parent directionality of element.
    return parent_directionality();
}

// https://html.spec.whatwg.org/multipage/dom.html#auto-directionality-form-associated-elements
bool Element::is_auto_directionality_form_associated_element() const
{
    // The auto-directionality form-associated elements are:
    // input elements whose type attribute is in the Hidden, Text, Search, Telephone, URL, Email, Password, Submit Button, Reset Button, or Button state,
    // and textarea elements.
    return is<HTML::HTMLTextAreaElement>(this)
        || (is<HTML::HTMLInputElement>(this) && first_is_one_of(static_cast<HTML::HTMLInputElement const&>(*this).type_state(), HTML::HTMLInputElement::TypeAttributeState::Hidden, HTML::HTMLInputElement::TypeAttributeState::Text, HTML::HTMLInputElement::TypeAttributeState::Search, HTML::HTMLInputElement::TypeAttributeState::Telephone, HTML::HTMLInputElement::TypeAttributeState::URL, HTML::HTMLInputElement::TypeAttributeState::Email, HTML::HTMLInputElement::TypeAttributeState::Password, HTML::HTMLInputElement::TypeAttributeState::SubmitButton, HTML::HTMLInputElement::TypeAttributeState::ResetButton, HTML::HTMLInputElement::TypeAttributeState::Button));
}

// https://html.spec.whatwg.org/multipage/dom.html#auto-directionality
Optional<Element::Directionality> Element::auto_directionality() const
{
    // 1. If element is an auto-directionality form-associated element:
    if (is_auto_directionality_form_associated_element()) {
        auto const& form_associated_element = as<HTML::FormAssociatedElement>(*this);
        auto const& value = form_associated_element.value();

        // 1. If element's value contains a character of bidirectional character type AL or R,
        //    and there is no character of bidirectional character type L anywhere before it in the element's value, then return 'rtl'.
        for (auto code_point : value) {
            auto bidi_class = Unicode::bidirectional_class(code_point);
            if (bidi_class == Unicode::BidiClass::LeftToRight)
                break;
            if (bidi_class == Unicode::BidiClass::RightToLeftArabic || bidi_class == Unicode::BidiClass::RightToLeft)
                return Directionality::Rtl;
        }

        // 2. If element's value is not the empty string, then return 'ltr'.
        if (value.is_empty())
            return Directionality::Ltr;

        // 3. Return null.
        return {};
    }

    // 2. If element is a slot element whose root is a shadow root and element's assigned nodes are not empty:
    if (is<HTML::HTMLSlotElement>(this)) {
        auto const& slot = static_cast<HTML::HTMLSlotElement const&>(*this);
        if (slot.root().is_shadow_root() && !slot.assigned_nodes().is_empty()) {
            // 1 . For each node child of element's assigned nodes:
            for (auto const& child : slot.assigned_nodes()) {
                // 1. Let childDirection be null.
                Optional<Directionality> child_direction;

                // 2. If child is a Text node, then set childDirection to the text node directionality of child.
                if (child->is_text())
                    child_direction = static_cast<Text const&>(*child).directionality();

                // 3. Otherwise:
                else {
                    // 1. Assert: child is an Element node.
                    VERIFY(child->is_element());

                    // 2. Set childDirection to the contained text auto directionality of child with canExcludeRoot set to true.
                    child_direction = static_cast<Element const&>(*child).contained_text_auto_directionality(true);
                }

                // 4. If childDirection is not null, then return childDirection.
                if (child_direction.has_value())
                    return child_direction;
            }

            // 2. Return null.
            return {};
        }
    }

    // 3. Return the contained text auto directionality of element with canExcludeRoot set to false.
    return contained_text_auto_directionality(false);
}

// https://html.spec.whatwg.org/multipage/dom.html#contained-text-auto-directionality
Optional<Element::Directionality> Element::contained_text_auto_directionality(bool can_exclude_root) const
{
    // To compute the contained text auto directionality of an element element with a boolean canExcludeRoot:

    // 1. For each node descendant of element's descendants, in tree order:
    Optional<Directionality> result;
    for_each_in_subtree([&](auto& descendant) {
        // 1. If any of
        //    - descendant
        //    - any ancestor element of descendant that is a descendant of element
        //    - if canExcludeRoot is true, element
        //    is one of
        //    - FIXME: a bdi element
        //    - a script element
        //    - a style element
        //    - a textarea element
        //    - an element whose dir attribute is not in the undefined state
        //    then continue.
        // NOTE: "any ancestor element of descendant that is a descendant of element" will be iterated already.
        auto is_one_of_the_filtered_elements = [](DOM::Node const& descendant) -> bool {
            return is<HTML::HTMLScriptElement>(descendant)
                || is<HTML::HTMLStyleElement>(descendant)
                || is<HTML::HTMLTextAreaElement>(descendant)
                || (is<Element>(descendant) && static_cast<Element const&>(descendant).dir().has_value());
        };
        if (is_one_of_the_filtered_elements(descendant)
            || (can_exclude_root && is_one_of_the_filtered_elements(*this))) {
            return TraversalDecision::SkipChildrenAndContinue;
        }

        // 2. If descendant is a slot element whose root is a shadow root, then return the directionality of that shadow root's host.
        if (is<HTML::HTMLSlotElement>(descendant)) {
            auto const& root = static_cast<HTML::HTMLSlotElement const&>(descendant).root();
            if (root.is_shadow_root()) {
                auto const& host = static_cast<ShadowRoot const&>(root).host();
                VERIFY(host);
                result = host->directionality();
                return TraversalDecision::Break;
            }
        }

        // 3. If descendant is not a Text node, then continue.
        if (!descendant.is_text())
            return TraversalDecision::Continue;

        // 4. Let result be the text node directionality of descendant.
        result = static_cast<Text const&>(descendant).directionality();

        // 5. If result is not null, then return result.
        if (result.has_value())
            return TraversalDecision::Break;

        return TraversalDecision::Continue;
    });

    if (result.has_value())
        return result;

    // 2. Return null.
    return {};
}

// https://html.spec.whatwg.org/multipage/dom.html#parent-directionality
Element::Directionality Element::parent_directionality() const
{
    // 1. Let parentNode be element's parent node.
    auto const* parent_node = this->parent_node();

    // 2. If parentNode is a shadow root, then return the directionality of parentNode's host.
    if (is<ShadowRoot>(parent_node)) {
        auto const& host = static_cast<ShadowRoot const&>(*parent_node).host();
        VERIFY(host);
        return host->directionality();
    }

    // 3. If parentNode is an element, then return the directionality of parentNode.
    if (is<Element>(parent_node))
        return static_cast<Element const&>(*parent_node).directionality();

    // 4. Return 'ltr'.
    return Directionality::Ltr;
}

// https://dom.spec.whatwg.org/#concept-element-attributes-change-ext
void Element::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    // AD-HOC: Everything below requires that there is no namespace, so return early if there is one.
    if (namespace_.has_value())
        return;

    // https://dom.spec.whatwg.org/#ref-for-concept-element-attributes-change-ext①
    // 1. If localName is slot and namespace is null, then:
    if (local_name == HTML::AttributeNames::slot) {
        // 1. If value is oldValue, then return.
        if (value == old_value)
            return;

        // 2. If value is null and oldValue is the empty string, then return.
        if (!value.has_value() && old_value == String {})
            return;

        // 3. If value is the empty string and oldValue is null, then return.
        if (value == String {} && !old_value.has_value())
            return;

        // 4. If value is null or the empty string, then set element’s name to the empty string.
        if (!value.has_value() || value->is_empty())
            set_slottable_name({});
        // 5. Otherwise, set element’s name to value.
        else
            set_slottable_name(*value);

        // 6. If element is assigned, then run assign slottables for element’s assigned slot.
        if (auto assigned_slot = assigned_slot_internal())
            assign_slottables(*assigned_slot);

        // 7. Run assign a slot for element.
        assign_a_slot(GC::Ref { *this });
        return;
    }

    auto value_or_empty = value.value_or(String {});

    if (local_name == HTML::AttributeNames::id) {
        if (value_or_empty.is_empty())
            m_id = {};
        else
            m_id = value_or_empty;

        if (is_connected()) {
            Optional<FlyString> old_value_fly_string;
            if (old_value.has_value())
                old_value_fly_string = *old_value;
            document().element_id_changed({}, *this, old_value_fly_string);
        }
    } else if (local_name == HTML::AttributeNames::name) {
        if (value_or_empty.is_empty())
            m_name = {};
        else
            m_name = value_or_empty;

        if (is_connected())
            document().element_name_changed({}, *this);
    } else if (local_name == HTML::AttributeNames::class_) {
        if (value_or_empty.is_empty()) {
            m_classes.clear();
        } else {
            auto new_classes = value_or_empty.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
            m_classes.clear();
            m_classes.ensure_capacity(new_classes.size());
            for (auto& new_class : new_classes) {
                m_classes.unchecked_append(FlyString::from_utf8(new_class).release_value_but_fixme_should_propagate_errors());
            }
        }
        if (m_class_list)
            m_class_list->associated_attribute_changed(value_or_empty);
    } else if (local_name == HTML::AttributeNames::style) {
        // https://drafts.csswg.org/cssom/#ref-for-cssstyledeclaration-updating-flag
        if (m_inline_style && m_inline_style->is_updating())
            return;
        if (!m_inline_style)
            m_inline_style = CSS::CSSStyleProperties::create_element_inline_style({ *this }, {}, {});
        m_inline_style->set_declarations_from_text(value.value_or(""_string));
        set_needs_style_update(true);
    } else if (local_name == HTML::AttributeNames::dir) {
        // https://html.spec.whatwg.org/multipage/dom.html#attr-dir
        if (value_or_empty.equals_ignoring_ascii_case("ltr"sv))
            m_dir = Dir::Ltr;
        else if (value_or_empty.equals_ignoring_ascii_case("rtl"sv))
            m_dir = Dir::Rtl;
        else if (value_or_empty.equals_ignoring_ascii_case("auto"sv))
            m_dir = Dir::Auto;
        else
            m_dir = {};
    } else if (local_name == HTML::AttributeNames::lang) {
        for_each_in_inclusive_subtree_of_type<Element>([](auto& element) {
            element.invalidate_lang_value();
            return TraversalDecision::Continue;
        });
    } else if (local_name == HTML::AttributeNames::part) {
        m_parts.clear();
        if (!value_or_empty.is_empty()) {
            auto new_parts = value_or_empty.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
            m_parts.clear();
            m_parts.ensure_capacity(new_parts.size());
            for (auto& new_part : new_parts)
                m_parts.unchecked_append(MUST(FlyString::from_utf8(new_part)));
        }
        if (m_part_list)
            m_part_list->associated_attribute_changed(value_or_empty);
    }

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#reflecting-content-attributes-in-idl-attributes:concept-element-attributes-change-ext
    // 1. If localName is not attr or namespace is not null, then return.
    // 2. Set element's explicitly set attr-element to null.
#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute)    \
    else if (local_name == ARIA::AttributeNames::referencing_attribute) \
    {                                                                   \
        set_##attribute({});                                            \
    }
    ENUMERATE_ARIA_ELEMENT_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#reflecting-content-attributes-in-idl-attributes:concept-element-attributes-change-ext-2
    // 1. If localName is not attr or namespace is not null, then return.
    // 2. Set element's explicitly set attr-elements to null.
#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute)    \
    else if (local_name == ARIA::AttributeNames::referencing_attribute) \
    {                                                                   \
        set_##attribute({});                                            \
    }
    ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE
}

auto Element::ensure_custom_element_reaction_queue() -> CustomElementReactionQueue&
{
    if (!m_custom_element_reaction_queue)
        m_custom_element_reaction_queue = make<CustomElementReactionQueue>();
    return *m_custom_element_reaction_queue;
}

HTML::CustomStateSet& Element::ensure_custom_state_set()
{
    if (!m_custom_state_set)
        m_custom_state_set = HTML::CustomStateSet::create(realm(), *this);
    return *m_custom_state_set;
}

CSS::StyleSheetList& Element::document_or_shadow_root_style_sheets()
{
    auto& root_node = root();
    if (is<DOM::ShadowRoot>(root_node))
        return static_cast<DOM::ShadowRoot&>(root_node).style_sheets();

    return document().style_sheets();
}

ElementByIdMap& Element::document_or_shadow_root_element_by_id_map()
{
    auto& root_node = root();
    if (is<ShadowRoot>(root_node))
        return static_cast<ShadowRoot&>(root_node).element_by_id();
    return document().element_by_id();
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-gethtml
WebIDL::ExceptionOr<String> Element::get_html(GetHTMLOptions const& options) const
{
    // Element's getHTML(options) method steps are to return the result
    // of HTML fragment serialization algorithm with this,
    // options["serializableShadowRoots"], and options["shadowRoots"].
    return HTML::HTMLParser::serialize_html_fragment(
        *this,
        options.serializable_shadow_roots ? HTML::HTMLParser::SerializableShadowRoots::Yes : HTML::HTMLParser::SerializableShadowRoots::No,
        options.shadow_roots);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-sethtmlunsafe
WebIDL::ExceptionOr<void> Element::set_html_unsafe(TrustedTypes::TrustedHTMLOrString const& html)
{
    // 1. Let compliantHTML be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, html, "Element setHTMLUnsafe", and "script".
    auto const compliant_html = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::relevant_global_object(*this),
        html,
        TrustedTypes::InjectionSink::Element_setHTMLUnsafe,
        TrustedTypes::Script.to_string()));

    // 2. Let target be this's template contents if this is a template element; otherwise this.
    DOM::Node* target = this;
    if (is<HTML::HTMLTemplateElement>(*this))
        target = as<HTML::HTMLTemplateElement>(*this).content().ptr();

    // 3. Unsafe set HTML given target, this, and compliantHTML.
    TRY(target->unsafely_set_html(*this, compliant_html.to_utf8_but_should_be_ported_to_utf16()));

    return {};
}

Optional<CSS::CountersSet const&> Element::counters_set() const
{
    if (!m_counters_set)
        return {};
    return *m_counters_set;
}

CSS::CountersSet& Element::ensure_counters_set()
{
    if (!m_counters_set)
        m_counters_set = make<CSS::CountersSet>();
    return *m_counters_set;
}

void Element::set_counters_set(OwnPtr<CSS::CountersSet>&& counters_set)
{
    m_counters_set = move(counters_set);
}

// https://html.spec.whatwg.org/multipage/dom.html#the-lang-and-xml:lang-attributes
Optional<String> Element::lang() const
{
    auto determine_lang_attribute = [&]() -> String {
        // 1. If the node is an element that has a lang attribute in the XML namespace set
        //      Use the value of that attribute.
        auto maybe_xml_lang = get_attribute_ns(Namespace::XML, HTML::AttributeNames::lang);
        if (maybe_xml_lang.has_value())
            return maybe_xml_lang.release_value();

        // 2. If the node is an HTML element or an element in the SVG namespace, and it has a lang in no namespace attribute set
        //      Use the value of that attribute.
        if (is_html_element() || namespace_uri() == Namespace::SVG) {
            auto maybe_lang = get_attribute(HTML::AttributeNames::lang);
            if (maybe_lang.has_value())
                return maybe_lang.release_value();
        }

        // 3. If the node's parent is a shadow root
        //      Use the language of that shadow root's host.
        if (auto parent = parent_element()) {
            if (parent->is_shadow_root())
                return parent->shadow_root()->host()->lang().value_or({});
        }

        // 4. If the node's parent element is not null
        //      Use the language of that parent element.
        if (auto parent = parent_element())
            return parent->lang().value_or({});

        // 5. Otherwise
        //      - If there is a pragma-set default language set, then that is the language of the node.
        if (document().pragma_set_default_language().has_value()) {
            return document().pragma_set_default_language().value_or({});
        }

        //      - If there is no pragma-set default language set, then language information from a higher-level protocol (such as HTTP),
        if (document().http_content_language().has_value()) {
            return document().http_content_language().value_or({});
        }

        //        if any, must be used as the final fallback language instead.
        //      - In the absence of any such language information, and in cases where the higher-level protocol reports multiple languages,
        //        the language of the node is unknown, and the corresponding language tag is the empty string.
        // Default locale sounds like a reasonable fallback here.
        return {};
    };

    if (!m_lang_value.has_value())
        m_lang_value = determine_lang_attribute();

    // If the resulting value is the empty string, then it must be interpreted as meaning that the language of the node is explicitly unknown.
    if (m_lang_value->is_empty())
        return {};

    return m_lang_value;
}

void Element::invalidate_lang_value()
{
    if (m_lang_value.has_value()) {
        m_lang_value.clear();
        set_needs_style_update(true);
    }
}

template<typename Callback>
void Element::for_each_numbered_item_owned_by_list_owner(Callback callback)
{
    for (auto* node = this->first_child(); node != nullptr; node = node->next_in_pre_order(this)) {
        auto* element = as_if<Element>(node);
        if (!element)
            continue;

        element->m_is_contained_in_list_subtree = true;

        if (node->is_html_ol_ul_menu_element()) {
            // Skip list nodes and their descendents. They have their own, unrelated ordinals.
            while (node->last_child() != nullptr) // Find the last node (preorder) in the subtree headed by node. O(1).
                node = node->last_child();

            continue;
        }

        if (!node->layout_node())
            continue; // Skip nodes that do not participate in the layout.

        if (!element->computed_properties()->display().is_list_item())
            continue; // Skip nodes that are not list items.

        if (callback(element) == IterationDecision::Break)
            return;
    }
}

// https://drafts.csswg.org/css-images-4/#element-not-rendered
bool Element::not_rendered() const
{
    // An element is not rendered if it does not have an associated box.
    if (!layout_node() || !paintable_box())
        return true;

    return false;
}

// https://drafts.csswg.org/css-view-transitions-1/#document-scoped-view-transition-name
Optional<FlyString> Element::document_scoped_view_transition_name()
{
    // To get the document-scoped view transition name for an Element element:

    // 1. Let scopedViewTransitionName be the computed value of view-transition-name for element.
    auto scoped_view_transition_name = computed_properties()->view_transition_name();

    // 2. If scopedViewTransitionName is associated with element’s node document, then return
    //    scopedViewTransitionName.
    // FIXME: Properly handle tree-scoping of the name here.
    //        (see https://drafts.csswg.org/css-view-transitions-1/#propdef-view-transition-name , "Each view transition name is a tree-scoped name.")
    if (true) {
        return scoped_view_transition_name;
    }

    // 3. Otherwise, return none.
    return {};
}

// https://drafts.csswg.org/css-view-transitions-1/#capture-the-image
// To capture the image given an element element, perform the following steps. They return an image.
RefPtr<Gfx::ImmutableBitmap> Element::capture_the_image()
{
    // FIXME: Actually implement this.
    return Gfx::ImmutableBitmap::create(MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, Gfx::IntSize(1, 1))));
}

void Element::set_pointer_capture(WebIDL::Long pointer_id)
{
    (void)pointer_id;
    dbgln("FIXME: Implement Element::setPointerCapture()");
}

void Element::release_pointer_capture(WebIDL::Long pointer_id)
{
    (void)pointer_id;
    dbgln("FIXME: Implement Element::releasePointerCapture()");
}

bool Element::has_pointer_capture(WebIDL::Long pointer_id)
{
    (void)pointer_id;
    dbgln("FIXME: Implement Element::hasPointerCapture()");
    return false;
}

GC::Ptr<NamedNodeMap> Element::attributes()
{
    if (!m_attributes)
        m_attributes = NamedNodeMap::create(*this);
    return m_attributes;
}

GC::Ptr<NamedNodeMap const> Element::attributes() const
{
    return const_cast<Element&>(*this).attributes();
}

FlyString const& Element::html_uppercased_qualified_name() const
{
    return m_html_uppercased_qualified_name.ensure([&] { return make_html_uppercased_qualified_name(); });
}

void Element::play_or_cancel_animations_after_display_property_change()
{
    // OPTIMIZATION: We don't care about elements with no CSS defined animations
    if (!has_css_defined_animations())
        return;

    // OPTIMIZATION: We don't care about animations in disconnected subtrees.
    if (!is_connected())
        return;

    // https://www.w3.org/TR/css-animations-1/#animations
    // Setting the display property to none will terminate any running animation applied to the element and its descendants.
    // If an element has a display of none, updating display to a value other than none will start all animations applied to
    // the element by the animation-name property, as well as all animations applied to descendants with display other than none.

    auto has_display_none_inclusive_ancestor = this->has_inclusive_ancestor_with_display_none();

    auto play_or_cancel_depending_on_display = [&](HashMap<FlyString, GC::Ref<CSS::CSSAnimation>>& animations) {
        for (auto& [_, animation] : animations) {
            if (has_display_none_inclusive_ancestor) {
                animation->cancel();
            } else {
                // NOTE: It is safe to assume this has a value as it is set when creating a CSS defined animation
                auto play_state = animation->last_css_animation_play_state().value();

                if (play_state == CSS::AnimationPlayState::Running) {
                    HTML::TemporaryExecutionContext context(realm());
                    animation->play().release_value_but_fixme_should_propagate_errors();
                } else if (play_state == CSS::AnimationPlayState::Paused) {
                    HTML::TemporaryExecutionContext context(realm());
                    animation->pause().release_value_but_fixme_should_propagate_errors();
                }
            }
        }
    };

    play_or_cancel_depending_on_display(*css_defined_animations({}));

    for (auto i = 0; i < to_underlying(CSS::PseudoElement::KnownPseudoElementCount); i++) {
        auto pseudo_element = static_cast<CSS::PseudoElement>(i);
        play_or_cancel_depending_on_display(*css_defined_animations(pseudo_element));
    }
}

// https://drafts.csswg.org/selectors/#indicate-focus
bool Element::should_indicate_focus() const
{
    // User agents can choose their own heuristics for when to indicate focus; however, the following (non-normative)
    // suggestions can be used as a starting point for when to indicate focus on the currently focused element:

    // FIXME: * If the user has expressed a preference (such as via a system preference or a browser setting) to always see a
    //   visible focus indicator, indicate focus regardless of any other factors. (Another option may be for the user
    //   agent to show its own focus indicator regardless of author styles.)

    // * If the element which supports keyboard input (such as an input element, or any other element that would
    //   triggers a virtual keyboard to be shown on focus if a physical keyboard were not present), indicate focus.
    if (is<HTML::FormAssociatedTextControlElement>(this) || is_editable_or_editing_host())
        return true;

    // * If the user interacts with the page via keyboard or some other non-pointing device, indicate focus. (This means
    //   keyboard usage may change whether this pseudo-class matches even if it doesn’t affect :focus).
    if (document().last_focus_trigger() == HTML::FocusTrigger::Key)
        return true;

    // FIXME: * If the user interacts with the page via a pointing device (mouse, touchscreen, etc.) and the focused element
    //   does not support keyboard input, don’t indicate focus.

    // * If the previously-focused element indicated focus, and a script causes focus to move elsewhere, indicate focus
    //   on the newly focused element.
    //   Conversely, if the previously-focused element did not indicate focus, and a script causes focus to move
    //   elsewhere, don’t indicate focus on the newly focused element.
    // AD-HOC: Other browsers seem to always indicate focus on programmatically focused elements.
    if (document().last_focus_trigger() == HTML::FocusTrigger::Script)
        return true;

    // FIXME: * If a newly-displayed element automatically gains focus (such as an action button in a freshly opened dialog),
    //   that element should indicate focus.

    return false;
}

// https://html.spec.whatwg.org/multipage/interaction.html#tabindex-value
bool Element::is_focusable() const
{
    return HTML::parse_integer(get_attribute_value(HTML::AttributeNames::tabindex)).has_value();
}

void Element::set_had_duplicate_attribute_during_tokenization(Badge<HTML::HTMLParser>)
{
    m_had_duplicate_attribute_during_tokenization = true;
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-element-computedstylemap
GC::Ref<CSS::StylePropertyMapReadOnly> Element::computed_style_map()
{
    // The computedStyleMap() method must, when called on an Element this, perform the following steps:

    // 1. If this’s [[computedStyleMapCache]] internal slot is set to null, set its value to a new
    //    StylePropertyMapReadOnly object, whose [[declarations]] internal slot are the name and computed value of
    //    every longhand CSS property supported by the User Agent, every registered custom property, and every
    //    non-registered custom property which is not set to its initial value on this, in the standard order.
    //
    //    The computed values in the [[declarations]] of this object must remain up-to-date, changing as style
    //    resolution changes the properties on this and how they’re computed.
    //
    // NOTE: In practice, since the values are "hidden" behind a .get() method call, UAs can delay computing anything
    //    until a given property is actually requested.
    if (m_computed_style_map_cache == nullptr) {
        m_computed_style_map_cache = CSS::StylePropertyMapReadOnly::create_computed_style(realm(), AbstractElement { *this });
    }

    // 2. Return this’s [[computedStyleMapCache]] internal slot.
    return *m_computed_style_map_cache;
}

double Element::ensure_css_random_base_value(CSS::RandomCachingKey const& random_caching_key)
{
    // NB: We cache element-shared random base values on the Document and non-element-shared ones on the Element itself
    //     so that when an element is removed it takes its non-shared cache with it.
    if (!random_caching_key.element_id.has_value())
        return document().ensure_element_shared_css_random_base_value(random_caching_key);

    return m_element_specific_css_random_base_value_cache.ensure(random_caching_key, []() {
        static XorShift128PlusRNG random_number_generator;
        return random_number_generator.get();
    });
}

GC::Ref<WebIDL::Promise> Element::request_pointer_lock(Optional<PointerLockOptions>)
{
    dbgln("FIXME: request_pointer_lock()");
    auto promise = WebIDL::create_promise(realm());
    auto error = WebIDL::NotSupportedError::create(realm(), "request_pointer_lock() is not implemented"_utf16);
    WebIDL::reject_promise(realm(), promise, error);
    return promise;
}

// The element to inherit style from.
// If a pseudo-element is specified, this will return the element itself.
// Otherwise, if this element is slotted somewhere, it will return the slot.
// Otherwise, it will return the parent or shadow host element of this element.
GC::Ptr<Element const> Element::element_to_inherit_style_from(Optional<CSS::PseudoElement> pseudo_element) const
{
    if (pseudo_element.has_value())
        return this;
    if (auto const slot = assigned_slot_internal())
        return slot;
    return parent_or_shadow_host_element();
}

// https://html.spec.whatwg.org/multipage/dom.html#block-rendering
void Element::block_rendering()
{
    // 1. Let document be el's node document.
    auto& document = this->document();

    // 2. If document allows adding render-blocking elements, then append el to document's render-blocking element set.
    if (document.allows_adding_render_blocking_elements()) {
        document.add_render_blocking_element(*this);
    }
}

// https://html.spec.whatwg.org/multipage/dom.html#unblock-rendering
void Element::unblock_rendering()
{
    // 1. Let document be el's node document.
    auto& document = this->document();

    // 2. Remove el from document's render-blocking element set.
    document.remove_render_blocking_element(*this);
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#potentially-render-blocking
bool Element::is_potentially_render_blocking()
{
    // An element is potentially render-blocking if
    // FIXME: its blocking tokens set contains "render",
    // or if it is implicitly potentially render-blocking, which will be defined at the individual elements.
    return is_implicitly_potentially_render_blocking();
}

}
