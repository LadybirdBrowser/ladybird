/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2025, Simon Farre <simon.farre.cx@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/IterationDecision.h>
#include <AK/JsonObjectSerializer.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <AK/SaturatingMath.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakHashMap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Object.h>
#include <LibURL/Parser.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Locale.h>
#include <LibWeb/Bindings/Element.h>
#include <LibWeb/Bindings/Window.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/GeneratedContent.h>
#include <LibWeb/CSS/Invalidation/AttributeInvalidator.h>
#include <LibWeb/CSS/Invalidation/CustomElementInvalidator.h>
#include <LibWeb/CSS/Invalidation/ElementStateInvalidator.h>
#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/CSS/Invalidation/PartInvalidator.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StylePropertyMap.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/RandomValueSharingStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/ElementRareData.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/MutationObserver.h>
#include <LibWeb/DOM/MutationType.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/SelectorQuery.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/EditingHistory.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectList.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementName.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactionNames.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactions.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLBaseElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLMenuElement.h>
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
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/ScrollOptions.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/XMLSerializer.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/IntersectionObserver/IntersectionObserver.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/TreeBuilder.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/MathML/TagNames.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/SVG/SVGAElement.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/XML/XMLFragmentParser.h>

namespace Web::DOM {

Element::RareData::~RareData() = default;

void Element::RareData::visit_edges(Cell::Visitor& visitor)
{
    Node::RareData::visit_edges(visitor);
    SlottableMixin::RareData::visit_edges(visitor);
    visitor.visit(attribute_map);
    visitor.visit(class_list);
    visitor.visit(part_list);
    if (custom_element_reaction_queue) {
        for (auto const& reaction : *custom_element_reaction_queue) {
            reaction.visit(
                [&](CustomElementUpgradeReaction const& upgrade_reaction) {
                    visitor.visit(upgrade_reaction.custom_element_definition);
                },
                [&](CustomElementCallbackReaction const& callback_reaction) {
                    visitor.visit(callback_reaction.callback);
                    callback_reaction.arguments.visit(
                        [](Empty) {},
                        [&](CustomElementAdoptedCallbackReactionArguments const& adopted_arguments) {
                            visitor.visit(adopted_arguments.old_document);
                            visitor.visit(adopted_arguments.new_document);
                        },
                        [&](CustomElementAttributeChangedCallbackReactionArguments const&) {},
                        [&](CustomElementFormAssociatedCallbackReactionArguments const& form_associated_arguments) {
                            visitor.visit(form_associated_arguments.form);
                        },
                        [&](CustomElementFormDisabledCallbackReactionArguments const&) {});
                },
                [&](CustomElementConnectedMoveCallbackReaction const& connected_move_reaction) {
                    visitor.visit(connected_move_reaction.disconnected_callback);
                    visitor.visit(connected_move_reaction.connected_callback);
                });
        }
    }
    visitor.visit(custom_state_set);
    visitor.visit(computed_style_map_cache);
    visitor.visit(attribute_style_map);
    visitor.visit(custom_element_definition);
    visitor.visit(custom_element_registry);
    visitor.visit(dataset);
    if (pseudo_element_data) {
        for (auto& pseudo_element : *pseudo_element_data)
            visitor.visit(pseudo_element.value);
    }
    if (registered_intersection_observers) {
        for (auto& observer : *registered_intersection_observers)
            visitor.visit(observer);
    }
    if (counters_set)
        counters_set->visit_edges(visitor);
}

OwnPtr<Node::RareData> Element::create_rare_data() const
{
    return make<RareData>();
}

SlottableMixin::RareData* Element::slottable_rare_data()
{
    return element_rare_data();
}

SlottableMixin::RareData const* Element::slottable_rare_data() const
{
    return element_rare_data();
}

SlottableMixin::RareData& Element::ensure_slottable_rare_data()
{
    return ensure_element_rare_data();
}

ARIA::ARIAMixin::RareData* Element::aria_rare_data()
{
    return element_rare_data();
}

ARIA::ARIAMixin::RareData const* Element::aria_rare_data() const
{
    return element_rare_data();
}

ARIA::ARIAMixin::RareData& Element::ensure_aria_rare_data()
{
    return ensure_element_rare_data();
}

Element::RareData& Element::ensure_element_rare_data() const
{
    return static_cast<RareData&>(ensure_rare_data());
}

Element::RareData* Element::element_rare_data()
{
    return static_cast<RareData*>(rare_data());
}

Element::RareData const* Element::element_rare_data() const
{
    return static_cast<RareData const*>(rare_data());
}

Element::PseudoElementData* Element::pseudo_element_data()
{
    auto* rare_data = element_rare_data();
    return rare_data ? rare_data->pseudo_element_data.ptr() : nullptr;
}

Element::PseudoElementData const* Element::pseudo_element_data() const
{
    auto const* rare_data = element_rare_data();
    return rare_data ? rare_data->pseudo_element_data.ptr() : nullptr;
}

GC_DEFINE_ALLOCATOR(Element);

static void invalidate_content_blocker_style_if_needed(Element& element)
{
    if (!element.is_connected())
        return;
    if (!ContentBlocker::the().filtering_enabled() || !ContentBlocker::the().has_cosmetic_rules())
        return;

    auto const& id = element.id();
    if (!element.document().content_blocker_style_sheet_may_need_refresh_for_class_or_id(id.has_value() ? &id.value() : nullptr, element.class_names()))
        return;

    element.document().page().invalidate_user_style();
}

static void for_each_ascii_whitespace_separated_token(Utf16View input, Function<IterationDecision(Utf16View)> const& callback)
{
    size_t start = 0;
    for (size_t i = 0; i <= input.length_in_code_units(); ++i) {
        if (i != input.length_in_code_units() && !Infra::is_ascii_whitespace(input.code_unit_at(i)))
            continue;

        if (i > start && callback(input.substring_view(start, i - start)) == IterationDecision::Break)
            return;
        start = i + 1;
    }
}

Element::Element(Document& document, DOM::QualifiedName qualified_name)
    : ParentNode(document, NodeType::ELEMENT_NODE)
    , m_qualified_name(move(qualified_name))
{
}

Element::~Element() = default;

Element::AttributeList& Element::ensure_attribute_list()
{
    if (!m_attributes)
        m_attributes = make<AttributeList>();
    return *m_attributes;
}

void Element::synchronize_attribute(Utf16FlyString const& qualified_name) const
{
    if (m_style_attribute_is_dirty && qualified_name == HTML::AttributeNames::style)
        synchronize_style_attribute();
}

void Element::synchronize_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& local_name) const
{
    if (m_style_attribute_is_dirty && !namespace_.has_value() && local_name == HTML::AttributeNames::style)
        synchronize_style_attribute();
}

void Element::synchronize_all_attributes() const
{
    synchronize_style_attribute();
}

Optional<size_t> Element::find_attribute_index(Utf16FlyString const& qualified_name) const
{
    Utf16FlyString const* effective_name = &qualified_name;
    Utf16FlyString lowercase_name;
    if (namespace_uri() == Namespace::HTML && document().is_html_document()) {
        lowercase_name = qualified_name.to_ascii_lowercase();
        effective_name = &lowercase_name;
    }

    synchronize_attribute(*effective_name);

    if (!m_attributes)
        return {};

    for (size_t index = 0; index < m_attributes->size(); ++index) {
        if (m_attributes->at(index).name.as_string() == *effective_name)
            return index;
    }
    return {};
}

Optional<size_t> Element::find_attribute_index_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& local_name) const
{
    Optional<Utf16FlyString> normalized_namespace;
    if (namespace_ != Utf16FlyString {})
        normalized_namespace = namespace_;

    synchronize_attribute_ns(normalized_namespace, local_name);

    if (!m_attributes)
        return {};

    for (size_t index = 0; index < m_attributes->size(); ++index) {
        auto const& attribute = m_attributes->at(index);
        if (attribute.name.namespace_() == normalized_namespace && attribute.name.local_name() == local_name)
            return index;
    }
    return {};
}

void Element::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    Animatable::visit_edges(visitor);

    visitor.visit(m_inline_style);
    visitor.visit(m_shadow_root);
}

size_t Element::external_memory_size() const
{
    auto size = Base::external_memory_size();
    if (!m_attributes)
        return size;

    size = JS::saturating_add_external_memory_size(size, sizeof(AttributeList));
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(*m_attributes));
    for (auto const& attribute : *m_attributes)
        size = JS::saturating_add_external_memory_size(size, JS::utf16_string_external_memory_size(attribute.value));
    return size;
}

// https://dom.spec.whatwg.org/#dom-element-getattribute
Optional<Utf16String> Element::get_attribute(Utf16FlyString const& name) const
{
    auto index = find_attribute_index(name);
    return index.has_value() ? Optional<Utf16String> { m_attributes->at(*index).value } : Optional<Utf16String> {};
}

// https://dom.spec.whatwg.org/#dom-element-getattributens
Optional<Utf16String> Element::get_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& name) const
{
    // 1. Let attr be the result of getting an attribute given namespace, localName, and this.
    auto index = find_attribute_index_ns(namespace_, name);

    // 2. If attr is null, return null.
    if (!index.has_value())
        return {};

    // 3. Return attr’s value.
    return m_attributes->at(*index).value;
}

// https://dom.spec.whatwg.org/#concept-element-attributes-get-value
Utf16String Element::get_attribute_value(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_) const
{
    // 1. Let attr be the result of getting an attribute given namespace, localName, and element.
    auto index = find_attribute_index_ns(namespace_, local_name);

    // 2. If attr is null, then return the empty string.
    if (!index.has_value())
        return {};

    // 3. Return attr’s value.
    return m_attributes->at(*index).value;
}

// https://html.spec.whatwg.org/multipage/semantics.html#get-an-element's-target
Utf16String Element::get_an_elements_target(Optional<Utf16String> target) const
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
    if (target.has_value() && target->contains(u"\t\n\r"sv) && target->contains('<'))
        target = "_blank"_utf16;

    // 3. Return target.
    if (target.has_value())
        return target.release_value();
    return {};
}

// https://html.spec.whatwg.org/multipage/links.html#get-an-element's-noopener
HTML::TokenizedFeature::NoOpener Element::get_an_elements_noopener(URL::URL const& url, Utf16View target) const
{
    // To get an element's noopener, given an a, area, or form element element, a URL record url, and a string target,
    // perform the following steps. They return a boolean.
    auto link_types = attribute(HTML::AttributeNames::rel).value_or({});
    auto has_link_type = [&](Utf16View link_type) {
        size_t start = 0;
        for (size_t i = 0; i <= link_types.length_in_code_units(); ++i) {
            if (i != link_types.length_in_code_units() && !Infra::is_ascii_whitespace(link_types.code_unit_at(i)))
                continue;

            if (i > start && link_types.substring_view(start, i - start).equals_ignoring_ascii_case(link_type))
                return true;
            start = i + 1;
        }
        return false;
    };

    // 1. If element's link types include the noopener or noreferrer keyword, then return true.
    if (has_link_type(u"noopener"sv) || has_link_type(u"noreferrer"sv))
        return HTML::TokenizedFeature::NoOpener::Yes;

    // 2. If element's link types do not include the opener keyword and
    //    target is an ASCII case-insensitive match for "_blank", then return true.
    if (!has_link_type(u"opener"sv) && target.equals_ignoring_ascii_case(u"_blank"sv))
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

// https://html.spec.whatwg.org/multipage/links.html#links-created-by-a-and-area-elements
HTML::HTMLHyperlinkElementUtils const* Element::created_hyperlink() const
{
    HTML::HTMLHyperlinkElementUtils const* hyperlink_element = as_if<HTML::HTMLAnchorElement>(*this);
    if (!hyperlink_element)
        hyperlink_element = as_if<HTML::HTMLAreaElement>(*this);
    if (!hyperlink_element)
        return nullptr;

    // NB: The href attribute on a and area elements is not required; when those elements do not have href attributes they
    // do not create hyperlinks.
    if (!has_attribute(HTML::AttributeNames::href))
        return nullptr;
    return hyperlink_element;
}

bool Element::creates_a_hyperlink() const
{
    return created_hyperlink() != nullptr;
}

// https://html.spec.whatwg.org/multipage/links.html#following-hyperlinks-2
void Element::follow_the_hyperlink(Optional<Utf16String> hyperlink_suffix, HTML::UserNavigationInvolvement user_involvement)
{
    // 1. If subject cannot navigate, then return.
    if (cannot_navigate())
        return;

    // 2. Let targetAttributeValue be the empty string.
    Utf16String target_attribute_value;

    // 3. If subject is an a or area element, then set targetAttributeValue to the result of getting an element's target given subject.
    if (is_html_anchor_element() || is_html_area_element() || is_svg_a_element())
        target_attribute_value = get_an_elements_target();

    // 4. Let urlRecord be the result of encoding-parsing a URL given subject's href attribute value, relative to subject's node document.
    auto url_record = document().encoding_parse_url(attribute(HTML::AttributeNames::href).value_or({}));

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
    Optional<Utf16String> url_string_with_suffix;
    if (hyperlink_suffix.has_value()) {
        Utf16StringBuilder url_string_builder;
        url_string_builder.append_ascii(url_string.bytes_as_string_view());
        url_string_builder.append(hyperlink_suffix->utf16_view());
        url_string_with_suffix = url_string_builder.to_string();
    }

    // 11. Let referrerPolicy be the current state of subject's referrerpolicy content attribute.
    auto referrer_policy_attribute = attribute(HTML::AttributeNames::referrerpolicy);
    auto referrer_policy = ReferrerPolicy::from_string(referrer_policy_attribute.has_value() ? referrer_policy_attribute->utf16_view() : u""sv).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);

    // FIXME: 12. If subject's link types includes the noreferrer keyword, then set referrerPolicy to "no-referrer".

    // 13. Navigate targetNavigable to urlString using subject's node document, with referrerPolicy set to referrerPolicy and userInvolvement set to userInvolvement.
    auto url = url_string_with_suffix.has_value()
        ? URL::Parser::basic_parse(url_string_with_suffix->utf16_view())
        : URL::Parser::basic_parse(url_string);
    VERIFY(url.has_value());
    MUST(target_navigable->navigate({ .url = url.release_value(), .source_document = document(), .referrer_policy = referrer_policy, .user_involvement = user_involvement }));
}

// https://html.spec.whatwg.org/multipage/links.html#downloading-hyperlinks
void Element::download_the_hyperlink(Optional<Utf16String> hyperlink_suffix, HTML::UserNavigationInvolvement user_involvement)
{
    // 1. If subject cannot navigate, then return.
    if (cannot_navigate())
        return;

    // 2. If subject's node document's active sandboxing flag set has the sandboxed downloads browsing context flag
    //    set, then return.
    if (has_flag(document().active_sandboxing_flag_set(), HTML::SandboxingFlagSet::SandboxedDownloads))
        return;

    // 3. Let urlString be the result of encoding-parsing-and-serializing a URL given subject's href attribute
    //    value, relative to subject's node document.
    auto url_record = document().encoding_parse_url(attribute(HTML::AttributeNames::href).value_or({}));

    // 4. If urlString is failure, then return.
    if (!url_record.has_value())
        return;

    auto url = url_record.release_value();

    // 5. If hyperlinkSuffix is non-null, then append it to urlString.
    if (hyperlink_suffix.has_value()) {
        auto url_string = url.serialize();
        Utf16StringBuilder url_string_builder;
        url_string_builder.append_ascii(url_string.bytes_as_string_view());
        url_string_builder.append(hyperlink_suffix->utf16_view());
        auto url_string_with_suffix = url_string_builder.to_string();
        auto url_with_suffix = URL::Parser::basic_parse(url_string_with_suffix.utf16_view());
        VERIFY(url_with_suffix.has_value());
        url = url_with_suffix.release_value();
    }

    // NB: The download attribute is read once here, before any script can run, so the proposed filename used when
    //     getting the suggested filename reflects its value at the time the download was initiated.
    auto download_attribute = attribute(HTML::AttributeNames::download);

    // 6. If userInvolvement is not "browser UI":
    if (user_involvement != HTML::UserNavigationInvolvement::BrowserUI) {
        // 1. Assert: subject has a download attribute.
        VERIFY(download_attribute.has_value());

        // 2. Let navigation be subject's relevant global object's navigation API.
        VERIFY(document().window());
        auto navigation = document().window()->navigation();

        // 3. Let filename be the value of subject's download attribute.
        auto filename = download_attribute.value();

        // 4. Let continue be the result of firing a download request navigate event at navigation with
        //    destinationURL set to urlString, userInvolvement set to userInvolvement, sourceElement set to
        //    subject, and filename set to filename.
        auto continue_ = navigation->fire_a_download_request_navigate_event(url, user_involvement, this, move(filename));

        // 5. If continue is false, then return.
        if (!continue_)
            return;

        // 6. Inform the navigation API about aborting navigation given subject's node navigable.
        if (auto navigable = document().navigable())
            navigable->inform_the_navigation_api_about_aborting_navigation();
    }

    // NB: Firing the navigate event above runs script, which may have detached this element's document from its
    //     navigable.
    auto navigable = document().navigable();
    if (!navigable)
        return;

    auto proposed_filename = download_attribute.map([](auto const& filename) {
        return filename.utf16_view().to_utf8_but_should_be_ported_to_utf16().to_byte_string();
    });

    // 7. Run these steps in parallel:
    //    1. Optionally, the user agent may abort these steps, if it believes doing so would safeguard the user
    //       from a potentially hostile download.
    //    2. Let request be a new request whose URL is urlString, client is entry settings object, initiator is
    //       "download", destination is the empty string, and whose synchronous flag and use-URL-credentials flag
    //       are set.
    // AD-HOC: The entry settings object may be empty when a hyperlink is activated by the user, so the node
    //         document's relevant settings object is used as the request client instead.
    auto request = Fetch::Infrastructure::Request::create();
    request->set_url(url);
    request->set_client(&document().relevant_settings_object());
    request->set_initiator(Fetch::Infrastructure::Request::Initiator::Download);
    request->set_use_url_credentials(true);
    // NB: The synchronous flag is not set; the response is instead processed asynchronously on the main thread.

    //    3. Let response be the result of fetching request.
    //    4. Handle as a download response with subject's node navigable and null.
    auto controller_holder = Fetch::Infrastructure::FetchControllerHolder::create();
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response = [navigable = GC::Ref { *navigable }, url, proposed_filename = move(proposed_filename), interface_origin = document().origin(), controller_holder](GC::Ref<Fetch::Infrastructure::Response> response) {
        if (response->is_network_error())
            return;
        navigable->handle_as_a_download(response->unsafe_response(), url, controller_holder->controller(), proposed_filename, interface_origin);
    };
    auto& relevant_realm = document().relevant_settings_object().realm();
    controller_holder->set_controller(Fetch::Fetching::fetch(relevant_realm, request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input))));
}

// https://html.spec.whatwg.org/multipage/links.html#links-created-by-a-and-area-elements
void Element::activate_the_hyperlink(Event const& event)
{
    // The activation behavior of an a or area element element given an event event is:

    // 1. If element has no href attribute, then return.
    // NB: SVG a elements may create a hyperlink using the deprecated xlink:href attribute.
    if (!has_attribute(HTML::AttributeNames::href) && !(is_svg_a_element() && has_attribute_ns(Namespace::XLink, HTML::AttributeNames::href)))
        return;

    auto const* mouse_event = as_if<UIEvents::MouseEvent>(event);

    // AD-HOC: Do not activate the element for clicks with the ctrl/cmd modifier present. This lets
    //         the browser process open the link in a new tab.
    if (mouse_event && mouse_event->platform_ctrl_key())
        return;

    // 2. Let hyperlinkSuffix be null.
    Optional<Utf16String> hyperlink_suffix {};

    // 3. If element is an a element, and event's target is an img with an ismap attribute specified:
    auto const* image_target = event.target() ? as_if<HTML::HTMLImageElement>(*event.target()) : nullptr;
    if (is_html_anchor_element() && image_target && image_target->has_attribute(HTML::AttributeNames::ismap)) {
        // 1. Let x and y be 0.
        CSSPixels x { 0 };
        CSSPixels y { 0 };

        // 2. If event's isTrusted attribute is initialized to true, then set x to the distance in CSS pixels from the left edge of the image
        //    to the location of the click, and set y to the distance in CSS pixels from the top edge of the image to the location of the click.
        if (event.is_trusted() && mouse_event) {
            x = CSSPixels { mouse_event->offset_x() };
            y = CSSPixels { mouse_event->offset_y() };
        }

        // 3. If x is negative, set x to 0.
        x = max(x, 0);

        // 4. If y is negative, set y to 0.
        y = max(y, 0);

        // 5. Set hyperlinkSuffix to the concatenation of U+003F (?), the value of x expressed as a base-ten integer using ASCII digits,
        //    U+002C (,), and the value of y expressed as a base-ten integer using ASCII digits.
        hyperlink_suffix = Utf16String::formatted("?{},{}", x.to_int(), y.to_int());
    }

    // 4. Let userInvolvement be event's user navigation involvement.
    auto user_involvement = HTML::user_navigation_involvement(event);

    // 5. If the user has expressed a preference to download the hyperlink, then set userInvolvement to "browser UI".
    // NOTE: That is, if the user has expressed a specific preference for downloading, this no longer counts as merely "activation".
    // NB: There is currently no way for the user to express a preference to download a hyperlink at activation
    //     time.

    // 6. If element has a download attribute, or if the user has expressed a preference to download the
    //    hyperlink, then download the hyperlink created by element with hyperlinkSuffix set to hyperlinkSuffix and
    //    userInvolvement set to userInvolvement.
    if (has_attribute(HTML::AttributeNames::download)) {
        download_the_hyperlink(hyperlink_suffix, user_involvement);
        return;
    }

    // 7. Otherwise, follow the hyperlink created by element with hyperlinkSuffix set to hyperlinkSuffix and userInvolvement set to userInvolvement.
    follow_the_hyperlink(hyperlink_suffix, user_involvement);
}

// https://dom.spec.whatwg.org/#dom-element-getattributenode
GC::Ptr<Attr> Element::get_attribute_node(Utf16FlyString const& name) const
{
    // The getAttributeNode(qualifiedName) method steps are to return the result of getting an attribute given qualifiedName and this.
    if (!find_attribute_index(name).has_value())
        return {};
    return const_cast<Element&>(*this).attributes()->get_attribute(name);
}

// https://dom.spec.whatwg.org/#dom-element-getattributenodens
GC::Ptr<Attr> Element::get_attribute_node_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& name) const
{
    // The getAttributeNodeNS(namespace, localName) method steps are to return the result of getting an attribute given namespace, localName, and this.
    if (!find_attribute_index_ns(namespace_, name).has_value())
        return {};
    return const_cast<Element&>(*this).attributes()->get_attribute_ns(namespace_, name);
}

void Element::set_attribute(FlyString qualified_name, Utf16String const& verified_value)
{
    auto utf16_qualified_name = Utf16FlyString::from_fly_string(qualified_name);
    // Let attribute be the first attribute in this’s attribute list whose qualified name is qualifiedName, and null otherwise.
    auto index = find_attribute_index(utf16_qualified_name);

    // If attribute is non-null, then change attribute to verifiedValue and return.
    if (index.has_value()) {
        auto& attribute = m_attributes->at(*index);
        auto old_value = move(attribute.value);
        attribute.value = verified_value;
        auto name = attribute.name;
        auto new_value = attribute.value;
        handle_attribute_changes(move(name), move(old_value), move(new_value));
        return;
    }

    // Set attribute to a new attribute whose local name is qualifiedName, value is verifiedValue,
    // and node document is this’s node document.
    append_attribute(QualifiedName { utf16_qualified_name, {}, {} }, verified_value);
}

// https://dom.spec.whatwg.org/#valid-namespace-prefix
bool is_valid_namespace_prefix(Utf16View prefix)
{
    // A string is a valid namespace prefix if its length is at least 1 and it does not contain ASCII whitespace, U+0000 NULL, U+002F (/), or U+003E (>).
    constexpr Array<u32, 8> INVALID_NAMESPACE_PREFIX_CHARACTERS { '\t', '\n', '\f', '\r', ' ', '\0', '/', '>' };
    return !prefix.is_empty() && !prefix.contains_any_of(INVALID_NAMESPACE_PREFIX_CHARACTERS);
}

// https://dom.spec.whatwg.org/#valid-attribute-local-name
bool is_valid_attribute_local_name(Utf16View local_name)
{
    // A string is a valid attribute local name if its length is at least 1 and it does not contain ASCII whitespace, U+0000 NULL, U+002F (/), U+003D (=), or U+003E (>).
    constexpr Array<u32, 9> INVALID_ATTRIBUTE_LOCAL_NAME_CHARACTERS { '\t', '\n', '\f', '\r', ' ', '\0', '/', '=', '>' };
    return !local_name.is_empty() && !local_name.contains_any_of(INVALID_ATTRIBUTE_LOCAL_NAME_CHARACTERS);
}

bool is_valid_element_local_name(Utf16View const& name)
{
    // 1. If name’s length is 0, then return false.
    if (name.is_empty())
        return false;

    // 2. If name’s 0th code point is an ASCII alpha, then:
    auto first_code_point = *name.begin();
    if (is_ascii_alpha(first_code_point)) {
        // 1. If name contains ASCII whitespace, U+0000 NULL, U+002F (/), or U+003E (>), then return false.
        for (auto code_point : name) {
            if (first_is_one_of(code_point, u'\t', u'\n', u'\f', u'\r', u' ', u'\0', u'/', u'>'))
                return false;
        }

        // 2. Return true.
        return true;
    }

    // 3. If name’s 0th code point is not U+003A (:), U+005F (_), or in the range U+0080 to U+10FFFF, inclusive, then return false.
    if (!first_is_one_of(first_code_point, 0x003Au, 0x005Fu) && (first_code_point < 0x0080 || first_code_point > 0x10FFFF))
        return false;

    // 4. If name’s subsequent code points, if any, are not ASCII alphas, ASCII digits, U+002D (-), U+002E (.), U+003A (:), U+005F (_), or in the range U+0080 to U+10FFFF, inclusive, then return false.
    for (auto code_point : name.unicode_substring_view(1)) {
        if (!is_ascii_alpha(code_point) && !is_ascii_digit(code_point) && !first_is_one_of(code_point, 0X002Du, 0X002Eu, 0X003Au, 0X005Fu) && (code_point < 0x0080 || code_point > 0x10FFFF))
            return false;
    }

    // 5. Return true.
    return true;
}

// https://dom.spec.whatwg.org/#validate-and-extract
ErrorOr<QualifiedName, ValidateAndExtractError> validate_and_extract(Optional<FlyString> namespace_, FlyString const& qualified_name, ValidationContext context)
{
    // To validate and extract a namespace and qualifiedName, run these steps:

    // 1. If namespace is the empty string, then set it to null.
    if (namespace_.has_value() && namespace_.value().is_empty())
        namespace_ = {};

    // 2. Let prefix be null.
    Optional<Utf16View> prefix_view = {};

    // 3. Let localName be qualifiedName.
    auto utf16_qualified_name = Utf16FlyString::from_fly_string(qualified_name);
    auto utf16_qualified_name_string = utf16_qualified_name.to_utf16_string();
    auto qualified_name_view = utf16_qualified_name_string.utf16_view();
    auto local_name_view = qualified_name_view;

    // 4. If qualifiedName contains a U+003A (:):
    auto colon_position = qualified_name_view.find_code_unit_offset(':');
    if (colon_position.has_value()) {
        // 1. Set prefix to the part of qualifiedName before the first U+003A (:).
        prefix_view = qualified_name_view.substring_view(0, *colon_position);

        // 2. Set localName to the part of qualifiedName after the first U+003A (:).
        local_name_view = qualified_name_view.substring_view(*colon_position + 1);

        // 3. If prefix is not a valid namespace prefix, then throw an "InvalidCharacterError" DOMException.
        if (!is_valid_namespace_prefix(*prefix_view))
            return ValidateAndExtractError::InvalidNamespacePrefix;
    }

    // 5. Assert: prefix is either null or a valid namespace prefix.
    ASSERT(!prefix_view.has_value() || is_valid_namespace_prefix(*prefix_view));

    // 6. If context is "attribute" and localName is not a valid attribute local name, then throw an "InvalidCharacterError" DOMException.
    if (context == ValidationContext::Attribute && !is_valid_attribute_local_name(local_name_view))
        return ValidateAndExtractError::InvalidAttributeLocalName;

    // 7. If context is "element" and localName is not a valid element local name, then throw an "InvalidCharacterError" DOMException.
    if (context == ValidationContext::Element && !is_valid_element_local_name(local_name_view))
        return ValidateAndExtractError::InvalidElementLocalName;

    // 8. If prefix is non-null and namespace is null, then throw a "NamespaceError" DOMException.
    if (prefix_view.has_value() && !namespace_.has_value())
        return ValidateAndExtractError::PrefixWithNullNamespace;

    // 9. If prefix is "xml" and namespace is not the XML namespace, then throw a "NamespaceError" DOMException.
    auto utf16_namespace = namespace_.map([](auto const& value) { return Utf16FlyString::from_fly_string(value); });
    if (prefix_view.has_value() && *prefix_view == u"xml"sv && utf16_namespace != Namespace::XML)
        return ValidateAndExtractError::XMLPrefixWithNonXMLNamespace;

    // 10. If either qualifiedName or prefix is "xmlns" and namespace is not the XMLNS namespace, then throw a "NamespaceError" DOMException.
    if ((qualified_name_view == u"xmlns"sv || (prefix_view.has_value() && *prefix_view == u"xmlns"sv)) && utf16_namespace != Namespace::XMLNS)
        return ValidateAndExtractError::XMLNSPrefixWithNonXMLNSNamespace;

    // 11. If namespace is the XMLNS namespace and neither qualifiedName nor prefix is "xmlns", then throw a "NamespaceError" DOMException.
    if (utf16_namespace == Namespace::XMLNS && !(qualified_name_view == u"xmlns"sv || (prefix_view.has_value() && *prefix_view == u"xmlns"sv)))
        return ValidateAndExtractError::XMLNSNamespaceWithoutXMLNSPrefix;

    // 12. Return (namespace, prefix, localName).
    auto local_name = colon_position.has_value() ? Utf16FlyString::from_utf16(local_name_view) : utf16_qualified_name;
    Optional<Utf16FlyString> prefix;
    if (prefix_view.has_value())
        prefix = Utf16FlyString::from_utf16(*prefix_view);
    return QualifiedName { local_name, prefix, utf16_namespace };
}

