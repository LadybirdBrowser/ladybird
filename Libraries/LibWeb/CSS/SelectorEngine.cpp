/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLDetailsElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLMeterElement.h>
#include <LibWeb/HTML/HTMLProgressElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/SVGAElement.h>

namespace Web::SelectorEngine {

static inline bool matches(CSS::Selector const& selector, int component_list_index, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ptr<DOM::ParentNode const> scope, SelectorKind selector_kind, GC::Ptr<DOM::Element const> anchor = nullptr);

// Upward traversal for descendant (' ') and immediate child combinator ('>')
// If we're starting inside a shadow tree, traversal stops at the nearest shadow host.
// This is an implementation detail of the :host selector. Otherwise we would just traverse up to the document root.
static inline GC::Ptr<DOM::Node const> traverse_up(GC::Ptr<DOM::Node const> node, GC::Ptr<DOM::Element const> shadow_host)
{
    if (!node)
        return nullptr;

    if (shadow_host) {
        // NOTE: We only traverse up to the shadow host, not beyond.
        if (node == shadow_host)
            return nullptr;

        return node->parent_or_shadow_host_element();
    }
    return node->parent();
}

// https://drafts.csswg.org/selectors-4/#the-lang-pseudo
static inline bool matches_lang_pseudo_class(DOM::Element const& element, Vector<FlyString> const& languages)
{
    auto maybe_element_language = element.lang();
    if (!maybe_element_language.has_value())
        return false;

    auto element_language = maybe_element_language.release_value();

    // FIXME: This is ad-hoc. Implement a proper language range matching algorithm as recommended by BCP47.
    for (auto const& language : languages) {
        if (language.is_empty())
            continue;
        if (language == "*"sv)
            return true;
        if (!element_language.contains('-') && Infra::is_ascii_case_insensitive_match(element_language, language))
            return true;
        auto parts = element_language.split_limit('-', 2).release_value_but_fixme_should_propagate_errors();
        if (!parts.is_empty() && Infra::is_ascii_case_insensitive_match(parts[0], language))
            return true;
    }
    return false;
}

// https://drafts.csswg.org/selectors-4/#relational
static inline bool matches_relative_selector(CSS::Selector const& selector, size_t compound_index, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ref<DOM::Element const> anchor)
{
    if (compound_index >= selector.compound_selectors().size())
        return matches(selector, element, shadow_host, context, {}, {}, SelectorKind::Relative, anchor);

    switch (selector.compound_selectors()[compound_index].combinator) {
    // Shouldn't be possible because we've parsed relative selectors, which always have a combinator, implicitly or explicitly.
    case CSS::Selector::Combinator::None:
        VERIFY_NOT_REACHED();
    case CSS::Selector::Combinator::Descendant: {
        bool has = false;
        element.for_each_in_subtree([&](auto const& descendant) {
            if (!descendant.is_element())
                return TraversalDecision::Continue;
            auto const& descendant_element = static_cast<DOM::Element const&>(descendant);
            if (matches(selector, descendant_element, shadow_host, context, {}, {}, SelectorKind::Relative, anchor)) {
                has = true;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        return has;
    }
    case CSS::Selector::Combinator::ImmediateChild: {
        bool has = false;
        element.for_each_child([&](DOM::Node const& child) {
            if (!child.is_element())
                return IterationDecision::Continue;
            auto const& child_element = static_cast<DOM::Element const&>(child);
            if (!matches(selector, compound_index, child_element, shadow_host, context, {}, SelectorKind::Relative, anchor))
                return IterationDecision::Continue;
            if (matches_relative_selector(selector, compound_index + 1, child_element, shadow_host, context, anchor)) {
                has = true;
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });
        return has;
    }
    case CSS::Selector::Combinator::NextSibling: {
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(*anchor).set_affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator(true);
        }
        auto* sibling = element.next_element_sibling();
        if (!sibling)
            return false;
        if (!matches(selector, compound_index, *sibling, shadow_host, context, {}, SelectorKind::Relative, anchor))
            return false;
        return matches_relative_selector(selector, compound_index + 1, *sibling, shadow_host, context, anchor);
    }
    case CSS::Selector::Combinator::SubsequentSibling: {
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(*anchor).set_affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator(true);
        }
        for (auto const* sibling = element.next_element_sibling(); sibling; sibling = sibling->next_element_sibling()) {
            if (!matches(selector, compound_index, *sibling, shadow_host, context, {}, SelectorKind::Relative, anchor))
                continue;
            if (matches_relative_selector(selector, compound_index + 1, *sibling, shadow_host, context, anchor))
                return true;
        }
        return false;
    }
    case CSS::Selector::Combinator::Column:
        TODO();
    }
    return false;
}

// https://drafts.csswg.org/selectors-4/#relational
static inline bool matches_has_pseudo_class(CSS::Selector const& selector, DOM::Element const& anchor, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context)
{
    return matches_relative_selector(selector, 0, anchor, shadow_host, context, anchor);
}

static bool matches_hover_pseudo_class(DOM::Element const& element)
{
    auto* hovered_node = element.document().hovered_node();
    if (!hovered_node)
        return false;
    if (&element == hovered_node)
        return true;
    return element.is_shadow_including_ancestor_of(*hovered_node);
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-indeterminate
static inline bool matches_indeterminate_pseudo_class(DOM::Element const& element)
{
    // The :indeterminate pseudo-class must match any element falling into one of the following categories:
    // - input elements whose type attribute is in the Checkbox state and whose indeterminate IDL attribute is set to true
    // FIXME: - input elements whose type attribute is in the Radio Button state and whose radio button group contains no input elements whose checkedness state is true.
    if (is<HTML::HTMLInputElement>(element)) {
        auto const& input_element = static_cast<HTML::HTMLInputElement const&>(element);
        switch (input_element.type_state()) {
        case HTML::HTMLInputElement::TypeAttributeState::Checkbox:
            // https://whatpr.org/html-attr-input-switch/9546/semantics-other.html#selector-indeterminate
            // input elements whose type attribute is in the Checkbox state, whose switch attribute is not set
            return input_element.indeterminate() && !element.has_attribute(HTML::AttributeNames::switch_);
        default:
            return false;
        }
    }
    // - progress elements with no value content attribute
    if (is<HTML::HTMLProgressElement>(element)) {
        return !element.has_attribute(HTML::AttributeNames::value);
    }
    return false;
}

static inline Web::DOM::Attr const* get_optionally_namespaced_attribute(CSS::Selector::SimpleSelector::Attribute const& attribute, GC::Ptr<CSS::CSSStyleSheet const> style_sheet_for_rule, DOM::Element const& element)
{
    auto const& qualified_name = attribute.qualified_name;
    auto const& attribute_name = qualified_name.name.name;
    auto const& namespace_type = qualified_name.namespace_type;

    if (element.namespace_uri() == Namespace::HTML) {
        if (namespace_type == CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Named) {
            return nullptr;
        }
        return element.attributes()->get_attribute(attribute_name);
    }

    switch (namespace_type) {
    // "In keeping with the Namespaces in the XML recommendation, default namespaces do not apply to attributes,
    //  therefore attribute selectors without a namespace component apply only to attributes that have no namespace (equivalent to "|attr")"
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Default:
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::None:
        return element.attributes()->get_attribute(attribute_name);
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Any:
        return element.attributes()->get_attribute_namespace_agnostic(attribute_name);
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Named:
        if (!style_sheet_for_rule)
            return nullptr;
        auto const& selector_namespace = style_sheet_for_rule->namespace_uri(qualified_name.namespace_);
        if (!selector_namespace.has_value())
            return nullptr;
        return element.attributes()->get_attribute_ns(selector_namespace, attribute_name);
    }
    VERIFY_NOT_REACHED();
}

static inline bool matches_attribute(CSS::Selector::SimpleSelector::Attribute const& attribute, [[maybe_unused]] GC::Ptr<CSS::CSSStyleSheet const> style_sheet_for_rule, DOM::Element const& element)
{
    auto const& attribute_name = attribute.qualified_name.name.name;

    auto const* attr = get_optionally_namespaced_attribute(attribute, style_sheet_for_rule, element);

    if (attribute.match_type == CSS::Selector::SimpleSelector::Attribute::MatchType::HasAttribute) {
        // Early way out in case of an attribute existence selector.
        return attr != nullptr;
    }

    if (!attr)
        return false;

    auto case_sensitivity = [&](CSS::Selector::SimpleSelector::Attribute::CaseType case_type) {
        switch (case_type) {
        case CSS::Selector::SimpleSelector::Attribute::CaseType::CaseInsensitiveMatch:
            return CaseSensitivity::CaseInsensitive;
        case CSS::Selector::SimpleSelector::Attribute::CaseType::CaseSensitiveMatch:
            return CaseSensitivity::CaseSensitive;
        case CSS::Selector::SimpleSelector::Attribute::CaseType::DefaultMatch:
            // See: https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
            if (element.document().is_html_document()
                && element.namespace_uri() == Namespace::HTML
                && attribute_name.is_one_of(
                    HTML::AttributeNames::accept, HTML::AttributeNames::accept_charset, HTML::AttributeNames::align,
                    HTML::AttributeNames::alink, HTML::AttributeNames::axis, HTML::AttributeNames::bgcolor, HTML::AttributeNames::charset,
                    HTML::AttributeNames::checked, HTML::AttributeNames::clear, HTML::AttributeNames::codetype, HTML::AttributeNames::color,
                    HTML::AttributeNames::compact, HTML::AttributeNames::declare, HTML::AttributeNames::defer, HTML::AttributeNames::dir,
                    HTML::AttributeNames::direction, HTML::AttributeNames::disabled, HTML::AttributeNames::enctype, HTML::AttributeNames::face,
                    HTML::AttributeNames::frame, HTML::AttributeNames::hreflang, HTML::AttributeNames::http_equiv, HTML::AttributeNames::lang,
                    HTML::AttributeNames::language, HTML::AttributeNames::link, HTML::AttributeNames::media, HTML::AttributeNames::method,
                    HTML::AttributeNames::multiple, HTML::AttributeNames::nohref, HTML::AttributeNames::noresize, HTML::AttributeNames::noshade,
                    HTML::AttributeNames::nowrap, HTML::AttributeNames::readonly, HTML::AttributeNames::rel, HTML::AttributeNames::rev,
                    HTML::AttributeNames::rules, HTML::AttributeNames::scope, HTML::AttributeNames::scrolling, HTML::AttributeNames::selected,
                    HTML::AttributeNames::shape, HTML::AttributeNames::target, HTML::AttributeNames::text, HTML::AttributeNames::type,
                    HTML::AttributeNames::valign, HTML::AttributeNames::valuetype, HTML::AttributeNames::vlink)) {
                return CaseSensitivity::CaseInsensitive;
            }

            return CaseSensitivity::CaseSensitive;
        }
        VERIFY_NOT_REACHED();
    }(attribute.case_type);
    auto case_insensitive_match = case_sensitivity == CaseSensitivity::CaseInsensitive;

    switch (attribute.match_type) {
    case CSS::Selector::SimpleSelector::Attribute::MatchType::ExactValueMatch:
        return case_insensitive_match
            ? Infra::is_ascii_case_insensitive_match(attr->value(), attribute.value)
            : attr->value() == attribute.value;
    case CSS::Selector::SimpleSelector::Attribute::MatchType::ContainsWord: {
        if (attribute.value.is_empty()) {
            // This selector is always false is match value is empty.
            return false;
        }
        auto const& attribute_value = attr->value();
        auto const view = attribute_value.bytes_as_string_view().split_view(' ');
        auto const size = view.size();
        for (size_t i = 0; i < size; ++i) {
            auto const value = view.at(i);
            if (case_insensitive_match
                    ? Infra::is_ascii_case_insensitive_match(value, attribute.value)
                    : value == attribute.value) {
                return true;
            }
        }
        return false;
    }
    case CSS::Selector::SimpleSelector::Attribute::MatchType::ContainsString:
        return !attribute.value.is_empty()
            && attr->value().contains(attribute.value, case_sensitivity);
    case CSS::Selector::SimpleSelector::Attribute::MatchType::StartsWithSegment: {
        auto const& element_attr_value = attr->value();
        if (element_attr_value.is_empty()) {
            // If the attribute value on element is empty, the selector is true
            // if the match value is also empty and false otherwise.
            return attribute.value.is_empty();
        }
        if (attribute.value.is_empty()) {
            return false;
        }
        auto segments = element_attr_value.bytes_as_string_view().split_view('-');
        return case_insensitive_match
            ? Infra::is_ascii_case_insensitive_match(segments.first(), attribute.value)
            : segments.first() == attribute.value;
    }
    case CSS::Selector::SimpleSelector::Attribute::MatchType::StartsWithString:
        return !attribute.value.is_empty()
            && attr->value().bytes_as_string_view().starts_with(attribute.value, case_sensitivity);
    case CSS::Selector::SimpleSelector::Attribute::MatchType::EndsWithString:
        return !attribute.value.is_empty()
            && attr->value().bytes_as_string_view().ends_with(attribute.value, case_sensitivity);
    default:
        break;
    }

    return false;
}

static inline DOM::Element const* previous_sibling_with_same_tag_name(DOM::Element const& element)
{
    for (auto const* sibling = element.previous_element_sibling(); sibling; sibling = sibling->previous_element_sibling()) {
        if (sibling->tag_name() == element.tag_name())
            return sibling;
    }
    return nullptr;
}

static inline DOM::Element const* next_sibling_with_same_tag_name(DOM::Element const& element)
{
    for (auto const* sibling = element.next_element_sibling(); sibling; sibling = sibling->next_element_sibling()) {
        if (sibling->tag_name() == element.tag_name())
            return sibling;
    }
    return nullptr;
}

/// Returns true if this selector should be blocked from matching against the shadow host from within a shadow tree.
/// Only :host pseudo-class is allowed to match the shadow host from within shadow tree, all other selectors (including other pseudo-classes) are blocked.
/// Compound selectors (:has(), :is(), :where()), nesting, and pseudo-elements are allowed as they may contain or be contained within :host.
static inline bool should_block_shadow_host_matching(CSS::Selector::SimpleSelector const& component, GC::Ptr<DOM::Element const> shadow_host, DOM::Element const& element)
{
    if (!shadow_host || &element != shadow_host.ptr())
        return false;

    // From within shadow tree, only :host pseudo-class can match the host element
    if (component.type == CSS::Selector::SimpleSelector::Type::PseudoClass) {
        auto const& pseudo_class = component.pseudo_class();
        return pseudo_class.type != CSS::PseudoClass::Host
            && pseudo_class.type != CSS::PseudoClass::Has
            && pseudo_class.type != CSS::PseudoClass::Is
            && pseudo_class.type != CSS::PseudoClass::Where;
    }

    // Allow nesting and PseudoElement as it may contain :host class
    if (component.type == CSS::Selector::SimpleSelector::Type::Nesting || component.type == CSS::Selector::SimpleSelector::Type::PseudoElement)
        return false;

    return true;
}
// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-read-write
static bool matches_read_write_pseudo_class(DOM::Element const& element)
{
    // The :read-write pseudo-class must match any element falling into one of the following categories,
    // which for the purposes of Selectors are thus considered user-alterable: [SELECTORS]
    // - input elements to which the readonly attribute applies, and that are mutable
    //   (i.e. that do not have the readonly attribute specified and that are not disabled)
    if (is<HTML::HTMLInputElement>(element)) {
        auto& input_element = static_cast<HTML::HTMLInputElement const&>(element);
        if (input_element.has_attribute(HTML::AttributeNames::readonly))
            return false;
        if (!input_element.enabled())
            return false;
        return true;
    }
    // - textarea elements that do not have a readonly attribute, and that are not disabled
    if (is<HTML::HTMLTextAreaElement>(element)) {
        auto& input_element = static_cast<HTML::HTMLTextAreaElement const&>(element);
        if (input_element.has_attribute(HTML::AttributeNames::readonly))
            return false;
        if (!input_element.enabled())
            return false;
        return true;
    }
    // - elements that are editing hosts or editable and are neither input elements nor textarea elements
    return element.is_editable_or_editing_host();
}

// https://drafts.csswg.org/selectors-4/#open-state
static bool matches_open_state_pseudo_class(DOM::Element const& element, bool open)
{
    // The :open pseudo-class represents an element that has both “open” and “closed” states,
    // and which is currently in the “open” state.

    // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-open
    // The :open pseudo-class must match any element falling into one of the following categories:
    // - details elements that have an open attribute
    // - dialog elements that have an open attribute
    if (is<HTML::HTMLDetailsElement>(element) || is<HTML::HTMLDialogElement>(element))
        return open == element.has_attribute(HTML::AttributeNames::open);
    // - select elements that are a drop-down box and whose drop-down boxes are open
    if (auto const* select = as_if<HTML::HTMLSelectElement>(element))
        return open == select->is_open();
    // - input elements that support a picker and whose pickers are open
    if (auto const* input = as_if<HTML::HTMLInputElement>(element))
        return open == (input->supports_a_picker() && input->is_open());

    return false;
}

// https://drafts.csswg.org/css-scoping/#host-selector
static inline bool matches_host_pseudo_class(GC::Ref<DOM::Element const> element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, CSS::SelectorList const& argument_selector_list)
{
    // When evaluated in the context of a shadow tree, it matches the shadow tree’s shadow host if the shadow host,
    // in its normal context, matches the selector argument. In any other context, it matches nothing.
    if (!shadow_host || element != shadow_host)
        return false;

    // NOTE: There's either 0 or 1 argument selector, since the syntax is :host or :host(<compound-selector>)
    if (!argument_selector_list.is_empty())
        return matches(argument_selector_list.first(), element, nullptr, context);

    return true;
}

static bool matches_optimal_value_pseudo_class(DOM::Element const& element, HTML::HTMLMeterElement::ValueState desired_state)
{
    if (auto* meter = as_if<HTML::HTMLMeterElement>(element))
        return meter->value_state() == desired_state;
    return false;
}

static inline bool matches_pseudo_class(CSS::Selector::SimpleSelector::PseudoClassSelector const& pseudo_class, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ptr<DOM::ParentNode const> scope, SelectorKind selector_kind)
{
    switch (pseudo_class.type) {
    case CSS::PseudoClass::Link:
    case CSS::PseudoClass::AnyLink:
        // NOTE: AnyLink should match whether the link is visited or not, so if we ever start matching
        //       :visited, we'll need to handle these differently.
        return element.matches_link_pseudo_class();
    case CSS::PseudoClass::LocalLink: {
        return element.matches_local_link_pseudo_class();
    }
    case CSS::PseudoClass::Visited:
        // FIXME: Maybe match this selector sometimes?
        return false;
    case CSS::PseudoClass::Active:
        return element.is_active();
    case CSS::PseudoClass::Hover:
        context.did_match_any_hover_rules = true;
        return matches_hover_pseudo_class(element);
    case CSS::PseudoClass::Focus:
        return element.is_focused();
    case CSS::PseudoClass::FocusVisible:
        // FIXME: We should only apply this when a visible focus is useful. Decide when that is!
        return element.is_focused();
    case CSS::PseudoClass::FocusWithin: {
        auto* focused_element = element.document().focused_element();
        return focused_element && element.is_inclusive_ancestor_of(*focused_element);
    }
    case CSS::PseudoClass::FirstChild:
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(element).set_affected_by_first_or_last_child_pseudo_class(true);
        }
        return !element.previous_element_sibling();
    case CSS::PseudoClass::LastChild:
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(element).set_affected_by_first_or_last_child_pseudo_class(true);
        }
        return !element.next_element_sibling();
    case CSS::PseudoClass::OnlyChild:
        return !(element.previous_element_sibling() || element.next_element_sibling());
    case CSS::PseudoClass::Empty: {
        if (!element.has_children())
            return true;
        if (element.first_child_of_type<DOM::Element>())
            return false;
        // NOTE: CSS Selectors level 4 changed ":empty" to also match whitespace-only text nodes.
        //       However, none of the major browser supports this yet, so let's just hang back until they do.
        bool has_nonempty_text_child = false;
        element.for_each_child_of_type<DOM::Text>([&](auto const& text_child) {
            if (!text_child.data().is_empty()) {
                has_nonempty_text_child = true;
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });
        return !has_nonempty_text_child;
    }
    case CSS::PseudoClass::Root:
        return is<HTML::HTMLHtmlElement>(element);
    case CSS::PseudoClass::Host:
        return matches_host_pseudo_class(element, shadow_host, context, pseudo_class.argument_selector_list);
    case CSS::PseudoClass::Scope:
        return scope ? &element == scope : is<HTML::HTMLHtmlElement>(element);
    case CSS::PseudoClass::FirstOfType:
        return !previous_sibling_with_same_tag_name(element);
    case CSS::PseudoClass::LastOfType:
        return !next_sibling_with_same_tag_name(element);
    case CSS::PseudoClass::OnlyOfType:
        return !previous_sibling_with_same_tag_name(element) && !next_sibling_with_same_tag_name(element);
    case CSS::PseudoClass::Lang:
        return matches_lang_pseudo_class(element, pseudo_class.languages);
    case CSS::PseudoClass::Disabled:
        return element.matches_disabled_pseudo_class();
    case CSS::PseudoClass::Enabled:
        return element.matches_enabled_pseudo_class();
    case CSS::PseudoClass::Checked:
        return element.matches_checked_pseudo_class();
    case CSS::PseudoClass::Indeterminate:
        return matches_indeterminate_pseudo_class(element);
    case CSS::PseudoClass::HighValue:
        if (auto* meter = as_if<HTML::HTMLMeterElement>(element))
            return meter->value() > meter->high();
        return false;
    case CSS::PseudoClass::LowValue:
        if (auto* meter = as_if<HTML::HTMLMeterElement>(element))
            return meter->value() < meter->low();
        return false;
    case CSS::PseudoClass::OptimalValue:
        return matches_optimal_value_pseudo_class(element, HTML::HTMLMeterElement::ValueState::Optimal);
    case CSS::PseudoClass::SuboptimalValue:
        return matches_optimal_value_pseudo_class(element, HTML::HTMLMeterElement::ValueState::Suboptimal);
    case CSS::PseudoClass::EvenLessGoodValue:
        return matches_optimal_value_pseudo_class(element, HTML::HTMLMeterElement::ValueState::EvenLessGood);
    case CSS::PseudoClass::Defined:
        return element.is_defined();
    case CSS::PseudoClass::Has:
        // :has() cannot be nested in a :has()
        if (selector_kind == SelectorKind::Relative)
            return false;
        if (context.collect_per_element_selector_involvement_metadata) {
            if (&element == context.subject) {
                const_cast<DOM::Element&>(element).set_affected_by_has_pseudo_class_in_subject_position(true);
            } else {
                const_cast<DOM::Element&>(element).set_affected_by_has_pseudo_class_in_non_subject_position(true);
            }
        }
        // These selectors should be relative selectors (https://drafts.csswg.org/selectors-4/#relative-selector)
        for (auto& selector : pseudo_class.argument_selector_list) {
            if (matches_has_pseudo_class(selector, element, shadow_host, context))
                return true;
        }
        return false;
    case CSS::PseudoClass::Is:
    case CSS::PseudoClass::Where:
        for (auto& selector : pseudo_class.argument_selector_list) {
            if (matches(selector, element, shadow_host, context))
                return true;
        }
        return false;
    case CSS::PseudoClass::Not:
        for (auto& selector : pseudo_class.argument_selector_list) {
            if (matches(selector, element, shadow_host, context))
                return false;
        }
        return true;
    case CSS::PseudoClass::NthChild:
    case CSS::PseudoClass::NthLastChild:
    case CSS::PseudoClass::NthOfType:
    case CSS::PseudoClass::NthLastOfType: {
        auto const step_size = pseudo_class.nth_child_pattern.step_size;
        auto const offset = pseudo_class.nth_child_pattern.offset;
        if (step_size == 0 && offset == 0)
            return false; // "If both a and b are equal to zero, the pseudo-class represents no element in the document tree."

        auto const* parent = element.parent();
        if (!parent)
            return false;

        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(element).set_affected_by_nth_child_pseudo_class(true);
        }

        auto matches_selector_list = [&context, shadow_host](CSS::SelectorList const& list, DOM::Element const& element) {
            if (list.is_empty())
                return true;
            for (auto const& child_selector : list) {
                if (matches(child_selector, element, shadow_host, context)) {
                    return true;
                }
            }
            return false;
        };

        int index = 1;
        switch (pseudo_class.type) {
        case CSS::PseudoClass::NthChild: {
            if (!matches_selector_list(pseudo_class.argument_selector_list, element))
                return false;
            for (auto* child = parent->first_child_of_type<DOM::Element>(); child && child != &element; child = child->next_element_sibling()) {
                if (matches_selector_list(pseudo_class.argument_selector_list, *child))
                    ++index;
            }
            break;
        }
        case CSS::PseudoClass::NthLastChild: {
            if (!matches_selector_list(pseudo_class.argument_selector_list, element))
                return false;
            for (auto* child = parent->last_child_of_type<DOM::Element>(); child && child != &element; child = child->previous_element_sibling()) {
                if (matches_selector_list(pseudo_class.argument_selector_list, *child))
                    ++index;
            }
            break;
        }
        case CSS::PseudoClass::NthOfType: {
            for (auto* child = previous_sibling_with_same_tag_name(element); child; child = previous_sibling_with_same_tag_name(*child))
                ++index;
            break;
        }
        case CSS::PseudoClass::NthLastOfType: {
            for (auto* child = next_sibling_with_same_tag_name(element); child; child = next_sibling_with_same_tag_name(*child))
                ++index;
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }

        // When "step_size == -1", selector represents first "offset" elements in document tree.
        if (step_size == -1)
            return !(offset <= 0 || index > offset);

        // When "step_size == 1", selector represents last "offset" elements in document tree.
        if (step_size == 1)
            return !(offset < 0 || index < offset);

        // When "step_size == 0", selector picks only the "offset" element.
        if (step_size == 0)
            return index == offset;

        // If both are negative, nothing can match.
        if (step_size < 0 && offset < 0)
            return false;

        // Like "a % b", but handles negative integers correctly.
        auto const canonical_modulo = [](int a, int b) -> int {
            int c = a % b;
            if ((c < 0 && b > 0) || (c > 0 && b < 0)) {
                c += b;
            }
            return c;
        };

        // When "step_size < 0", we start at "offset" and count backwards.
        if (step_size < 0)
            return index <= offset && canonical_modulo(index - offset, -step_size) == 0;

        // Otherwise, we start at "offset" and count forwards.
        return index >= offset && canonical_modulo(index - offset, step_size) == 0;
    }
    case CSS::PseudoClass::Playing: {
        if (!is<HTML::HTMLMediaElement>(element))
            return false;
        auto const& media_element = static_cast<HTML::HTMLMediaElement const&>(element);
        return !media_element.paused();
    }
    case CSS::PseudoClass::Paused: {
        if (!is<HTML::HTMLMediaElement>(element))
            return false;
        auto const& media_element = static_cast<HTML::HTMLMediaElement const&>(element);
        return media_element.paused();
    }
    case CSS::PseudoClass::Seeking: {
        if (!is<HTML::HTMLMediaElement>(element))
            return false;
        auto const& media_element = static_cast<HTML::HTMLMediaElement const&>(element);
        return media_element.seeking();
    }
    case CSS::PseudoClass::Muted: {
        if (!is<HTML::HTMLMediaElement>(element))
            return false;
        auto const& media_element = static_cast<HTML::HTMLMediaElement const&>(element);
        return media_element.muted();
    }
    case CSS::PseudoClass::VolumeLocked: {
        // FIXME: Currently we don't allow the user to specify an override volume, so this is always false.
        //        Once we do, implement this!
        return false;
    }
    case CSS::PseudoClass::Buffering: {
        if (!is<HTML::HTMLMediaElement>(element))
            return false;
        auto const& media_element = static_cast<HTML::HTMLMediaElement const&>(element);
        return media_element.blocked();
    }
    case CSS::PseudoClass::Stalled: {
        if (!is<HTML::HTMLMediaElement>(element))
            return false;
        auto const& media_element = static_cast<HTML::HTMLMediaElement const&>(element);
        return media_element.stalled();
    }
    case CSS::PseudoClass::Target:
        return element.is_target();
    case CSS::PseudoClass::TargetWithin: {
        auto* target_element = element.document().target_element();
        if (!target_element)
            return false;
        return element.is_inclusive_ancestor_of(*target_element);
    }
    case CSS::PseudoClass::Dir: {
        // "Values other than ltr and rtl are not invalid, but do not match anything."
        // - https://www.w3.org/TR/selectors-4/#the-dir-pseudo
        if (!first_is_one_of(pseudo_class.keyword, CSS::Keyword::Ltr, CSS::Keyword::Rtl))
            return false;
        switch (element.directionality()) {
        case DOM::Element::Directionality::Ltr:
            return pseudo_class.keyword == CSS::Keyword::Ltr;
        case DOM::Element::Directionality::Rtl:
            return pseudo_class.keyword == CSS::Keyword::Rtl;
        }
        VERIFY_NOT_REACHED();
    }
    case CSS::PseudoClass::ReadOnly:
        return !matches_read_write_pseudo_class(element);
    case CSS::PseudoClass::ReadWrite:
        return matches_read_write_pseudo_class(element);
    case CSS::PseudoClass::PlaceholderShown: {
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-placeholder-shown
        //  The :placeholder-shown pseudo-class must match any element falling into one of the following categories:
        // - input elements that have a placeholder attribute whose value is currently being presented to the user.
        if (is<HTML::HTMLInputElement>(element) && element.has_attribute(HTML::AttributeNames::placeholder)) {
            auto const& input_element = static_cast<HTML::HTMLInputElement const&>(element);
            return input_element.placeholder_element() && input_element.placeholder_value().has_value();
        }
        // - FIXME: textarea elements that have a placeholder attribute whose value is currently being presented to the user.
        return false;
    }
    case CSS::PseudoClass::Open:
        return matches_open_state_pseudo_class(element, pseudo_class.type == CSS::PseudoClass::Open);
    case CSS::PseudoClass::Modal: {
        // https://drafts.csswg.org/selectors/#modal-state
        if (is<HTML::HTMLDialogElement>(element)) {
            auto const& dialog_element = static_cast<HTML::HTMLDialogElement const&>(element);
            return dialog_element.is_modal();
        }
        // FIXME: fullscreen elements are also modal.
        return false;
    }
    case CSS::PseudoClass::PopoverOpen: {
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-popover-open
        // The :popover-open pseudo-class is defined to match any HTML element whose popover attribute is not in the no popover state and whose popover visibility state is showing.
        if (is<HTML::HTMLElement>(element) && element.has_attribute(HTML::AttributeNames::popover)) {
            auto& html_element = static_cast<HTML::HTMLElement const&>(element);
            return html_element.popover_visibility_state() == HTML::HTMLElement::PopoverVisibilityState::Showing;
        }

        return false;
    }
    case CSS::PseudoClass::Valid: {
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-valid
        // The :valid pseudo-class must match any element falling into one of the following categories:

        // - elements that are candidates for constraint validation and that satisfy their constraints
        if (auto form_associated_element = as_if<Web::HTML::FormAssociatedElement>(element))
            if (form_associated_element->is_candidate_for_constraint_validation() && form_associated_element->satisfies_its_constraints())
                return true;

        // - form elements that are not the form owner of any elements that themselves are candidates for constraint validation but do not satisfy their constraints
        if (auto form_element = as_if<Web::HTML::HTMLFormElement>(element)) {
            bool has_invalid_elements = false;
            element.for_each_in_subtree([&](auto& node) {
                if (auto form_associated_element = as_if<Web::HTML::FormAssociatedElement>(&node)) {
                    if (form_associated_element->form() == form_element && form_associated_element->is_candidate_for_constraint_validation() && !form_associated_element->satisfies_its_constraints()) {
                        has_invalid_elements = true;
                        return TraversalDecision::Break;
                    }
                }
                return TraversalDecision::Continue;
            });
            if (!has_invalid_elements)
                return true;
        }

        // - fieldset elements that have no descendant elements that themselves are candidates for constraint validation but do not satisfy their constraints
        if (is<Web::HTML::HTMLFieldSetElement>(element)) {
            bool has_invalid_children = false;
            element.for_each_in_subtree([&](auto& node) {
                if (auto form_associated_element = as_if<Web::HTML::FormAssociatedElement>(&node)) {
                    if (form_associated_element->is_candidate_for_constraint_validation() && !form_associated_element->satisfies_its_constraints()) {
                        has_invalid_children = true;
                        return TraversalDecision::Break;
                    }
                }
                return TraversalDecision::Continue;
            });
            if (!has_invalid_children)
                return true;
        }

        return false;
    }
    case CSS::PseudoClass::Invalid: {
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-invalid
        // The :invalid pseudo-class must match any element falling into one of the following categories:

        // - elements that are candidates for constraint validation but that do not satisfy their constraints
        if (auto form_associated_element = as_if<Web::HTML::FormAssociatedElement>(element))
            if (form_associated_element->is_candidate_for_constraint_validation() && !form_associated_element->satisfies_its_constraints())
                return true;

        // - form elements that are the form owner of one or more elements that themselves are candidates for constraint validation but do not satisfy their constraints
        if (auto form_element = as_if<Web::HTML::HTMLFormElement>(element)) {
            bool has_invalid_elements = false;
            element.for_each_in_subtree([&](auto& node) {
                if (auto form_associated_element = as_if<Web::HTML::FormAssociatedElement>(&node)) {
                    if (form_associated_element->form() == form_element && form_associated_element->is_candidate_for_constraint_validation() && !form_associated_element->satisfies_its_constraints()) {
                        has_invalid_elements = true;
                        return TraversalDecision::Break;
                    }
                }
                return TraversalDecision::Continue;
            });
            if (has_invalid_elements)
                return true;
        }

        // - fieldset elements that have of one or more descendant elements that themselves are candidates for constraint validation but do not satisfy their constraints
        if (is<Web::HTML::HTMLFieldSetElement>(element)) {
            bool has_invalid_children = false;
            element.for_each_in_subtree([&](auto& node) {
                if (auto form_associated_element = as_if<Web::HTML::FormAssociatedElement>(&node)) {
                    if (form_associated_element->is_candidate_for_constraint_validation() && !form_associated_element->satisfies_its_constraints()) {
                        has_invalid_children = true;
                        return TraversalDecision::Break;
                    }
                }
                return TraversalDecision::Continue;
            });
            if (has_invalid_children)
                return true;
        }

        return false;
    }
    case CSS::PseudoClass::UserValid: {
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-user-valid
        // The :user-valid pseudo-class must match input, textarea, and select elements whose user validity is true,
        bool user_validity = false;
        if (auto input_element = as_if<Web::HTML::HTMLInputElement>(element)) {
            user_validity = input_element->user_validity();
        } else if (auto select_element = as_if<Web::HTML::HTMLSelectElement>(element)) {
            user_validity = select_element->user_validity();
        } else if (auto text_area_element = as_if<Web::HTML::HTMLTextAreaElement>(element)) {
            user_validity = text_area_element->user_validity();
        }
        if (!user_validity)
            return false;

        // are candidates for constraint validation, and that satisfy their constraints.
        auto& form_associated_element = as<Web::HTML::FormAssociatedElement>(element);
        if (form_associated_element.is_candidate_for_constraint_validation() && form_associated_element.satisfies_its_constraints())
            return true;

        return false;
    }
    case CSS::PseudoClass::UserInvalid: {
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-user-invalid
        // The :user-invalid pseudo-class must match input, textarea, and select elements whose user validity is true,
        bool user_validity = false;
        if (auto input_element = as_if<Web::HTML::HTMLInputElement>(element)) {
            user_validity = input_element->user_validity();
        } else if (auto select_element = as_if<Web::HTML::HTMLSelectElement>(element)) {
            user_validity = select_element->user_validity();
        } else if (auto text_area_element = as_if<Web::HTML::HTMLTextAreaElement>(element)) {
            user_validity = text_area_element->user_validity();
        }
        if (!user_validity)
            return false;

        // are candidates for constraint validation but do not satisfy their constraints.
        auto& form_associated_element = as<Web::HTML::FormAssociatedElement>(element);
        if (form_associated_element.is_candidate_for_constraint_validation() && !form_associated_element.satisfies_its_constraints())
            return true;

        return false;
    }
    }

    return false;
}

static ALWAYS_INLINE bool matches_namespace(
    CSS::Selector::SimpleSelector::QualifiedName const& qualified_name,
    DOM::Element const& element,
    GC::Ptr<CSS::CSSStyleSheet const> style_sheet_for_rule)
{
    switch (qualified_name.namespace_type) {
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Default:
        // "if no default namespace has been declared for selectors, this is equivalent to *|E."
        if (!style_sheet_for_rule || !style_sheet_for_rule->default_namespace_rule())
            return true;
        // "Otherwise it is equivalent to ns|E where ns is the default namespace."
        return element.namespace_uri() == style_sheet_for_rule->default_namespace_rule()->namespace_uri();
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::None:
        // "elements with name E without a namespace"
        return !element.namespace_uri().has_value();
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Any:
        // "elements with name E in any namespace, including those without a namespace"
        return true;
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Named:
        // "elements with name E in namespace ns"
        // Unrecognized namespace prefixes are invalid, so don't match.
        // (We can't detect this at parse time, since a namespace rule may be inserted later.)
        // So, if we don't have a context to look up namespaces from, we fail to match.
        if (!style_sheet_for_rule)
            return false;

        auto selector_namespace = style_sheet_for_rule->namespace_uri(qualified_name.namespace_);
        return selector_namespace.has_value() && selector_namespace.value() == element.namespace_uri();
    }
    VERIFY_NOT_REACHED();
}

static inline bool matches(CSS::Selector::SimpleSelector const& component, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ptr<DOM::ParentNode const> scope, SelectorKind selector_kind, [[maybe_unused]] GC::Ptr<DOM::Element const> anchor)
{
    if (should_block_shadow_host_matching(component, shadow_host, element))
        return false;
    switch (component.type) {
    case CSS::Selector::SimpleSelector::Type::Universal:
    case CSS::Selector::SimpleSelector::Type::TagName: {
        auto const& qualified_name = component.qualified_name();

        // Reject if the tag name doesn't match
        if (component.type == CSS::Selector::SimpleSelector::Type::TagName) {
            // See https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
            if (element.document().document_type() == DOM::Document::Type::HTML && element.namespace_uri() == Namespace::HTML) {
                if (qualified_name.name.lowercase_name != element.local_name())
                    return false;
            } else if (!Infra::is_ascii_case_insensitive_match(qualified_name.name.name, element.local_name())) {
                return false;
            }
        }

        return matches_namespace(qualified_name, element, context.style_sheet_for_rule);
    }
    case CSS::Selector::SimpleSelector::Type::Id:
        return component.name() == element.id();
    case CSS::Selector::SimpleSelector::Type::Class: {
        // Class selectors are matched case insensitively in quirks mode.
        // See: https://drafts.csswg.org/selectors-4/#class-html
        auto case_sensitivity = element.document().in_quirks_mode() ? CaseSensitivity::CaseInsensitive : CaseSensitivity::CaseSensitive;
        return element.has_class(component.name(), case_sensitivity);
    }
    case CSS::Selector::SimpleSelector::Type::Attribute:
        return matches_attribute(component.attribute(), context.style_sheet_for_rule, element);
    case CSS::Selector::SimpleSelector::Type::PseudoClass:
        return matches_pseudo_class(component.pseudo_class(), element, shadow_host, context, scope, selector_kind);
    case CSS::Selector::SimpleSelector::Type::PseudoElement:
        // Pseudo-element matching/not-matching is handled in the top level matches().
        return true;
    case CSS::Selector::SimpleSelector::Type::Nesting:
        // Nesting either behaves like :is(), or like :scope.
        // :is() is handled already, by us replacing it with :is() directly, so if we
        // got here, it's :scope.
        return matches_pseudo_class(CSS::Selector::SimpleSelector::PseudoClassSelector { .type = CSS::PseudoClass::Scope }, element, shadow_host, context, scope, selector_kind);
    case CSS::Selector::SimpleSelector::Type::Invalid:
        // Invalid selectors never match
        return false;
    }
    VERIFY_NOT_REACHED();
}

bool matches(CSS::Selector const& selector, int component_list_index, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ptr<DOM::ParentNode const> scope, SelectorKind selector_kind, GC::Ptr<DOM::Element const> anchor)
{
    auto& compound_selector = selector.compound_selectors()[component_list_index];
    for (auto& simple_selector : compound_selector.simple_selectors) {
        if (!matches(simple_selector, element, shadow_host, context, scope, selector_kind, anchor)) {
            return false;
        }
    }

    if (selector_kind == SelectorKind::Relative && component_list_index == 0) {
        VERIFY(anchor);
        return &element != anchor;
    }

    switch (compound_selector.combinator) {
    case CSS::Selector::Combinator::None:
        VERIFY(selector_kind != SelectorKind::Relative);
        return true;
    case CSS::Selector::Combinator::Descendant:
        VERIFY(component_list_index != 0);
        for (auto ancestor = traverse_up(element, shadow_host); ancestor; ancestor = traverse_up(ancestor, shadow_host)) {
            if (!is<DOM::Element>(*ancestor))
                continue;
            if (ancestor == anchor)
                return false;
            if (matches(selector, component_list_index - 1, static_cast<DOM::Element const&>(*ancestor), shadow_host, context, scope, selector_kind, anchor))
                return true;
        }
        return false;
    case CSS::Selector::Combinator::ImmediateChild: {
        VERIFY(component_list_index != 0);
        auto parent = traverse_up(element, shadow_host);
        if (!parent || !parent->is_element())
            return false;
        return matches(selector, component_list_index - 1, static_cast<DOM::Element const&>(*parent), shadow_host, context, scope, selector_kind, anchor);
    }
    case CSS::Selector::Combinator::NextSibling:
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(element).set_affected_by_direct_sibling_combinator(true);
            auto new_sibling_invalidation_distance = max(selector.sibling_invalidation_distance(), element.sibling_invalidation_distance());
            const_cast<DOM::Element&>(element).set_sibling_invalidation_distance(new_sibling_invalidation_distance);
        }
        VERIFY(component_list_index != 0);
        if (auto* sibling = element.previous_element_sibling())
            return matches(selector, component_list_index - 1, *sibling, shadow_host, context, scope, selector_kind, anchor);
        return false;
    case CSS::Selector::Combinator::SubsequentSibling:
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(element).set_affected_by_indirect_sibling_combinator(true);
        }
        VERIFY(component_list_index != 0);
        for (auto* sibling = element.previous_element_sibling(); sibling; sibling = sibling->previous_element_sibling()) {
            if (matches(selector, component_list_index - 1, *sibling, shadow_host, context, scope, selector_kind, anchor))
                return true;
        }
        return false;
    case CSS::Selector::Combinator::Column:
        TODO();
    }
    VERIFY_NOT_REACHED();
}

bool fast_matches(CSS::Selector const& selector, DOM::Element const& element_to_match, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context);

bool matches(CSS::Selector const& selector, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, Optional<CSS::PseudoElement> pseudo_element, GC::Ptr<DOM::ParentNode const> scope, SelectorKind selector_kind, GC::Ptr<DOM::Element const> anchor)
{
    if (selector_kind == SelectorKind::Normal && selector.can_use_fast_matches()) {
        return fast_matches(selector, element, shadow_host, context);
    }
    VERIFY(!selector.compound_selectors().is_empty());
    if (pseudo_element.has_value() && selector.pseudo_element().has_value() && selector.pseudo_element().value().type() != pseudo_element)
        return false;
    if (!pseudo_element.has_value() && selector.pseudo_element().has_value())
        return false;
    return matches(selector, selector.compound_selectors().size() - 1, element, shadow_host, context, scope, selector_kind, anchor);
}

static bool fast_matches_simple_selector(CSS::Selector::SimpleSelector const& simple_selector, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context)
{
    if (should_block_shadow_host_matching(simple_selector, shadow_host, element))
        return false;

    switch (simple_selector.type) {
    case CSS::Selector::SimpleSelector::Type::Universal:
        return matches_namespace(simple_selector.qualified_name(), element, context.style_sheet_for_rule);
    case CSS::Selector::SimpleSelector::Type::TagName:
        // https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
        // When comparing a CSS element type selector to the names of HTML elements in HTML documents, the CSS element type selector must first be converted to ASCII lowercase. The
        // same selector when compared to other elements must be compared according to its original case. In both cases, to match the values must be identical to each other (and therefore
        // the comparison is case sensitive).
        if (element.namespace_uri() == Namespace::HTML && element.document().document_type() == DOM::Document::Type::HTML) {
            if (simple_selector.qualified_name().name.lowercase_name != element.local_name())
                return false;
        } else if (simple_selector.qualified_name().name.name != element.local_name()) {
            // NOTE: Any other elements are either SVG, XHTML or MathML, all of which are case-sensitive.
            return false;
        }
        return matches_namespace(simple_selector.qualified_name(), element, context.style_sheet_for_rule);
    case CSS::Selector::SimpleSelector::Type::Class: {
        // Class selectors are matched case insensitively in quirks mode.
        // See: https://drafts.csswg.org/selectors-4/#class-html
        auto case_sensitivity = element.document().in_quirks_mode() ? CaseSensitivity::CaseInsensitive : CaseSensitivity::CaseSensitive;
        return element.has_class(simple_selector.name(), case_sensitivity);
    }
    case CSS::Selector::SimpleSelector::Type::Id:
        return simple_selector.name() == element.id();
    case CSS::Selector::SimpleSelector::Type::Attribute:
        return matches_attribute(simple_selector.attribute(), context.style_sheet_for_rule, element);
    case CSS::Selector::SimpleSelector::Type::PseudoClass:
        return matches_pseudo_class(simple_selector.pseudo_class(), element, shadow_host, context, nullptr, SelectorKind::Normal);
    default:
        VERIFY_NOT_REACHED();
    }
}

static bool fast_matches_compound_selector(CSS::Selector::CompoundSelector const& compound_selector, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context)
{
    for (auto const& simple_selector : compound_selector.simple_selectors) {
        if (!fast_matches_simple_selector(simple_selector, element, shadow_host, context))
            return false;
    }
    return true;
}

bool fast_matches(CSS::Selector const& selector, DOM::Element const& element_to_match, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context)
{
    DOM::Element const* current = &element_to_match;

    ssize_t compound_selector_index = selector.compound_selectors().size() - 1;

    if (!fast_matches_compound_selector(selector.compound_selectors().last(), *current, shadow_host, context))
        return false;

    // NOTE: If we fail after following a child combinator, we may need to backtrack
    //       to the last matched descendant. We store the state here.
    struct {
        GC::Ptr<DOM::Element const> element;
        ssize_t compound_selector_index = 0;
    } backtrack_state;

    for (;;) {
        // NOTE: There should always be a leftmost compound selector without combinator that kicks us out of this loop.
        VERIFY(compound_selector_index >= 0);

        auto const* compound_selector = &selector.compound_selectors()[compound_selector_index];

        switch (compound_selector->combinator) {
        case CSS::Selector::Combinator::None:
            return true;
        case CSS::Selector::Combinator::Descendant:
            backtrack_state = { current->parent_element(), compound_selector_index };
            compound_selector = &selector.compound_selectors()[--compound_selector_index];
            for (current = current->parent_element(); current; current = current->parent_element()) {
                if (fast_matches_compound_selector(*compound_selector, *current, shadow_host, context))
                    break;
            }
            if (!current)
                return false;
            break;
        case CSS::Selector::Combinator::ImmediateChild:
            compound_selector = &selector.compound_selectors()[--compound_selector_index];
            current = current->parent_element();
            if (!current)
                return false;
            if (!fast_matches_compound_selector(*compound_selector, *current, shadow_host, context)) {
                if (backtrack_state.element) {
                    current = backtrack_state.element;
                    compound_selector_index = backtrack_state.compound_selector_index;
                    continue;
                }
                return false;
            }
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
}

}