GC::Ref<WebIDL::DOMException> validate_and_extract_error_to_dom_exception(ValidateAndExtractError error)
{
    switch (error) {
    case ValidateAndExtractError::InvalidNamespacePrefix:
        return WebIDL::InvalidCharacterError::create("Prefix not a valid namespace prefix."_utf16);
    case ValidateAndExtractError::InvalidAttributeLocalName:
        return WebIDL::InvalidCharacterError::create("Local name not a valid attribute local name."_utf16);
    case ValidateAndExtractError::InvalidElementLocalName:
        return WebIDL::InvalidCharacterError::create("Local name not a valid element local name."_utf16);
    case ValidateAndExtractError::PrefixWithNullNamespace:
        return WebIDL::NamespaceError::create("Prefix is non-null and namespace is null."_utf16);
    case ValidateAndExtractError::XMLPrefixWithNonXMLNamespace:
        return WebIDL::NamespaceError::create("Prefix is 'xml' and namespace is not the XML namespace."_utf16);
    case ValidateAndExtractError::XMLNSPrefixWithNonXMLNSNamespace:
        return WebIDL::NamespaceError::create("Either qualifiedName or prefix is 'xmlns' and namespace is not the XMLNS namespace."_utf16);
    case ValidateAndExtractError::XMLNSNamespaceWithoutXMLNSPrefix:
        return WebIDL::NamespaceError::create("Namespace is the XMLNS namespace and neither qualifiedName nor prefix is 'xmlns'."_utf16);
    }
    VERIFY_NOT_REACHED();
}

void Element::set_attribute_ns(QualifiedName const& qualified_name, Utf16String const& verified_value)
{
    // Set an attribute value for this using localName, verifiedValue, and also prefix and namespace.
    set_attribute_value(qualified_name.local_name(), verified_value, qualified_name.prefix(), qualified_name.namespace_());
}

// https://dom.spec.whatwg.org/#concept-element-attributes-append
void Element::append_attribute(Attr& attribute)
{
    attributes()->append_attribute(GC::Ref { attribute });
}

void Element::append_attribute(QualifiedName name, Utf16String value)
{
    auto old_value = Optional<Utf16String> {};
    auto& attributes = ensure_attribute_list();
    attributes.empend(move(name), move(value));
    auto& attribute = attributes.last();
    auto attribute_name = attribute.name;
    auto new_value = attribute.value;
    handle_attribute_changes(move(attribute_name), move(old_value), move(new_value));
}

void Element::change_attribute_value(GC::Ref<Attr> attribute_node, Utf16String value)
{
    auto index = find_attribute_index_ns(attribute_node->namespace_uri(), attribute_node->local_name());
    VERIFY(index.has_value());
    auto& attribute = m_attributes->at(*index);
    auto old_value = move(attribute.value);
    attribute.value = move(value);
    auto name = attribute.name;
    auto new_value = attribute.value;
    handle_attribute_changes(move(name), move(old_value), move(new_value));
}

// https://dom.spec.whatwg.org/#handle-attribute-changes
void Element::handle_attribute_changes(QualifiedName name, Optional<Utf16String> old_value, Optional<Utf16String> new_value)
{
    // NB: Mutations during a recorded editing command must go through the Editing proxy functions.
    if (auto history = document().editing_history_if_exists())
        history->notify_dom_mutation();

    // 1. Queue a mutation record of "attributes" for element with attribute’s local name, attribute’s namespace, oldValue, « », « », null, and null.
    queue_mutation_record(MutationType::attributes, name.local_name(), name.namespace_(), old_value, {}, {}, nullptr, nullptr);

    // 2. If element is custom, then enqueue a custom element callback reaction with element, callback name "attributeChangedCallback",
    //    and « attribute’s local name, oldValue, newValue, attribute’s namespace ».
    if (is_custom())
        enqueue_an_attribute_changed_callback_reaction(name.local_name(), old_value, new_value, name.namespace_());

    // 3. Run the attribute change steps with element, attribute’s local name, oldValue, newValue, and attribute’s namespace.
    run_attribute_change_steps(name.local_name(), old_value, new_value, name.namespace_());
}

// https://dom.spec.whatwg.org/#concept-element-attributes-set-value
void Element::set_attribute_value(Utf16FlyString const& local_name, Utf16View value, Optional<Utf16FlyString> const& prefix, Optional<Utf16FlyString> const& namespace_)
{
    set_attribute_value(local_name, Utf16String::from_utf16(value), prefix, namespace_);
}

void Element::set_attribute_value(Utf16FlyString const& local_name, Utf16String value, Optional<Utf16FlyString> const& prefix, Optional<Utf16FlyString> const& namespace_)
{
    // 1. Let attribute be the result of getting an attribute given namespace, localName, and element.
    auto index = find_attribute_index_ns(namespace_, local_name);

    // 2. If attribute is null, create an attribute whose namespace is namespace, namespace prefix is prefix, local name
    //    is localName, value is value, and node document is element’s node document, then append this attribute to element,
    //    and then return.
    if (!index.has_value()) {
        QualifiedName name { local_name, prefix, namespace_ };

        append_attribute(move(name), move(value));

        return;
    }

    // 3. Change attribute to value.
    auto& attribute = m_attributes->at(*index);
    auto old_value = move(attribute.value);
    attribute.value = move(value);
    auto name = attribute.name;
    auto new_value = attribute.value;
    handle_attribute_changes(move(name), move(old_value), move(new_value));
}

// https://dom.spec.whatwg.org/#dom-element-setattributenode
WebIDL::ExceptionOr<GC::Ptr<Attr>> Element::set_attribute_node(Attr& attr)
{
    // The setAttributeNode(attr) and setAttributeNodeNS(attr) methods steps are to return the result of setting an attribute given attr and this.
    return attributes()->set_attribute(GC::Ref { attr });
}

// https://dom.spec.whatwg.org/#dom-element-setattributenodens
WebIDL::ExceptionOr<GC::Ptr<Attr>> Element::set_attribute_node_ns(Attr& attr)
{
    // The setAttributeNode(attr) and setAttributeNodeNS(attr) methods steps are to return the result of setting an attribute given attr and this.
    return attributes()->set_attribute(GC::Ref { attr });
}

// https://dom.spec.whatwg.org/#dom-element-removeattribute
void Element::remove_attribute(Utf16FlyString const& name)
{
    // The removeAttribute(qualifiedName) method steps are to remove an attribute given qualifiedName and this, and then return undefined.
    Utf16FlyString const* effective_name = &name;
    Utf16FlyString lowercase_name;
    if (namespace_uri() == Namespace::HTML && document().is_html_document()) {
        lowercase_name = name.to_ascii_lowercase();
        effective_name = &lowercase_name;
    }

    if (auto index = find_attribute_index(*effective_name); index.has_value())
        remove_attribute_at(*index);
}

// https://dom.spec.whatwg.org/#dom-element-removeattributens
void Element::remove_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& name)
{
    // The removeAttributeNS(namespace, localName) method steps are to remove an attribute given namespace, localName, and this, and then return undefined.
    if (auto index = find_attribute_index_ns(namespace_, name); index.has_value())
        remove_attribute_at(*index);
}

void Element::remove_attribute_at(size_t index)
{
    auto name = m_attributes->at(index).name;
    auto old_value = m_attributes->at(index).value;
    if (auto* rare_data = element_rare_data(); rare_data && rare_data->attribute_map)
        rare_data->attribute_map->detach_attribute_node(name, old_value);
    m_attributes->remove(index);
    handle_attribute_changes(name, old_value, {});
}

// https://dom.spec.whatwg.org/#dom-element-removeattributenode
WebIDL::ExceptionOr<GC::Ref<Attr>> Element::remove_attribute_node(GC::Ref<Attr> attr)
{
    return attributes()->remove_attribute_node(attr);
}

// https://dom.spec.whatwg.org/#dom-element-hasattribute
bool Element::has_attribute(Utf16FlyString const& name) const
{
    return attribute(name).has_value();
}

// https://dom.spec.whatwg.org/#dom-element-hasattributens
bool Element::has_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& name) const
{
    // 1. If namespace is the empty string, then set it to null.
    // 2. Return true if this has an attribute whose namespace is namespace and local name is localName; otherwise false.
    if (namespace_ == Utf16FlyString {})
        return find_attribute_index_ns(Optional<Utf16FlyString> {}, name).has_value();

    return find_attribute_index_ns(namespace_, name).has_value();
}

// https://dom.spec.whatwg.org/#dom-element-toggleattribute
WebIDL::ExceptionOr<bool> Element::toggle_attribute(Utf16FlyString const& name, Optional<bool> force)
{
    // 1. If qualifiedName is not a valid attribute local name, then throw an "InvalidCharacterError" DOMException.
    if (!is_valid_attribute_local_name(name))
        return WebIDL::InvalidCharacterError::create("Attribute name must not be empty or contain invalid characters"_utf16);

    // 2. If this is in the HTML namespace and its node document is an HTML document, then set qualifiedName to qualifiedName in ASCII lowercase.
    auto effective_name = name;
    if (namespace_uri() == Namespace::HTML && document().document_type() == Document::Type::HTML)
        effective_name = name.to_ascii_lowercase();

    // 3. Let attribute be the first attribute in this’s attribute list whose qualified name is qualifiedName, and null otherwise.
    auto index = find_attribute_index(effective_name);

    // 4. If attribute is null, then:
    if (!index.has_value()) {
        // 1. If force is not given or is true, create an attribute whose local name is qualifiedName, value is the empty
        //    string, and node document is this’s node document, then append this attribute to this, and then return true.
        if (!force.has_value() || force.value()) {
            append_attribute(QualifiedName { effective_name, {}, {} }, {});

            return true;
        }

        // 2. Return false.
        return false;
    }

    // 5. Otherwise, if force is not given or is false, remove an attribute given qualifiedName and this, and then return false.
    if (!force.has_value() || !force.value()) {
        remove_attribute_at(*index);
        return false;
    }

    // 6. Return true.
    return true;
}

// https://dom.spec.whatwg.org/#dom-element-getattributenames
Vector<Utf16FlyString> Element::get_attribute_names() const
{
    // The getAttributeNames() method steps are to return the qualified names of the attributes in this’s attribute list, in order; otherwise a new list.
    synchronize_all_attributes();
    if (!m_attributes)
        return {};
    Vector<Utf16FlyString> names;
    for (auto const& attribute : *m_attributes)
        names.append(attribute.name.as_string());
    return names;
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#attr-associated-element
GC::Ptr<DOM::Element> Element::get_the_attribute_associated_element(Utf16FlyString const& content_attribute, GC::Ptr<DOM::Element> explicitly_set_attribute_element) const
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
        return element.document().get_element_by_id(content_attribute_value->utf16_view());

    // 5. If no such element exists, then return null.
    // 6. Return null.
    return {};
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#attr-associated-elements
Optional<GC::RootVector<GC::Ref<DOM::Element>>> Element::get_the_attribute_associated_elements(Utf16FlyString const& content_attribute, Optional<Vector<GC::Weak<DOM::Element>> const&> explicitly_set_attribute_elements) const
{
    // 1. Let elements be an empty list.
    GC::RootVector<GC::Ref<DOM::Element>> elements;

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
        for_each_ascii_whitespace_separated_token(content_attribute_value->utf16_view(), [&](auto id) {
            // 4. For each id of tokens:
            // 1. Let candidate be the first element, in tree order, that meets the following criteria:
            //     * candidate's root is the same as element's root;
            //     * candidate's ID is id; and
            //     * candidate implements T.
            auto candidate = element.document().get_element_by_id(id);

            // 2. If no such element exists, then continue.
            if (!candidate)
                return IterationDecision::Continue;

            // 3. Append candidate to elements.
            elements.append(*candidate);
            return IterationDecision::Continue;
        });
    }

    // 5. Return elements.
    return elements;
}

RefPtr<Layout::Node> Element::create_layout_node(CSS::LayoutStyle style)
{
    if (local_name() == u"noscript"sv && document().is_scripting_enabled())
        return nullptr;

    auto computed_style = this->computed_style();
    VERIFY(computed_style);
    auto display = computed_style->display();
    return create_layout_node_for_display_type(document(), display, style, this);
}

RefPtr<Layout::NodeWithStyle> Element::create_layout_node_for_display_type(DOM::Document& document, CSS::Display const& display, CSS::LayoutStyle style, Element* element)
{
    if (display.is_none())
        return {};

    if (display.is_contents())
        return {};

    if (display.is_table_inside() || display.is_table_row_group() || display.is_table_header_group() || display.is_table_footer_group() || display.is_table_row())
        return make_ref_counted<Layout::Box>(document, element, style);

    if (display.is_list_item())
        return make_ref_counted<Layout::BlockContainer>(document, element, style, Layout::RustFFI::NodeKind::ListItemBox);

    if (display.is_table_cell())
        return make_ref_counted<Layout::BlockContainer>(document, element, style);

    if (display.is_table_column() || display.is_table_column_group() || display.is_table_caption()) {
        // FIXME: This is just an incorrect placeholder until we improve table layout support.
        return make_ref_counted<Layout::BlockContainer>(document, element, style);
    }

    if (display.is_math_inside()) {
        // https://w3c.github.io/mathml-core/#new-display-math-value
        // MathML elements with a computed display value equal to block math or inline math control box generation
        // and layout according to their tag name, as described in the relevant sections.
        // FIXME: Figure out what kind of node we should make for them. For now, we'll stick with a generic Box.
        return make_ref_counted<Layout::BlockContainer>(document, element, style);
    }

    if (display.is_inline_outside()) {
        if (display.is_flow_root_inside())
            return make_ref_counted<Layout::BlockContainer>(document, element, style);
        if (display.is_flow_inside())
            return make_ref_counted<Layout::NodeWithStyle>(document, element, style, Layout::RustFFI::NodeKind::InlineNode);
        if (display.is_flex_inside())
            return make_ref_counted<Layout::Box>(document, element, style);
        if (display.is_grid_inside())
            return make_ref_counted<Layout::Box>(document, element, style);
        dbgln_if(LIBWEB_CSS_DEBUG, "FIXME: Support display: {}", display.to_string());
        return make_ref_counted<Layout::NodeWithStyle>(document, element, style, Layout::RustFFI::NodeKind::InlineNode);
    }

    if (display.is_flex_inside() || display.is_grid_inside())
        return make_ref_counted<Layout::Box>(document, element, style);

    if (display.is_flow_inside() || display.is_flow_root_inside())
        return make_ref_counted<Layout::BlockContainer>(document, element, style);

    dbgln("FIXME: CSS display '{}' not implemented yet.", display.to_string());

    // FIXME: We don't actually support `display: block ruby`, this is just a hack to prevent a crash
    if (display.is_ruby_inside())
        return make_ref_counted<Layout::BlockContainer>(document, element, style);

    return make_ref_counted<Layout::NodeWithStyle>(document, element, style, Layout::RustFFI::NodeKind::InlineNode);
}

void Element::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    // https://html.spec.whatwg.org/multipage/rendering.html#the-page
    // When a body element has a link attribute, its value is expected to be parsed using the rules for parsing a legacy
    // color value, and if that does not return failure, the user agent is expected to treat the attribute as a
    // presentational hint setting the 'color' property of any element in the Document matching the :link pseudo-class
    // to the resulting color.
    if (matches_link_pseudo_class()) {
        if (auto const& link_color = document().normal_link_color(); link_color.has_value())
            properties.append({ .property_id = CSS::PropertyID::Color, .value = CSS::ColorStyleValue::create_from_color(*link_color, CSS::ColorSyntax::Legacy) });
    }

    // When a body element has a vlink attribute, its value is expected to be parsed using the rules for parsing a
    // legacy color value, and if that does not return failure, the user agent is expected to treat the attribute as a
    // presentational hint setting the 'color' property of any element in the Document matching the :visited
    // pseudo-class to the resulting color.
    if (matches_visited_pseudo_class()) {
        if (auto const& visited_link_color = document().visited_link_color(); visited_link_color.has_value())
            properties.append({ .property_id = CSS::PropertyID::Color, .value = CSS::ColorStyleValue::create_from_color(*visited_link_color, CSS::ColorSyntax::Legacy) });
    }

    // When a body element has an alink attribute, its value is expected to be parsed using the rules for parsing a
    // legacy color value, and if that does not return failure, the user agent is expected to treat the attribute as a
    // presentational hint setting the 'color' property of any element in the Document matching the :active pseudo-class
    // and either the :link pseudo-class or the :visited pseudo-class to the resulting color.
    if (is_being_activated() && (matches_link_pseudo_class() || matches_visited_pseudo_class())) {
        if (auto const& active_link_color = document().active_link_color(); active_link_color.has_value())
            properties.append({ .property_id = CSS::PropertyID::Color, .value = CSS::ColorStyleValue::create_from_color(*active_link_color, CSS::ColorSyntax::Legacy) });
    }
}

bool Element::presentational_hint_properties_need_publication(ReadonlySpan<CSS::StyleProperty> properties) const
{
    if (m_published_presentational_hint_properties.size() == properties.size()) {
        bool properties_are_unchanged = true;
        for (size_t index = 0; index < properties.size(); ++index)
            properties_are_unchanged &= m_published_presentational_hint_properties[index] == properties[index];
        if (properties_are_unchanged)
            return false;
    }

    return true;
}

void Element::did_publish_presentational_hint_properties(ReadonlySpan<CSS::StyleProperty> properties)
{
    m_published_presentational_hint_properties.clear();
    m_published_presentational_hint_properties.ensure_capacity(properties.size());
    for (auto const& property : properties)
        m_published_presentational_hint_properties.unchecked_append(property);
}

void Element::run_attribute_change_steps(Utf16FlyString const& local_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    attribute_changed(local_name, old_value, value, namespace_);

    // Published here rather than inside attribute_changed, because that function returns early for
    // the names whose change steps are specified elsewhere - and one of them is `style`, whose
    // early return is taken by exactly the CSSOM writes that change it. An attribute a selector
    // tests has to be heard about however it was written.
    if (old_value != value)
        CSS::record_element_attribute_changed(*this, local_name, namespace_, old_value, value);

    if (old_value != value) {
        if (local_name.is_one_of(HTML::AttributeNames::colspan, HTML::AttributeNames::rowspan, HTML::AttributeNames::span)) {
            if (auto* layout_node = unsafe_layout_node()) {
                if (layout_node->synchronize_table_span_data())
                    layout_node->set_needs_layout_update(SetNeedsLayoutReason::TableSpanAttributeChange);
            }
        }
        if (!document().suppresses_attribute_style_invalidation()) {
            CSS::Invalidation::invalidate_style_after_attribute_change(
                *this,
                local_name,
                old_value,
                value);
            if (local_name == HTML::AttributeNames::id || local_name == HTML::AttributeNames::class_)
                invalidate_content_blocker_style_if_needed(*this);
        }
        auto html_collection_invalidation_types = document().html_collection_attribute_invalidation_types_for_attribute(local_name, namespace_);
        if (html_collection_invalidation_types != 0)
            invalidate_html_collection_caches_in_ancestors_for_attribute_change(html_collection_invalidation_types);
        document().bump_dom_tree_version();
    }
}

static CSS::StyleComputer::ComputedStyleInvalidation compute_required_invalidation_between_computed_values(CSS::ComputedValues const& old_computed_values, CSS::ComputedValues const& new_computed_values, bool element_folds_transform_into_layout = false)
{
    CSS::StyleComputer::ComputedStyleInvalidation result;

    // Shortcut the per-longhand diff below when every style value group payload of the new style
    // is (or has just been made) shared with the old style: equal group payloads mean equal
    // computed values for every longhand. Animated values live outside the groups, so the
    // shortcut only applies when neither side has any.
    // NB: The adoption also makes an unchanged element keep sharing group storage with its
    //     previous style generation, which future diffs turn into pure pointer compares.
    bool const all_group_payloads_shared = new_computed_values.adopt_identical_group_payloads(old_computed_values);
    // The computed longhand table, resolved font list and inheritance-dependent specified values
    // live outside the group payloads, so equal payloads alone cannot prove the diff away. When
    // all longhands are equal, adopting identical group payloads also adopts the previous table.
    bool const property_diff_can_be_skipped = all_group_payloads_shared
        && old_computed_values.computed_longhand_values().data() == new_computed_values.computed_longhand_values().data()
        && old_computed_values.font_list().equals(new_computed_values.font_list())
        && !CSS::ComputedValues::either_carries_animated_overlay(old_computed_values, new_computed_values)
        && old_computed_values.inheritance_dependent_specified_values_equal(new_computed_values);
    static bool const verify_fast_path = getenv("LIBWEB_VERIFY_STYLE_DIFF_FAST_PATH") != nullptr;

    if (!property_diff_can_be_skipped || verify_fast_path) {
        if (!old_computed_values.font_list().equals(new_computed_values.font_list())) {
            result.any_computed_value_changed = true;
            result.invalidation.ensure_at_least(CSS::InvalidationLevel::Relayout);
        }

        constexpr auto longhand_count = CSS::number_of_longhand_properties;
        constexpr auto longhand_bitmap_bytes = (longhand_count + 7) / 8;
        Array<u16, longhand_count> old_physical_properties;
        Array<u16, longhand_count> new_physical_properties;
        for (size_t index = 0; index < longhand_count; ++index) {
            auto property_id = static_cast<CSS::PropertyID>(to_underlying(CSS::first_longhand_property_id) + index);
            auto old_physical_property_id = property_id;
            auto new_physical_property_id = property_id;
            if (CSS::property_is_logical_alias(property_id)) {
                old_physical_property_id = CSS::map_logical_alias_to_physical_property(property_id, CSS::LogicalAliasMappingContext { old_computed_values.writing_mode(), old_computed_values.direction() });
                new_physical_property_id = CSS::map_logical_alias_to_physical_property(property_id, CSS::LogicalAliasMappingContext { new_computed_values.writing_mode(), new_computed_values.direction() });
            }
            old_physical_properties[index] = to_underlying(old_physical_property_id);
            new_physical_properties[index] = to_underlying(new_physical_property_id);
        }

        auto old_longhands = old_computed_values.computed_longhand_values();
        auto new_longhands = new_computed_values.computed_longhand_values();
        VERIFY(old_longhands.size() == longhand_count);
        VERIFY(new_longhands.size() == longhand_count);
        auto old_importance = old_computed_values.property_importance_bitmap();
        auto new_importance = new_computed_values.property_importance_bitmap();
        Array<u8, longhand_bitmap_bytes> changed_properties;
        auto const* old_animated_properties = old_computed_values.animated_properties();
        auto const* new_animated_properties = new_computed_values.animated_properties();
        VERIFY(CSS::StyleValueFFI::rust_style_value_diff_effective_longhands(
            old_longhands.data(),
            new_longhands.data(),
            old_physical_properties.data(),
            new_physical_properties.data(),
            longhand_count,
            old_animated_properties ? old_animated_properties->overlay() : nullptr,
            new_animated_properties ? new_animated_properties->overlay() : nullptr,
            old_importance.data(),
            new_importance.data(),
            old_importance.size(),
            changed_properties.data(),
            changed_properties.size()));

        for (size_t index = 0; index < longhand_count; ++index) {
            if (!(changed_properties[index / 8] & (1 << (index % 8))))
                continue;
            auto property_id = static_cast<CSS::PropertyID>(to_underlying(CSS::first_longhand_property_id) + index);
            auto new_physical_property_id = static_cast<CSS::PropertyID>(new_physical_properties[index]);
            result.any_computed_value_changed = true;
            if (CSS::is_inherited_property(property_id)) {
                // Equal groups adopted the old payload above, so a changed value names a group
                // whose post-adoption identity changed without a separate fixed-size group scan.
                auto group = CSS::ComputedValues::style_group_of_property(new_physical_property_id);
                if (group.has_value() && to_underlying(*group) < CSS::ComputedValues::inherited_style_group_count)
                    result.invalidation.mark_inherited_style_group_changed(to_underlying(*group));
                else
                    result.invalidation.mark_all_inherited_style_groups_changed();
            }
            auto property_invalidation = CSS::compute_property_invalidation(property_id, old_computed_values, new_computed_values);
            // SVG layout folds element transforms into container bounding boxes, so a transform
            // change needs layout there even though it stays paint-only for CSS boxes.
            if (element_folds_transform_into_layout
                && AK::first_is_one_of(property_id, CSS::PropertyID::Transform, CSS::PropertyID::Translate, CSS::PropertyID::Rotate, CSS::PropertyID::Scale))
                property_invalidation.ensure_at_least(CSS::InvalidationLevel::Relayout);
            result.invalidation |= property_invalidation;
        }

        // The diff above compares the values that animations produce, so a change to a value that a running
        // animation overrides is invisible to it. Descendants inherit the overridden value as their base value,
        // and the after-change style rules start their transitions from it. Diff the longhands an animation
        // covers separately so the change still invalidates the styles of descendants that inherit the
        // property, whether by default or through an explicit `inherit`.
        if (old_animated_properties || new_animated_properties) {
            for (size_t index = 0; index < longhand_count; ++index) {
                if (changed_properties[index / 8] & (1 << (index % 8)))
                    continue;
                auto property_id = static_cast<CSS::PropertyID>(to_underlying(CSS::first_longhand_property_id) + index);
                bool const animation_covers_property = (old_animated_properties && old_animated_properties->has_property(property_id))
                    || (new_animated_properties && new_animated_properties->has_property(property_id));
                if (!animation_covers_property)
                    continue;
                auto old_index = old_physical_properties[index] - to_underlying(CSS::first_longhand_property_id);
                auto new_index = new_physical_properties[index] - to_underlying(CSS::first_longhand_property_id);
                auto const* old_value = static_cast<CSS::StyleValueFFI::StyleValueData const*>(old_longhands[old_index]);
                auto const* new_value = static_cast<CSS::StyleValueFFI::StyleValueData const*>(new_longhands[new_index]);
                if (old_value == new_value || CSS::StyleValueFFI::rust_style_value_equals(old_value, new_value))
                    continue;
                result.any_computed_value_changed = true;
                if (!CSS::is_inherited_property(property_id)) {
                    result.invalidation.non_inherited_property_inheritance_sources_changed = true;
                    continue;
                }
                auto group = CSS::ComputedValues::style_group_of_property(static_cast<CSS::PropertyID>(new_physical_properties[index]));
                if (group.has_value() && to_underlying(*group) < CSS::ComputedValues::inherited_style_group_count) {
                    result.invalidation.mark_inherited_style_group_changed(to_underlying(*group));
                } else {
                    result.invalidation.mark_all_inherited_style_groups_changed();
                }
            }
        }

        // With the verification mode enabled, the full diff must agree that a skippable style
        // change produces no invalidation at all.
        if (verify_fast_path && property_diff_can_be_skipped)
            VERIFY(result.invalidation.is_none());
    }

    return result;
}

struct ElementDependentInvalidationState {
    Layout::NodeWithStyle const* layout_node { nullptr };
    Optional<Vector<ValueComparingRefPtr<CSS::CounterStyle const>>> content_counter_style_dependencies;
    Optional<ValueComparingRefPtr<CSS::CounterStyle const>> list_counter_style;
    bool has_snapshot { false };

    void snapshot()
    {
        if (!layout_node)
            return;
        if (auto const& content = layout_node->content(); content.has_value())
            content_counter_style_dependencies = content->counter_style_dependencies;
        if (auto const& list_style_type = layout_node->list_style_type(); list_style_type.has<RefPtr<CSS::CounterStyle const>>())
            list_counter_style = list_style_type.get<RefPtr<CSS::CounterStyle const>>();
        layout_node = nullptr;
        has_snapshot = true;
    }
};

enum class ElementDependentInvalidationMode : u8 {
    Full,
    InheritedGroupSwap,
};

static void add_element_dependent_invalidation(CSS::RequiredInvalidationAfterStyleChange& invalidation, CSS::ComputedValues const& new_computed_values, ElementDependentInvalidationState const& old_state, DOM::AbstractElement& abstract_element)
{
    // NB: Even if the computed value hasn't changed the resolved counter style may have (e.g. if the relevant
    //     @counter-style rule was modified, or a new rule with the same name took precedence over the old one).
    // Generated content and the marker live inside the element's own layout subtree, so, like a
    // 'content' change, they rebuild from the element rather than its parent.
    auto compare = [&](Optional<Vector<ValueComparingRefPtr<CSS::CounterStyle const>>> const& old_content_dependencies, Optional<ValueComparingRefPtr<CSS::CounterStyle const>> const& old_list_counter_style) {
        if (old_content_dependencies.has_value()
            && *old_content_dependencies != new_computed_values.resolved_content(abstract_element, 0).content_data.counter_style_dependencies)
            invalidation |= CSS::RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(CSS::LayoutTreeRebuildRoot::Self);

        if (old_list_counter_style.has_value()) {
            auto new_list_style_type = new_computed_values.list_style_type(abstract_element.style_scope());
            if (new_list_style_type.has<RefPtr<CSS::CounterStyle const>>()) {
                ValueComparingRefPtr<CSS::CounterStyle const> new_counter_style = new_list_style_type.get<RefPtr<CSS::CounterStyle const>>();
                if (*old_list_counter_style != new_counter_style)
                    invalidation |= CSS::RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(CSS::LayoutTreeRebuildRoot::Self);
            }
        }
    };

    if (old_state.layout_node) {
        Optional<Vector<ValueComparingRefPtr<CSS::CounterStyle const>>> old_content_dependencies;
        if (auto const& content = old_state.layout_node->content(); content.has_value())
            old_content_dependencies = content->counter_style_dependencies;
        Optional<ValueComparingRefPtr<CSS::CounterStyle const>> old_list_counter_style;
        if (auto const& list_style_type = old_state.layout_node->list_style_type(); list_style_type.has<RefPtr<CSS::CounterStyle const>>())
            old_list_counter_style = list_style_type.get<RefPtr<CSS::CounterStyle const>>();
        compare(old_content_dependencies, old_list_counter_style);
    } else if (old_state.has_snapshot) {
        compare(old_state.content_counter_style_dependencies, old_state.list_counter_style);
    }
}

static bool style_record_is_unchanged(CSS::StyleEngine::StyleRecordDelta const& delta)
{
    return !!delta.old_style_record && delta.old_style_record == delta.new_style_record;
}

// SVG container layout unions each child's bounding box mapped by the child's own transform. An
// outermost <svg> is laid out by its CSS parent (as is one re-rooted by foreignObject), so its
// own transform stays paint-only like any CSS box.
static bool element_folds_transform_into_svg_container_layout(DOM::Element const& element)
{
    if (!is<SVG::SVGGraphicsElement>(element))
        return false;
    auto parent = element.parent_element();
    return parent && is<SVG::SVGElement>(*parent) && !is<SVG::SVGForeignObjectElement>(*parent);
}

static CSS::StyleComputer::ComputedStyleInvalidation compute_required_invalidation_with_cache(CSS::StyleComputer& style_computer, CSS::ComputedValues const& old_computed_values, CSS::ComputedValues const& new_computed_values, ElementDependentInvalidationState const& old_state, DOM::AbstractElement& abstract_element, CSS::StyleEngine::StyleRecordDelta const& style_record_delta, ElementDependentInvalidationMode mode = ElementDependentInvalidationMode::Full)
{
    CSS::StyleComputer::ComputedStyleInvalidation result;
    bool element_folds_transform_into_layout = element_folds_transform_into_svg_container_layout(abstract_element.element());
    if (style_record_is_unchanged(style_record_delta)) {
        ++abstract_element.document().style_invalidation_counters().style_record_property_diffs_skipped;
    } else if (auto cached = style_computer.cached_computed_style_invalidation(style_record_delta, element_folds_transform_into_layout); cached.has_value()) {
        ++abstract_element.document().style_invalidation_counters().style_record_property_damage_cache_hits;
        result = cached.release_value();
    } else {
        result = compute_required_invalidation_between_computed_values(old_computed_values, new_computed_values, element_folds_transform_into_layout);
        style_computer.cache_computed_style_invalidation(style_record_delta, element_folds_transform_into_layout, result);
    }

    // An SVG currentColor stroke stores its resolved color alongside the fact that it came from
    // currentColor. A color-only change can therefore alter the visible stroke width and the SVG
    // container bounds without changing the stroke longhand itself.
    if (is<SVG::SVGGraphicsElement>(abstract_element.element())
        && old_computed_values.color() != new_computed_values.color()) {
        auto stroke_uses_current_color = [](CSS::ComputedValues const& computed_values) {
            auto stroke = computed_values.stroke();
            return stroke.has_value() && stroke->color_is_currentcolor();
        };
        if (stroke_uses_current_color(old_computed_values) || stroke_uses_current_color(new_computed_values))
            result.invalidation.ensure_at_least(CSS::InvalidationLevel::Relayout);
    }

    // The table fixup algorithm needs an authored box's display from before box type
    // transformation. A flex or grid item can therefore keep the same blockified display while
    // changing whether it needs anonymous table wrappers. Generated pseudo-element boxes are
    // anonymous, so fixup uses their adjusted display instead.
    if (!abstract_element.pseudo_element().has_value()) {
        auto is_table_fixup_child = [](CSS::Display const& display) {
            return display.is_table_row_group()
                || display.is_table_header_group()
                || display.is_table_footer_group()
                || display.is_table_column_group()
                || display.is_table_caption();
        };
        auto old_display = old_computed_values.display_before_box_type_transformation();
        auto new_display = new_computed_values.display_before_box_type_transformation();
        if (is_table_fixup_child(old_display) != is_table_fixup_child(new_display)) {
            result.any_computed_value_changed = true;
            result.invalidation |= CSS::RequiredInvalidationAfterStyleChange::full();
        }
    }

    // An unchanged style record already names the same counter-style environment. A group swap
    // changes only inherited payloads under that environment. In either case, re-resolving through
    // a temporary record projection can manufacture a change even though the durable inputs did
    // not change.
    if (mode == ElementDependentInvalidationMode::Full && !style_record_is_unchanged(style_record_delta))
        add_element_dependent_invalidation(result.invalidation, new_computed_values, old_state, abstract_element);
    return result;
}

CSS::RequiredInvalidationAfterStyleChange Element::recompute_pseudo_element_styles(bool& did_change_custom_properties, bool had_list_marker, CSS::ComputedValues const* old_originating_style, CSS::StyleEngineMatchResult* reusable_matches, PreservedPseudoElementStyles* preserved_pseudo_element_styles)
{
    CSS::RequiredInvalidationAfterStyleChange invalidation;

    auto& style_computer = document().style_computer();
    auto originating_style = computed_style();
    CSS::StyleEngineMatchResult local_matches;
    if (!reusable_matches)
        reusable_matches = &local_matches;

    // Any document change that can cause this element's style to change, could also affect its pseudo-elements.
    auto recompute_pseudo_element_style = [&](CSS::PseudoElement pseudo_element, bool has_implicit_style = false) {
        auto pseudo_element_style = computed_style(pseudo_element);
        auto const* pseudo_element_values = pseudo_element_style ? &*pseudo_element_style : nullptr;
        if (!pseudo_element_values && preserved_pseudo_element_styles)
            pseudo_element_values = preserved_pseudo_element_styles->at(to_underlying(pseudo_element)).ptr();
        ElementDependentInvalidationState old_state {
            .layout_node = pseudo_element_unsafe_layout_node(pseudo_element),
            .content_counter_style_dependencies = {},
            .list_counter_style = {},
            .has_snapshot = false,
        };
        RefPtr<CSS::ComputedValues const> style_to_preserve_for_detachment;
        if (pseudo_element_values && pseudo_element_values->animated_properties()) {
            auto had_layout_node = !!old_state.layout_node;
            old_state.snapshot();
            if (preserved_pseudo_element_styles)
                style_to_preserve_for_detachment = preserved_pseudo_element_styles->at(to_underlying(pseudo_element));
            else if (had_layout_node)
                style_to_preserve_for_detachment = CSS::ComputedValues::Builder { *pseudo_element_values }.build();
            else if (pseudo_element_style)
                pseudo_element_style.retain_across_style_record_publication();
        }
        auto should_recompute = has_implicit_style
            || pseudo_element_values
            || (old_originating_style && old_originating_style->has_pseudo_element_style(pseudo_element))
            || (originating_style && originating_style->has_pseudo_element_style(pseudo_element));
        if (!should_recompute)
            return;

        CSS::StyleEngine::StyleRecordDelta style_record_delta {};
        auto new_pseudo_element_style = style_computer.compute_pseudo_element_style_if_needed({ *this, pseudo_element }, did_change_custom_properties, reusable_matches, style_record_delta);
        if (style_record_is_unchanged(style_record_delta))
            ++document().style_invalidation_counters().unchanged_style_record_deltas;

        // A non-inline generated box can split an inline originating element and mutate anonymous structure in its
        // parent. Inline ::before and ::after boxes remain confined to the originating element's layout subtree.
        auto pseudo_style_can_escape_originating_element = [&](CSS::ComputedValues const* pseudo_style) {
            if (!pseudo_style || !originating_style->display().is_inline_outside())
                return false;
            auto pseudo_display = pseudo_style->display();
            return !pseudo_display.is_none() && !pseudo_display.is_contents() && !pseudo_display.is_inline_outside();
        };

        // A marker box is always attached inside the originating box, as is a ::before or ::after
        // box that cannot escape it, so replacing that box in place creates, removes, or rebuilds
        // the pseudo-element box along with it.
        auto pseudo_box_stays_inside_originating_box = [&](CSS::ComputedValues const* new_style) {
            return pseudo_element == CSS::PseudoElement::Marker
                || (first_is_one_of(pseudo_element, CSS::PseudoElement::Before, CSS::PseudoElement::After)
                    && !pseudo_style_can_escape_originating_element(pseudo_element_values)
                    && !pseudo_style_can_escape_originating_element(new_style));
        };

        if (pseudo_element_values && new_pseudo_element_style) {
            DOM::AbstractElement abstract_element { *this, pseudo_element };
            auto result = compute_required_invalidation_with_cache(style_computer, *pseudo_element_values, *new_pseudo_element_style, old_state, abstract_element, style_record_delta);
            // A display: contents pseudo-element has no principal layout node to receive its updated style. A
            // list-item pseudo-element also owns a generated marker whose layout state is not updated through the
            // originating element. Rebuild their layout subtrees when a style change otherwise requires relayout.
            if (result.invalidation.needs_relayout()
                && !result.invalidation.needs_layout_tree_rebuild()
                && (pseudo_element_values->display().is_contents()
                    || pseudo_element_values->display().is_list_item()
                    || new_pseudo_element_style->display().is_contents()
                    || new_pseudo_element_style->display().is_list_item())) {
                result.invalidation.ensure_at_least(CSS::InvalidationLevel::RebuildLayoutTree);
            }
            if (result.invalidation.needs_layout_tree_rebuild()
                && result.invalidation.layout_tree_rebuild_root() != CSS::LayoutTreeRebuildRoot::Parent) {
                if (!pseudo_box_stays_inside_originating_box(new_pseudo_element_style.ptr()))
                    result.invalidation.ensure_at_least(CSS::InvalidationLevel::RebuildLayoutTree);
                else if (result.invalidation.layout_tree_rebuild_root() == CSS::LayoutTreeRebuildRoot::BoxPresenceChange)
                    result.invalidation.set_layout_tree_rebuild_root(CSS::LayoutTreeRebuildRoot::Self);
            }
            if (result.any_computed_value_changed)
                document().style_invalidation_counters().element_computed_style_changes++;
            invalidation |= result.invalidation;
        } else if (pseudo_element_values || new_pseudo_element_style) {
            document().style_invalidation_counters().element_computed_style_changes++;
            auto rebuild_root = pseudo_box_stays_inside_originating_box(new_pseudo_element_style.ptr())
                ? CSS::LayoutTreeRebuildRoot::Self
                : CSS::LayoutTreeRebuildRoot::Parent;
            invalidation |= CSS::RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(rebuild_root);
        }

        if (new_pseudo_element_style) {
            if (CSS::is_element_reference_pseudo_element(pseudo_element))
                refresh_computed_style(pseudo_element, style_record_delta.new_style_record);
            else
                set_computed_style(pseudo_element, style_record_delta.new_style_record);
        } else if (auto existing_pseudo_element = get_synthetic_pseudo_element(pseudo_element); existing_pseudo_element.has_value())
            existing_pseudo_element->clear_computed_style(move(style_to_preserve_for_detachment));
    };

    recompute_pseudo_element_style(CSS::PseudoElement::Before);
    recompute_pseudo_element_style(CSS::PseudoElement::After);
    recompute_pseudo_element_style(CSS::PseudoElement::FirstLetter);
    recompute_pseudo_element_style(CSS::PseudoElement::Selection);
    if (m_rendered_in_top_layer)
        recompute_pseudo_element_style(CSS::PseudoElement::Backdrop);
    if (had_list_marker || originating_style->display().is_list_item())
        recompute_pseudo_element_style(CSS::PseudoElement::Marker, true);
    for (auto i = to_underlying(CSS::first_element_reference_pseudo_element); i <= to_underlying(CSS::last_element_reference_pseudo_element); ++i) {
        auto pseudo_element = static_cast<CSS::PseudoElement>(i);
        if (get_pseudo_element(pseudo_element).has_value())
            recompute_pseudo_element_style(pseudo_element);
    }

    return invalidation;
}

CSS::RequiredInvalidationAfterStyleChange Element::recompute_pseudo_element_styles_after_animation_update(Badge<Web::Animations::AnimationUpdateContext>)
{
    auto computed_values = this->computed_style();
    VERIFY(computed_values);

    bool did_change_custom_properties = false;
    auto invalidation = recompute_pseudo_element_styles(did_change_custom_properties, computed_values->display().is_list_item(), nullptr);
    if (!invalidation.is_none())
        document().style_invalidation_counters().committed_style_observer_consequences++;
    apply_computed_pseudo_element_styles_to_layout_nodes_if_needed(invalidation);
    return invalidation;
}

void Element::set_needs_layout_tree_rebuild(SetNeedsLayoutTreeUpdateReason reason, CSS::LayoutTreeRebuildRoot rebuild_root)
{
    // A self-scoped style invalidation can replace the element's principal box in place. Anonymous-parent escalation
    // in Node::set_needs_layout_tree_update() still widens the rebuild when the old box participates in an outer
    // wrapper. Other style changes rebuild from the parent. Top layer elements are handled separately because their
    // boxes are siblings of the root.
    // NB: Called outside layout tree construction.
    auto* layout_node = unsafe_layout_node();
    // An element that just left the top layer keeps its box as a viewport child until the
    // pending membership change is processed, so the parent must not be rebuilt for it either.
    bool element_box_is_placed_in_top_layer = layout_node && layout_node->topmost_layout_node_of_top_layer_placement();
    if (rendered_in_top_layer() || element_box_is_placed_in_top_layer) {
        // An attached box is replaced in its viewport slot, keeping top layer order; a fresh
        // insert of a detached member appends out of order, so it needs a zone rebuild.
        if (!layout_node || !layout_node->parent())
            document().set_top_layer_needs_layout_zone_rebuild();
        set_needs_layout_tree_update(true, reason);
        return;
    }
    // A newly inserted element already has its entire subtree scheduled for construction. Its
    // initial style computation cannot require rebuilding an existing principal box, so keep
    // the insertion-specific invalidation on its parent instead of widening it to StyleChange.
    if (!layout_node && may_reuse_layout_node_for_child_list_insertion())
        return;
    if (rebuild_root == CSS::LayoutTreeRebuildRoot::BoxPresenceChange && apply_box_presence_change_in_place(reason))
        return;
    bool can_rebuild_from_self = rebuild_root == CSS::LayoutTreeRebuildRoot::Self
        || (rebuild_root == CSS::LayoutTreeRebuildRoot::SelfUnlessDocumentElementOrBody
            && !is_html_html_element() && !is_html_body_element());
    if (can_rebuild_from_self) {
        set_needs_layout_tree_update(true, reason);
        return;
    }
    if (auto parent = parent_element())
        parent->set_needs_layout_tree_update(true, reason);
    else
        set_needs_layout_tree_update(true, reason);
}

static bool content_displays_a_counter(CSS::ComputedContentData const& content)
{
    return any_of(content.items, [](CSS::ComputedContentItem const& item) {
        return item.has<CSS::ComputedContentCounter>();
    });
}

// A marker whose text is the same for every counter value (disc, circle, square, ...) does not
// reveal renumbering.
static bool counter_style_representation_depends_on_value(CSS::CounterStyle const& counter_style)
{
    auto first = counter_style.generate_an_initial_representation_for_the_counter_value(1);
    auto second = counter_style.generate_an_initial_representation_for_the_counter_value(2);
    auto third = counter_style.generate_an_initial_representation_for_the_counter_value(3);
    return first != second || second != third;
}

static bool pseudo_element_content_displays_a_counter(Element const& element, CSS::PseudoElement pseudo_element)
{
    if (!element.has_style(pseudo_element))
        return false;
    auto const* content = element.style_group<CSS::ComputedValues::ContentValues>(pseudo_element);
    return content && content_displays_a_counter(content->computed_content_value());
}

// NB: Reads style groups directly; this runs over every element after the list item, and
//     materializing a ComputedValues per element would cost more than the relayout it saves.
static bool element_displays_a_list_item_counter_value(Element const& element)
{
    if (!element.has_style())
        return false;
    for (auto pseudo_element : { CSS::PseudoElement::Before, CSS::PseudoElement::After, CSS::PseudoElement::Marker }) {
        if (pseudo_element_content_displays_a_counter(element, pseudo_element))
            return true;
    }
    if (!CSS::display_from_ffi_display(element.style_group<CSS::ComputedValues::BoxValues>()->display).is_list_item())
        return false;
    auto list_style_type = element.style_group<CSS::ComputedValues::InheritedListValues>()->list_style_type_value(element.style_scope());
    return list_style_type.visit(
        [](Empty const&) {
            return false;
        },
        [](RefPtr<CSS::CounterStyle const> const& counter_style) {
            return !counter_style || counter_style_representation_depends_on_value(*counter_style);
        },
        [](Utf16String const&) {
            return false;
        },
        [](Utf16FlyString const&) {
            return true;
        },
        [](CSS::ListStyleSymbols const& symbols) {
            return counter_style_representation_depends_on_value(*symbols.counter_style);
        });
}

// A list item box that appears or disappears renumbers the list-item counter for everything after
// it in the list. That only matters when some later content would display the counter value.
static bool list_item_box_change_is_observable(Element const& list_item, Element const& list_owner)
{
    if (!Node::list_item_box_change_renumbers_list(list_item))
        return false;
    if (pseudo_element_content_displays_a_counter(list_owner, CSS::PseudoElement::After))
        return true;
    for (auto const* sibling = list_item.next_element_sibling(); sibling; sibling = sibling->next_element_sibling()) {
        bool observable = false;
        sibling->for_each_in_inclusive_subtree([&](Node const& descendant) {
            auto const* element = as_if<Element>(descendant);
            if (element && element_displays_a_list_item_counter_value(*element)) {
                observable = true;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        if (observable)
            return true;
    }
    return false;
}

static bool dom_subtree_generates_list_item_boxes(Element const& element)
{
    bool generates_list_item_boxes = false;
    element.for_each_in_inclusive_subtree([&](Node const& descendant) {
        auto const* descendant_element = as_if<Element>(descendant);
        if (!descendant_element)
            return TraversalDecision::Continue;
        if (descendant_element->is_html_li_element()
            || (descendant_element->has_style() && CSS::display_from_ffi_display(descendant_element->style_group<CSS::ComputedValues::BoxValues>()->display).is_list_item())) {
            generates_list_item_boxes = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return generates_list_item_boxes;
}

// NB: Reads box kinds rather than styles: the element whose box is going away already carries
//     its display: none style.
static bool layout_subtree_contains_list_item_boxes(Layout::Node const& layout_node)
{
    bool contains_list_item_boxes = false;
    layout_node.for_each_in_inclusive_subtree([&](Layout::Node const& descendant) {
        if (descendant.kind() == Layout::RustFFI::NodeKind::ListItemBox) {
            contains_list_item_boxes = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return contains_list_item_boxes;
}

// The element's box stopped or started existing (display: none <-> a box display). Instead of
// rebuilding the parent's whole layout subtree, schedule the element alone for a rebuild that
// drops its box, or a DOM-order insertion of its new box into the parent's existing box, when the
// surrounding structure is unaffected. Returns false when the parent has to be rebuilt after all.
// NB: Only schedules; the layout tree update removes the box. Style updates must not free layout
//     nodes, as callers keep layout node pointers across them (e.g. getComputedStyle()).
bool Element::apply_box_presence_change_in_place(SetNeedsLayoutTreeUpdateReason reason)
{
    if (is_html_html_element() || is_html_body_element() || rendered_in_top_layer() || is<SVG::SVGElement>(*this))
        return false;
    GC::Ptr<Element> parent = parent_element();
    if (!parent || parent->shadow_root() || assigned_slot() || is<HTML::HTMLSlotElement>(*parent))
        return false;
    auto* parent_layout_node = parent->unsafe_layout_node();
    if (!parent_layout_node)
        return false;
    if (first_letter_owner_for_layout_subtree_from(*parent))
        return false;
    if (CSS::subtree_affects_generated_content_state(*this))
        return false;

    auto style = computed_style();
    VERIFY(style);
    auto display = style->display();

    if (display.is_none()) {
        auto* layout_node = unsafe_layout_node();
        if (!layout_node)
            return false;
        // The box's own style already says display: none, so its level comes from where it sits: a
        // block container with block-level children holds block-level boxes directly, one with
        // inline-level children holds inline-level boxes. Only atomic inlines detach from an inline
        // run; an inline box may have been split around block-level descendants.
        bool box_is_block_level = !parent_layout_node->children_are_inline();
        if (!box_is_block_level && layout_node->kind() == Layout::RustFFI::NodeKind::InlineNode)
            return false;
        if (!Node::can_detach_layout_subtree_in_place(*this, *parent, box_is_block_level))
            return false;
        if (layout_subtree_contains_list_item_boxes(*layout_node) && list_item_box_change_is_observable(*this, *parent))
            return false;

        // Rebuilding the element in place with display: none clears its stale box out of the
        // retained parent.
        set_needs_layout_tree_update(true, reason);
        return true;
    }

    if (unsafe_layout_node())
        return false;
    if (style->position() == CSS::Positioning::Absolute || style->position() == CSS::Positioning::Fixed || style->float_() != CSS::Float::None)
        return false;

    auto parent_display = parent_layout_node->display();
    bool parent_is_block_container = (parent_display.is_flow_inside() || parent_display.is_flow_root_inside()) && !parent_display.is_inline_outside();
    bool can_insert_in_place = false;
    if (parent_display.is_flex_inside() || parent_display.is_grid_inside())
        can_insert_in_place = true;
    else if (parent_is_block_container && display.is_block_outside())
        can_insert_in_place = !parent_layout_node->children_are_inline() || !parent_layout_node->has_children();
    else if (parent_is_block_container && display.is_inline_outside())
        can_insert_in_place = parent_layout_node->children_are_inline() && parent_layout_node->has_children();
    if (!can_insert_in_place)
        return false;
    if (dom_subtree_generates_list_item_boxes(*this) && list_item_box_change_is_observable(*this, *parent))
        return false;

    parent->set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::NodeInsertBefore);
    set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::NodeInsertBefore);
    return true;
}

void Element::apply_computed_style_to_layout_node_if_needed(CSS::RequiredInvalidationAfterStyleChange const& invalidation)
{
    auto* layout_node = unsafe_layout_node();
    if (invalidation.needs_layout_tree_rebuild() || !layout_node)
        return;

    // If we're keeping the layout tree, we can just apply the new style to the existing layout tree.
    VERIFY(has_style());
    layout_node->apply_style(style_record_identity());
    if (Painting::has_committed_box(*layout_node))
        Painting::repaint_after_style_change(*layout_node, invalidation);

    apply_computed_pseudo_element_styles_to_layout_nodes_if_needed(invalidation);
}

void Element::apply_computed_pseudo_element_styles_to_layout_nodes_if_needed(CSS::RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.needs_layout_tree_rebuild())
        return;

    for_each_synthetic_pseudo_element([&](CSS::PseudoElement pseudo_element_type, SyntheticPseudoElement const& pseudo_element) {
        if (!has_style(pseudo_element_type))
            return;

        if (auto node_with_style = pseudo_element.unsafe_layout_node()) {
            node_with_style->apply_style(style_record_identity(pseudo_element_type));
            if (Painting::has_committed_box(*node_with_style))
                Painting::repaint_after_style_change(*node_with_style, invalidation);
        }
    });
}

void Element::set_style_input_record(OwnPtr<CSS::StyleInputRecord> record)
{
    m_style_input_record = move(record);
}

void Element::retire_style_input_record()
{
    m_style_input_record = nullptr;
}

OwnPtr<CSS::StyleInputRecord> Element::take_style_input_record()
{
    return move(m_style_input_record);
}

void Element::record_style_custom_property_reference(Utf16FlyString const& name)
{
    document().style_computer().record_style_custom_property_reference(*this, name);
}

void Element::finish_recording_style_custom_property_references()
{
    if (!m_style_input_record)
        return;
    m_style_input_record->style_uses_attr_css_function = m_style_uses_attr_css_function;
    m_style_input_record->style_uses_var_css_function = m_style_uses_var_css_function;
    m_style_input_record->style_uses_if_css_function = m_style_uses_if_css_function;
    m_style_input_record->style_uses_custom_function = m_style_uses_custom_function;
    m_style_input_record->style_uses_inherit_css_function = m_style_uses_inherit_css_function;
    m_style_input_record->style_uses_tree_counting_function = m_style_uses_tree_counting_function;
    m_style_input_record->style_depends_on_size_container_query = m_style_depends_on_size_container_query;
    m_style_input_record->style_depends_on_style_container_query = m_style_depends_on_style_container_query;
    auto& references = m_style_input_record->custom_property_references;
    quick_sort(references);
    for (size_t index = references.size(); index > 1; --index) {
        if (references[index - 1] == references[index - 2])
            references.remove(index - 1);
    }
}

CSS::RequiredInvalidationAfterStyleChange Element::apply_style_engine_reaction(bool& did_change_custom_properties, StyleEngineRecomputeReason recompute_reason, u8 inherited_style_groups)
{
    VERIFY(parent());

    auto& counters = document().style_invalidation_counters();
    auto& style_computer = document().style_computer();
    counters.element_style_recomputations++;
    auto recompute_started_at = MonotonicTime::now();
    ScopeGuard record_recompute_time = [&] {
        counters.style_recompute_microseconds += (MonotonicTime::now() - recompute_started_at).to_microseconds();
    };
    ScopeGuard finish_custom_property_references = [&] {
        finish_recording_style_custom_property_references();
    };

    CSS::StyleEngineMatchResult style_engine_matches;
    CSS::StyleEngineMatchResult* reusable_style_engine_matches = nullptr;
    CSS::StyleEngine::StyleRecordDelta style_record_delta {};
    auto old_computed_values = computed_style();
    auto root_font_metrics_before_recompute = style_computer.root_element_font_metrics();
    auto root_font_metrics_depended_on_viewport_before_recompute = style_computer.root_element_font_metrics_depend_on_viewport_metrics();
    // Top-layer membership controls whether ::backdrop is materialized, but does not change its
    // cascade inputs. Do not let an unchanged cascade hide a still-missing backdrop style.
    auto backdrop_style_needs_materialization = [&] {
        return m_rendered_in_top_layer && !computed_style(CSS::PseudoElement::Backdrop);
    };
    ElementDependentInvalidationState old_state {
        .layout_node = unsafe_layout_node(),
        .content_counter_style_dependencies = {},
        .list_counter_style = {},
        .has_snapshot = false,
    };
    if (old_computed_values && old_computed_values->animated_properties()) {
        old_state.snapshot();
        old_computed_values.retain_across_style_record_publication();
    }
    RefPtr<CSS::ComputedValues const> new_style;
    bool used_inherited_style_group_swap = false;
    if (recompute_reason == StyleEngineRecomputeReason::InheritedOnly && old_computed_values
        && !has_relevant_animations() && !has_css_defined_animations()
        && property_ids_with_existing_transitions({}).is_empty()
        && property_ids_with_matching_transition_property_entry({}).is_empty()) {
        auto inheritance_parent = AbstractElement { *this }.element_to_inherit_style_from();
        auto parent_style = inheritance_parent.has_value() ? inheritance_parent->computed_style() : CSS::ComputedStyleRecordView {};
        if (parent_style)
            new_style = style_computer.inherited_style_group_swap(*this, *old_computed_values, *parent_style);
        if (new_style) {
            style_record_delta = style_computer.publish_computed_style_inputs({ *this }, *new_style);
            ++counters.element_inherited_style_group_swaps;
            used_inherited_style_group_swap = true;
        }
    }
    if (!new_style) {
        m_style_uses_attr_css_function = false;
        m_style_uses_var_css_function = false;
        m_style_uses_if_css_function = false;
        m_style_uses_custom_function = false;
        m_style_uses_inherit_css_function = false;
        m_style_uses_tree_counting_function = false;
        m_style_depends_on_size_container_query = false;
        m_style_depends_on_style_container_query = false;
        reusable_style_engine_matches = &style_engine_matches;
        new_style = style_computer.materialize_style_record({ *this }, did_change_custom_properties, reusable_style_engine_matches, style_record_delta, inherited_style_groups);
    }
    bool root_font_metrics_changed = is_html_html_element()
        && (root_font_metrics_before_recompute != style_computer.root_element_font_metrics()
            || root_font_metrics_depended_on_viewport_before_recompute != style_computer.root_element_font_metrics_depend_on_viewport_metrics());
    if (style_record_is_unchanged(style_record_delta))
        ++counters.unchanged_style_record_deltas;

    // An exact StyleEngine input record can answer a recomputation with the very style the element
    // already holds. Nothing derived from the originating style needs to be compared or published
    // again in that case. Pseudo-element declarations are a separate cascade projected from the
    // originating element's matches, so they still have to consume that shared match result.
    if (style_record_is_unchanged(style_record_delta) && !did_change_custom_properties && !root_font_metrics_changed) {
        if (recompute_reason == StyleEngineRecomputeReason::PseudoInputsUnchanged
            && !backdrop_style_needs_materialization()
            && style_engine_matches.node != 0
            && style_computer.style_engine().pseudo_cascade_states_are_unchanged(style_engine_matches.node)) {
            counters.element_style_noop_recomputations++;
            return {};
        }
        auto invalidation = recompute_pseudo_element_styles(
            did_change_custom_properties,
            old_computed_values->display().is_list_item(),
            &*old_computed_values,
            reusable_style_engine_matches);
        if (invalidation.is_none()) {
            counters.element_style_noop_recomputations++;
            return invalidation;
        }
        apply_computed_style_to_layout_node_if_needed(invalidation);
        return invalidation;
    }

    bool had_list_marker = false;

    CSS::RequiredInvalidationAfterStyleChange invalidation;
    bool element_computed_style_changed = true;
    if (old_computed_values) {
        DOM::AbstractElement abstract_element { *this };
        auto result = compute_required_invalidation_with_cache(
            style_computer,
            *old_computed_values,
            *new_style,
            old_state,
            abstract_element,
            style_record_delta,
            used_inherited_style_group_swap ? ElementDependentInvalidationMode::InheritedGroupSwap : ElementDependentInvalidationMode::Full);
        invalidation = result.invalidation;
        element_computed_style_changed = result.any_computed_value_changed;
        had_list_marker = old_computed_values->display().is_list_item();
    } else {
        invalidation = CSS::RequiredInvalidationAfterStyleChange::full();
    }
    if (root_font_metrics_changed) {
        // Root-relative units read document-global font metrics rather than inherited style. Every
        // descendant must recompute even when an ancestor absorbs the root's inherited changes.
        style_computer.drop_style_sharing_cache();
        invalidation.recompute_descendant_styles = true;
    }

    // https://drafts.csswg.org/css-anchor-position-1/#determining
    // Update the anchor name registry when anchor-name changes.
    // FIXME: The tree root should be determined by the stylesheet origin, not the element's position in the tree.
    if (is_connected()) {
        auto& anchor_names = as_if<ShadowRoot>(root())
            ? as<ShadowRoot>(root()).anchor_name_map()
            : document().anchor_name_map();
        bool element_had_registered_anchor_names = false;
        if (old_computed_values) {
            for (auto const& name : old_computed_values->anchor_names()) {
                element_had_registered_anchor_names = true;
                anchor_names.unregister_name(name, *this);
            }
        }
        bool element_has_anchor_names = false;
        for (auto const& name : new_style->anchor_names()) {
            element_has_anchor_names = true;
            anchor_names.register_name(name, *this);
        }

        // Anchor names that vanish here become invisible to the dispatch-time check of the
        // live anchor-name maps, while positioned boxes anywhere may hold geometry resolved
        // against them; names still registered keep that check refusing partial relayout.
        if (element_had_registered_anchor_names && !element_has_anchor_names)
            document().partial_relayout_invalidation().record_escape(PartialRelayoutEscapeReason::AnchorNamesUnregisteredByStyleChange);
    }

    // Which animations an element references is an index StyleEngine keeps, in the same shape as the
    // anchor-name registry above: nothing about selector matching can say it, and without it a
    // `@keyframes` rule cannot find the elements running the animation it describes.
    auto indexable_animation_names = [](CSS::ComputedValues const& style) {
        Vector<Utf16FlyString> animation_names;
        for (auto const& animation_name : style.animation_names()) {
            if (animation_name.syntax != CSS::ComputedAnimationNameSyntax::None)
                animation_names.append(animation_name.name);
        }
        return animation_names;
    };
    auto animation_names = indexable_animation_names(*new_style);
    if (old_computed_values ? indexable_animation_names(*old_computed_values) != animation_names : !animation_names.is_empty())
        CSS::record_element_animation_names(*this, animation_names);
    // Which custom properties this element declares or references decides which `@property`
    // registrations reach it. Declaring one matters because registration changes how it computes;
    // referencing one matters because registration gives it a value where it had none.
    //
    // The names follow from the environment the element resolved to, so a recomputation that landed
    // on the environment it landed on last time has nothing new to say. Saying it anyway is one
    // interned atom per name and a crossing into the engine, per element, per pass.
    PublishedCustomPropertyNames published_names {
        .data = custom_property_data({}),
        .uses_var_css_function = m_style_uses_var_css_function,
        .uses_custom_function = m_style_uses_custom_function,
    };
    if (published_names != m_published_custom_property_names) {
        CSS::record_element_custom_property_names(*this, published_names.data.ptr(), m_style_uses_var_css_function, m_style_uses_custom_function);
        m_published_custom_property_names = move(published_names);
    }

    auto old_non_animated_display_is_none = old_computed_values ? old_computed_values->base_values().display().is_none() : true;
    auto new_non_animated_display_is_none = new_style->base_values().display().is_none();

    PreservedPseudoElementStyles preserved_pseudo_element_styles;
    for_each_synthetic_pseudo_element([&](CSS::PseudoElement pseudo_element, SyntheticPseudoElement const& synthetic_pseudo_element) {
        if (!synthetic_pseudo_element.unsafe_layout_node())
            return;
        auto pseudo_element_style = computed_style(pseudo_element);
        if (pseudo_element_style && pseudo_element_style->animated_properties())
            preserved_pseudo_element_styles[to_underlying(pseudo_element)] = CSS::ComputedValues::Builder { *pseudo_element_style }.build();
    });

    set_computed_style({}, style_record_delta.new_style_record);

    if (old_non_animated_display_is_none != new_non_animated_display_is_none) {
        for_each_shadow_including_inclusive_descendant([&](auto& node) {
            if (!node.is_element())
                return TraversalDecision::Continue;
            auto& element = static_cast<Element&>(node);
            element.play_or_cancel_animations_after_display_property_change();
            return TraversalDecision::Continue;
        });
    }

    // NB: Elements inside a display:none subtree keep their last computed style without being recomputed, so
    //     entering the subtree must mark those retained styles as hidden. The caller's typed reactions propagate a
    //     display reveal to descendants.
    auto current_computed_values = computed_style();
    VERIFY(current_computed_values);
    if (old_computed_values && old_computed_values->display().is_none() != current_computed_values->display().is_none()) {
        if (current_computed_values->display().is_none())
            set_in_display_none_subtree_on_descendant_styles();
    }

    auto const element_style_changed = !invalidation.is_none();
    auto const element_custom_properties_changed = did_change_custom_properties;
    if (element_computed_style_changed || element_custom_properties_changed)
        counters.element_computed_style_changes++;

    auto pseudo_inherited_inputs_are_unchanged = old_computed_values
        && old_computed_values->inherited_style_group_identities() == new_style->inherited_style_group_identities();
    if (recompute_reason != StyleEngineRecomputeReason::PseudoInputsUnchanged
        || backdrop_style_needs_materialization()
        || element_custom_properties_changed
        || !pseudo_inherited_inputs_are_unchanged
        || had_list_marker != new_style->display().is_list_item()
        || style_engine_matches.node == 0
        || !style_computer.style_engine().pseudo_cascade_states_are_unchanged(style_engine_matches.node)) {
        invalidation |= recompute_pseudo_element_styles(did_change_custom_properties, had_list_marker, old_computed_values ? &*old_computed_values : nullptr, reusable_style_engine_matches, &preserved_pseudo_element_styles);
    }

    if (old_computed_values && (element_style_changed || element_custom_properties_changed))
        invalidate_descendant_styles_depending_on_style_container_query();

    if (invalidation.is_none()) {
        counters.element_style_noop_recomputations++;
        return invalidation;
    }

    counters.committed_style_observer_consequences++;
    apply_computed_style_to_layout_node_if_needed(invalidation);

    return invalidation;
}

void Element::set_in_display_none_subtree_on_descendant_styles()
{
    for_each_shadow_including_descendant([](auto& node) {
        auto* element = as_if<Element>(node);
        if (!element)
            return TraversalDecision::Continue;
        auto computed_values = element->computed_style();
        if (!computed_values)
            return TraversalDecision::SkipChildrenAndContinue;
        if (computed_values->in_display_none_subtree())
            return TraversalDecision::SkipChildrenAndContinue;
        CSS::ComputedValues::Builder builder(*computed_values);
        builder->set_in_display_none_subtree(true);
        if (computed_values->has_animated_values()) {
            CSS::ComputedValues::Builder base_values_builder(computed_values->base_values());
            base_values_builder->set_in_display_none_subtree(true);
            builder->set_base_values(move(base_values_builder).build());
        }
        auto updated_values = move(builder).build();
        auto publication = element->document().style_computer().publish_computed_style_inputs({ *element }, *updated_values);
        element->refresh_computed_style({}, publication.new_style_record);
        element->for_each_synthetic_pseudo_element([&](CSS::PseudoElement pseudo_element_type, SyntheticPseudoElement& pseudo_element) {
            pseudo_element.set_computed_values_in_display_none_subtree({ *element, pseudo_element_type });
        });
        return TraversalDecision::Continue;
    });
}

void Element::invalidate_descendant_styles_depending_on_style_container_query()
{
    // Only an element some style container query resolved against can be what a dependent under it
    // was asking about, and most documents have no style container queries at all.
    if (!m_is_style_query_container)
        return;

    ++document().style_invalidation_counters().style_query_container_scans;

    for_each_shadow_including_descendant([](auto& node) {
        auto* element = as_if<Element>(node);
        if (!element || !element->style_depends_on_style_container_query())
            return TraversalDecision::Continue;
        element->document().style_computer().style_engine().record_element_style_input_change(element->style_node_id());
        return TraversalDecision::Continue;
    });
}

GC::Ref<DOMTokenList> Element::class_list()
{
    auto& rare_data = ensure_element_rare_data();
    if (!rare_data.class_list)
        rare_data.class_list = DOMTokenList::create(*this, HTML::AttributeNames::class_);
    return *rare_data.class_list;
}

// https://drafts.csswg.org/css-shadow-1/#dom-element-part
GC::Ref<DOMTokenList> Element::part_list()
{
    // The part attribute’s getter must return a DOMTokenList object whose associated element is the context object and
    // whose associated attribute’s local name is part.
    auto& rare_data = ensure_element_rare_data();
    if (!rare_data.part_list)
        rare_data.part_list = DOMTokenList::create(*this, HTML::AttributeNames::part);
    return *rare_data.part_list;
}

ReadonlySpan<Utf16FlyString> Element::part_names() const
{
    auto const* rare_data = element_rare_data();
    if (!rare_data)
        return {};
    return rare_data->parts;
}

// https://dom.spec.whatwg.org/#valid-shadow-host-name
static bool is_valid_shadow_host_name(Utf16FlyString const& name)
{
    // A valid shadow host name is:
    // - a valid custom element name
    // - "article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3", "h4", "h5", "h6", "header", "main", "nav", "p", "section", or "span"
    if (!HTML::is_valid_custom_element_name(name)
        && !name.is_one_of(HTML::TagNames::article, HTML::TagNames::aside, HTML::TagNames::blockquote, HTML::TagNames::body, HTML::TagNames::div, HTML::TagNames::footer, HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5, HTML::TagNames::h6, HTML::TagNames::header, HTML::TagNames::main, HTML::TagNames::nav, HTML::TagNames::p, HTML::TagNames::section, HTML::TagNames::span)) {
        return false;
    }
    return true;
}

// https://dom.spec.whatwg.org/#concept-attach-a-shadow-root
WebIDL::ExceptionOr<void> Element::attach_a_shadow_root(ShadowRootMode mode, bool clonable, bool serializable, bool delegates_focus, SlotAssignmentMode slot_assignment, GC::Ptr<HTML::CustomElementRegistry> registry)
{
    // 1. If element’s namespace is not the HTML namespace, then throw a "NotSupportedError" DOMException.
    if (namespace_uri() != Namespace::HTML)
        return WebIDL::NotSupportedError::create("Element's namespace is not the HTML namespace"_utf16);

    // 2. If element’s local name is not a valid shadow host name, then throw a "NotSupportedError" DOMException.
    if (!is_valid_shadow_host_name(local_name()))
        return WebIDL::NotSupportedError::create("Element's local name is not a valid shadow host name"_utf16);

    // 3. If element’s local name is a valid custom element name, or element’s is value is not null:
    if (HTML::is_valid_custom_element_name(local_name()) || is_value().has_value()) {
        // 1. Let definition be the result of looking up a custom element definition given element’s custom element
        //    registry, its namespace, its local name, and its is value.
        auto definition = HTML::look_up_a_custom_element_definition(custom_element_registry(), namespace_uri(), local_name(), is_value());

        // 2. If definition is non-null and definition’s disable shadow is true, then throw a "NotSupportedError"
        //    DOMException.
        if (definition && definition->disable_shadow())
            return WebIDL::NotSupportedError::create("Cannot attach a shadow root to a custom element that has disabled shadow roots"_utf16);
    }

    // 4. If element is a shadow host:
    if (is_shadow_host()) {
        // 1. Let currentShadowRoot be element’s shadow root.
        auto current_shadow_root = shadow_root();

        // 2. If any of the following are true:
        // - currentShadowRoot’s declarative is false; or
        // - currentShadowRoot’s mode is not mode,
        // then throw a "NotSupportedError" DOMException.
        if (!current_shadow_root->declarative() || current_shadow_root->mode() != mode) {
            return WebIDL::NotSupportedError::create("Element already is a shadow host"_utf16);
        }

        // 3. Otherwise:
        //    1. Remove all of currentShadowRoot’s children, in tree order.
        current_shadow_root->remove_all_children();

        //    2. Set currentShadowRoot’s declarative to false.
        current_shadow_root->set_declarative(false);

        //    3. Return.
        return {};
    }

    // 5. Let shadow be a new shadow root whose node document is element’s node document, host is this, and mode is
    //    mode.
    auto shadow = ShadowRoot::create(document(), *this, mode);

    // 6. Set shadow’s delegates focus to delegatesFocus.
    shadow->set_delegates_focus(delegates_focus);

    // 7. If element’s custom element state is "precustomized" or "custom", then set shadow’s available to element
    //    internals to true.
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

    // 12. Set shadow’s custom element registry to registry.
    shadow->set_custom_element_registry(registry);

    // 13. Set element’s shadow root to shadow.
    set_shadow_root(shadow);
    return {};
}

// https://dom.spec.whatwg.org/#dom-element-attachshadow
WebIDL::ExceptionOr<GC::Ref<ShadowRoot>> Element::attach_shadow(ShadowRootOptions const& init)
{
    // 1. Let registry be this’s node document’s custom element registry.
    auto registry = document().custom_element_registry();

    // 2. If init["customElementRegistry"] exists, then set registry to it.
    if (init.custom_element_registry.has_value())
        registry = init.custom_element_registry.value();

    // 3. If registry is non-null, registry’s is scoped is false, and registry is not this’s node document’s custom
    //    element registry, then throw a "NotSupportedError" DOMException.
    if (registry && !registry->is_scoped() && registry != document().custom_element_registry())
        return WebIDL::NotSupportedError::create("'customElementRegistry' in ShadowRootInit must either be scoped or the document's custom element registry."_utf16);

    // 4. Run attach a shadow root with this, init["mode"], init["clonable"], init["serializable"],
    //    init["delegatesFocus"], init["slotAssignment"], and registry.
    TRY(attach_a_shadow_root(init.mode, init.clonable, init.serializable, init.delegates_focus, init.slot_assignment, registry));

    // 5. Return this’s shadow root.
    return GC::Ref { *shadow_root() };
}

// https://dom.spec.whatwg.org/#dom-element-shadowroot
GC::Ptr<ShadowRoot> Element::open_shadow_root() const
{
    // 1. Let shadow be this’s shadow root.
    auto shadow = m_shadow_root;

    // 2. If shadow is null or its mode is "closed", then return null.
    if (shadow == nullptr || shadow->mode() == ShadowRootMode::Closed)
        return nullptr;

    // 3. Return shadow.
    return shadow;
}

// https://dom.spec.whatwg.org/#dom-element-matches
WebIDL::ExceptionOr<bool> Element::matches(Utf16View selectors) const
{
    // 1. Let s be the result of parse a selector from selectors.
    auto query = document().selector_query_for(selectors);

    // 2. If s is failure, then throw a "SyntaxError" DOMException.
    if (!query)
        return WebIDL::SyntaxError::create("Failed to parse selector"_utf16);

    // 3. If the result of match a selector against an element, using s, this, and scoping root this, returns success, then return true; otherwise, return false.
    return query->matches(*this, *this);
}

// https://dom.spec.whatwg.org/#dom-element-closest
WebIDL::ExceptionOr<DOM::Element const*> Element::closest(Utf16View selectors) const
{
    // 1. Let s be the result of parse a selector from selectors.
    auto query = document().selector_query_for(selectors);

    // 2. If s is failure, then throw a "SyntaxError" DOMException.
    if (!query)
        return WebIDL::SyntaxError::create("Failed to parse selector"_utf16);

    // 3. Let elements be this’s inclusive ancestors that are elements, in reverse tree order.
    // 4. For each element in elements, if match a selector against an element, using s, element, and scoping root this, returns success, return element.
    // 5. Return null.
    return query->closest(*this).ptr();
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-innerhtml
WebIDL::ExceptionOr<void> Element::set_inner_html(StringView html)
{
    auto markup = Utf16String::from_utf8(html);
    Variant<GC::Ref<Element>, GC::Ref<DocumentFragment>> target = GC::Ref { *this };
    auto* template_element = as_if<HTML::HTMLTemplateElement>(*this);
    if (template_element)
        target = template_element->content();
    auto fragment = TRY(parse_fragment(target, markup.utf16_view()));

    // 5. Replace all with fragment within target.
    target.visit([&](auto node) {
        node->replace_all(fragment);
    });

    // NOTE: We don't invalidate style & layout for <template> elements since they don't affect rendering.
    if (!template_element) {
        if (is_connected()) {
            // NOTE: Since the DOM has changed, we have to rebuild the layout tree.
            set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::ElementSetInnerHTML);
        }
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-innerhtml
WebIDL::ExceptionOr<Utf16String> Element::inner_html() const
{
    return TRY(serialize_fragment(HTML::RequireWellFormed::Yes));
}

bool Element::is_focused() const
{
    return document().focused_area().ptr() == this;
}

bool Element::is_the_active_element() const
{
    return document().active_element() == this;
}

bool Element::is_being_activated() const
{
    return m_is_being_activated;
}

void Element::set_being_activated(bool active)
{
    if (m_is_being_activated == active)
        return;
    m_is_being_activated = active;
    CSS::Invalidation::invalidate_style_after_active_state_change(*this, active);
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
    if (m_shadow_root) {
        if (is_connected())
            CSS::record_subtree_disconnecting(*m_shadow_root);
        m_shadow_root->set_host(nullptr);
        m_shadow_root->set_is_connected(false);
        // NB: We don't need to run the removed steps if the children have already been disconnected (or were never
        //     connected in the first place)
        if (is_connected()) {
            m_shadow_root->for_each_shadow_including_descendant([&](DOM::Node& descendant) {
                descendant.removed_from(IsSubtreeRoot::No, m_shadow_root.ptr(), *m_shadow_root);
                return TraversalDecision::Continue;
            });
        }
    }
    m_shadow_root = move(shadow_root);
    if (m_shadow_root) {
        // NB: Children shouldn't be set on shadow roots until the shadow root is attached to a host so that they
        //     correctly inherit connectedness.
        VERIFY(!m_shadow_root->has_children());

        m_shadow_root->set_host(this);
        m_shadow_root->set_is_connected(is_connected());
    }
    set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::ElementSetShadowRoot);
}

GC::Ref<CSS::CSSStyleProperties> Element::style()
{
    if (!m_inline_style)
        m_inline_style = CSS::CSSStyleProperties::create_element_inline_style({ *this }, {}, {});
    return *m_inline_style;
}

GC::Ref<CSS::StylePropertyMap> Element::attribute_style_map()
{
    auto& rare_data = ensure_element_rare_data();
    if (!rare_data.attribute_style_map)
        rare_data.attribute_style_map = CSS::StylePropertyMap::create(style());
    return *rare_data.attribute_style_map;
}

void Element::set_inline_style(GC::Ptr<CSS::CSSStyleProperties> style)
{
    if (m_inline_style == style)
        return;
    auto had_declarations = m_inline_style && !m_inline_style->properties().is_empty();
    m_inline_style = style;
    if (auto* rare_data = element_rare_data())
        rare_data->attribute_style_map = nullptr;

    // The element's own declarations moved without an attribute moving: a user-agent shadow tree
    // hands its inner elements a style object directly, and an `<input>`'s placeholder swaps
    // between two of them as it becomes shown or hidden. That is the same input as writing a style
    // attribute, and nothing else tells the engine about it.
    CSS::record_element_declarations_changed(
        *this,
        CSS::ElementDeclarationKind::InlineStyle,
        had_declarations,
        style && !style->properties().is_empty());
}

void Element::prepare_for_inline_style_change()
{
    if (is_custom() || document().page().listen_for_dom_mutations()) {
        synchronize_style_attribute();
        return;
    }

    for (Node* node = this; node; node = node->parent()) {
        auto* registered_observers = node->registered_observer_list();
        if (!registered_observers)
            continue;
        for (auto const& registered_observer : *registered_observers) {
            auto const& options = registered_observer->options();
            if (node != this && !options.subtree)
                continue;
            if (!options.attributes.value_or(false))
                continue;
            if (options.attribute_filter.has_value() && !options.attribute_filter->contains_slow(HTML::AttributeNames::style))
                continue;
            synchronize_style_attribute();
            return;
        }
    }
}

bool Element::can_defer_inline_style_attribute_update() const
{
    return !is_custom() && !document().page().listen_for_dom_mutations();
}

void Element::did_update_inline_style()
{
    VERIFY(can_defer_inline_style_attribute_update());
    VERIFY(m_inline_style);

    bool had_style_attribute = m_style_attribute_is_dirty;
    Optional<Utf16String> old_value;
    if (m_attributes) {
        for (auto const& attribute : *m_attributes) {
            if (!attribute.name.namespace_().has_value() && attribute.name.local_name() == HTML::AttributeNames::style) {
                had_style_attribute = true;
                old_value = attribute.value;
                break;
            }
        }
    }

    if (auto history = document().editing_history_if_exists())
        history->notify_dom_mutation();

    m_style_attribute_is_dirty = true;
    document().mark_style_attribute_dirty();
    queue_mutation_record(MutationType::attributes, HTML::AttributeNames::style, {}, old_value, {}, {}, nullptr, nullptr);

    if (!document().suppresses_attribute_style_invalidation())
        CSS::record_element_declarations_changed(*this, CSS::ElementDeclarationKind::InlineStyle, had_style_attribute, true);
    document().bump_dom_tree_version();
}

void Element::synchronize_style_attribute() const
{
    if (!m_style_attribute_is_dirty)
        return;

    auto& element = const_cast<Element&>(*this);
    element.m_style_attribute_is_dirty = false;

    Optional<Utf16String> old_value;
    Optional<size_t> style_attribute_index;
    if (element.m_attributes) {
        for (size_t index = 0; index < element.m_attributes->size(); ++index) {
            auto const& attribute = element.m_attributes->at(index);
            if (!attribute.name.namespace_().has_value() && attribute.name.local_name() == HTML::AttributeNames::style) {
                old_value = attribute.value;
                style_attribute_index = index;
                break;
            }
        }
    }

    auto new_value = element.m_inline_style->serialized();
    if (style_attribute_index.has_value()) {
        element.m_attributes->at(*style_attribute_index).value = new_value;
    } else {
        element.ensure_attribute_list().empend(QualifiedName { HTML::AttributeNames::style, {}, {} }, new_value);
    }

    CSS::record_element_attribute_changed(element, HTML::AttributeNames::style, {}, old_value, new_value);
}

// https://dom.spec.whatwg.org/#element-html-uppercased-qualified-name
Utf16FlyString Element::make_html_uppercased_qualified_name() const
{
    // This is allowed by the spec: "User agents could optimize qualified name and HTML-uppercased qualified name by storing them in internal slots."
    if (namespace_uri() == Namespace::HTML && document().document_type() == Document::Type::HTML)
        return qualified_name().to_ascii_uppercase();
    return qualified_name();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#queue-an-element-task
HTML::TaskID Element::queue_an_element_task(HTML::Task::Source source, Function<void()> steps)
{
    return queue_a_task(source, HTML::main_thread_event_loop(), document(), GC::create_function(GC::Heap::the(), move(steps)));
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

static CSSPixelRect bounding_rect_from_client_rects(Vector<CSSPixelRect> const& list)
{
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

// https://drafts.csswg.org/cssom-view/#dom-element-getboundingclientrect
CSSPixelRect Element::get_bounding_client_rect() const
{
    // 1. Let list be the result of invoking getClientRects() on element.
    return bounding_rect_from_client_rects(get_client_rects());
}

enum class VisualContextTransform {
    Apply,
    Identity,
};

static void append_border_box_rect(Vector<CSSPixelRect>& rects, Layout::Node const& layout_node, VisualContextTransform visual_context_transform)
{
    auto absolute_rect = Painting::absolute_border_box_rect(layout_node);
    if (visual_context_transform == VisualContextTransform::Identity)
        rects.append(absolute_rect);
    else
        rects.append(Painting::transform_rect_to_viewport(layout_node, absolute_rect, Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::No));
}

static Vector<CSSPixelRect> compute_client_rects_assuming_layout_clean(Element const& element, VisualContextTransform visual_context_transform = VisualContextTransform::Apply)
{
    // 1. If the element on which it was invoked does not have an associated layout box return an empty DOMRectList
    //    object and stop this algorithm.
    auto const* layout_node = element.layout_node();
    if (!layout_node)
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

    Vector<CSSPixelRect> rects;
    if (Painting::is_inline_paintable(*layout_node)) {
        Vector<CSSPixelRect> piece_border_box_rects;
        Painting::inline_piece_border_box_rects(*layout_node, piece_border_box_rects);
        for (auto const& absolute_rect : piece_border_box_rects) {
            if (visual_context_transform == VisualContextTransform::Identity)
                rects.append(absolute_rect);
            else
                rects.append(Painting::transform_rect_to_viewport(*layout_node, absolute_rect, Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::No));
        }
        // An inline element whose content is only interrupting blocks generates no line fragments, but per CSSOM
        // we still report its (zero-sized) border area instead of an empty list.
        if (rects.is_empty())
            append_border_box_rect(rects, *layout_node, visual_context_transform);
        return rects;
    }

    if (Painting::has_committed_box(*layout_node))
        append_border_box_rect(rects, *layout_node, visual_context_transform);

    return rects;
}

// https://drafts.csswg.org/cssom-view/#dom-element-getclientrects
Vector<CSSPixelRect> Element::get_client_rects() const
{
    auto navigable = document().navigable();
    if (!navigable)
        return {};

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout_if_needed_for_node(*this, UpdateLayoutReason::ElementGetClientRects);

    if (!layout_node())
        return {};

    if (document().can_compute_client_rects_without_accumulated_visual_contexts_update(*layout_node()))
        return compute_client_rects_assuming_layout_clean(*this, VisualContextTransform::Identity);

    // NOTE: Make sure CSS transforms are resolved before they are used to calculate the rect position.
    const_cast<Document&>(document()).update_paint_and_hit_testing_properties_if_needed();

    return compute_client_rects_assuming_layout_clean(*this);
}

Vector<CSSPixelRect> Element::client_rects_assuming_layout_clean() const
{
    if (!document().navigable())
        return {};
    return compute_client_rects_assuming_layout_clean(*this);
}

CSSPixelRect Element::bounding_client_rect_assuming_layout_clean() const
{
    return bounding_rect_from_client_rects(client_rects_assuming_layout_clean());
}

int Element::client_top() const
{
    // NOTE: We only need style information here, not layout metrics.
    const_cast<Document&>(document()).update_style_for_element(AbstractElement { const_cast<Element&>(*this) }, Document::StyleUpdateMode::OnlyIfNeeded);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    auto const* box_values = style_group<CSS::ComputedValues::BoxValues>();
    if (!box_values)
        return 0;
    auto display = CSS::display_from_ffi_display(box_values->display);
    if (display.is_none() || display.is_contents())
        return 0;
    if (display.is_inline_outside() && display.is_flow_inside())
        return 0;

    // 2. Return the computed value of the border-top-width property
    //    plus the height of any scrollbar rendered between the top padding edge and the top border edge,
    //    ignoring any transforms that apply to the element and its ancestors.
    auto const& border_top = style_group<CSS::ComputedValues::BorderValues>()->border_top_value();
    if (border_top.line_style == CSS::LineStyle::None || border_top.line_style == CSS::LineStyle::Hidden)
        return 0;
    return border_top.width.to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-element-clientleft
int Element::client_left() const
{
    // NOTE: We only need style information here, not layout metrics.
    const_cast<Document&>(document()).update_style_for_element(AbstractElement { const_cast<Element&>(*this) }, Document::StyleUpdateMode::OnlyIfNeeded);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    auto const* box_values = style_group<CSS::ComputedValues::BoxValues>();
    if (!box_values)
        return 0;
    auto display = CSS::display_from_ffi_display(box_values->display);
    if (display.is_none() || display.is_contents())
        return 0;
    if (display.is_inline_outside() && display.is_flow_inside())
        return 0;

    // 2. Return the computed value of the border-left-width property
    //    plus the width of any scrollbar rendered between the left padding edge and the left border edge,
    //    ignoring any transforms that apply to the element and its ancestors.
    auto const& border_left = style_group<CSS::ComputedValues::BorderValues>()->border_left_value();
    if (border_left.line_style == CSS::LineStyle::None || border_left.line_style == CSS::LineStyle::Hidden)
        return 0;
    return border_left.width.to_int();
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
    const_cast<Document&>(document()).update_layout_if_needed_for_node(*this, UpdateLayoutReason::ElementClientWidth);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    auto const* layout_node = this->layout_node();
    if (!layout_node || !Painting::has_committed_box(*layout_node))
        return 0;

    // 3. Return the width of the padding edge excluding the width of any rendered scrollbar between the padding edge and the border edge,
    // ignoring any transforms that apply to the element and its ancestors.
    return Painting::absolute_padding_box_rect(*layout_node).width().to_int();
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
    const_cast<Document&>(document()).update_layout_if_needed_for_node(*this, UpdateLayoutReason::ElementClientHeight);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    auto const* layout_node = this->layout_node();
    if (!layout_node || !Painting::has_committed_box(*layout_node))
        return 0;

    // 3. Return the height of the padding edge excluding the height of any rendered scrollbar between the padding edge and the border edge,
    //    ignoring any transforms that apply to the element and its ancestors.
    return Painting::absolute_padding_box_rect(*layout_node).height().to_int();
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
        // The MathML and SVG user-agent sheets decide only for elements in their own namespaces, so
        // the first such element to connect is what brings them into the document's user-agent
        // origin. Asking here rather than at style time keeps a page without either from ever
        // building their rules.
        if (namespace_uri() == Namespace::MathML || namespace_uri() == Namespace::SVG)
            document().set_needs_mathml_and_svg_user_agent_style_sheets();
        if (m_id.has_value())
            document().element_with_id_was_added({}, *this);
        if (m_has_name)
            document().element_with_name_was_added({}, *this);
        if (m_id.has_value() || !m_classes.is_empty())
            invalidate_content_blocker_style_if_needed(*this);
    }

    play_or_cancel_animations_after_display_property_change();
}

void Element::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    auto const* anchor_values = style_group<CSS::ComputedValues::AnchorValues>();

    // https://html.spec.whatwg.org/multipage/dom.html#render-blocking-mechanism
    // Whenever a render-blocking element el becomes browsing-context disconnected, unblock rendering on el.
    if (old_root.is_connected() && document().is_render_blocking_element(*this))
        unblock_rendering();

    if (m_id.has_value() && is<ShadowRoot>(old_root))
        static_cast<ShadowRoot&>(old_root).element_by_id().remove(*m_id, *this);

    if (old_root.is_connected()) {
        if (m_id.has_value())
            document().element_with_id_was_removed({}, *this);
        if (m_has_name)
            document().element_with_name_was_removed({}, *this);
        if (anchor_values) {
            auto& anchor_names = is<ShadowRoot>(old_root)
                ? as<ShadowRoot>(old_root).anchor_name_map()
                : document().anchor_name_map();
            bool element_had_registered_anchor_names = false;
            for (auto const& name : anchor_values->anchor_names_span()) {
                element_had_registered_anchor_names = true;
                anchor_names.unregister_name(name, *this);
            }
            // Positioned boxes anywhere may hold geometry resolved against these names, which
            // the dispatch-time check of the live anchor-name maps can no longer see.
            if (element_had_registered_anchor_names)
                document().partial_relayout_invalidation().record_escape(PartialRelayoutEscapeReason::AnchorNamesUnregisteredByElementRemoval);
        }
    }

    play_or_cancel_animations_after_display_property_change();
    exit_fullscreen_on_element_removal();

    replace_style_record(0);
    for_each_synthetic_pseudo_element([&](CSS::PseudoElement pseudo_element_type, SyntheticPseudoElement&) {
        set_computed_style(pseudo_element_type, 0);
    });
}

void Element::moved_from(IsSubtreeRoot is_subtree_root, GC::Ptr<Node> old_ancestor)
{
    Base::moved_from(is_subtree_root, old_ancestor);
}

void Element::children_changed(ChildrenChangedMetadata const& metadata)
{
    Node::children_changed(metadata);

    if (child_style_uses_tree_counting_function()) {
        for_each_child_of_type<Element>([&](Element& element) {
            if (!element.style_uses_tree_counting_function())
                return IterationDecision::Continue;

            document().style_computer().style_engine().record_element_style_input_change(element.style_node_id());

            return IterationDecision::Continue;
        });
    }
}

void Element::set_synthetic_pseudo_element_node(Badge<Layout::LayoutTreeBuilderAccess>, CSS::PseudoElement pseudo_element, Layout::NodeWithStyle* pseudo_element_node)
{
    auto existing_pseudo_element = get_synthetic_pseudo_element(pseudo_element);
    if (!existing_pseudo_element.has_value() && !pseudo_element_node)
        return;

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element)) {
        return;
    }

    ensure_synthetic_pseudo_element(pseudo_element).set_layout_node(move(pseudo_element_node));
}

Layout::NodeWithStyle* Element::pseudo_element_layout_node(CSS::PseudoElement pseudo_element) const
{
    if (auto element_data = get_pseudo_element(pseudo_element); element_data.has_value())
        return element_data->layout_node();
    return nullptr;
}

Layout::NodeWithStyle* Element::pseudo_element_unsafe_layout_node(CSS::PseudoElement pseudo_element) const
{
    if (auto element_data = get_pseudo_element(pseudo_element); element_data.has_value())
        return element_data->unsafe_layout_node();
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-enabled
bool Element::matches_enabled_pseudo_class() const
{
    // The :enabled pseudo-class must match any button, input, select, textarea, optgroup, option, fieldset element, or form-associated custom element that is not actually disabled.
    auto is_form_associated_custom_element = is<HTML::HTMLElement>(*this) && static_cast<HTML::HTMLElement const&>(*this).is_form_associated_custom_element();
    return (is<HTML::HTMLButtonElement>(*this) || is<HTML::HTMLInputElement>(*this) || is<HTML::HTMLSelectElement>(*this) || is<HTML::HTMLTextAreaElement>(*this) || is<HTML::HTMLOptGroupElement>(*this) || is<HTML::HTMLOptionElement>(*this) || is<HTML::HTMLFieldSetElement>(*this) || is_form_associated_custom_element)
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
        return input_element.placeholder_value().has_value();
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

// https://drafts.csswg.org/selectors/#visited-pseudo
bool Element::matches_visited_pseudo_class() const
{
    // The :visited pseudo-class comes with obvious privacy implications—​letting random websites know what other
    // websites you’ve visited can be problematic for a number of reasons—​and so user agents must preserve user
    // privacy in their implementation of :visited.

    // NOTE: This specification intentionally does not specify exactly how to preserve user privacy in this regard, to
    //       allow for user agents to innovate in this space. The following methods are suggested, however:
    //       - Have :visited never match, so all links match :link instead.
    //       - Carefully track what history entries could have been observed by a given origin on their own, and only
    //         have links match :visited if that visit would have been observable from the site’s origin. A possible
    //         specific approach for this is described in Appendix C: Example Privacy-Preserving :visited Restrictions.
    //       - Allow links to match :visited on any origin, but carefully restrict what styles they can apply and what
    //         information is returned by style-querying APIs like getComputedStyle(), to prevent sites from observing
    //         whether a link is styled with :link or :visited. (This is documented at MDN, and was the historical
    //         approach browsers took, but is not perfect; there are several ways for a hostile page to still extract
    //         history information.)

    // FIXME: For simplicity we currently take the first approach and have :visited never match. We may want to rethink
    //        this in the future.
    return false;
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

bool Element::matches_focus_within_pseudo_class() const
{
    auto focused_area = document().focused_area();
    if (!focused_area)
        return false;

    for (auto const* node = focused_area.ptr(); node; node = node->flat_tree_parent()) {
        if (node == this)
            return true;
    }
    return false;
}

bool Element::has_synthetic_pseudo_elements() const
{
    if (pseudo_element_data()) {
        bool has_any_synthetic_pseudo_elements = false;

        for_each_synthetic_pseudo_element([&](CSS::PseudoElement, SyntheticPseudoElement const& pseudo_element) {
            if (pseudo_element.layout_node()) {
                has_any_synthetic_pseudo_elements = true;
                return IterationDecision::Break;
            }

            return IterationDecision::Continue;
        });

        return has_any_synthetic_pseudo_elements;
    }
    return false;
}

void Element::clear_synthetic_pseudo_element_layout_nodes()
{
    for_each_synthetic_pseudo_element([&](CSS::PseudoElement, SyntheticPseudoElement& pseudo_element) {
        if (auto layout_node = pseudo_element.layout_node()) {
            layout_node->for_each_in_inclusive_subtree([](Layout::Node& node) {
                node.clear_committed_box();
                return TraversalDecision::Continue;
            });
            layout_node->prepare_subtree_for_detach_from_layout_tree();
            Layout::detach_layout_node_for_destruction(*layout_node);
        }
        pseudo_element.set_layout_node(nullptr);
    });
}

void Element::serialize_children_as_json(JsonObjectSerializer<Utf16StringBuilder>& element_object) const
{
    bool has_pseudo_elements = this->has_synthetic_pseudo_elements();
    if (!is_shadow_host() && !has_child_nodes() && !has_pseudo_elements)
        return;

    auto children = MUST(element_object.add_array("children"sv));

    auto serialize_pseudo_element = [&](CSS::PseudoElement pseudo_element_type, PseudoElement const& pseudo_element) {
        // FIXME: Find a way to make these still inspectable? (eg, `::before { display: none }`)
        if (!pseudo_element.layout_node())
            return;
        auto object = MUST(children.add_object());
        auto pseudo_element_name = Utf16String::formatted("::{}", CSS::pseudo_element_name(pseudo_element_type));
        MUST(object.add("name"sv, pseudo_element_name.utf16_view()));
        MUST(object.add("type"sv, "pseudo-element"));
        MUST(object.add("parent-id"sv, unique_id().value()));
        MUST(object.add("pseudo-element"sv, to_underlying(pseudo_element_type)));
        MUST(object.finish());
    };

    if (has_pseudo_elements) {
        auto const& pseudo_elements = *pseudo_element_data();
        if (auto backdrop = pseudo_elements.get(CSS::PseudoElement::Backdrop); backdrop.has_value()) {
            serialize_pseudo_element(CSS::PseudoElement::Backdrop, backdrop.value());
        }
        if (auto marker = pseudo_elements.get(CSS::PseudoElement::Marker); marker.has_value()) {
            serialize_pseudo_element(CSS::PseudoElement::Marker, marker.value());
        }
        if (auto before = pseudo_elements.get(CSS::PseudoElement::Before); before.has_value()) {
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
        if (auto after = pseudo_element_data()->get(CSS::PseudoElement::After); after.has_value()) {
            serialize_pseudo_element(CSS::PseudoElement::After, after.value());
        }

        // Any other pseudo-elements, as a catch-all.
        for_each_synthetic_pseudo_element([&](CSS::PseudoElement type, PseudoElement const& pseudo_element) {
            if (first_is_one_of(type, CSS::PseudoElement::After, CSS::PseudoElement::Backdrop, CSS::PseudoElement::Before, CSS::PseudoElement::Marker))
                return;

            serialize_pseudo_element(type, pseudo_element);
        });
    }

    MUST(children.finish());
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 Element::default_tab_index_value() const
{
    // NB: See tab_index() for spec.
    return -1;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 Element::tab_index() const
{
    // The tabIndex getter steps are:
    // 1. Let attribute be this's tabindex attribute.
    auto attribute = get_attribute(HTML::AttributeNames::tabindex);

    // 2. If attribute is not null:
    if (attribute.has_value()) {
        // 1. Let parsedValue be the result of integer parsing attribute's value.
        auto parsed_value = HTML::parse_integer(attribute.value());

        // 2. If parsedValue is not an error and is within the long range, then return parsedValue.
        if (parsed_value.has_value())
            return parsed_value.release_value();
    }

    // 3. Return 0 if this is an a, area, button, frame, iframe, input, object, select, textarea, or SVG a element, or
    //    MathML a element, or is a summary element that is a summary for its parent details; otherwise −1.
    // NB: We implement this by overriding default_tab_index_value().
    return default_tab_index_value();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
void Element::set_tab_index(i32 tab_index)
{
    set_attribute_value(HTML::AttributeNames::tabindex, Utf16String::number(tab_index));
}

// https://drafts.csswg.org/cssom-view/#potentially-scrollable
bool Element::is_potentially_scrollable(TreatOverflowClipOnBodyParentAsOverflowHidden treat_overflow_clip_on_body_parent_as_overflow_hidden = TreatOverflowClipOnBodyParentAsOverflowHidden::No) const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementIsPotentiallyScrollable);

    // NB: Since this should always be the body element, the body element must have a <html> element parent. See Document::body().
    VERIFY(parent_element());

    // An element body (which will be the body element) is potentially scrollable if all of the following conditions are true:
    VERIFY(is<HTML::HTMLBodyElement>(this) || is<HTML::HTMLFrameSetElement>(this));

    // - body has an associated box.
    if (!layout_node())
        return false;

    // - body’s parent element’s computed value of the overflow-x or overflow-y properties is neither visible nor clip.
    auto const* parent_box_values = parent_element()->style_group<CSS::ComputedValues::BoxValues>();
    VERIFY(parent_box_values);
    auto parent_overflow_x = static_cast<CSS::Overflow>(parent_box_values->overflow_x);
    auto parent_overflow_y = static_cast<CSS::Overflow>(parent_box_values->overflow_y);
    if (parent_overflow_x == CSS::Overflow::Visible || parent_overflow_y == CSS::Overflow::Visible)
        return false;
    // NOTE: When treating 'overflow:clip' as 'overflow:hidden', we can never fail this condition
    if (treat_overflow_clip_on_body_parent_as_overflow_hidden == TreatOverflowClipOnBodyParentAsOverflowHidden::No && (parent_overflow_x == CSS::Overflow::Clip || parent_overflow_y == CSS::Overflow::Clip))
        return false;

    // - body’s computed value of the overflow-x or overflow-y properties is neither visible nor clip.
    auto const* box_values = style_group<CSS::ComputedValues::BoxValues>();
    VERIFY(box_values);
    if (first_is_one_of(static_cast<CSS::Overflow>(box_values->overflow_x), CSS::Overflow::Visible, CSS::Overflow::Clip) || first_is_one_of(static_cast<CSS::Overflow>(box_values->overflow_y), CSS::Overflow::Visible, CSS::Overflow::Clip))
        return false;

    return true;
}

bool Element::is_scroll_container() const
{
    // NB: We should only call this if we know that computed_properties has already been computed
    auto const* box_values = style_group<CSS::ComputedValues::BoxValues>();
    VERIFY(box_values);

    if (is_document_element())
        return true;

    return Layout::overflow_value_makes_box_a_scroll_container(static_cast<CSS::Overflow>(box_values->overflow_x))
        || Layout::overflow_value_makes_box_a_scroll_container(static_cast<CSS::Overflow>(box_values->overflow_y));
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

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementScrollTop);

    // 6. If the element is the root element return the value of scrollY on window.
    if (document.document_element() == this)
        return window->scroll_y();

    // 7. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, return the value of scrollY on window.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return window->scroll_y();

    // 8. If the element does not have any associated box, return zero and terminate these steps.
    // NB: A box that is not a scroll container is never scrolled away from its default alignment, even if it keeps a
    //     stored scroll offset from when it was one for restoration when it becomes one again.
    auto const* layout_node = this->layout_node();
    if (!layout_node || !layout_node->is_scroll_container())
        return 0.0;

    // 9. Return the y-coordinate of the scrolling area at the alignment point with the top of the padding edge of the element.
    // FIXME: Is this correct?
    return Painting::scroll_offset(*layout_node).y().to_double();
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

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementScrollLeft);

    // 6. If the element is the root element return the value of scrollX on window.
    if (document.document_element() == this)
        return window->scroll_x();

    // 7. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, return the value of scrollX on window.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return window->scroll_x();

    // 8. If the element does not have any associated box, return zero and terminate these steps.
    // NB: A box that is not a scroll container is never scrolled away from its default alignment, even if it keeps a
    //     stored scroll offset from when it was one for restoration when it becomes one again.
    auto const* layout_node = this->layout_node();
    if (!layout_node || !layout_node->is_scroll_container())
        return 0.0;

    // 9. Return the x-coordinate of the scrolling area at the alignment point with the left of the padding edge of the element.
    // FIXME: Is this correct?
    return Painting::scroll_offset(*layout_node).x().to_double();
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
        window->scroll(x, window->scroll_y(), nullptr);
        return;
    }

    // NOTE: Ensure that layout is up-to-date before looking at metrics or scrolling the page.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementSetScrollLeft);

    // 9. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, invoke scroll() on window with x as first argument and scrollY on window as second argument, and terminate these steps.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable()) {
        window->scroll(x, window->scroll_y(), nullptr);
        return;
    }

    // 10. If the element does not have any associated box, the element has no associated scrolling box, or the element has no overflow, terminate these steps.
    auto* layout_node = this->layout_node();
    if (!layout_node)
        return;

    if (!layout_node->is_scroll_container())
        return;

    // FIXME: or the element has no overflow.

    // 11. Scroll the element to x,scrollTop, with the scroll behavior being "auto".
    auto scroll_offset = Painting::scroll_offset(*layout_node);
    scroll_offset.set_x(CSSPixels::nearest_value_for(x));
    if (auto navigable = document.navigable())
        navigable->perform_a_scroll_of_an_element(*this, scroll_offset, Bindings::ScrollBehavior::Auto);
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
        window->scroll(window->scroll_x(), y, nullptr);
        return;
    }

    // NOTE: Ensure that layout is up-to-date before looking at metrics or scrolling the page.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementSetScrollTop);

    // 9. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, invoke scroll() on window with scrollX as first argument and y as second argument, and terminate these steps.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable()) {
        window->scroll(window->scroll_x(), y, nullptr);
        return;
    }

    // 10. If the element does not have any associated box, the element has no associated scrolling box, or the element has no overflow, terminate these steps.
    auto* layout_node = this->layout_node();
    if (!layout_node)
        return;

    if (!layout_node->is_scroll_container())
        return;

    // FIXME: or the element has no overflow.

    // 11. Scroll the element to scrollLeft,y, with the scroll behavior being "auto".
    auto scroll_offset = Painting::scroll_offset(*layout_node);
    scroll_offset.set_y(CSSPixels::nearest_value_for(y));
    if (auto navigable = document.navigable())
        navigable->perform_a_scroll_of_an_element(*this, scroll_offset, Bindings::ScrollBehavior::Auto);
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
    auto const* viewport_layout_node = document.layout_node();
    VERIFY(viewport_layout_node && Painting::has_committed_box(*viewport_layout_node));
    auto viewport_scrollable_overflow_rect = Painting::scrollable_overflow_rect(*viewport_layout_node);
    VERIFY(viewport_scrollable_overflow_rect.has_value());

    // 3. Let viewport width be the width of the viewport excluding the width of the scroll bar, if any,
    //    or zero if there is no viewport.
    auto viewport_width = document.viewport_rect().width().to_int();
    auto viewport_scrolling_area_width = viewport_scrollable_overflow_rect->width().to_int();

    // 4. If the element is the root element and document is not in quirks mode
    //    return max(viewport scrolling area width, viewport width).
    if (document.document_element() == this && !document.in_quirks_mode())
        return max(viewport_scrolling_area_width, viewport_width);

    // 5. If the element is the body element, document is in quirks mode and the element is not potentially scrollable,
    //    return max(viewport scrolling area width, viewport width).
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return max(viewport_scrolling_area_width, viewport_width);

    // 6. If the element does not have any associated box return zero and terminate these steps.
    auto const* layout_node = this->layout_node();
    if (!layout_node || !Painting::has_committed_box(*layout_node))
        return 0;

    // 7. Return the width of the element’s scrolling area.
    if (auto scrollable_overflow_rect = Painting::scrollable_overflow_rect(*layout_node); scrollable_overflow_rect.has_value())
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
    auto const* viewport_layout_node = document.layout_node();
    VERIFY(viewport_layout_node && Painting::has_committed_box(*viewport_layout_node));
    auto viewport_scrollable_overflow_rect = Painting::scrollable_overflow_rect(*viewport_layout_node);
    VERIFY(viewport_scrollable_overflow_rect.has_value());

    // 3. Let viewport height be the height of the viewport excluding the height of the scroll bar, if any,
    //    or zero if there is no viewport.
    auto viewport_height = document.viewport_rect().height().to_int();
    auto viewport_scrolling_area_height = viewport_scrollable_overflow_rect->height().to_int();

    // 4. If the element is the root element and document is not in quirks mode
    //    return max(viewport scrolling area height, viewport height).
    if (document.document_element() == this && !document.in_quirks_mode())
        return max(viewport_scrolling_area_height, viewport_height);

    // 5. If the element is the body element, document is in quirks mode and the element is not potentially scrollable,
    //    return max(viewport scrolling area height, viewport height).
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return max(viewport_scrolling_area_height, viewport_height);

    // 6. If the element does not have any associated box return zero and terminate these steps.
    auto const* layout_node = this->layout_node();
    if (!layout_node || !Painting::has_committed_box(*layout_node))
        return 0;

    // 7. Return the height of the element’s scrolling area.
    if (auto scrollable_overflow_rect = Painting::scrollable_overflow_rect(*layout_node); scrollable_overflow_rect.has_value()) {
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

    auto nearest_ancestor_select_is_disabled = [this] {
        if (auto select = HTML::get_nearest_ancestor_select(*this))
            return select->has_attribute(HTML::AttributeNames::disabled);
        return false;
    };

    // - an optgroup element that has a disabled attribute or whose nearest ancestor select is disabled
    if (is<HTML::HTMLOptGroupElement>(this))
        return has_attribute(HTML::AttributeNames::disabled) || nearest_ancestor_select_is_disabled();

    // - an option element that is disabled or whose nearest ancestor select is disabled
    if (auto* option = as_if<HTML::HTMLOptionElement>(this))
        return option->disabled() || nearest_ancestor_select_is_disabled();

    // - a fieldset element that is a disabled fieldset
    if (is<HTML::HTMLFieldSetElement>(this))
        return static_cast<HTML::HTMLFieldSetElement const&>(*this).is_disabled();

    // - a form-associated custom element that is disabled
    if (auto const* html_element = as_if<HTML::HTMLElement>(this); html_element && html_element->is_form_associated_custom_element())
        return !html_element->enabled();

    return false;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#fragment-parsing-algorithm-steps
WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> Element::parse_fragment(Variant<GC::Ref<Element>, GC::Ref<DocumentFragment>> target, Utf16View markup, HTML::ParserScriptingMode scripting_mode)
{
    // 1. Assert: scriptingMode is either Inert or Fragment.
    VERIFY(scripting_mode == HTML::ParserScriptingMode::Inert || scripting_mode == HTML::ParserScriptingMode::Fragment);

    // 2. If target's node document is an XML document, then return the result of invoking the XML fragment parsing
    //    algorithm given target and markup.
    if (target.visit([](auto node) { return node->document().is_xml_document(); }))
        return XMLFragmentParser::parse_xml_fragment(target, markup);

    // 3. If context's node document is an XML document, then set newChildren to the result of invoking the XML fragment parsing algorithm given context and markup.
    return HTML::HTMLParser::parse_html_fragment(target, markup, HTML::HTMLParser::AllowDeclarativeShadowRoots::No, scripting_mode);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-outerhtml
WebIDL::ExceptionOr<Utf16String> Element::outer_html() const
{
    return TRY(serialize_fragment(HTML::RequireWellFormed::Yes, FragmentSerializationMode::Outer));
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-outerhtml
WebIDL::ExceptionOr<void> Element::set_outer_html(StringView html)
{
    // 2. Let parent be this's parent.
    GC::Ptr<Node> parent = this->parent();

    // 3. If parent is null, return. There would be no way to obtain a reference to the nodes created even if the remaining steps were run.
    if (!parent)
        return {};

    // 4. If parent is a Document, throw a "NoModificationAllowedError" DOMException.
    if (parent->is_document())
        return WebIDL::NoModificationAllowedError::create("Cannot set outer HTML on document"_utf16);

    // 5. If parent is a DocumentFragment, set parent to the result of creating an element given this's node document, "body", and the HTML namespace.
    if (parent->is_document_fragment())
        parent = TRY(create_element(document(), HTML::TagNames::body, Namespace::HTML));

    // 6. Let fragment be the result of invoking the fragment parsing algorithm steps given parent and compliantString.
    auto markup = Utf16String::from_utf8(html);
    auto fragment = TRY(parse_fragment(Variant<GC::Ref<Element>, GC::Ref<DocumentFragment>> { GC::Ref { as<Element>(*parent) } }, markup.utf16_view()));

    // 6. Replace this with fragment within this's parent.
    TRY(this->parent()->replace_child(fragment, *this));

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#the-insertadjacenthtml()-method
WebIDL::ExceptionOr<void> Element::insert_adjacent_html(String const& position, StringView html)
{
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
            return WebIDL::NoModificationAllowedError::create("insertAdjacentHTML: context is null or a Document"_utf16);
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
        return WebIDL::SyntaxError::create("insertAdjacentHTML: invalid position argument"_utf16);
    }

    // 4. If context is not an Element or the following are all true:
    //    - context's node document is an HTML document,
    //    - context's local name is "html", and
    //    - context's namespace is the HTML namespace;
    if (!is<Element>(*context)
        || (context->document().document_type() == Document::Type::HTML
            && static_cast<Element const&>(*context).local_name() == "html"_utf16_fly_string
            && static_cast<Element const&>(*context).namespace_uri() == Namespace::HTML)) {
        // then set context to the result of creating an element given this's node document, "body", and the HTML namespace.
        context = TRY(create_element(document(), HTML::TagNames::body, Namespace::HTML));
    }

    // 5. Let fragment be the result of invoking the fragment parsing algorithm steps with context and compliantString.
    auto markup = Utf16String::from_utf8(html);
    auto fragment = TRY(parse_fragment(Variant<GC::Ref<Element>, GC::Ref<DocumentFragment>> { GC::Ref { as<Element>(*context) } }, markup.utf16_view()));

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

// https://fullscreen.spec.whatwg.org/#dom-element-requestfullscreen
void Element::request_fullscreen(GC::Ptr<WebIDL::Promise> promise, FullscreenRequester fullscreen_requester, Fullscreen::RequestType request_type)
{
    // 1. Let pendingDoc be this’s node document.
    auto pending_doc = m_document;

    // 3. If pendingDoc is not fully active, then reject promise with a TypeError exception and return promise.
    if (!pending_doc->is_fully_active()) {
        if (promise) {
            auto& realm = WebIDL::promise_realm(*promise);
            WebIDL::reject_promise(*promise, JS::TypeError::create(realm, "Document not fully active."_utf16));
        }
        return;
    }

    // 4. Let error be false.
    // 5. If any of conditions are false, set error to true
    auto error = is_element_allowed_to_enter_fullscreen(fullscreen_requester);

    // 6. If error is false, then consume user activation given pendingDoc’s relevant global object.
    if (error == RequestFullscreenError::False) {
        auto& relevant_global = HTML::relevant_window(*pending_doc);
        relevant_global.consume_user_activation();
    }

    // 7. Return promise, and run the remaining steps in parallel.
    pending_doc->page().enqueue_fullscreen_enter(*this, *pending_doc, error, promise, request_type);
}

void Element::webkit_request_fullscreen()
{
    request_fullscreen(nullptr, FullscreenRequester::Bindings, Fullscreen::RequestType::WebKit);
}

// https://fullscreen.spec.whatwg.org/#removing-steps
void Element::exit_fullscreen_on_element_removal()
{
    // 1. Let document be removedNode’s node document.
    auto& document = this->document();

    // 2. Let nodes be removedNode’s shadow-including inclusive descendants that have their fullscreen flag set, in
    //    shadow-including tree order.
    // 3. For each node in nodes:
    for_each_shadow_including_inclusive_descendant([&](Node& node) {
        auto* element = as_if<Element>(node);
        if (!element)
            return TraversalDecision::Continue;

        if (!element->is_fullscreen_element())
            return TraversalDecision::Continue;

        // 1. If node is document’s fullscreen element, exit fullscreen document.
        if (document.fullscreen_element().ptr() == element)
            document.exit_fullscreen(nullptr);
        // 2. Otherwise, unfullscreen node.
        else
            document.unfullscreen_element(*element);

        // 3. If document’s top layer contains node, remove from the top layer immediately given node
        if (element->in_top_layer())
            document.remove_an_element_from_the_top_layer_immediately(*element);

        return TraversalDecision::Continue;
    });
}

// https://fullscreen.spec.whatwg.org/#dom-element-requestfullscreen
// 5. If any of conditions are false, set error to true
RequestFullscreenError Element::is_element_allowed_to_enter_fullscreen(FullscreenRequester fullscreen_requester) const
{
    // * This’s namespace is the HTML namespace or this is an SVG svg or MathML math element. [SVG] [MATHML]
    // FIXME: This likely wants to use is<MathML::MathMLMathElement> instead.
    if (!(namespace_uri() == Namespace::HTML || is_svg_svg_element() || (is<MathML::MathMLElement>(*this) && tag_name() == MathML::TagNames::math)))
        return RequestFullscreenError::UnsupportedElement;

    // * This is not a dialog element
    if (is<HTML::HTMLDialogElement>(*this))
        return RequestFullscreenError::UnsupportedElement;

    // * The fullscreen element ready check for this returns true.
    if (!is_element_ready_for_fullscreen())
        return RequestFullscreenError::ElementReadyCheckFailed;

    // FIXME: * Fullscreen is supported.

    // * This’s relevant global object has transient activation or the algorithm is triggered by a user generated
    //   orientation change.
    // FIXME: Handle user generated orientation changes.
    // FIXME: Spec issue: We don't require transient activations for WebDriver.
    //        https://github.com/w3c/webdriver/issues/1888
    if (fullscreen_requester != FullscreenRequester::WebDriver) {
        auto& window = HTML::relevant_window(*this);
        if (!window.has_transient_activation())
            return RequestFullscreenError::NoTransientUserActivation;
    }

    return RequestFullscreenError::False;
}

// https://fullscreen.spec.whatwg.org/#fullscreen-element-ready-check
bool Element::is_element_ready_for_fullscreen() const
{
    // A fullscreen element ready check for an element element returns true if all of the following are true, and false otherwise:

    // * element is connected.
    if (!is_connected())
        return false;

    // * element’s node document is allowed to use the "fullscreen" feature.
    if (!m_document->is_allowed_to_use_feature(PolicyControlledFeature::Fullscreen))
        return false;

    // * element namespace is not the HTML namespace or element’s popover visibility state is hidden.
    if (namespace_uri() != Namespace::HTML)
        return true;

    auto const* html_element = as_if<HTML::HTMLElement>(this);
    return html_element ? (html_element->popover_visibility_state() == HTML::HTMLElement::PopoverVisibilityState::Hidden) : false;
}

GC::Ptr<WebIDL::CallbackType> Element::onfullscreenchange()
{
    return event_handler_attribute(HTML::EventNames::fullscreenchange);
}

void Element::set_onfullscreenchange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::fullscreenchange, event_handler);
}

GC::Ptr<WebIDL::CallbackType> Element::onfullscreenerror()
{
    return event_handler_attribute(HTML::EventNames::fullscreenerror);
}

void Element::set_onfullscreenerror(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::fullscreenerror, event_handler);
}

GC::Ptr<WebIDL::CallbackType> Element::onwebkitfullscreenchange()
{
    return event_handler_attribute(HTML::EventNames::webkitfullscreenchange);
}

void Element::set_onwebkitfullscreenchange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::webkitfullscreenchange, event_handler);
}

GC::Ptr<WebIDL::CallbackType> Element::onwebkitfullscreenerror()
{
    return event_handler_attribute(HTML::EventNames::webkitfullscreenerror);
}

void Element::set_onwebkitfullscreenerror(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::webkitfullscreenerror, event_handler);
}

// https://dom.spec.whatwg.org/#insert-adjacent
WebIDL::ExceptionOr<GC::Ptr<Node>> Element::insert_adjacent(Utf16View where, GC::Ref<Node> node)
{
    // To insert adjacent, given an element element, string where, and a node node, run the steps associated with the first ASCII case-insensitive match for where:
    if (where.equals_ignoring_ascii_case(u"beforebegin"sv)) {
        // -> "beforebegin"
        // If element’s parent is null, return null.
        if (!parent())
            return GC::Ptr<Node> { nullptr };

        // Return the result of pre-inserting node into element’s parent before element.
        return GC::Ptr<Node> { TRY(parent()->pre_insert(move(node), this)) };
    }

    if (where.equals_ignoring_ascii_case(u"afterbegin"sv)) {
        // -> "afterbegin"
        // Return the result of pre-inserting node into element before element’s first child.
        return GC::Ptr<Node> { TRY(pre_insert(move(node), first_child())) };
    }

    if (where.equals_ignoring_ascii_case(u"beforeend"sv)) {
        // -> "beforeend"
        // Return the result of pre-inserting node into element before null.
        return GC::Ptr<Node> { TRY(pre_insert(move(node), nullptr)) };
    }

    if (where.equals_ignoring_ascii_case(u"afterend"sv)) {
        // -> "afterend"
        // If element’s parent is null, return null.
        if (!parent())
            return GC::Ptr<Node> { nullptr };

        // Return the result of pre-inserting node into element’s parent before element’s next sibling.
        return GC::Ptr<Node> { TRY(parent()->pre_insert(move(node), next_sibling())) };
    }

    // -> Otherwise
    // Throw a "SyntaxError" DOMException.
    return WebIDL::SyntaxError::create(Utf16String::formatted("Unknown position '{}'. Must be one of 'beforebegin', 'afterbegin', 'beforeend' or 'afterend'", where));
}

// https://dom.spec.whatwg.org/#dom-element-insertadjacentelement
WebIDL::ExceptionOr<GC::Ptr<Element>> Element::insert_adjacent_element(Utf16View where, GC::Ref<Element> element)
{
    // The insertAdjacentElement(where, element) method steps are to return the result of running insert adjacent, give this, where, and element.
    auto returned_node = TRY(insert_adjacent(where, element));
    if (!returned_node)
        return GC::Ptr<Element> { nullptr };
    return GC::Ptr<Element> { as<Element>(*returned_node) };
}

// https://dom.spec.whatwg.org/#dom-element-insertadjacenttext
WebIDL::ExceptionOr<void> Element::insert_adjacent_text(Utf16View where, Utf16View data)
{
    // 1. Let text be a new Text node whose data is data and node document is this’s node document.
    auto text_data = Utf16String::from_utf16(data);
    auto text = DOM::Text::create(document(), move(text_data));

    // 2. Run insert adjacent, given this, where, and text.
    // Spec Note: This method returns nothing because it existed before we had a chance to design it.
    (void)TRY(insert_adjacent(where, text));
    return {};
}

// https://drafts.csswg.org/cssom-view-1/#determine-the-scroll-into-view-position
static CSSPixelPoint determine_the_scroll_into_view_position(Element& target, CSSPixelRect target_bounding_border_box, Element::ScrollLogicalPosition block, Element::ScrollLogicalPosition inline_, Node& scrolling_box)
{
    // To determine the scroll-into-view position of a target, which is an Element, pseudo-element, or Range, with a
    // block flow direction position block, an inline base direction position inline, and a scrolling box scrolling box,
    // run the following steps:

    CSSPixelRect scrolling_box_rect;
    CSSPixelPoint current_scroll_position;
    if (scrolling_box.is_document()) {
        auto& document = scrolling_box.document();
        auto& visual_viewport = *document.visual_viewport();
        // NB: Use the visual viewport as the scrolling box, this ensures that the target is scrolled into the visible
        //     region on screen when the page is pinch-zoomed.
        CSSPixelSize visible_size {
            CSSPixels::nearest_value_for(visual_viewport.width()),
            CSSPixels::nearest_value_for(visual_viewport.height()),
        };
        scrolling_box_rect = { visual_viewport.offset(), visible_size };
        if (auto* layout_node = document.layout_node())
            scrolling_box_rect = Painting::scroll_snapport_rect(*layout_node, scrolling_box_rect);
        current_scroll_position = document.navigable()->viewport_scroll_offset() + visual_viewport.offset();
    } else if (auto* layout_node = scrolling_box.layout_node(); layout_node && Painting::has_committed_box(*layout_node)) {
        current_scroll_position = Painting::scroll_offset(*layout_node);
        scrolling_box_rect = Painting::transform_rect_to_viewport(*layout_node, Painting::scroll_snapport_rect(*layout_node), Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::No);
    } else {
        return {};
    }

    // FIXME: All of this needs to support different block/inline directions.

    // 1. Let target bounding border box be the box represented by the return value of invoking Element’s
    //    getBoundingClientRect(), if target is an Element, or Range’s getBoundingClientRect(),
    //    if target is a Range.
    // AD-HOC: The caller performs this step once and passes the result in, moving it outward as each scrolling box is
    //         scrolled, so that an outer scrolling box sees where the target is going to be.

    // AD-HOC: The spec doesn't specify when to do this, but we need to apply scroll-margin and scroll-margin to target
    //         bounding border box (https://drafts.csswg.org/cssom-view-1/#example-51af1565).
    auto scroll_margin = target.computed_style()->scroll_margin();
    auto scroll_margin_top = scroll_margin.top().to_px_or_zero(CSSPixels { 0 });
    auto scroll_margin_right = scroll_margin.right().to_px_or_zero(CSSPixels { 0 });
    auto scroll_margin_bottom = scroll_margin.bottom().to_px_or_zero(CSSPixels { 0 });
    auto scroll_margin_left = scroll_margin.left().to_px_or_zero(CSSPixels { 0 });

    target_bounding_border_box.set_top(target_bounding_border_box.top() - scroll_margin_top);
    target_bounding_border_box.set_right(target_bounding_border_box.right() + scroll_margin_left + scroll_margin_right);
    target_bounding_border_box.set_bottom(target_bounding_border_box.bottom() + scroll_margin_top + scroll_margin_bottom);
    target_bounding_border_box.set_left(target_bounding_border_box.left() - scroll_margin_left);

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
        auto x = current_scroll_position.x();
        auto y = current_scroll_position.y();

        // 1. If block is "start", then align element edge A with scrolling box edge A.
        if (block == Element::ScrollLogicalPosition::Start) {
            y += element_edge_a - scrolling_box_edge_a;
        }
        // 2. Otherwise, if block is "end", then align element edge B with scrolling box edge B.
        else if (block == Element::ScrollLogicalPosition::End) {
            y += element_edge_b - scrolling_box_edge_b;
        }
        // 3. Otherwise, if block is "center", then align the center of target bounding border box with the center of
        //    scrolling box in scrolling box’s block flow direction.
        else if (block == Element::ScrollLogicalPosition::Center) {
            y += (element_edge_a + element_height / 2) - (scrolling_box_edge_a + scrolling_box_height / 2);
        }
        // 4. Otherwise, block is "nearest":
        else {
            // If element edge A and element edge B are both outside scrolling box edge A and scrolling box edge B
            if (element_edge_a <= scrolling_box_edge_a && element_edge_b >= scrolling_box_edge_b) {
                // Do nothing.
            }
            // If element edge A is outside scrolling box edge A and element height is less than scrolling box height
            // If element edge B is outside scrolling box edge B and element height is greater than scrolling box height
            else if ((element_edge_a <= scrolling_box_edge_a && element_height < scrolling_box_height) || (element_edge_b >= scrolling_box_edge_b && element_height > scrolling_box_height)) {
                // Align element edge A with scrolling box edge A.
                y += element_edge_a - scrolling_box_edge_a;
            }
            // If element edge A is outside scrolling box edge A and element height is greater than scrolling box height
            // If element edge B is outside scrolling box edge B and element height is less than scrolling box height
            else if ((element_edge_b >= scrolling_box_edge_b && element_height < scrolling_box_height) || (element_edge_a <= scrolling_box_edge_a && element_height > scrolling_box_height)) {
                // Align element edge B with scrolling box edge B.
                y += element_edge_b - scrolling_box_edge_b;
            }
        }

        // 5. If inline is "start", then align element edge C with scrolling box edge C.
        if (inline_ == Element::ScrollLogicalPosition::Start) {
            x += element_edge_c - scrolling_box_edge_c;
        }
        // 6. Otherwise, if inline is "end", then align element edge D with scrolling box edge D.
        else if (inline_ == Element::ScrollLogicalPosition::End) {
            x += element_edge_d - scrolling_box_edge_d;
        }
        // 7. Otherwise, if inline is "center", then align the center of target bounding border box with the center of
        //    scrolling box in scrolling box’s inline base direction.
        else if (inline_ == Element::ScrollLogicalPosition::Center) {
            x += (element_edge_c + element_width / 2) - (scrolling_box_edge_c + scrolling_box_width / 2);
        }
        // 8. Otherwise, inline is "nearest":
        else {
            // If element edge C and element edge D are both outside scrolling box edge C and scrolling box edge D
            if (element_edge_c <= scrolling_box_edge_c && element_edge_d >= scrolling_box_edge_d) {
                // Do nothing.
            }
            // If element edge C is outside scrolling box edge C and element width is less than scrolling box width
            // If element edge D is outside scrolling box edge D and element width is greater than scrolling box width
            else if ((element_edge_c <= scrolling_box_edge_c && element_width < scrolling_box_width) || (element_edge_d >= scrolling_box_edge_d && element_width > scrolling_box_width)) {
                // Align element edge C with scrolling box edge C.
                x += element_edge_c - scrolling_box_edge_c;
            }
            // If element edge C is outside scrolling box edge C and element width is greater than scrolling box width
            // If element edge D is outside scrolling box edge D and element width is less than scrolling box width
            else if ((element_edge_d >= scrolling_box_edge_d && element_width < scrolling_box_width) || (element_edge_c <= scrolling_box_edge_c && element_width > scrolling_box_width)) {
                // Align element edge D with scrolling box edge D.
                x += element_edge_d - scrolling_box_edge_d;
            }
        }

        // FIXME: 9. If target is an Element, and the target element defines some scroll snap positions, then the user
        //           agent must scroll snap the resulting position to one of that element’s scroll snap positions if its
        //           nearest scroll container is a scroll snap container. The user agent may also do this even when the
        //           scroll container has scroll-snap-type: none.

        return CSSPixelPoint { x, y };
    }();

    // 11. Return position.
    return position;
}

// https://drafts.csswg.org/cssom-view-1/#scroll-a-target-into-view
static void scroll_an_element_into_view(Element& target, Element::ScrollBehavior behavior, Element::ScrollLogicalPosition block, Element::ScrollLogicalPosition inline_, GC::Ptr<Element> container, GC::Ptr<WebIDL::Promise> promise)
{
    // 1. Let ancestorPromises be an empty set of Promises.
    GC::RootVector<GC::Ref<WebIDL::Promise>> ancestor_promises;

    // 2. For each ancestor element or viewport that establishes a scrolling box scrolling box, in order of innermost
    //    to outermost scrolling box, run these substeps:
    auto* ancestor = target.parent();
    Vector<Node&> scrolling_boxes;
    while (ancestor) {
        auto const* ancestor_layout_node = ancestor->layout_node();
        if (ancestor_layout_node && Painting::has_committed_box(*ancestor_layout_node) && Painting::has_scrollable_overflow(*ancestor_layout_node))
            scrolling_boxes.append(*ancestor);
        ancestor = ancestor->parent();
    }

    auto target_bounding_border_box = target.get_bounding_client_rect();

    for (auto& scrolling_box : scrolling_boxes) {
        // 1. If the Document associated with target is not same origin with the Document associated with the element
        //    or viewport associated with scrolling box, abort any remaining iteration of this loop.
        if (target.document().origin() != scrolling_box.document().origin())
            break;

        // 2. Let position be the scroll position resulting from running the steps to determine the scroll-into-view
        //    position of target with behavior as the scroll behavior, block as the block flow position, inline as the
        //    inline base direction position and scrolling box as the scrolling box.
        auto position = determine_the_scroll_into_view_position(target, target_bounding_border_box, block, inline_, scrolling_box);

        // AD-HOC: A smooth scroll leaves the scrolling box at its old scroll position while the outer scrolling boxes
        //         are considered, so move the target to where this scroll is going to leave it. The viewport is always
        //         the outermost scrolling box, so its own scroll cannot affect a later iteration.
        if (!scrolling_box.is_document()) {
            if (auto const* layout_node = scrolling_box.layout_node(); layout_node && Painting::has_committed_box(*layout_node)) {
                target_bounding_border_box.translate_by(Painting::scroll_offset(*layout_node) - Painting::clamp_scroll_offset(*layout_node, position));
                auto scrollport_rect = Painting::transform_rect_to_viewport(*layout_node, Painting::absolute_padding_box_rect(*layout_node), Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::No);
                auto visible_rect = target_bounding_border_box.intersected(scrollport_rect);
                if (!visible_rect.is_empty())
                    target_bounding_border_box = visible_rect;
            }
        }

        // 3. If position is not the same as scrolling box’s current scroll position, or scrolling box has an ongoing
        //    smooth scroll,
        // FIXME: Actually check this condition.
        if (true) {
            // -> If scrolling box is associated with an element
            if (auto* element = as_if<Element>(scrolling_box)) {
                if (auto navigable = element->document().navigable())
                    ancestor_promises.append(navigable->perform_a_scroll_of_an_element(*element, position, behavior));
            }
            // -> If scrolling box is associated with a viewport
            else if (scrolling_box.is_document()) {
                // 1. Let document be the viewport’s associated Document.
                auto& document = static_cast<Document&>(scrolling_box);

                // FIXME: 2. Let root element be document’s root element, if there is one, or null otherwise.
                // FIXME: 3. Perform a scroll of the viewport to position, with root element as the associated element and behavior as the scroll behavior.
                //           Add the Promise returned from this step in the set ancestorPromises.
                ancestor_promises.append(document.navigable()->perform_a_scroll_of_the_viewport(position, behavior));
            }
        }

        // 4. If container is not null and either scrolling box is a shadow-including inclusive ancestor of container
        //    or is a viewport whose document is a shadow-including inclusive ancestor of container, abort any
        //    remaining iteration of this loop.
        // NB: Our viewports *are* Documents in the DOM, so both checks are equivalent.
        if (container != nullptr && scrolling_box.is_shadow_including_inclusive_ancestor_of(*container))
            break;
    }

    // 4. Return scrollPromise, and run the remaining steps in parallel.
    // 5. Resolve scrollPromise when all Promises in ancestorPromises have settled.
    if (promise) {
        auto& realm = WebIDL::promise_realm(*promise);
        auto all_promises = WebIDL::get_promise_for_wait_for_all(realm, ancestor_promises.span());
        auto completion_promise = WebIDL::react_to_promise(*all_promises,
            GC::create_function(GC::Heap::the(), [](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
                return JS::js_undefined();
            }),
            nullptr);
        WebIDL::resolve_promise(*promise, completion_promise->promise());
    }
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollintoview
void Element::scroll_into_view(Element::ScrollIntoViewOptions const& options, GC::Ptr<WebIDL::Promise> promise)
{
    // 1. Let behavior be "auto".
    auto behavior = options.behavior;

    // 2. Let block be "start".
    auto block = options.block;

    // 3. Let inline be "nearest".
    auto inline_ = options.inline_;

    // 4. Let container be null.
    GC::Ptr<Element> container = nullptr;

    // 5. If the container dictionary member of options is "nearest", set container to the element.
    if (options.container == Element::ScrollIntoViewContainer::Nearest)
        container = this;

    // 7. If the element does not have any associated box, or is not available to user-agent features, then return a
    //    resolved Promise and abort the remaining steps.
    document().update_layout(UpdateLayoutReason::ElementScrollIntoView);
    HTML::TemporaryExecutionContext temporary_execution_context { document().relevant_settings_object() };
    if (!layout_node()) {
        if (promise)
            WebIDL::resolve_promise(*promise);
        return;
    }

    // 8. Scroll the element into view with behavior, block, inline, and container. Let scrollPromise be the Promise
    //    returned from this step.
    scroll_an_element_into_view(*this, behavior, block, inline_, container, promise);

    // FIXME: 9. Optionally perform some other action that brings the element to the user’s attention.
}

void Element::scroll_into_view(Variant<bool, ScrollIntoViewOptions> const& arg, GC::Ptr<WebIDL::Promise> promise)
{
    if (arg.has<bool>()) {
        ScrollIntoViewOptions options;
        if (!arg.get<bool>())
            options.block = ScrollLogicalPosition::End;
        scroll_into_view(options, promise);
        return;
    }

    scroll_into_view(arg.get<ScrollIntoViewOptions>(), promise);
}

#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute)                  \
    Optional<Utf16String> Element::name() const                      \
    {                                                                \
        return get_attribute(ARIA::AttributeNames::name);            \
    }                                                                \
                                                                     \
    void Element::set_##name(Optional<Utf16String> const& value)     \
    {                                                                \
        if (value.has_value())                                       \
            set_attribute_value(ARIA::AttributeNames::name, *value); \
        else                                                         \
            remove_attribute(ARIA::AttributeNames::name);            \
    }
ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

bool Element::is_hidden() const
{
    if (layout_node() == nullptr)
        return true;
    if (layout_node()->visibility() == CSS::Visibility::Hidden || layout_node()->visibility() == CSS::Visibility::Collapse || layout_node()->content_visibility() == CSS::ContentVisibility::Hidden)
        return true;
    for (ParentNode const* self_or_ancestor = this; self_or_ancestor; self_or_ancestor = self_or_ancestor->parent_or_shadow_host()) {
        if (self_or_ancestor->is_element()) {
            auto aria_hidden = static_cast<DOM::Element const*>(self_or_ancestor)->aria_hidden();
            if (aria_hidden.has_value() && aria_hidden->utf16_view() == u"true"sv)
                return true;
        }
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
        auto id_view = id()->view();
        root().for_each_in_subtree_of_type<HTML::HTMLElement>([&](auto& element) {
            auto aria_data = MUST(Web::ARIA::AriaData::build_data(element));
            for (auto const& id_reference : aria_data->aria_labelled_by_or_default()) {
                if (id_reference.utf16_view() != id_view)
                    continue;

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
    auto aria_hidden = this->aria_hidden();
    if ((role_or_default().has_value() || has_global_aria_attribute()) && (!aria_hidden.has_value() || aria_hidden->utf16_view() != u"true"sv))
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
        HTML::queue_a_microtask(&document(), GC::create_function(GC::Heap::the(), [this]() {
            auto& reactions_stack = HTML::relevant_similar_origin_window_agent(*this).custom_element_reactions_stack;

            // 1. Invoke custom element reactions in reactionsStack's backup element queue.
            HTML::invoke_custom_element_reactions(reactions_stack.backup_element_queue);

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
void Element::enqueue_a_custom_element_callback_reaction(Utf16FlyString const& callback_name)
{
    enqueue_a_custom_element_callback_reaction(callback_name, Empty {});
}

void Element::enqueue_an_adopted_callback_reaction(Document& old_document, Document& new_document)
{
    enqueue_a_custom_element_callback_reaction(HTML::CustomElementReactionNames::adoptedCallback, CustomElementAdoptedCallbackReactionArguments {
                                                                                                      .old_document = old_document,
                                                                                                      .new_document = new_document,
                                                                                                  });
}

void Element::enqueue_an_attribute_changed_callback_reaction(Utf16FlyString const& attribute_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, Optional<Utf16FlyString> const& namespace_uri)
{
    enqueue_a_custom_element_callback_reaction(HTML::CustomElementReactionNames::attributeChangedCallback, CustomElementAttributeChangedCallbackReactionArguments {
                                                                                                               .attribute_name = attribute_name,
                                                                                                               .old_value = old_value,
                                                                                                               .new_value = new_value,
                                                                                                               .namespace_uri = namespace_uri,
                                                                                                           });
}

void Element::enqueue_a_form_associated_callback_reaction(GC::Ptr<HTML::HTMLFormElement> form)
{
    enqueue_a_custom_element_callback_reaction(HTML::CustomElementReactionNames::formAssociatedCallback, CustomElementFormAssociatedCallbackReactionArguments {
                                                                                                             .form = form,
                                                                                                         });
}

void Element::enqueue_a_form_disabled_callback_reaction(bool is_disabled)
{
    enqueue_a_custom_element_callback_reaction(HTML::CustomElementReactionNames::formDisabledCallback, CustomElementFormDisabledCallbackReactionArguments {
                                                                                                           .is_disabled = is_disabled,
                                                                                                       });
}

GC::Ptr<HTML::CustomElementDefinition> Element::custom_element_definition() const
{
    auto const* rare_data = element_rare_data();
    return rare_data ? rare_data->custom_element_definition : nullptr;
}

void Element::set_custom_element_definition(GC::Ptr<HTML::CustomElementDefinition> definition)
{
    auto old_definition = custom_element_definition();
    bool was_form_associated = old_definition && old_definition->form_associated();
    bool is_form_associated = definition && definition->form_associated();

    if (!definition) {
        if (auto* rare_data = element_rare_data())
            rare_data->custom_element_definition = nullptr;
    } else {
        ensure_element_rare_data().custom_element_definition = definition;
    }

    if (was_form_associated != is_form_associated)
        document().bump_form_controls_version();
}

GC::Ptr<HTML::CustomElementRegistry> Element::custom_element_registry() const
{
    if (m_uses_document_global_custom_element_registry)
        return document().effective_global_custom_element_registry();
    auto const* rare_data = element_rare_data();
    return rare_data ? rare_data->custom_element_registry : nullptr;
}

void Element::set_custom_element_registry(GC::Ptr<HTML::CustomElementRegistry> registry)
{
    if (HTML::is_a_global_custom_element_registry(registry) && registry == document().custom_element_registry()) {
        m_uses_document_global_custom_element_registry = true;
        if (auto* rare_data = element_rare_data())
            rare_data->custom_element_registry = nullptr;
        return;
    }

    m_uses_document_global_custom_element_registry = false;
    if (!registry) {
        if (auto* rare_data = element_rare_data())
            rare_data->custom_element_registry = nullptr;
        return;
    }
    ensure_element_rare_data().custom_element_registry = registry;
}

void Element::enqueue_a_custom_element_callback_reaction(Utf16FlyString const& callback_name, CustomElementCallbackReactionArguments arguments)
{
    // 1. Let definition be element's custom element definition.
    auto definition = custom_element_definition();
    VERIFY(definition);

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

        // 4. Set callback to the following steps.
        ensure_custom_element_reaction_queue().append(CustomElementConnectedMoveCallbackReaction {
            .disconnected_callback = disconnected_callback,
            .connected_callback = connected_callback,
        });
        enqueue_an_element_on_the_appropriate_element_queue();
        return;
    }

    // 3. If callback is null, then return.
    if (!callback)
        return;

    // 5. If callbackName is "attributeChangedCallback":
    if (callback_name == HTML::CustomElementReactionNames::attributeChangedCallback) {
        // 1. Let attributeName be the first element of args.
        VERIFY(arguments.has<CustomElementAttributeChangedCallbackReactionArguments>());
        auto const& attribute_name = arguments.get<CustomElementAttributeChangedCallbackReactionArguments>().attribute_name;

        // 2. If definition's observed attributes does not contain attributeName, then return.
        if (!definition->observed_attributes().contains_slow(attribute_name))
            return;
    }

    // 6. Add a new callback reaction to element's custom element reaction queue, with callback function callback and arguments args.
    ensure_custom_element_reaction_queue().append(CustomElementCallbackReaction { .callback = *callback, .arguments = move(arguments) });

    // 7. Enqueue an element on the appropriate element queue given element.
    enqueue_an_element_on_the_appropriate_element_queue();
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#concept-try-upgrade
void Element::try_to_upgrade()
{
    // 1. Let definition be the result of looking up a custom element definition given element's custom element
    //    registry, element's namespace, element's local name, and element's is value.
    auto definition = HTML::look_up_a_custom_element_definition(custom_element_registry(), namespace_uri(), local_name(), is_value());

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
    // Several custom element states are `:defined` and several are not, so the state moving is not
    // the same as `:defined` moving. Saying it moved when it did not has the engine journal a change
    // from a value the element never held, and the real change later cancels against it.
    auto was_defined = is_defined();
    m_custom_element_state = state;
    if (was_defined == is_defined())
        return;

    CSS::Invalidation::invalidate_style_after_custom_element_state_change(*this);
}

void Element::clear_custom_element_reaction_queue()
{
    if (auto* rare_data = element_rare_data(); rare_data && rare_data->custom_element_reaction_queue)
        rare_data->custom_element_reaction_queue->clear();
}

// https://html.spec.whatwg.org/multipage/dom.html#html-element-constructors
void Element::setup_custom_element_from_constructor(HTML::CustomElementDefinition& custom_element_definition, Optional<Utf16FlyString> const& is_value)
{
    // 7.6. Set element's custom element state to "custom".
    set_custom_element_state(CustomElementState::Custom);

    // 7.7. Set element's custom element definition to definition.
    set_custom_element_definition(custom_element_definition);

    // 7.8. Set element's is value to is value.
    set_is_value(is_value);
}

void Element::set_prefix(Optional<Utf16FlyString> value)
{
    m_qualified_name.set_prefix(move(value));
    if (auto* rare_data = element_rare_data())
        rare_data->html_uppercased_qualified_name.clear();
}

// https://dom.spec.whatwg.org/#locate-a-namespace-prefix
Optional<Utf16String> Element::locate_a_namespace_prefix(Optional<Utf16View> namespace_) const
{
    // 1. If element’s namespace is namespace and its namespace prefix is non-null, then return its namespace prefix.
    if (this->namespace_uri().has_value() && namespace_.has_value() && this->namespace_uri()->view() == *namespace_ && this->prefix().has_value())
        return this->prefix()->to_utf16_string();

    // 2. If element has an attribute whose namespace prefix is "xmlns" and value is namespace, then return element’s first such attribute’s local name.
    if (namespace_.has_value()) {
        Optional<Utf16String> matching_prefix;
        for_each_attribute([&](QualifiedName name, Utf16String value) {
            if (!matching_prefix.has_value() && name.prefix() == u"xmlns"sv && value == *namespace_)
                matching_prefix = name.local_name().to_utf16_string();
        });
        if (matching_prefix.has_value())
            return matching_prefix;
    }

    // 3. If element’s parent element is not null, then return the result of running locate a namespace prefix on that element using namespace.
    if (auto parent = this->parent_element())
        return parent->locate_a_namespace_prefix(namespace_);

    // 4. Return null
    return {};
}

void Element::for_each_attribute(Function<void(Attr&)> callback)
{
    synchronize_all_attributes();
    if (!m_attributes)
        return;
    auto attribute_map = attributes();
    for (size_t i = 0; i < m_attributes->size(); ++i)
        callback(*attribute_map->item(i));
}

void Element::for_each_attribute(Function<void(Attr const&)> callback) const
{
    synchronize_all_attributes();
    if (!m_attributes)
        return;
    auto attribute_map = attributes();
    for (size_t i = 0; i < m_attributes->size(); ++i)
        callback(*attribute_map->item(i));
}

void Element::for_each_attribute(Function<void(Utf16FlyString, Utf16String)> callback) const
{
    synchronize_all_attributes();
    if (!m_attributes)
        return;
    for (size_t index = 0; index < m_attributes->size(); ++index) {
        auto name = m_attributes->at(index).name.as_string();
        auto value = m_attributes->at(index).value;
        callback(move(name), move(value));
    }
}

void Element::for_each_attribute(Function<void(QualifiedName, Utf16String)> callback) const
{
    synchronize_all_attributes();
    if (!m_attributes)
        return;
    for (size_t index = 0; index < m_attributes->size(); ++index) {
        auto name = m_attributes->at(index).name;
        auto value = m_attributes->at(index).value;
        callback(move(name), move(value));
    }
}

Layout::NodeWithStyle* Element::layout_node()
{
    return static_cast<Layout::NodeWithStyle*>(Node::layout_node());
}

Layout::NodeWithStyle const* Element::layout_node() const
{
    return static_cast<Layout::NodeWithStyle const*>(Node::layout_node());
}

Layout::NodeWithStyle* Element::unsafe_layout_node()
{
    return static_cast<Layout::NodeWithStyle*>(Node::unsafe_layout_node());
}

Layout::NodeWithStyle const* Element::unsafe_layout_node() const
{
    return static_cast<Layout::NodeWithStyle const*>(Node::unsafe_layout_node());
}

bool Element::has_attributes() const
{
    synchronize_all_attributes();
    return m_attributes && !m_attributes->is_empty();
}

size_t Element::attribute_list_size() const
{
    synchronize_all_attributes();
    return m_attributes ? m_attributes->size() : 0;
}

CSS::ComputedStyleRecordView Element::computed_style(Optional<CSS::PseudoElement> pseudo_element_type) const
{
    return document().style_computer().computed_style_record_view(style_record_identity(pseudo_element_type));
}

CSS::StyleRecordID Element::style_record_identity(Optional<CSS::PseudoElement> pseudo_element_type) const
{
    if (pseudo_element_type.has_value()) {
        if (auto pseudo_element = get_pseudo_element(*pseudo_element_type); pseudo_element.has_value())
            return pseudo_element->style_record_identity();
        return 0;
    }
    return m_style_record_identity;
}

void const* Element::style_record_payloads(Optional<CSS::PseudoElement> pseudo_element_type) const
{
    return document().style_computer().style_record_payloads(style_record_identity(pseudo_element_type));
}

void Element::update_animated_properties(Badge<Web::Animations::KeyframeEffect> const& badge, Optional<CSS::PseudoElement> pseudo_element_type, Web::Animations::KeyframeEffect& effect, Web::Animations::AnimationUpdateContext& context)
{
    DOM::AbstractElement abstract_element { *this, pseudo_element_type };
    if (pseudo_element_type.has_value()) {
        if (auto pseudo_element = get_pseudo_element(*pseudo_element_type); pseudo_element.has_value())
            pseudo_element->update_animated_properties(badge, abstract_element, effect, context);
        return;
    }

    update_animated_properties_for_abstract_element(badge, abstract_element, effect, context);
}

void Element::update_animated_properties_for_abstract_element(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement abstract_element, Web::Animations::KeyframeEffect& effect, Web::Animations::AnimationUpdateContext& context)
{
    if (!has_style())
        return;
    effect.update_computed_properties_for_style(context, abstract_element);
}

void Element::replace_style_record(CSS::StyleRecordID style_record_identity)
{
    VERIFY(!style_record_identity || style_node_id() != 0);
    auto old_style_record_identity = m_style_record_identity;
    if (old_style_record_identity == style_record_identity)
        return;
    m_style_record_identity = style_record_identity;
    if (auto* layout_node = unsafe_layout_node())
        layout_node->set_style_record_identity(style_record_identity);
}

void Element::set_computed_style(Optional<CSS::PseudoElement> pseudo_element_type, CSS::StyleRecordID style_record_identity)
{
    if (pseudo_element_type.has_value()) {
        VERIFY(is_synthetic_pseudo_element(*pseudo_element_type));
        if (!!style_record_identity)
            ensure_synthetic_pseudo_element(*pseudo_element_type).set_computed_style(style_record_identity);
        else if (auto existing_pseudo_element = get_synthetic_pseudo_element(*pseudo_element_type); existing_pseudo_element.has_value())
            existing_pseudo_element->set_computed_style(0);
        return;
    }
    replace_style_record(style_record_identity);
    computed_properties_changed();
}

void Element::refresh_computed_style(Optional<CSS::PseudoElement> pseudo_element_type, CSS::StyleRecordID style_record_identity)
{
    VERIFY(style_node_id() == 0 || !!style_record_identity);
    if (pseudo_element_type.has_value()) {
        if (CSS::is_element_reference_pseudo_element(*pseudo_element_type)) {
            auto pseudo_element = get_pseudo_element(*pseudo_element_type);
            VERIFY(pseudo_element.has_value());
            auto& referenced_element = as<ElementReferencePseudoElement>(*pseudo_element).referenced_element();
            referenced_element->refresh_computed_style({}, style_record_identity);
            return;
        }
        auto pseudo_element = get_synthetic_pseudo_element(*pseudo_element_type);
        VERIFY(pseudo_element.has_value());
        pseudo_element->refresh_computed_style(style_record_identity);
        return;
    }

    replace_style_record(style_record_identity);
    VERIFY(has_style());
}

void Element::set_associated_shadow_host_pseudo_element(CSS::PseudoElement type)
{
    VERIFY(CSS::is_element_reference_pseudo_element(type));

    auto& root = this->root();
    VERIFY(is<ShadowRoot>(root));

    auto& shadow_root = as<ShadowRoot>(root);
    VERIFY(shadow_root.host());

    shadow_root.host()->register_element_reference_pseudo_element(type, *this);

    ensure_element_rare_data().associated_shadow_host_pseudo_element = type;
}

Optional<CSS::PseudoElement> Element::associated_shadow_host_pseudo_element() const
{
    auto const* rare_data = element_rare_data();
    if (!rare_data)
        return {};
    return rare_data->associated_shadow_host_pseudo_element;
}

Optional<SyntheticPseudoElement&> Element::get_synthetic_pseudo_element(CSS::PseudoElement type) const
{
    VERIFY(is_synthetic_pseudo_element(type));

    auto pseudo_element = get_pseudo_element(type);

    if (!pseudo_element.has_value())
        return {};

    return as<SyntheticPseudoElement>(pseudo_element.value());
}

Optional<PseudoElement&> Element::get_pseudo_element(CSS::PseudoElement type) const
{
    auto const* pseudo_element_data = this->pseudo_element_data();
    if (!pseudo_element_data)
        return {};

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(type)) {
        return {};
    }

    auto pseudo_element = pseudo_element_data->get(type);
    if (!pseudo_element.has_value())
        return {};

    return *(pseudo_element.value());
}

void Element::register_element_reference_pseudo_element(CSS::PseudoElement type, GC::Ref<Element> element)
{
    VERIFY(CSS::is_element_reference_pseudo_element(type));

    auto& pseudo_element_data = ensure_element_rare_data().pseudo_element_data;
    if (!pseudo_element_data)
        pseudo_element_data = make<PseudoElementData>();

    pseudo_element_data->set(type, GC::Heap::the().allocate<ElementReferencePseudoElement>(element));
}

void Element::clear_element_reference_pseudo_elements()
{
    auto* pseudo_element_data = this->pseudo_element_data();
    if (!pseudo_element_data)
        return;

    for (auto i = to_underlying(CSS::first_element_reference_pseudo_element); i <= to_underlying(CSS::last_element_reference_pseudo_element); ++i)
        pseudo_element_data->remove(static_cast<CSS::PseudoElement>(i));
}

SyntheticPseudoElement& Element::ensure_synthetic_pseudo_element(CSS::PseudoElement type) const
{
    auto& pseudo_element_data = ensure_element_rare_data().pseudo_element_data;
    if (!pseudo_element_data)
        pseudo_element_data = make<PseudoElementData>();

    VERIFY(CSS::is_synthetic_pseudo_element(type));

    if (!pseudo_element_data->get(type).has_value()) {
        if (is_pseudo_element_root(type))
            pseudo_element_data->set(type, heap().allocate<SyntheticPseudoElementTreeNode>(const_cast<Element&>(*this)));
        else
            pseudo_element_data->set(type, heap().allocate<SyntheticPseudoElement>(const_cast<Element&>(*this)));
    }

    return as<SyntheticPseudoElement>(*pseudo_element_data->get(type).value());
}

void Element::set_custom_property_data(Optional<CSS::PseudoElement> pseudo_element, RefPtr<CSS::CustomPropertyData const> data)
{
    if (!pseudo_element.has_value()) {
        m_custom_property_data = move(data);
        return;
    }

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value()))
        return;

    if (data) {
        if (is_synthetic_pseudo_element(pseudo_element.value())) {
            ensure_synthetic_pseudo_element(pseudo_element.value()).set_custom_property_data(move(data));
        } else {
            if (auto existing_pseudo_element = get_pseudo_element(pseudo_element.value()); existing_pseudo_element.has_value())
                existing_pseudo_element->set_custom_property_data(move(data));

            // FIXME: In the case that an originating element doesn't support a given element-reference pseudo-element
            //        we will end up here, we can't create an element-reference pseudo-element on demand to store the
            //        custom property data so we just ignore it.
            //
            //        The issue with this is it means the relevant custom properties aren't included in
            //        getComputedStyle, which would be fixed if we stored CustomPropertyData on the computed style
            //        instead of on the Element/PseudoElement directly. Chrome displays this same (presumably broken)
            //        behavior whereas Firefox includes the properties in getComputedStyle.
        }

    } else if (auto existing_pseudo_element = get_pseudo_element(pseudo_element.value()); existing_pseudo_element.has_value())
        existing_pseudo_element->set_custom_property_data({});
}

RefPtr<CSS::CustomPropertyData const> Element::custom_property_data(Optional<CSS::PseudoElement> pseudo_element) const
{
    if (!pseudo_element.has_value())
        return m_custom_property_data;

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value()))
        return nullptr;

    if (auto existing_pseudo_element = get_pseudo_element(pseudo_element.value()); existing_pseudo_element.has_value())
        return existing_pseudo_element->custom_property_data();

    return nullptr;
}

bool Element::refresh_inherited_custom_property_data()
{
    RefPtr<CSS::CustomPropertyData const> parent_data;
    if (auto inherit_from = element_to_inherit_style_from({})) {
        if (auto data = inherit_from->custom_property_data({}))
            parent_data = data->inheritable(document());
    }

    if (m_custom_property_data == parent_data)
        return false;
    m_custom_property_data = move(parent_data);
    return true;
}

// https://drafts.csswg.org/cssom-view/#dom-element-scroll
void Element::scroll(double x, double y, GC::Ptr<WebIDL::Promise> promise)
{
    // 1. If invoked with one argument, follow these substeps:
    //    NOTE: Not relevant here.
    // 2. If invoked with two arguments, follow these substeps:
    //     1. Let options be null converted to a ScrollToOptions dictionary. [WEBIDL]
    //     2. Let x and y be the arguments, respectively.
    //     3. Normalize non-finite values for x and y.
    //     4. Let the left dictionary member of options have the value x.
    //     5. Let the top dictionary member of options have the value y.
    Bindings::ScrollToOptions options;
    options.left = HTML::normalize_non_finite_values(x);
    options.top = HTML::normalize_non_finite_values(y);
    scroll(options, promise);
}

// https://drafts.csswg.org/cssom-view/#dom-element-scroll
void Element::scroll(Bindings::ScrollToOptions options, GC::Ptr<WebIDL::Promise> promise, Optional<CSSPixelPoint> relative_displacement)
{
    // 1. If invoked with one argument, follow these substeps:
    //     1. Let options be the argument.
    //     2. Normalize non-finite values for left and top dictionary members of options, if present.
    //     3. Let x be the value of the left dictionary member of options, if present, or the element’s current scroll position on the x axis otherwise.
    //     4. Let y be the value of the top dictionary member of options, if present, or the element’s current scroll position on the y axis otherwise.
    auto x = options.left.has_value() ? HTML::normalize_non_finite_values(options.left.value()) : scroll_left();
    auto y = options.top.has_value() ? HTML::normalize_non_finite_values(options.top.value()) : scroll_top();

    // 3. Let document be the element’s node document.
    auto& document = this->document();

    // 4. If document is not the active document, return a resolved Promise and abort the remaining steps.
    if (!document.is_active()) {
        if (promise)
            WebIDL::resolve_promise(*promise);
        return;
    }

    // 5. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 6. If window is null, return a resolved Promise and abort the remaining steps.
    if (!window) {
        if (promise)
            WebIDL::resolve_promise(*promise);
        return;
    }

    // 7. If the element is the root element and document is in quirks mode, return a resolved Promise and abort the
    //    remaining steps.
    if (document.document_element() == this && document.in_quirks_mode()) {
        if (promise)
            WebIDL::resolve_promise(*promise);
        return;
    }

    // OPTIMIZATION: Scrolling an unscrolled element to (0, 0) is a no-op as long
    //               as the element is not eligible to be the Document.scrollingElement.
    if (x == 0
        && y == 0
        && scroll_offset({}).is_zero()
        && this != document.body()
        && this != document.document_element()) {
        document.update_style();
        auto const* misc_reset_values = style_group<CSS::ComputedValues::MiscResetValues>();
        if (!misc_reset_values || misc_reset_values->scroll_snap_type_value().strictness == CSS::ScrollSnapStrictness::None) {
            if (promise)
                WebIDL::resolve_promise(*promise);
            return;
        }
    }

    // NB: Ensure that layout is up-to-date before looking at metrics.
    document.update_layout(UpdateLayoutReason::ElementScroll);

    // 8. If the element is the root element, return the Promise returned by scroll() on window after the method is
    //    invoked with scrollX on window as first argument and y as second argument, and abort the remaining steps.
    if (document.document_element() == this) {
        window->scroll(x, y, promise, relative_displacement);
        return;
    }

    // 9. If the element is the body element, document is in quirks mode, and the element is not potentially
    //    scrollable, return the Promise returned by scroll() on window after the method is invoked with options as the
    //    only argument, and abort the remaining steps.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable()) {
        window->scroll(x, y, promise, relative_displacement);
        return;
    }

    // 10. If the element does not have any associated box, the element has no associated scrolling box, or the element
    //     has no overflow, return a resolved Promise and abort the remaining steps.
    // FIXME: or the element has no overflow
    auto* layout_node = this->layout_node();
    if (!layout_node || !layout_node->is_scroll_container()) {
        if (promise)
            WebIDL::resolve_promise(*promise);
        return;
    }

    // 11. Scroll the element to x,y, with the scroll behavior being the value of the behavior dictionary member of
    //     options. Let scrollPromise be the Promise returned from this step.
    auto scroll_offset = CSSPixelPoint { CSSPixels::nearest_value_for(x), CSSPixels::nearest_value_for(y) };
    if (auto navigable = document.navigable()) {
        auto scroll_promise = navigable->perform_a_scroll_of_an_element(*this, scroll_offset, options.behavior, relative_displacement);
        if (promise)
            WebIDL::resolve_promise(*promise, scroll_promise->promise());
        (void)scroll_promise;
    } else if (promise) {
        WebIDL::resolve_promise(*promise);
    }
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollby
void Element::scroll_by(double x, double y, GC::Ptr<WebIDL::Promise> promise)
{
    // 2. If invoked with two arguments, follow these substeps:
    //    1. Let options be null converted to a ScrollToOptions dictionary. [WEBIDL]
    ScrollToOptions options;

    //    2. Let x and y be the arguments, respectively.
    //    3. Normalize non-finite values for x and y.
    //    4. Let the left dictionary member of options have the value x.
    //    5. Let the top dictionary member of options have the value y.
    // NOTE: Element::scroll_by(ScrollToOptions) performs the normalization and following steps.
    options.left = x;
    options.top = y;
    scroll_by(options, promise);
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollby
void Element::scroll_by(ScrollToOptions options, GC::Ptr<WebIDL::Promise> promise)
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
    CSSPixelPoint relative_displacement { CSSPixels::nearest_value_for(left), CSSPixels::nearest_value_for(top) };
    scroll(options, promise, relative_displacement);
}

// https://drafts.csswg.org/cssom-view-1/#dom-element-checkvisibility
bool Element::check_visibility(CheckVisibilityOptions const& options)
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    document().update_layout_if_needed_for_node(*this, UpdateLayoutReason::ElementCheckVisibility);

    // 1. If this does not have an associated box, return false.
    auto const* layout_node = this->layout_node();
    if (!layout_node || !Painting::has_committed_box(*layout_node))
        return false;

    // 2. If an ancestor of this in the flat tree has content-visibility: hidden, return false.
    for (auto* element = flat_tree_parent_element(); element; element = element->flat_tree_parent_element()) {
        if (static_cast<CSS::ContentVisibility>(element->style_group<CSS::ComputedValues::InheritedBoxValues>()->content_visibility) == CSS::ContentVisibility::Hidden)
            return false;
    }

    // 3. If either the opacityProperty or the checkOpacity dictionary members of options are true, and this, or an
    //    ancestor of this in the flat tree, has a computed opacity value of 0, return false.
    if (options.opacity_property || options.check_opacity) {
        for (auto* element = this; element; element = element->flat_tree_parent_element()) {
            if (element->style_group<CSS::ComputedValues::EffectsValues>()->opacity == 0.0f)
                return false;
        }
    }

    // 4. If either the visibilityProperty or the checkVisibilityCSS dictionary members of options are true, and this
    //    is invisible, return false.
    if (options.visibility_property || options.check_visibility_css) {
        if (static_cast<CSS::Visibility>(style_group<CSS::ComputedValues::InheritedBoxValues>()->visibility) == CSS::Visibility::Hidden)
            return false;
    }

    // 5. If the contentVisibilityAuto dictionary member of options is true and an ancestor of this in the flat tree
    //    skips its contents due to content-visibility: auto, return false.
    // FIXME: Currently we do not skip any content if content-visibility is auto: https://drafts.csswg.org/css-contain-2/#proximity-to-the-viewport
    auto const skipped_contents_due_to_content_visibility_auto = false;
    if (options.content_visibility_auto && skipped_contents_due_to_content_visibility_auto) {
        for (auto* element = flat_tree_parent_element(); element; element = element->flat_tree_parent_element()) {
            if (static_cast<CSS::ContentVisibility>(element->style_group<CSS::ComputedValues::InheritedBoxValues>()->content_visibility) == CSS::ContentVisibility::Auto)
                return false;
        }
    }

    // 6. Return true.
    return true;
}

ProximityToTheViewport Element::proximity_to_the_viewport() const
{
    auto const* rare_data = element_rare_data();
    return rare_data ? rare_data->proximity_to_the_viewport : ProximityToTheViewport::NotDetermined;
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
    if (Painting::absolute_rect(*unsafe_layout_node()).intersects(viewport_rect)) {
        ensure_element_rare_data().proximity_to_the_viewport = ProximityToTheViewport::CloseToTheViewport;
        return;
    }

    // FIXME: If a filter (see [FILTER-EFFECTS-1]) with non local effects includes the element as part of its input, the user
    //        agent should also treat the element as relevant to the user when the filter’s output can affect the rendering
    //        within the viewport (or within the user-agent defined margin around the viewport), even if the element itself is
    //        still off-screen.

    // - The element is far away from the viewport: In this state, the element’s proximity to the viewport has been
    //   computed and is not close to the viewport.
    ensure_element_rare_data().proximity_to_the_viewport = ProximityToTheViewport::FarAwayFromTheViewport;

    // - The element’s proximity to the viewport is not determined: In this state, the computation to determine the
    //   element’s proximity to the viewport has not been done since the last time the element was connected.
    // NOTE: This function is what does the computation to determine the element’s proximity to the viewport, so this is not the case.
}

// https://drafts.csswg.org/css-contain/#relevant-to-the-user
bool Element::is_relevant_to_the_user()
{
    // An element is relevant to the user if any of the following conditions are true:

    // The element is close to the viewport.
    if (proximity_to_the_viewport() == ProximityToTheViewport::CloseToTheViewport)
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

bool Element::captured_in_a_view_transition() const
{
    auto const* rare_data = element_rare_data();
    return rare_data && rare_data->captured_in_a_view_transition;
}

void Element::set_captured_in_a_view_transition(bool value)
{
    if (!value) {
        if (auto* rare_data = element_rare_data())
            rare_data->captured_in_a_view_transition = false;
        return;
    }
    ensure_element_rare_data().captured_in_a_view_transition = true;
}

// https://drafts.csswg.org/css-contain-2/#skips-its-contents
bool Element::skips_its_contents()
{
    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-hidden
    // The element skips its contents.
    auto style = computed_style();
    VERIFY(style);
    if (style->content_visibility() == CSS::ContentVisibility::Hidden)
        return true;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // If the element is not relevant to the user, it also skips its contents.
    if (style->content_visibility() == CSS::ContentVisibility::Auto && !this->is_relevant_to_the_user()) {
        return true;
    }

    return false;
}

void Element::invalidate_list_item_counters_for_list_owner()
{
    for_each_ancestor([](GC::Ref<Node> node) {
        if (node->is_html_ol_ul_menu_element()) {
            static_cast<Element&>(*node).set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::ListItemCounters);
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
}

bool Element::id_reference_exists(Utf16View id_reference) const
{
    return !!document().get_element_by_id(id_reference);
}

void Element::register_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, GC::Ref<IntersectionObserver::IntersectionObserver> observer)
{
    auto& registered_intersection_observers = ensure_element_rare_data().registered_intersection_observers;
    if (!registered_intersection_observers)
        registered_intersection_observers = make<Vector<GC::Ref<IntersectionObserver::IntersectionObserver>>>();
    registered_intersection_observers->append(observer);
}

void Element::unregister_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, GC::Ref<IntersectionObserver::IntersectionObserver> observer)
{
    auto* rare_data = element_rare_data();
    if (!rare_data || !rare_data->registered_intersection_observers)
        return;
    rare_data->registered_intersection_observers->remove_first_matching([&observer](GC::Ref<IntersectionObserver::IntersectionObserver> const& entry) {
        return entry == observer;
    });
}

CSSPixelPoint Element::scroll_offset(Optional<CSS::PseudoElement> pseudo_element_type) const
{
    if (pseudo_element_type.has_value()) {
        if (auto pseudo_element = get_synthetic_pseudo_element(*pseudo_element_type); pseudo_element.has_value())
            return pseudo_element->scroll_offset();
        return {};
    }
    auto const* rare_data = element_rare_data();
    return rare_data ? rare_data->scroll_offset : CSSPixelPoint {};
}

void Element::set_scroll_offset(Optional<CSS::PseudoElement> pseudo_element_type, CSSPixelPoint offset)
{
    if (pseudo_element_type.has_value()) {
        if (auto pseudo_element = get_synthetic_pseudo_element(*pseudo_element_type); pseudo_element.has_value())
            pseudo_element->set_scroll_offset(offset);
    } else {
        if (!offset.is_zero())
            ensure_element_rare_data().scroll_offset = offset;
        else if (auto* rare_data = element_rare_data())
            rare_data->scroll_offset = {};
    }
}

Optional<Element::Dir> Element::dir() const
{
    auto const* rare_data = element_rare_data();
    return rare_data ? rare_data->dir : Optional<Dir> {};
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
    if (maybe_translate_attribute.has_value() && (maybe_translate_attribute.value().is_empty() || maybe_translate_attribute.value().equals_ignoring_ascii_case(u"yes"sv)))
        return TranslationMode::TranslateEnabled;

    // otherwise, if the element's translate attribute is in the No state, then the element's translation mode is in
    // the no-translate state.
    if (maybe_translate_attribute.has_value() && maybe_translate_attribute.value().equals_ignoring_ascii_case(u"no"sv)) {
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

bool Element::has_auto_directionality() const
{
    return dir() == Dir::Auto || (!dir().has_value() && local_name() == HTML::TagNames::bdi);
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
    if (has_auto_directionality()) {
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
        auto const& value = form_associated_element.form_value();

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

static void prefetch_inline_style_image_resources(CSS::CSSStyleProperties const& inline_style, Document& document)
{
    auto load_image_if_needed = [&](CSS::StyleValue const& value) {
        if (value.is_abstract_image())
            const_cast<CSS::AbstractImageStyleValue&>(value.as_abstract_image()).load_any_resources(document);
    };
    for (auto const& property : inline_style.properties()) {
        if (property.value->is_value_list()) {
            for (auto const& item : property.value->as_value_list().values())
                load_image_if_needed(*item);
        } else {
            load_image_if_needed(*property.value);
        }
    }
}

// https://dom.spec.whatwg.org/#concept-element-attributes-change-ext
void Element::attribute_changed(Utf16FlyString const& local_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    // AD-HOC: Everything below requires that there is no namespace, so return early if there is one.
    //         `xml:lang` is the one exception: it is the element's language exactly as a bare `lang`
    //         is, and the style engine is told a language moved by being told, not by asking.
    if (namespace_.has_value()) {
        if (namespace_ == Namespace::XML && local_name == HTML::AttributeNames::lang)
            CSS::Invalidation::invalidate_style_after_language_change(*this);
        return;
    }

    // https://dom.spec.whatwg.org/#ref-for-concept-element-attributes-change-ext①
    // 1. If localName is slot and namespace is null, then:
    if (local_name == HTML::AttributeNames::slot) {
        // 1. If value is oldValue, then return.
        if (value == old_value)
            return;

        // 2. If value is null and oldValue is the empty string, then return.
        if (!value.has_value() && old_value == Utf16String {})
            return;

        // 3. If value is the empty string and oldValue is null, then return.
        if (value == Utf16String {} && !old_value.has_value())
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

    auto value_or_empty = value.has_value() ? value->utf16_view() : u""sv;

    if (local_name == HTML::AttributeNames::id) {
        // StyleEngine keys local features by interned atom, so capture the old side before the
        // element's own copy is replaced.
        auto old_style_engine_id = m_id;

        if (value_or_empty.is_empty())
            m_id = {};
        else
            m_id = Utf16FlyString::from_utf16(value_or_empty);

        if (is_connected()) {
            Optional<Utf16FlyString> old_id;
            if (old_value.has_value())
                old_id = Utf16FlyString::from_utf16(old_value->utf16_view());
            document().element_id_changed({}, *this, old_id);
        }

        CSS::record_element_id_changed(*this, old_style_engine_id, m_id);
    } else if (local_name == HTML::AttributeNames::name) {
        m_has_name = !value_or_empty.is_empty();
        if (m_has_name) {
            ensure_element_rare_data().name = Utf16FlyString::from_utf16(value_or_empty);
        } else if (auto* rare_data = element_rare_data()) {
            rare_data->name = {};
        }

        if (is_connected())
            document().element_name_changed({}, *this);
    } else if (local_name == HTML::AttributeNames::class_) {
        // Elements without a StyleEngine identity have no feature state to update. Avoid copying
        // their class list just for record_element_class_list_changed() to reject the update.
        Optional<Vector<Utf16FlyString>> old_style_engine_classes;
        if (style_node_id().value() != 0)
            old_style_engine_classes = m_classes;

        if (value_or_empty.is_empty()) {
            m_classes.clear();
        } else {
            m_classes.clear();
            for_each_ascii_whitespace_separated_token(value_or_empty, [&](auto new_class) {
                m_classes.append(Utf16FlyString::from_utf16(new_class));
                return IterationDecision::Continue;
            });
        }
        if (auto* rare_data = element_rare_data(); rare_data && rare_data->class_list)
            rare_data->class_list->associated_attribute_changed(value_or_empty);

        if (old_style_engine_classes.has_value())
            CSS::record_element_class_list_changed(*this, *old_style_engine_classes, m_classes);
    } else if (local_name == HTML::AttributeNames::style) {
        // https://drafts.csswg.org/cssom/#ref-for-cssstyledeclaration-updating-flag
        if (m_inline_style && m_inline_style->is_updating())
            return;
        // The new declaration block has not replaced the old one yet, so this is the last point at
        // which a deferred geometry-read boundary can commit its before-change style.
        document().flush_deferred_style_change_event();
        if (!m_inline_style)
            m_inline_style = CSS::CSSStyleProperties::create_element_inline_style({ *this }, {}, {});
        m_inline_style->set_declarations_from_text(value_or_empty);
        prefetch_inline_style_image_resources(*m_inline_style, document());
    } else if (local_name == HTML::AttributeNames::dir || local_name == HTML::AttributeNames::lang) {
        bool const is_dir = local_name == HTML::AttributeNames::dir;
        if (is_dir) {
            // https://html.spec.whatwg.org/multipage/dom.html#attr-dir
            Optional<Dir> dir;
            if (value_or_empty.equals_ignoring_ascii_case(u"ltr"sv))
                dir = Dir::Ltr;
            else if (value_or_empty.equals_ignoring_ascii_case(u"rtl"sv))
                dir = Dir::Rtl;
            else if (value_or_empty.equals_ignoring_ascii_case(u"auto"sv))
                dir = Dir::Auto;

            if (dir.has_value())
                ensure_element_rare_data().dir = dir;
            else if (auto* rare_data = element_rare_data())
                rare_data->dir = {};
        }
        if (is_dir)
            CSS::Invalidation::invalidate_style_after_directionality_change(*this);
        else
            CSS::Invalidation::invalidate_style_after_language_change(*this);
    } else if (local_name == HTML::AttributeNames::part) {
        if (auto* rare_data = element_rare_data())
            rare_data->parts.clear();
        if (!value_or_empty.is_empty()) {
            auto& parts = ensure_element_rare_data().parts;
            auto new_parts = value_or_empty;
            auto append_part = [&](Utf16View new_part) {
                if (!parts.contains_slow(new_part))
                    parts.append(Utf16FlyString::from_utf16(new_part));
            };
            size_t start = 0;
            for (size_t i = 0; i <= new_parts.length_in_code_units(); ++i) {
                if (i != new_parts.length_in_code_units() && !Infra::is_ascii_whitespace(new_parts.code_unit_at(i)))
                    continue;
                if (i > start)
                    append_part(new_parts.substring_view(start, i - start));
                start = i + 1;
            }
        }
        if (auto* rare_data = element_rare_data(); rare_data && rare_data->part_list)
            rare_data->part_list->associated_attribute_changed(value_or_empty);
        CSS::record_element_parts_changed(*this);
    } else if (local_name == HTML::AttributeNames::exportparts) {
        CSS::Invalidation::invalidate_style_after_exportparts_attribute_change(*this);
    }

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#reflecting-content-attributes-in-idl-attributes:concept-element-attributes-change-ext
    // 1. If localName is not attr or namespace is not null, then return.
    // 2. Set element's explicitly set attr-element to null.
    if (local_name.view().starts_with(u"aria-"sv)) {
#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute) \
    if (local_name == ARIA::AttributeNames::referencing_attribute) { \
        set_##attribute({});                                         \
    }
        ENUMERATE_ARIA_ELEMENT_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

        // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#reflecting-content-attributes-in-idl-attributes:concept-element-attributes-change-ext-2
        // 1. If localName is not attr or namespace is not null, then return.
        // 2. Set element's explicitly set attr-elements to null.
#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute) \
    if (local_name == ARIA::AttributeNames::referencing_attribute) { \
        set_##attribute({});                                         \
    }
        ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE
    }
}

Optional<Utf16FlyString> Element::name() const
{
    if (!m_has_name)
        return {};
    auto const* rare_data = element_rare_data();
    VERIFY(rare_data);
    VERIFY(rare_data->name.has_value());
    return rare_data->name;
}

auto Element::ensure_custom_element_reaction_queue() -> CustomElementReactionQueue&
{
    auto& custom_element_reaction_queue = ensure_element_rare_data().custom_element_reaction_queue;
    if (!custom_element_reaction_queue)
        custom_element_reaction_queue = make<CustomElementReactionQueue>();
    return *custom_element_reaction_queue;
}

auto Element::custom_element_reaction_queue() -> CustomElementReactionQueue*
{
    auto* rare_data = element_rare_data();
    return rare_data ? rare_data->custom_element_reaction_queue.ptr() : nullptr;
}

auto Element::custom_element_reaction_queue() const -> CustomElementReactionQueue const*
{
    auto const* rare_data = element_rare_data();
    return rare_data ? rare_data->custom_element_reaction_queue.ptr() : nullptr;
}

HTML::CustomStateSet& Element::ensure_custom_state_set()
{
    auto& custom_state_set = ensure_element_rare_data().custom_state_set;
    if (!custom_state_set)
        custom_state_set = HTML::CustomStateSet::create(*this);
    return *custom_state_set;
}

GC::Ptr<HTML::CustomStateSet const> Element::custom_state_set() const
{
    auto const* rare_data = element_rare_data();
    return rare_data ? rare_data->custom_state_set : nullptr;
}

Optional<Utf16FlyString> const& Element::is_value() const
{
    static NeverDestroyed<Optional<Utf16FlyString>> empty_is_value;
    auto const* rare_data = element_rare_data();
    return rare_data ? rare_data->is_value : *empty_is_value;
}

void Element::set_is_value(Optional<Utf16FlyString> const& is)
{
    if (is.has_value())
        ensure_element_rare_data().is_value = is;
    else if (auto* rare_data = element_rare_data())
        rare_data->is_value.clear();
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
WebIDL::ExceptionOr<Utf16String> Element::get_html(HTMLSerializationOptions const& options) const
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
WebIDL::ExceptionOr<void> Element::set_html_unsafe(StringView html)
{
    // 2. Let target be this's template contents if this is a template element; otherwise this.
    Variant<GC::Ref<DOM::Element>, GC::Ref<DOM::DocumentFragment>> target = GC::Ref { *this };
    if (is<HTML::HTMLTemplateElement>(*this))
        target = as<HTML::HTMLTemplateElement>(*this).content();

    // 3. Unsafe set HTML given target, this, and compliantHTML.
    auto markup = Utf16String::from_utf8(html);
    TRY(unsafely_set_html(move(target), markup.utf16_view()));

    return {};
}

Optional<CSS::CountersSet const&> Element::counters_set() const
{
    auto const* rare_data = element_rare_data();
    if (!rare_data || !rare_data->counters_set)
        return {};
    return *rare_data->counters_set;
}

CSS::CountersSet& Element::ensure_counters_set()
{
    auto& counters_set = ensure_element_rare_data().counters_set;
    if (!counters_set)
        counters_set = make<CSS::CountersSet>();
    return *counters_set;
}

void Element::set_counters_set(OwnPtr<CSS::CountersSet>&& counters_set)
{
    if (counters_set)
        ensure_element_rare_data().counters_set = move(counters_set);
    else if (auto* rare_data = element_rare_data())
        rare_data->counters_set = nullptr;
}

bool Element::has_non_empty_counters_set() const
{
    auto const* rare_data = element_rare_data();
    return rare_data && rare_data->counters_set;
}

// https://html.spec.whatwg.org/multipage/dom.html#the-lang-and-xml:lang-attributes
Optional<Utf16String> Element::lang() const
{
    auto determine_lang_attribute = [&]() -> Utf16String {
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
        if (auto* parent = this->parent(); parent && parent->is_shadow_root()) {
            if (auto language = static_cast<ShadowRoot const&>(*parent).host()->lang(); language.has_value())
                return language.release_value();
            return {};
        }

        // 4. If the node's parent element is not null
        //      Use the language of that parent element.
        if (auto parent = parent_element()) {
            if (auto language = parent->lang(); language.has_value())
                return language.release_value();
            return {};
        }

        // 5. Otherwise
        //      - If there is a pragma-set default language set, then that is the language of the node.
        if (auto language = document().pragma_set_default_language(); language.has_value())
            return language.release_value();

        //      - If there is no pragma-set default language set, then language information from a higher-level protocol (such as HTTP),
        if (auto language = document().http_content_language(); language.has_value())
            return language.release_value();

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

Optional<Utf16View> Element::lang_view() const
{
    if (!m_lang_value.has_value())
        (void)lang();
    if (m_lang_value->is_empty())
        return {};
    return m_lang_value->utf16_view();
}

void Element::invalidate_lang_value()
{
    m_lang_value.clear();
}

// https://drafts.csswg.org/css-images-4/#element-not-rendered
bool Element::not_rendered() const
{
    // An element is not rendered if it does not have an associated box.
    auto const* layout_node = this->layout_node();
    if (!layout_node || !Painting::has_committed_box(*layout_node))
        return true;

    return false;
}

bool Element::meets_focusable_area_rendering_requirements() const
{
    // https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
    // Elements can only be focusable areas if they are being rendered, delegating their rendering to
    // their children, or being used as relevant canvas fallback content. Reject display: none
    // subtrees without forcing layout; display: contents is intentionally allowed by this check.
    if (!is_connected())
        return false;

    return const_cast<Element&>(*this).document().update_style_for_element(AbstractElement { *this }, Document::StyleUpdateMode::StopAtDisplayNone);
}

// https://drafts.csswg.org/css-view-transitions-1/#document-scoped-view-transition-name
Optional<Utf16FlyString> Element::document_scoped_view_transition_name()
{
    // To get the document-scoped view transition name for an Element element:

    // 1. Let scopedViewTransitionName be the computed value of view-transition-name for element.
    auto const* values = style_group<CSS::ComputedValues::MiscResetValues>();
    VERIFY(values);
    auto scoped_view_transition_name = values->view_transition_name_value();

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
Optional<Gfx::DecodedImageFrame> Element::capture_the_image()
{
    // FIXME: Actually implement this.
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, Gfx::IntSize(1, 1)));
    return Gfx::DecodedImageFrame { *bitmap };
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
    auto& attribute_map = ensure_element_rare_data().attribute_map;
    if (!attribute_map)
        attribute_map = NamedNodeMap::create(*this);
    return attribute_map;
}

GC::Ptr<NamedNodeMap const> Element::attributes() const
{
    return const_cast<Element&>(*this).attributes();
}

Utf16FlyString const& Element::html_uppercased_qualified_name() const
{
    return ensure_element_rare_data().html_uppercased_qualified_name.ensure([&] { return make_html_uppercased_qualified_name(); });
}

void Element::play_or_cancel_animations_after_display_property_change()
{
    // OPTIMIZATION: We don't care about elements with no CSS defined animations
    if (!has_css_defined_animations())
        return;

    // OPTIMIZATION: We don't care about animations in disconnected subtrees.
    if (!is_connected())
        return;

    // https://drafts.csswg.org/css-animations-2/#owning-element-section
    // If the display property of an element is set to none and its display value would compute to none when ignoring
    // the Transitions and Animations cascade origins, then terminate running animations with this owning element. If
    // an element has a display of none and its display value had computed to none when ignoring the Transitions and
    // Animations cascade origins, updating display to a value other than none will start all animations applied to
    // the element by the animation-name property.

    // FIXME: What does it mean that "is set to none" is separate from "would compute to none...", do we need to check
    //        the cascaded value as well?

    // AD-HOC: Other browsers also check for ancestors which meet the above criteria, so we do that as well.
    // FIXME: Do we need to open a spec issue for this?

    auto has_inclusive_ancestor_with_display_none_ignoring_animations = this->has_inclusive_ancestor_with_display_none_ignoring_animations();

    auto play_or_cancel_depending_on_display = [&](Vector<GC::Ref<CSS::CSSAnimation>> const& animations) {
        for (auto& animation : animations) {
            if (has_inclusive_ancestor_with_display_none_ignoring_animations) {
                animation->cancel();
            } else {
                // NOTE: It is safe to assume this has a value as it is set when creating a CSS defined animation
                auto play_state = animation->last_css_animation_play_state().value();

                if (play_state == CSS::AnimationPlayState::Running) {
                    HTML::TemporaryExecutionContext context(document().relevant_settings_object());
                    animation->play_from_css();
                } else if (play_state == CSS::AnimationPlayState::Paused) {
                    HTML::TemporaryExecutionContext context(document().relevant_settings_object());
                    animation->pause_from_css();
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
    return HTML::parse_integer(attribute(HTML::AttributeNames::tabindex).value_or({})).has_value()
        && meets_focusable_area_rendering_requirements();
}

void Element::set_had_duplicate_attribute_during_tokenization(Badge<HTML::HTMLParser>)
{
    ensure_element_rare_data().had_duplicate_attribute_during_tokenization = true;
}

bool Element::had_duplicate_attribute_during_tokenization() const
{
    auto const* rare_data = element_rare_data();
    return rare_data && rare_data->had_duplicate_attribute_during_tokenization;
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
    auto& computed_style_map_cache = ensure_element_rare_data().computed_style_map_cache;
    if (computed_style_map_cache == nullptr) {
        computed_style_map_cache = CSS::StylePropertyMapReadOnly::create_computed_style(AbstractElement { *this });
    }

    // 2. Return this’s [[computedStyleMapCache]] internal slot.
    return *computed_style_map_cache;
}

double Element::ensure_css_random_base_value(CSS::RandomCachingKey const& random_caching_key)
{
    // NB: We cache element-shared random base values on the Document and non-element-shared ones on the Element itself
    //     so that when an element is removed it takes its non-shared cache with it.
    if (!random_caching_key.element_id.has_value())
        return document().ensure_element_shared_css_random_base_value(random_caching_key);

    return ensure_element_rare_data().element_specific_css_random_base_value_cache.ensure(random_caching_key, []() {
        static XorShift128PlusRNG random_number_generator;
        return random_number_generator.get();
    });
}

WebIDL::ExceptionOr<void> Element::request_pointer_lock(PointerLockOptions const&)
{
    dbgln("FIXME: request_pointer_lock()");
    return WebIDL::NotSupportedError::create("request_pointer_lock() is not implemented"_utf16);
}

// The element to inherit style from.
// If a pseudo-element is specified, this will return the element itself.
// Otherwise, if this element is slotted somewhere, it will return the slot.
// Otherwise, it will return the parent or shadow host element of this element.
//
// The slot case is specified in https://drafts.csswg.org/css-shadow-1/#slots-in-shadow-tree:
// NOTE: A non-obvious result of assigning elements to slots is that they inherit from the slot they're assigned to.
//       Their original light tree parent, and any deeper slots that their slot gets assigned to, don't affect
//       inheritance.
// `:fullscreen` is a fact about the element, and the flag is the only thing that decides it.
void Element::set_fullscreen_flag(bool is_fullscreen)
{
    if (m_fullscreen_flag == is_fullscreen)
        return;
    m_fullscreen_flag = is_fullscreen;
    CSS::record_element_state_changed(*this, CSS::PseudoClass::Fullscreen, is_fullscreen);
}

void Element::set_fullscreen_request_type(Fullscreen::RequestType request_type)
{
    if (request_type == Fullscreen::RequestType::Standard) {
        if (auto* rare_data = element_rare_data())
            rare_data->fullscreen_request_type = request_type;
        return;
    }
    ensure_element_rare_data().fullscreen_request_type = request_type;
}

Fullscreen::RequestType Element::fullscreen_request_type() const
{
    auto const* rare_data = element_rare_data();
    return rare_data ? rare_data->fullscreen_request_type : Fullscreen::RequestType::Standard;
}

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

namespace Web::Bindings {

struct PerElementReflectedArrayCache {
    HashMap<FlyString, WrapperWorldWeakValueCache<JS::Array>> arrays_by_attribute;
};

static GC::WeakHashMap<DOM::Element, PerElementReflectedArrayCache>& reflected_element_array_caches()
{
    static NeverDestroyed<GC::WeakHashMap<DOM::Element, PerElementReflectedArrayCache>> caches;
    return *caches;
}

static WrapperWorldWeakValueCache<JS::Array>& reflected_element_array_cache_for(DOM::Element& element, FlyString const& reflected_attribute)
{
    auto& per_element = reflected_element_array_caches().ensure(element);
    return per_element.arrays_by_attribute.ensure(reflected_attribute);
}

GC::Ptr<JS::Array> cached_reflected_element_array(DOM::Element& element, WrapperWorld const& wrapper_world, FlyString const& reflected_attribute)
{
    return reflected_element_array_cache_for(element, reflected_attribute).get(wrapper_world);
}

GC::Ptr<JS::Array> cached_reflected_element_array(DOM::Element& element, WrapperWorld const& wrapper_world, Utf16FlyString const& reflected_attribute)
{
    return cached_reflected_element_array(element, wrapper_world, FlyString { reflected_attribute.to_utf16_string().to_utf8() });
}

void set_cached_reflected_element_array(DOM::Element& element, WrapperWorld const& wrapper_world, FlyString const& reflected_attribute, GC::Ptr<JS::Array> array)
{
    reflected_element_array_cache_for(element, reflected_attribute).set(wrapper_world, array);
}

void set_cached_reflected_element_array(DOM::Element& element, WrapperWorld const& wrapper_world, Utf16FlyString const& reflected_attribute, GC::Ptr<JS::Array> array)
{
    set_cached_reflected_element_array(element, wrapper_world, FlyString { reflected_attribute.to_utf16_string().to_utf8() }, array);
}

// Used by generated [SameObject] cache checks for reflected element arrays.
bool cached_reflected_element_array_contains_same_elements(GC::Ptr<JS::Array> array, Optional<GC::RootVector<GC::Ref<DOM::Element>>> const& elements)
{
    if (!array || !elements.has_value())
        return !array && !elements.has_value();

    bool is_equivalent = array->indexed_array_like_size() == elements->size();

    for (size_t i = 0; is_equivalent && i < elements->size(); ++i) {
        auto cached_value = array->get_without_side_effects(JS::PropertyKey { i });
        auto const* cached_element = element_from_value(cached_value);
        VERIFY(cached_element);

        auto it = elements->find_if([&](auto const& element) { return element.ptr() == cached_element; });
        if (it == elements->end())
            is_equivalent = false;
    }

    return is_equivalent;
}

JS::Value element(JS::Realm& realm, GC::Ref<DOM::Element> element)
{
    return wrap(host_defined_wrapper_world(realm), realm, element);
}

DOM::Element* element_from_value(JS::Value value)
{
    if (!value.is_object())
        return nullptr;
    return Bindings::impl_from<DOM::Element>(&value.as_object());
}

static GC::Ref<Geometry::DOMRect> create_dom_rect(CSSPixelRect const& rect)
{
    return Geometry::DOMRect::create(static_cast<double>(rect.x()), static_cast<double>(rect.y()), static_cast<double>(rect.width()), static_cast<double>(rect.height()));
}

GC::Ref<Geometry::DOMRect> get_bounding_client_rect(DOM::Element const& element)
{
    return create_dom_rect(element.get_bounding_client_rect());
}

GC::Ref<Geometry::DOMRectList> get_client_rects(DOM::Element const& element)
{
    Vector<GC::Root<Geometry::DOMRect>> rects;
    for (auto const& rect : element.get_client_rects())
        rects.append(create_dom_rect(rect));

    return Geometry::DOMRectList::create(move(rects));
}

GC::Ref<WebIDL::Promise> request_pointer_lock(DOM::Element& element, Optional<PointerLockOptions> const& options)
{
    DOM::Element::PointerLockOptions dom_options;
    if (options.has_value()) {
        dom_options = DOM::Element::PointerLockOptions {
            .unadjusted_movement = options->unadjusted_movement,
        };
    }
    auto promise = WebIDL::create_promise_for(element);
    auto result = element.request_pointer_lock(dom_options);
    if (result.is_error())
        WebIDL::reject_promise_with_exception(promise, result.release_error());
    else
        WebIDL::resolve_promise(promise);
    return promise;
}

WebIDL::ExceptionOr<TrustedTypes::TrustedHTMLOrString> inner_html(DOM::Element& element)
{
    return TRY(element.inner_html());
}

WebIDL::ExceptionOr<void> set_inner_html(DOM::Element& element, TrustedTypes::TrustedHTMLOrString const& value)
{
    // 1. Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, the given value, "Element innerHTML", and "script".
    auto const compliant_string = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::relevant_global_object(element),
        value,
        TrustedTypes::InjectionSink::Element_innerHTML,
        "script"sv));

    return element.set_inner_html(compliant_string.to_utf8_but_should_be_ported_to_utf16());
}

WebIDL::ExceptionOr<TrustedTypes::TrustedHTMLOrString> outer_html(DOM::Element& element)
{
    return TRY(element.outer_html());
}

WebIDL::ExceptionOr<void> set_outer_html(DOM::Element& element, TrustedTypes::TrustedHTMLOrString const& value)
{
    // 1. Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, the given value, "Element outerHTML", and "script".
    auto const compliant_string = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::relevant_global_object(element),
        value,
        TrustedTypes::InjectionSink::Element_outerHTML,
        "script"sv));

    return element.set_outer_html(compliant_string.to_utf8_but_should_be_ported_to_utf16());
}

WebIDL::ExceptionOr<void> set_html_unsafe(DOM::Element& element, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const& html)
{
    // 1. Let compliantHTML be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, html, "Element setHTMLUnsafe", and "script".
    auto const compliant_html = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::relevant_global_object(element),
        html,
        TrustedTypes::InjectionSink::Element_setHTMLUnsafe,
        "script"sv));

    return element.set_html_unsafe(compliant_html.to_utf8_but_should_be_ported_to_utf16());
}

WebIDL::ExceptionOr<void> insert_adjacent_html(DOM::Element& element, Utf16String const& position, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const& text)
{
    // 1. Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, string, "Element insertAdjacentHTML", and "script".
    auto const compliant_string = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::relevant_global_object(element),
        text,
        TrustedTypes::InjectionSink::Element_insertAdjacentHTML,
        "script"sv));

    return element.insert_adjacent_html(position.to_utf8(), compliant_string.to_utf8_but_should_be_ported_to_utf16());
}

WebIDL::ExceptionOr<void> set_attribute(DOM::Element& element, Utf16String const& qualified_name, Variant<GC::Ref<TrustedTypes::TrustedHTML>, GC::Ref<TrustedTypes::TrustedScript>, GC::Ref<TrustedTypes::TrustedScriptURL>, Utf16String> const& value)
{
    auto utf16_qualified_name = Utf16FlyString::from_utf16(qualified_name.utf16_view());
    auto qualified_name_string = qualified_name;
    if (!DOM::is_valid_attribute_local_name(qualified_name_string.utf16_view()))
        return WebIDL::InvalidCharacterError::create("Attribute name must not be empty or contain invalid characters"_utf16);

    if (element.namespace_uri() == Namespace::HTML && element.document().document_type() == DOM::Document::Type::HTML)
        utf16_qualified_name = utf16_qualified_name.to_ascii_lowercase();

    auto const verified_value = TRY(TrustedTypes::get_trusted_types_compliant_attribute_value(utf16_qualified_name, {}, element, value));
    element.set_attribute(FlyString { utf16_qualified_name.to_utf16_string().to_utf8() }, verified_value);
    return {};
}

WebIDL::ExceptionOr<void> set_attribute_ns(DOM::Element& element, Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& qualified_name, Variant<GC::Ref<TrustedTypes::TrustedHTML>, GC::Ref<TrustedTypes::TrustedScript>, GC::Ref<TrustedTypes::TrustedScriptURL>, Utf16String> const& value)
{
    auto namespace_utf8 = namespace_.map([](auto const& value) { return FlyString { value.to_utf16_string().to_utf8() }; });
    auto qualified_name_utf8 = FlyString { qualified_name.to_utf16_string().to_utf8() };
    auto extracted_qualified_name_or_error = DOM::validate_and_extract(namespace_utf8, qualified_name_utf8, DOM::ValidationContext::Attribute);
    if (extracted_qualified_name_or_error.is_error())
        return DOM::validate_and_extract_error_to_dom_exception(extracted_qualified_name_or_error.release_error());
    auto extracted_qualified_name = extracted_qualified_name_or_error.release_value();

    auto const verified_value = TRY(TrustedTypes::get_trusted_types_compliant_attribute_value(
        extracted_qualified_name.local_name(),
        extracted_qualified_name.namespace_().has_value() ? Optional<Utf16FlyString> { extracted_qualified_name.namespace_().value() } : Optional<Utf16FlyString> {},
        element,
        value));
    element.set_attribute_ns(extracted_qualified_name, verified_value);
    return {};
}

}
