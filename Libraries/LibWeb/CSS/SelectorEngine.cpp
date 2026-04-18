/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLDetailsElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLHeadingElement.h>
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

static bool fast_matches_simple_selector(CSS::Selector::SimpleSelector const& simple_selector, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context);
static bool fast_matches_compound_selector(CSS::Selector::CompoundSelector const& compound_selector, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context);

static CSS::Selector::SimpleSelector const* simple_has_child_tag_selector(CSS::Selector const& selector)
{
    if (selector.compound_selectors().size() != 1)
        return nullptr;

    auto const& first = selector.compound_selectors().first();
    if (first.combinator != CSS::Selector::Combinator::ImmediateChild)
        return nullptr;
    if (first.simple_selectors.size() != 1)
        return nullptr;

    auto const& simple_selector = first.simple_selectors.first();
    if (simple_selector.type != CSS::Selector::SimpleSelector::Type::TagName)
        return nullptr;

    return &simple_selector;
}

static CSS::Selector::CompoundSelector const* simple_has_descendant_tag_and_class_compound(CSS::Selector const& selector)
{
    if (selector.compound_selectors().size() != 1)
        return nullptr;

    auto const& first = selector.compound_selectors().first();
    if (first.combinator != CSS::Selector::Combinator::Descendant)
        return nullptr;
    if (first.simple_selectors.is_empty())
        return nullptr;

    for (auto const& simple_selector : first.simple_selectors) {
        switch (simple_selector.type) {
        case CSS::Selector::SimpleSelector::Type::TagName:
        case CSS::Selector::SimpleSelector::Type::Class:
            break;
        default:
            return nullptr;
        }
    }

    return &first;
}

static bool matches_has_child_tag_fast_path(CSS::Selector::SimpleSelector const& simple_selector, DOM::Element const& anchor, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context)
{
    bool has = false;
    anchor.for_each_child([&](DOM::Node const& child) {
        if (!child.is_element())
            return IterationDecision::Continue;

        auto const& child_element = static_cast<DOM::Element const&>(child);
        if (!fast_matches_simple_selector(simple_selector, child_element, shadow_host, context))
            return IterationDecision::Continue;

        has = true;
        return IterationDecision::Break;
    });
    return has;
}

static bool matches_has_descendant_tag_and_class_fast_path(CSS::Selector const& selector, CSS::Selector::CompoundSelector const& compound_selector, DOM::Element const& anchor, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context)
{
    bool has = false;
    DOM::Element const* matching_descendant = nullptr;
    anchor.for_each_in_subtree([&](auto const& descendant) {
        if (!descendant.is_element())
            return TraversalDecision::Continue;

        auto const& descendant_element = static_cast<DOM::Element const&>(descendant);
        if (!fast_matches_compound_selector(compound_selector, descendant_element, shadow_host, context))
            return TraversalDecision::Continue;

        has = true;
        matching_descendant = &descendant_element;
        return TraversalDecision::Break;
    });

    if (has && matching_descendant && context.has_result_cache) {
        for (auto ancestor = matching_descendant->parent_element(); ancestor && ancestor.ptr() != &anchor; ancestor = ancestor->parent_element())
            context.has_result_cache->set({ &selector, ancestor.ptr() }, HasMatchResult::Matched);
    }

    return has;
}

static inline bool matches_compound_selector(CSS::Selector const& selector, int component_list_index, DOM::AbstractElement const& target, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ptr<DOM::ParentNode const> scope, SelectorKind selector_kind, GC::Ptr<DOM::Element const> anchor = nullptr);

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

// FIXME: This doesn't support pseudo-elements with pseudo-element parents, but neither does the rest of the codebase.
static inline GC::Ptr<DOM::Node const> traverse_up(DOM::AbstractElement const& node, GC::Ptr<DOM::Element const> shadow_host)
{
    if (node.pseudo_element().has_value())
        return node.element();
    return traverse_up(&node.element(), shadow_host);
}

// https://www.rfc-editor.org/rfc/rfc4647.html#section-3.3.2
// NB: Language tags only use ASCII characters, so we can get away with using StringView.
static bool language_range_matches_tag(StringView language_range, StringView language_tag)
{
    // 1. Split both the extended language range and the language tag being compared into a list of subtags by
    //    dividing on the hyphen (%x2D) character.
    auto range_subtags = language_range.split_view('-', SplitBehavior::KeepEmpty);
    auto tag_subtags = language_tag.split_view('-', SplitBehavior::KeepEmpty);

    //    Two subtags match if either they are the same when compared case-insensitively or the language range's subtag
    //    is the wildcard '*'.
    auto subtags_match = [](StringView language_range_subtag, StringView language_subtag) {
        return language_range_subtag == "*"sv
            || language_range_subtag.equals_ignoring_ascii_case(language_subtag);
    };

    // 2. Begin with the first subtag in each list. If the first subtag in the range does not match the first
    //    subtag in the tag, the overall match fails. Otherwise, move to the next subtag in both the range and the
    //    tag.
    auto tag_subtag = tag_subtags.begin();
    auto range_subtag = range_subtags.begin();
    if (!subtags_match(*range_subtag, *tag_subtag))
        return false;
    ++tag_subtag;
    ++range_subtag;

    // 3. While there are more subtags left in the language range's list:
    while (!range_subtag.is_end()) {
        // A. If the subtag currently being examined in the range is the wildcard ('*'), move to the next subtag in
        //    the range and continue with the loop.
        if (*range_subtag == "*"sv) {
            ++range_subtag;
            continue;
        }

        // B. Else, if there are no more subtags in the language tag's list, the match fails.
        if (tag_subtag.is_end())
            return false;

        // C. Else, if the current subtag in the range's list matches the current subtag in the language tag's
        //    list, move to the next subtag in both lists and continue with the loop.
        if (subtags_match(*range_subtag, *tag_subtag)) {
            ++range_subtag;
            ++tag_subtag;
            continue;
        }

        // D. Else, if the language tag's subtag is a "singleton" (a single letter or digit, which includes the
        //    private-use subtag 'x') the match fails.
        if (tag_subtag->length() == 1 && is_ascii_alphanumeric((*tag_subtag)[0])) {
            return false;
        }

        // E. Else, move to the next subtag in the language tag's list and continue with the loop.
        ++tag_subtag;
    }

    // 4. When the language range's list has no more subtags, the match succeeds.
    return true;
}

// https://drafts.csswg.org/selectors-4/#the-lang-pseudo
static inline bool matches_lang_pseudo_class(DOM::Element const& element, Vector<FlyString> const& languages)
{
    auto maybe_element_language = element.lang();
    if (!maybe_element_language.has_value())
        return false;

    auto element_language = maybe_element_language.release_value();

    // The element’s content language matches a language range if its content language, as represented in BCP 47
    // syntax, matches the given language range in an extended filtering operation per [RFC4647] Matching of Language
    // Tags (section 3.3.2). Both the content language and the language range must be canonicalized and converted to
    // extlang form as per section 4.5 of [RFC5646] prior to the extended filtering operation. The matching is
    // performed case-insensitively within the ASCII range.

    // FIXME: Canonicalize both as extlang.

    for (auto const& language_range : languages) {
        if (language_range_matches_tag(language_range, element_language))
            return true;
    }
    return false;
}

// https://drafts.csswg.org/selectors-4/#relational
static inline bool matches_relative_selector(CSS::Selector const& selector, size_t compound_index, DOM::Element const& element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ref<DOM::Element const> anchor, GC::Ptr<DOM::ParentNode const> scope)
{
    if (compound_index >= selector.compound_selectors().size())
        return matches(selector, element, shadow_host, context, scope, SelectorKind::Relative, anchor);

    switch (selector.compound_selectors()[compound_index].combinator) {
    // Shouldn't be possible because we've parsed relative selectors, which always have a combinator, implicitly or explicitly.
    case CSS::Selector::Combinator::None:
        VERIFY_NOT_REACHED();
    case CSS::Selector::Combinator::Descendant: {
        bool has = false;
        DOM::Element const* matching_descendant = nullptr;
        element.for_each_in_subtree([&](auto const& descendant) {
            if (!descendant.is_element())
                return TraversalDecision::Continue;
            auto const& descendant_element = static_cast<DOM::Element const&>(descendant);
            if (context.inside_has_argument_match && context.collect_per_element_selector_involvement_metadata)
                const_cast<DOM::Element&>(descendant_element).set_in_has_scope(true);
            if (matches(selector, descendant_element, shadow_host, context, scope, SelectorKind::Relative, anchor)) {
                has = true;
                matching_descendant = &descendant_element;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        // Cache ancestors as also matching (they have the matching descendant too)
        if (has && matching_descendant && context.has_result_cache) {
            for (auto ancestor = matching_descendant->parent_element(); ancestor && ancestor.ptr() != &element; ancestor = ancestor->parent_element()) {
                context.has_result_cache->set({ &selector, ancestor.ptr() }, HasMatchResult::Matched);
            }
        }
        return has;
    }
    case CSS::Selector::Combinator::ImmediateChild: {
        bool has = false;
        element.for_each_child([&](DOM::Node const& child) {
            if (!child.is_element())
                return IterationDecision::Continue;
            auto const& child_element = static_cast<DOM::Element const&>(child);
            if (context.inside_has_argument_match && context.collect_per_element_selector_involvement_metadata)
                const_cast<DOM::Element&>(child_element).set_in_has_scope(true);
            if (!matches_compound_selector(selector, compound_index, child_element, shadow_host, context, scope, SelectorKind::Relative, anchor))
                return IterationDecision::Continue;
            if (matches_relative_selector(selector, compound_index + 1, child_element, shadow_host, context, anchor, scope)) {
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
        if (context.inside_has_argument_match && context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(*sibling).set_in_has_scope(true);
            const_cast<DOM::Element&>(*sibling).set_in_subtree_of_has_pseudo_class_relative_selector_with_sibling_combinator(true);
        }
        if (!matches_compound_selector(selector, compound_index, *sibling, shadow_host, context, scope, SelectorKind::Relative, anchor))
            return false;
        return matches_relative_selector(selector, compound_index + 1, *sibling, shadow_host, context, anchor, scope);
    }
    case CSS::Selector::Combinator::SubsequentSibling: {
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(*anchor).set_affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator(true);
        }
        for (auto const* sibling = element.next_element_sibling(); sibling; sibling = sibling->next_element_sibling()) {
            if (context.inside_has_argument_match && context.collect_per_element_selector_involvement_metadata) {
                const_cast<DOM::Element&>(*sibling).set_in_has_scope(true);
                const_cast<DOM::Element&>(*sibling).set_in_subtree_of_has_pseudo_class_relative_selector_with_sibling_combinator(true);
            }
            if (!matches_compound_selector(selector, compound_index, *sibling, shadow_host, context, scope, SelectorKind::Relative, anchor))
                continue;
            if (matches_relative_selector(selector, compound_index + 1, *sibling, shadow_host, context, anchor, scope))
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
static inline bool matches_has_pseudo_class(CSS::Selector const& selector, DOM::Element const& anchor, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ptr<DOM::ParentNode const> scope)
{
    auto& counters = anchor.document().style_invalidation_counters();
    ++counters.has_match_invocations;

    if (context.has_result_cache) {
        if (auto cached = context.has_result_cache->get({ &selector, &anchor }); cached.has_value()) {
            ++counters.has_result_cache_hits;
            return cached.value() == HasMatchResult::Matched;
        }
        ++counters.has_result_cache_misses;
    }

    bool saved_inside_has = context.inside_has_argument_match;
    context.inside_has_argument_match = true;
    ScopeGuard restore_inside_has = [&] { context.inside_has_argument_match = saved_inside_has; };

    bool result;
    if (context.collect_per_element_selector_involvement_metadata) {
        result = matches_relative_selector(selector, 0, anchor, shadow_host, context, anchor, scope);
    } else if (auto const* simple_selector = simple_has_child_tag_selector(selector)) {
        result = matches_has_child_tag_fast_path(*simple_selector, anchor, shadow_host, context);
    } else if (auto const* compound_selector = simple_has_descendant_tag_and_class_compound(selector)) {
        result = matches_has_descendant_tag_and_class_fast_path(selector, *compound_selector, anchor, shadow_host, context);
    } else {
        result = matches_relative_selector(selector, 0, anchor, shadow_host, context, anchor, scope);
    }

    if (context.has_result_cache)
        context.has_result_cache->set({ &selector, &anchor }, result ? HasMatchResult::Matched : HasMatchResult::NotMatched);

    return result;
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

static inline void for_each_matching_attribute(CSS::Selector::SimpleSelector::Attribute const& attribute_selector, GC::Ptr<CSS::CSSStyleSheet const> style_sheet_for_rule, DOM::Element const& element, Function<IterationDecision(DOM::Attr const&)> const& process_attribute)
{
    auto const& qualified_name = attribute_selector.qualified_name;
    auto const& attribute_name = qualified_name.name.name;

    switch (qualified_name.namespace_type) {
    // "In keeping with the Namespaces in the XML recommendation, default namespaces do not apply to attributes,
    //  therefore attribute selectors without a namespace component apply only to attributes that have no namespace (equivalent to "|attr")"
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Default:
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::None:
        if (auto const* attribute = element.attributes()->get_attribute(attribute_name))
            (void)process_attribute(*attribute);
        return;
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Any: {
        // When comparing the name part of a CSS attribute selector to the names of attributes on HTML elements in HTML
        // documents, the name part of the CSS attribute selector must first be converted to ASCII lowercase. The same
        // selector when compared to other attributes must be compared according to its original case. In both cases, the
        // comparison is case-sensitive.
        // https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
        bool const case_insensitive = element.document().is_html_document() && element.namespace_uri() == Namespace::HTML;

        for (auto i = 0u; i < element.attributes()->length(); ++i) {
            auto const* attr = element.attributes()->item(i);
            bool matches = case_insensitive
                ? attr->local_name().equals_ignoring_ascii_case(attribute_name)
                : attr->local_name() == attribute_name;
            if (matches) {
                if (process_attribute(*attr) == IterationDecision::Break)
                    break;
            }
        }
        return;
    }
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Named:
        if (!style_sheet_for_rule)
            return;
        auto const& selector_namespace = style_sheet_for_rule->namespace_uri(qualified_name.namespace_);
        if (!selector_namespace.has_value())
            return;

        if (auto const* attribute = element.attributes()->get_attribute_ns(selector_namespace, attribute_name))
            (void)process_attribute(*attribute);
        return;
    }
    VERIFY_NOT_REACHED();
}

static bool matches_single_attribute(CSS::Selector::SimpleSelector::Attribute const& attribute_selector, DOM::Attr const& attribute, CaseSensitivity case_sensitivity)
{
    auto const case_insensitive_match = case_sensitivity == CaseSensitivity::CaseInsensitive;

    switch (attribute_selector.match_type) {
    case CSS::Selector::SimpleSelector::Attribute::MatchType::ExactValueMatch:
        return case_insensitive_match
            ? attribute.value().equals_ignoring_ascii_case(attribute_selector.value)
            : attribute.value() == attribute_selector.value;
    case CSS::Selector::SimpleSelector::Attribute::MatchType::ContainsWord: {
        if (attribute_selector.value.is_empty()) {
            // This selector is always false is match value is empty.
            return false;
        }
        auto const view = attribute.value().bytes_as_string_view().split_view(' ');
        return view.contains([&](auto const& value) {
            return case_insensitive_match ? value.equals_ignoring_ascii_case(attribute_selector.value)
                                          : value == attribute_selector.value;
        });
    }
    case CSS::Selector::SimpleSelector::Attribute::MatchType::ContainsString:
        return !attribute_selector.value.is_empty()
            && attribute.value().contains(attribute_selector.value, case_sensitivity);
    case CSS::Selector::SimpleSelector::Attribute::MatchType::StartsWithSegment: {
        // https://www.w3.org/TR/CSS2/selector.html#attribute-selectors
        // [att|=val]
        // Represents an element with the att attribute, its value either being exactly "val" or beginning with "val" immediately followed by "-" (U+002D).

        auto const& element_attr_value = attribute.value();
        if (element_attr_value.is_empty()) {
            // If the attribute value on element is empty, the selector is true
            // if the match value is also empty and false otherwise.
            return attribute_selector.value.is_empty();
        }
        if (attribute_selector.value.is_empty()) {
            return false;
        }

        auto element_attribute_length = element_attr_value.bytes_as_string_view().length();
        auto attribute_length = attribute_selector.value.bytes_as_string_view().length();
        if (element_attribute_length < attribute_length)
            return false;

        if (attribute_length == element_attribute_length) {
            return case_insensitive_match
                ? element_attr_value.equals_ignoring_ascii_case(attribute_selector.value)
                : element_attr_value == attribute_selector.value;
        }

        return element_attr_value.starts_with_bytes(attribute_selector.value, case_insensitive_match ? CaseSensitivity::CaseInsensitive : CaseSensitivity::CaseSensitive) && element_attr_value.bytes_as_string_view()[attribute_length] == '-';
    }
    case CSS::Selector::SimpleSelector::Attribute::MatchType::StartsWithString:
        return !attribute_selector.value.is_empty()
            && attribute.value().bytes_as_string_view().starts_with(attribute_selector.value, case_sensitivity);
    case CSS::Selector::SimpleSelector::Attribute::MatchType::EndsWithString:
        return !attribute_selector.value.is_empty()
            && attribute.value().bytes_as_string_view().ends_with(attribute_selector.value, case_sensitivity);
    case CSS::Selector::SimpleSelector::Attribute::MatchType::HasAttribute:
        return true;
    }
    return false;
}

static inline bool matches_attribute(CSS::Selector::SimpleSelector::Attribute const& attribute, [[maybe_unused]] GC::Ptr<CSS::CSSStyleSheet const> style_sheet_for_rule, DOM::Element const& element)
{
    auto const& attribute_name = attribute.qualified_name.name.name;

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
                && attribute.qualified_name.namespace_type == CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Default
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

    bool found_matching_attribute = false;
    for_each_matching_attribute(attribute, style_sheet_for_rule, element, [&attribute, case_sensitivity, &found_matching_attribute](DOM::Attr const& attr) {
        if (matches_single_attribute(attribute, attr, case_sensitivity)) {
            found_matching_attribute = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });

    return found_matching_attribute;
}

static inline DOM::Element const* previous_sibling_with_same_type(DOM::Element const& element)
{
    for (auto const* sibling = element.previous_element_sibling(); sibling; sibling = sibling->previous_element_sibling()) {
        if (sibling->local_name() == element.local_name() && sibling->namespace_uri() == element.namespace_uri())
            return sibling;
    }
    return nullptr;
}

static inline DOM::Element const* next_sibling_with_same_type(DOM::Element const& element)
{
    for (auto const* sibling = element.next_element_sibling(); sibling; sibling = sibling->next_element_sibling()) {
        if (sibling->local_name() == element.local_name() && sibling->namespace_uri() == element.namespace_uri())
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
    if (auto const* input_element = as_if<HTML::HTMLInputElement>(element))
        return input_element->is_allowed_to_be_readonly()
            && !input_element->has_attribute(HTML::AttributeNames::readonly) && input_element->enabled();
    // - textarea elements that do not have a readonly attribute, and that are not disabled
    if (auto const* input_element = as_if<HTML::HTMLTextAreaElement>(element))
        return !input_element->has_attribute(HTML::AttributeNames::readonly) && input_element->enabled();
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

// https://drafts.csswg.org/css-shadow-1/#host-selector
static inline bool matches_host_pseudo_class(GC::Ref<DOM::Element const> element, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, CSS::SelectorList const& argument_selector_list)
{
    // When evaluated in the context of a shadow tree, it matches the shadow tree’s shadow host if the shadow host,
    // in its normal context, matches the selector argument. In any other context, it matches nothing.
    if (!shadow_host || element != shadow_host)
        return false;

    // NOTE: There's either 0 or 1 argument selector, since the syntax is :host or :host(<compound-selector>)
    if (!argument_selector_list.is_empty())
        return matches(argument_selector_list.first(), *element, nullptr, context);

    return true;
}

static bool matches_optimal_value_pseudo_class(DOM::Element const& element, HTML::HTMLMeterElement::ValueState desired_state)
{
    if (auto* meter = as_if<HTML::HTMLMeterElement>(element))
        return meter->value_state() == desired_state;
    return false;
}

static inline bool matches_pseudo_class(CSS::Selector::SimpleSelector::PseudoClassSelector const& pseudo_class, DOM::AbstractElement const& target, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ptr<DOM::ParentNode const> scope, SelectorKind selector_kind)
{
    context.attempted_pseudo_class_matches.set(pseudo_class.type, true);
    switch (pseudo_class.type) {
    case CSS::PseudoClass::__Count:
        VERIFY_NOT_REACHED();
    case CSS::PseudoClass::Link:
    case CSS::PseudoClass::AnyLink:
        if (target.pseudo_element().has_value())
            return false;
        // NOTE: AnyLink should match whether the link is visited or not, so if we ever start matching
        //       :visited, we'll need to handle these differently.
        return target.element().matches_link_pseudo_class();
    case CSS::PseudoClass::LocalLink: {
        if (target.pseudo_element().has_value())
            return false;
        return target.element().matches_local_link_pseudo_class();
    }
    case CSS::PseudoClass::Visited:
        // FIXME: Maybe match this selector sometimes?
        return false;
    case CSS::PseudoClass::Active:
        // FIXME: Match pseudo-elements
        if (target.pseudo_element().has_value())
            return false;
        return target.element().is_being_activated();
    case CSS::PseudoClass::Hover:
        // FIXME: Match pseudo-elements
        if (target.pseudo_element().has_value())
            return false;
        return matches_hover_pseudo_class(target.element());
    case CSS::PseudoClass::Focus:
        // FIXME: Match pseudo-elements
        if (target.pseudo_element().has_value())
            return false;
        return target.element().is_focused();
    case CSS::PseudoClass::FocusVisible:
        // FIXME: Match pseudo-elements
        if (target.pseudo_element().has_value())
            return false;
        return target.element().is_focused() && target.element().should_indicate_focus();
    case CSS::PseudoClass::FocusWithin: {
        // FIXME: Match pseudo-elements
        if (target.pseudo_element().has_value())
            return false;
        auto focused_area = target.document().focused_area();
        return focused_area && target.element().is_inclusive_ancestor_of(*focused_area);
    }
    case CSS::PseudoClass::Fullscreen: {
        if (target.pseudo_element().has_value())
            return false;
        return target.element().is_fullscreen_element();
    }
    case CSS::PseudoClass::FirstChild: {
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = const_cast<DOM::Element&>(target.element());
        if (context.collect_per_element_selector_involvement_metadata) {
            target_element.set_affected_by_first_child_pseudo_class(true);
        }
        return !target_element.previous_element_sibling();
    }
    case CSS::PseudoClass::LastChild: {
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = const_cast<DOM::Element&>(target.element());
        if (context.collect_per_element_selector_involvement_metadata) {
            target_element.set_affected_by_last_child_pseudo_class(true);
        }
        return !target_element.next_element_sibling();
    }
    case CSS::PseudoClass::OnlyChild: {
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = const_cast<DOM::Element&>(target.element());
        if (context.collect_per_element_selector_involvement_metadata) {
            target_element.set_affected_by_first_child_pseudo_class(true);
            target_element.set_affected_by_last_child_pseudo_class(true);
        }
        return !(target_element.previous_element_sibling() || target_element.next_element_sibling());
    }
    case CSS::PseudoClass::Empty: {
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = target.element();
        if (!target_element.has_children())
            return true;
        if (target_element.first_child_of_type<DOM::Element>())
            return false;
        // NOTE: CSS Selectors level 4 changed ":empty" to also match whitespace-only text nodes.
        //       However, none of the major browser supports this yet, so let's just hang back until they do.
        bool has_nonempty_text_child = false;
        target_element.for_each_child_of_type<DOM::Text>([&](auto const& text_child) {
            if (!text_child.data().is_empty()) {
                has_nonempty_text_child = true;
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });
        return !has_nonempty_text_child;
    }
    case CSS::PseudoClass::Root:
        if (target.pseudo_element().has_value())
            return false;
        return is<HTML::HTMLHtmlElement>(target.element());
    case CSS::PseudoClass::Host:
        if (target.pseudo_element().has_value())
            return false;
        return matches_host_pseudo_class(target.element(), shadow_host, context, pseudo_class.argument_selector_list);
    case CSS::PseudoClass::Scope:
        if (target.pseudo_element().has_value())
            return false;
        return scope ? &target.element() == scope : is<HTML::HTMLHtmlElement>(target.element());
    case CSS::PseudoClass::FirstOfType:
        if (target.pseudo_element().has_value())
            return false;
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(target.element()).set_affected_by_forward_positional_pseudo_class(true);
        }
        return !previous_sibling_with_same_type(target.element());
    case CSS::PseudoClass::LastOfType:
        if (target.pseudo_element().has_value())
            return false;
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(target.element()).set_affected_by_backward_positional_pseudo_class(true);
        }
        return !next_sibling_with_same_type(target.element());
    case CSS::PseudoClass::OnlyOfType: {
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = const_cast<DOM::Element&>(target.element());
        if (context.collect_per_element_selector_involvement_metadata) {
            target_element.set_affected_by_forward_positional_pseudo_class(true);
            target_element.set_affected_by_backward_positional_pseudo_class(true);
        }
        return !previous_sibling_with_same_type(target_element) && !next_sibling_with_same_type(target_element);
    }
    case CSS::PseudoClass::Lang:
        if (target.pseudo_element().has_value())
            return false;
        return matches_lang_pseudo_class(target.element(), pseudo_class.languages);
    case CSS::PseudoClass::Disabled:
        if (target.pseudo_element().has_value())
            return false;
        return target.element().matches_disabled_pseudo_class();
    case CSS::PseudoClass::Enabled:
        if (target.pseudo_element().has_value())
            return false;
        return target.element().matches_enabled_pseudo_class();
    case CSS::PseudoClass::Checked:
        if (target.pseudo_element().has_value())
            return false;
        return target.element().matches_checked_pseudo_class();
    case CSS::PseudoClass::Unchecked:
        if (target.pseudo_element().has_value())
            return false;
        return target.element().matches_unchecked_pseudo_class();
    case CSS::PseudoClass::Indeterminate:
        if (target.pseudo_element().has_value())
            return false;
        return matches_indeterminate_pseudo_class(target.element());
    case CSS::PseudoClass::HighValue:
        if (target.pseudo_element().has_value())
            return false;
        if (auto* meter = as_if<HTML::HTMLMeterElement>(target.element()))
            return meter->value() > meter->high();
        return false;
    case CSS::PseudoClass::LowValue:
        if (target.pseudo_element().has_value())
            return false;
        if (auto* meter = as_if<HTML::HTMLMeterElement>(target.element()))
            return meter->value() < meter->low();
        return false;
    case CSS::PseudoClass::OptimalValue:
        if (target.pseudo_element().has_value())
            return false;
        return matches_optimal_value_pseudo_class(target.element(), HTML::HTMLMeterElement::ValueState::Optimal);
    case CSS::PseudoClass::SuboptimalValue:
        if (target.pseudo_element().has_value())
            return false;
        return matches_optimal_value_pseudo_class(target.element(), HTML::HTMLMeterElement::ValueState::Suboptimal);
    case CSS::PseudoClass::EvenLessGoodValue:
        if (target.pseudo_element().has_value())
            return false;
        return matches_optimal_value_pseudo_class(target.element(), HTML::HTMLMeterElement::ValueState::EvenLessGood);
    case CSS::PseudoClass::Defined:
        if (target.pseudo_element().has_value())
            return false;
        return target.element().is_defined();
    case CSS::PseudoClass::Has: {
        // FIXME: Is ::pseudo:has() allowed?
        if (target.pseudo_element().has_value())
            return false;
        // :has() cannot be nested in a :has()
        if (selector_kind == SelectorKind::Relative)
            return false;
        auto& target_element = const_cast<DOM::Element&>(target.element());
        if (context.collect_per_element_selector_involvement_metadata) {
            if (&target_element == context.subject) {
                target_element.set_affected_by_has_pseudo_class_in_subject_position(true);
            } else {
                target_element.set_affected_by_has_pseudo_class_in_non_subject_position(true);
            }
        }
        // These selectors should be relative selectors (https://drafts.csswg.org/selectors-4/#relative-selector)
        for (auto const& selector : pseudo_class.argument_selector_list) {
            if (matches_has_pseudo_class(selector, target_element, shadow_host, context, scope))
                return true;
        }
        return false;
    }
    case CSS::PseudoClass::Is:
    case CSS::PseudoClass::Where:
        for (auto const& selector : pseudo_class.argument_selector_list) {
            if (matches(selector, target, shadow_host, context, scope))
                return true;
        }
        return false;
    case CSS::PseudoClass::Not:
        for (auto const& selector : pseudo_class.argument_selector_list) {
            if (matches(selector, target, shadow_host, context, scope))
                return false;
        }
        return true;
    case CSS::PseudoClass::NthChild:
    case CSS::PseudoClass::NthLastChild:
    case CSS::PseudoClass::NthOfType:
    case CSS::PseudoClass::NthLastOfType: {
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = target.element();
        auto const* parent = target_element.parent();

        if (context.collect_per_element_selector_involvement_metadata) {
            auto& mutable_element = const_cast<DOM::Element&>(target_element);
            switch (pseudo_class.type) {
            case CSS::PseudoClass::NthChild:
            case CSS::PseudoClass::NthOfType:
                mutable_element.set_affected_by_forward_positional_pseudo_class(true);
                break;
            case CSS::PseudoClass::NthLastChild:
            case CSS::PseudoClass::NthLastOfType:
                mutable_element.set_affected_by_backward_positional_pseudo_class(true);
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }

        auto matches_selector_list = [&context, shadow_host](CSS::SelectorList const& list, DOM::Element const& element) {
            if (list.is_empty())
                return true;
            return list.contains([&](auto const& selector) { return matches(selector, element, shadow_host, context); });
        };

        // https://drafts.csswg.org/selectors-4/#child-index
        // The pseudo-classes defined in this section select elements based on their index amongst their inclusive siblings.
        // NB: An element without a parent has no siblings, so its index is 1.
        int index = 1;
        switch (pseudo_class.type) {
        case CSS::PseudoClass::__Count:
            VERIFY_NOT_REACHED();
        case CSS::PseudoClass::NthChild: {
            if (!matches_selector_list(pseudo_class.argument_selector_list, target_element))
                return false;
            if (!parent)
                break;
            for (auto const* child = parent->first_child_of_type<DOM::Element>(); child && child != &target_element; child = child->next_element_sibling()) {
                if (matches_selector_list(pseudo_class.argument_selector_list, *child))
                    ++index;
            }
            break;
        }
        case CSS::PseudoClass::NthLastChild: {
            if (!matches_selector_list(pseudo_class.argument_selector_list, target_element))
                return false;
            if (!parent)
                break;
            for (auto const* child = parent->last_child_of_type<DOM::Element>(); child && child != &target_element; child = child->previous_element_sibling()) {
                if (matches_selector_list(pseudo_class.argument_selector_list, *child))
                    ++index;
            }
            break;
        }
        case CSS::PseudoClass::NthOfType: {
            for (auto const* child = previous_sibling_with_same_type(target_element); child; child = previous_sibling_with_same_type(*child))
                ++index;
            break;
        }
        case CSS::PseudoClass::NthLastOfType: {
            for (auto const* child = next_sibling_with_same_type(target_element); child; child = next_sibling_with_same_type(*child))
                ++index;
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }
        return pseudo_class.an_plus_b_pattern.matches(index);
    }
    case CSS::PseudoClass::Playing: {
        if (target.pseudo_element().has_value())
            return false;
        if (auto* media_element = as_if<HTML::HTMLMediaElement>(target.element()))
            return !media_element->paused();
        return false;
    }
    case CSS::PseudoClass::Paused: {
        if (target.pseudo_element().has_value())
            return false;
        if (auto* media_element = as_if<HTML::HTMLMediaElement>(target.element()))
            return media_element->paused();
        return false;
    }
    case CSS::PseudoClass::Seeking: {
        if (target.pseudo_element().has_value())
            return false;
        if (auto const* media_element = as_if<HTML::HTMLMediaElement>(target.element()))
            return media_element->seeking();
        return false;
    }
    case CSS::PseudoClass::Muted: {
        if (target.pseudo_element().has_value())
            return false;
        if (auto const* media_element = as_if<HTML::HTMLMediaElement>(target.element()))
            return media_element->muted();
        return false;
    }
    case CSS::PseudoClass::VolumeLocked: {
        if (target.pseudo_element().has_value())
            return false;
        // FIXME: Currently we don't allow the user to specify an override volume, so this is always false.
        //        Once we do, implement this!
        return false;
    }
    case CSS::PseudoClass::Buffering: {
        if (target.pseudo_element().has_value())
            return false;
        if (auto const* media_element = as_if<HTML::HTMLMediaElement>(target.element()))
            return media_element->blocked();
        return false;
    }
    case CSS::PseudoClass::Stalled: {
        if (target.pseudo_element().has_value())
            return false;
        if (auto const* media_element = as_if<HTML::HTMLMediaElement>(target.element()))
            return media_element->stalled();
        return false;
    }
    case CSS::PseudoClass::Target:
        if (target.pseudo_element().has_value())
            return false;
        return target.element().is_target();
    case CSS::PseudoClass::Dir: {
        // FIXME: Should we support pseudo-elements here?
        if (target.pseudo_element().has_value())
            return false;
        // "Values other than ltr and rtl are not invalid, but do not match anything."
        // - https://www.w3.org/TR/selectors-4/#the-dir-pseudo
        if (!pseudo_class.ident.has_value())
            return false;
        if (!first_is_one_of(pseudo_class.ident->keyword, CSS::Keyword::Ltr, CSS::Keyword::Rtl))
            return false;
        switch (target.element().directionality()) {
        case DOM::Element::Directionality::Ltr:
            return pseudo_class.ident->keyword == CSS::Keyword::Ltr;
        case DOM::Element::Directionality::Rtl:
            return pseudo_class.ident->keyword == CSS::Keyword::Rtl;
        }
        VERIFY_NOT_REACHED();
    }
    case CSS::PseudoClass::ReadOnly:
        if (target.pseudo_element().has_value())
            return false;
        return !matches_read_write_pseudo_class(target.element());
    case CSS::PseudoClass::ReadWrite:
        if (target.pseudo_element().has_value())
            return false;
        return matches_read_write_pseudo_class(target.element());
    case CSS::PseudoClass::PlaceholderShown:
        if (target.pseudo_element().has_value())
            return false;
        return target.element().matches_placeholder_shown_pseudo_class();
    case CSS::PseudoClass::Open:
        if (target.pseudo_element().has_value())
            return false;
        return matches_open_state_pseudo_class(target.element(), pseudo_class.type == CSS::PseudoClass::Open);
    case CSS::PseudoClass::Modal: {
        if (target.pseudo_element().has_value())
            return false;
        // https://drafts.csswg.org/selectors/#modal-state
        if (auto const* dialog_element = as_if<HTML::HTMLDialogElement>(target.element()))
            return dialog_element->is_modal();
        // FIXME: fullscreen elements are also modal.
        return false;
    }
    case CSS::PseudoClass::PopoverOpen: {
        if (target.pseudo_element().has_value())
            return false;
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-popover-open
        // The :popover-open pseudo-class is defined to match any HTML element whose popover attribute is not in the
        // No Popover state and whose popover visibility state is showing.
        if (auto const* html_element = as_if<HTML::HTMLElement>(target.element());
            html_element && html_element->has_attribute(HTML::AttributeNames::popover)) {
            return html_element->popover_visibility_state() == HTML::HTMLElement::PopoverVisibilityState::Showing;
        }

        return false;
    }
    case CSS::PseudoClass::Valid: {
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = target.element();

        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-valid
        // The :valid pseudo-class must match any element falling into one of the following categories:

        // - elements that are candidates for constraint validation and that satisfy their constraints
        if (auto form_associated_element = as_if<HTML::FormAssociatedElement>(target_element))
            if (form_associated_element->is_candidate_for_constraint_validation() && form_associated_element->satisfies_its_constraints())
                return true;

        // - form elements that are not the form owner of any elements that themselves are candidates for constraint validation but do not satisfy their constraints
        if (auto form_element = as_if<HTML::HTMLFormElement>(target_element)) {
            bool has_invalid_elements = false;
            target_element.for_each_in_subtree([&](auto& node) {
                if (auto form_associated_element = as_if<HTML::FormAssociatedElement>(&node)) {
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
        if (is<HTML::HTMLFieldSetElement>(target_element)) {
            bool has_invalid_children = false;
            target_element.for_each_in_subtree([&](auto& node) {
                if (auto form_associated_element = as_if<HTML::FormAssociatedElement>(&node)) {
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
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = target.element();
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-invalid
        // The :invalid pseudo-class must match any element falling into one of the following categories:

        // - elements that are candidates for constraint validation but that do not satisfy their constraints
        if (auto form_associated_element = as_if<HTML::FormAssociatedElement>(target_element))
            if (form_associated_element->is_candidate_for_constraint_validation() && !form_associated_element->satisfies_its_constraints())
                return true;

        // - form elements that are the form owner of one or more elements that themselves are candidates for constraint validation but do not satisfy their constraints
        if (auto form_element = as_if<HTML::HTMLFormElement>(target_element)) {
            bool has_invalid_elements = false;
            target_element.for_each_in_subtree([&](auto& node) {
                if (auto form_associated_element = as_if<HTML::FormAssociatedElement>(&node)) {
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
        if (is<HTML::HTMLFieldSetElement>(target_element)) {
            bool has_invalid_children = false;
            target_element.for_each_in_subtree([&](auto& node) {
                if (auto form_associated_element = as_if<HTML::FormAssociatedElement>(&node)) {
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
        if (target.pseudo_element().has_value())
            return false;
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-user-valid
        // The :user-valid pseudo-class must match input, textarea, and select elements whose user validity is true,
        bool user_validity = false;
        if (auto* input_element = as_if<HTML::HTMLInputElement>(target.element())) {
            user_validity = input_element->user_validity();
        } else if (auto* select_element = as_if<HTML::HTMLSelectElement>(target.element())) {
            user_validity = select_element->user_validity();
        } else if (auto* text_area_element = as_if<HTML::HTMLTextAreaElement>(target.element())) {
            user_validity = text_area_element->user_validity();
        }
        if (!user_validity)
            return false;

        // are candidates for constraint validation, and that satisfy their constraints.
        auto& form_associated_element = as<HTML::FormAssociatedElement>(target.element());
        if (form_associated_element.is_candidate_for_constraint_validation() && form_associated_element.satisfies_its_constraints())
            return true;

        return false;
    }
    case CSS::PseudoClass::UserInvalid: {
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = target.element();
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-user-invalid
        // The :user-invalid pseudo-class must match input, textarea, and select elements whose user validity is true,
        bool user_validity = false;
        if (auto* input_element = as_if<HTML::HTMLInputElement>(target_element)) {
            user_validity = input_element->user_validity();
        } else if (auto* select_element = as_if<HTML::HTMLSelectElement>(target_element)) {
            user_validity = select_element->user_validity();
        } else if (auto* text_area_element = as_if<HTML::HTMLTextAreaElement>(target_element)) {
            user_validity = text_area_element->user_validity();
        }
        if (!user_validity)
            return false;

        // are candidates for constraint validation but do not satisfy their constraints.
        auto& form_associated_element = as<HTML::FormAssociatedElement>(target_element);
        if (form_associated_element.is_candidate_for_constraint_validation() && !form_associated_element.satisfies_its_constraints())
            return true;

        return false;
    }
    case CSS::PseudoClass::Required: {
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = target.element();
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-required

        // The :required pseudo-class must match any element falling into one of the following categories:
        // - input elements that are required
        if (auto const* input_element = as_if<HTML::HTMLInputElement>(target_element)) {
            if (input_element->required_applies() && input_element->has_attribute(HTML::AttributeNames::required))
                return true;
        }
        // - select elements that have a required attribute
        else if (auto const* select_element = as_if<HTML::HTMLSelectElement>(target_element)) {
            if (select_element->has_attribute(HTML::AttributeNames::required))
                return true;
        }
        // - textarea elements that have a required attribute
        else if (auto const* textarea_element = as_if<HTML::HTMLTextAreaElement>(target_element)) {
            if (textarea_element->has_attribute(HTML::AttributeNames::required))
                return true;
        }

        return false;
    }
    case CSS::PseudoClass::Optional: {
        if (target.pseudo_element().has_value())
            return false;
        auto& target_element = target.element();
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-optional

        // The :optional pseudo-class must match any element falling into one of the following categories:
        // - input elements to which the required attribute applies that are not required
        if (auto const* input_element = as_if<HTML::HTMLInputElement>(target_element)) {
            if (input_element->required_applies() && !input_element->has_attribute(HTML::AttributeNames::required))
                return true;

            // AD-HOC: Chromium and Webkit also match for hidden inputs (and WPT expects this)
            // See: https://github.com/whatwg/html/issues/11273
            if (input_element->type_state() == HTML::HTMLInputElement::TypeAttributeState::Hidden)
                return true;
        }
        // - select elements that do not have a required attribute
        else if (auto const* select_element = as_if<HTML::HTMLSelectElement>(target_element)) {
            if (!select_element->has_attribute(HTML::AttributeNames::required))
                return true;
        }
        // - textarea elements that do not have a required attribute
        else if (auto const* textarea_element = as_if<HTML::HTMLTextAreaElement>(target_element)) {
            if (!textarea_element->has_attribute(HTML::AttributeNames::required))
                return true;
        }

        return false;
    }
    case CSS::PseudoClass::Default: {
        if (target.pseudo_element().has_value())
            return false;
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-default

        // The :default pseudo-class must match any element falling into one of the following categories:
        if (auto const* form_associated_element = as_if<HTML::FormAssociatedElement>(target.element())) {
            // - Submit buttons that are default buttons of their form owner.
            if (form_associated_element->is_submit_button() && form_associated_element->form() && form_associated_element->form()->default_button() == form_associated_element)
                return true;

            // - input elements to which the checked attribute applies and that have a checked attribute
            if (auto const* input_element = as_if<HTML::HTMLInputElement>(form_associated_element)) {
                if (input_element->checked_applies() && input_element->has_attribute(HTML::AttributeNames::checked))
                    return true;
            }

            // - option elements that have a selected attribute
            if (auto const* option_element = as_if<HTML::HTMLOptionElement>(form_associated_element)) {
                if (option_element->has_attribute(HTML::AttributeNames::selected))
                    return true;
            }
        }
        return false;
    }
    case CSS::PseudoClass::Autofill: {
        if (target.pseudo_element().has_value())
            return false;
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-autofill
        // FIXME: The :autofill and :-webkit-autofill pseudo-classes must match input elements which have been autofilled by
        //        user agent. These pseudo-classes must stop matching if the user edits the autofilled field.
        // NB: We don't support autofilling inputs yet, so this is always false.
        return false;
    }
    case CSS::PseudoClass::State: {
        if (target.pseudo_element().has_value())
            return false;
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-custom
        // The :state(identifier) pseudo-class must match all custom elements whose states set's set entries contains identifier.
        if (!target.element().is_custom())
            return false;
        if (auto custom_state_set = target.element().custom_state_set())
            return custom_state_set->has_state(pseudo_class.ident->string_value);
        return false;
    }
    case CSS::PseudoClass::Heading: {
        if (target.pseudo_element().has_value())
            return false;
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-heading
        // The :heading pseudo-class must match all h1, h2, h3, h4, h5, and h6 elements.

        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-heading-functional
        // The :heading(integer#) pseudo-class must match all h1, h2, h3, h4, h5, and h6 elements that have a heading level of integer. [CSSSYNTAX] [CSSVALUES]

        if (auto const* heading_element = as_if<HTML::HTMLHeadingElement>(target.element())) {
            if (pseudo_class.levels.is_empty())
                return true;
            return pseudo_class.levels.contains_slow(heading_element->heading_level());
        }

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

        // https://www.w3.org/TR/css-namespaces-3/#terminology
        // In CSS Namespaces a namespace name consisting of the empty string is taken to represent the null namespace
        // or lack of a namespace.
        if (selector_namespace.has_value() && selector_namespace.value().is_empty())
            return !element.namespace_uri().has_value();

        return selector_namespace.has_value() && selector_namespace.value() == element.namespace_uri();
    }
    VERIFY_NOT_REACHED();
}

static inline bool matches_simple_selector(CSS::Selector::SimpleSelector const& component, DOM::AbstractElement const& target, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ptr<DOM::ParentNode const> scope, SelectorKind selector_kind, [[maybe_unused]] GC::Ptr<DOM::Element const> anchor)
{
    if (target.pseudo_element().has_value() || should_block_shadow_host_matching(component, shadow_host, target.element()))
        return false;
    switch (component.type) {
    case CSS::Selector::SimpleSelector::Type::Universal:
    case CSS::Selector::SimpleSelector::Type::TagName: {
        if (target.pseudo_element().has_value())
            return false;

        auto const& target_element = target.element();
        auto const& qualified_name = component.qualified_name();

        // Reject if the tag name doesn't match
        if (component.type == CSS::Selector::SimpleSelector::Type::TagName) {
            // See https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
            if (target_element.document().document_type() == DOM::Document::Type::HTML && target_element.namespace_uri() == Namespace::HTML) {
                if (qualified_name.name.lowercase_name != target_element.local_name())
                    return false;
            } else if (!qualified_name.name.name.equals_ignoring_ascii_case(target_element.local_name())) {
                return false;
            }
        }

        return matches_namespace(qualified_name, target_element, context.style_sheet_for_rule);
    }
    case CSS::Selector::SimpleSelector::Type::Id:
        if (target.pseudo_element().has_value())
            return false;
        return component.name() == target.element().id();
    case CSS::Selector::SimpleSelector::Type::Class: {
        if (target.pseudo_element().has_value())
            return false;
        // Class selectors are matched case insensitively in quirks mode.
        // See: https://drafts.csswg.org/selectors-4/#class-html
        auto case_sensitivity = target.document().in_quirks_mode() ? CaseSensitivity::CaseInsensitive : CaseSensitivity::CaseSensitive;
        return target.element().has_class(component.name(), case_sensitivity);
    }
    case CSS::Selector::SimpleSelector::Type::Attribute:
        if (target.pseudo_element().has_value())
            return false;
        return matches_attribute(component.attribute(), context.style_sheet_for_rule, target.element());
    case CSS::Selector::SimpleSelector::Type::PseudoClass:
        return matches_pseudo_class(component.pseudo_class(), target, shadow_host, context, scope, selector_kind);
    case CSS::Selector::SimpleSelector::Type::PseudoElement:
        if (component.pseudo_element().type() == CSS::PseudoElement::Slotted) {
            VERIFY(context.slotted_element);
            return matches(component.pseudo_element().compound_selector(), *context.slotted_element, shadow_host, context);
        }
        if (component.pseudo_element().type() == CSS::PseudoElement::Part) {
            // All part names need to match the [pseudo-]element.
            // FIXME: Support matching pseudo-elements.

            // https://drafts.csswg.org/css-shadow-1/#part
            // "The ::part() pseudo-element only matches anything when the originating element is a shadow host."
            // FIXME: How does this interact with :host ?
            for (auto ancestor_shadow_root = target.element().containing_shadow_root();
                ancestor_shadow_root;
                ancestor_shadow_root = ancestor_shadow_root->containing_shadow_root()) {

                // https://drafts.csswg.org/css-shadow-1/#part-element-map
                // "The descendants of an element [...] does not include the shadow trees of the element."
                bool const is_direct_child_scope = ancestor_shadow_root->host()->containing_shadow_root() == context.rule_shadow_root;
                bool const is_host_part_own_scope = ancestor_shadow_root == context.rule_shadow_root && context.for_host_part_matching;
                if (!is_direct_child_scope && !is_host_part_own_scope)
                    continue;

                auto const& part_element_map = ancestor_shadow_root->part_element_map();
                bool all_part_names_match = true;
                for (auto const& part_name : component.pseudo_element().ident_list()) {
                    if (auto matching_parts = part_element_map.get(part_name);
                        !matching_parts.has_value() || !matching_parts->contains(target)) {
                        all_part_names_match = false;
                        break;
                    }
                }
                if (all_part_names_match) {
                    context.part_owning_parent = ancestor_shadow_root->host();
                    return true;
                }
            }
            return false;
        }
        // Other pseudo-element matching/not-matching is handled in the top level matches().
        return true;

    case CSS::Selector::SimpleSelector::Type::Nesting:
        // Nesting either behaves like :is(), or like :scope.
        // :is() is handled already, by us replacing it with :is() directly, so if we
        // got here, it's :scope.
        return matches_pseudo_class(CSS::Selector::SimpleSelector::PseudoClassSelector { .type = CSS::PseudoClass::Scope }, target, shadow_host, context, scope, selector_kind);
    case CSS::Selector::SimpleSelector::Type::Invalid:
        // Invalid selectors never match
        return false;
    }
    VERIFY_NOT_REACHED();
}

bool matches_compound_selector(CSS::Selector const& selector, int component_list_index, DOM::AbstractElement const& target,
    GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ptr<DOM::ParentNode const> scope,
    SelectorKind selector_kind, GC::Ptr<DOM::Element const> anchor)
{
    auto& compound_selector = selector.compound_selectors()[component_list_index];

    // NB: :host::part() must consult the rule shadow root's part map even when the direct-child scope check would skip
    //     it. That path only applies when the rule comes from a shadow stylesheet (rule_shadow_root is set); otherwise
    //     the same-shadow-root exception cannot trigger. Scan this compound for :host, including inside :is() (nesting
    //     expands &::part() in a :host rule to :is(:host)::part()).
    bool const saved_for_host_part_matching = context.for_host_part_matching;
    ScopeGuard restore_for_host_part = [&] { context.for_host_part_matching = saved_for_host_part_matching; };
    for (auto const& simple : compound_selector.simple_selectors) {
        if (!context.rule_shadow_root)
            break;
        if (simple.type != CSS::Selector::SimpleSelector::Type::PseudoClass)
            continue;
        auto const& pseudo_class = simple.pseudo_class();
        if (pseudo_class.type == CSS::PseudoClass::Host) {
            context.for_host_part_matching = true;
            break;
        }
        if (pseudo_class.type == CSS::PseudoClass::Is) {
            bool found = false;
            for (auto const& arg : pseudo_class.argument_selector_list) {
                for (auto const& arg_compound : arg->compound_selectors()) {
                    for (auto const& arg_simple : arg_compound.simple_selectors) {
                        if (arg_simple.type == CSS::Selector::SimpleSelector::Type::PseudoClass
                            && arg_simple.pseudo_class().type == CSS::PseudoClass::Host) {
                            found = true;
                            break;
                        }
                    }
                    if (found)
                        break;
                }
                if (found)
                    break;
            }
            if (found) {
                context.for_host_part_matching = true;
                break;
            }
        }
    }

    // Defer :has() until every other simple selector in the compound has matched.
    // :has() has side effects (setting per-element flags used by invalidation) and
    // is expensive, so running it at compounds that ultimately fail is both
    // wasteful and produces spuriously permissive flags.
    auto is_has_pseudo_class = [](CSS::Selector::SimpleSelector const& s) {
        return s.type == CSS::Selector::SimpleSelector::Type::PseudoClass
            && s.pseudo_class().type == CSS::PseudoClass::Has;
    };
    bool has_part_pseudo_element = false;
    for (auto const& simple_selector : compound_selector.simple_selectors) {
        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoElement
            && simple_selector.pseudo_element().type() == CSS::PseudoElement::Part) {
            has_part_pseudo_element = true;
            break;
        }
    }
    auto defer_has_pseudo_class = !has_part_pseudo_element;

    auto element_for_compound_matching { target };
    for (auto& simple_selector : compound_selector.simple_selectors.in_reverse()) {
        if (defer_has_pseudo_class && is_has_pseudo_class(simple_selector))
            continue;
        if (!matches_simple_selector(simple_selector, element_for_compound_matching, shadow_host, context, scope, selector_kind, anchor)) {
            return false;
        }
        if (context.part_owning_parent) {
            // Match the rest of the compound selector against the shadow host that element is a part of.
            element_for_compound_matching = *context.part_owning_parent;
            context.part_owning_parent = nullptr;
            // Also have to update the shadow host we're using.
            // If the rule comes from the element's own shadow root, we're matching
            // :host::part() from within the shadow DOM's own stylesheet.
            // Keep shadow_host as-is so that :host can match.
            auto is_internal_part = context.rule_shadow_root
                && context.rule_shadow_root == element_for_compound_matching.element().shadow_root();
            if (!is_internal_part) {
                if (auto shadow_root = element_for_compound_matching.element().containing_shadow_root()) {
                    shadow_host = shadow_root->host();
                } else {
                    shadow_host = nullptr;
                }
            }
        }
    }
    if (defer_has_pseudo_class) {
        for (auto& simple_selector : compound_selector.simple_selectors.in_reverse()) {
            if (!is_has_pseudo_class(simple_selector))
                continue;
            if (!matches_simple_selector(simple_selector, element_for_compound_matching, shadow_host, context, scope, selector_kind, anchor)) {
                return false;
            }
        }
    }
    auto const& element = element_for_compound_matching;

    if (selector_kind == SelectorKind::Relative && component_list_index == 0) {
        VERIFY(anchor);
        return &element.element() != anchor;
    }

    switch (compound_selector.combinator) {
    case CSS::Selector::Combinator::None:
        VERIFY(selector_kind != SelectorKind::Relative);
        return true;
    case CSS::Selector::Combinator::Descendant:
        VERIFY(component_list_index != 0);
        for (auto ancestor = traverse_up(element, shadow_host); ancestor; ancestor = traverse_up(ancestor, shadow_host)) {
            auto const* ancestor_element = as_if<DOM::Element>(*ancestor);
            if (!ancestor_element)
                continue;
            if (ancestor_element == anchor)
                return false;
            if (context.inside_has_argument_match && context.collect_per_element_selector_involvement_metadata)
                const_cast<DOM::Element&>(*ancestor_element).set_in_has_scope(true);
            if (matches_compound_selector(selector, component_list_index - 1, *ancestor_element, shadow_host, context, scope, selector_kind, anchor))
                return true;
        }
        return false;
    case CSS::Selector::Combinator::ImmediateChild: {
        VERIFY(component_list_index != 0);
        auto parent = traverse_up(element, shadow_host);
        if (!parent || !parent->is_element())
            return false;
        auto& parent_element = static_cast<DOM::Element const&>(*parent);
        if (context.inside_has_argument_match && context.collect_per_element_selector_involvement_metadata)
            const_cast<DOM::Element&>(parent_element).set_in_has_scope(true);
        return matches_compound_selector(selector, component_list_index - 1, parent_element, shadow_host, context, scope, selector_kind, anchor);
    }
    case CSS::Selector::Combinator::NextSibling:
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(element.element()).set_affected_by_direct_sibling_combinator(true);
            auto new_sibling_invalidation_distance = max(selector.sibling_invalidation_distance(), element.element().sibling_invalidation_distance());
            const_cast<DOM::Element&>(element.element()).set_sibling_invalidation_distance(new_sibling_invalidation_distance);
        }
        VERIFY(component_list_index != 0);
        if (auto* sibling = element.element().previous_element_sibling()) {
            if (context.inside_has_argument_match && context.collect_per_element_selector_involvement_metadata)
                const_cast<DOM::Element&>(*sibling).set_in_has_scope(true);
            return matches_compound_selector(selector, component_list_index - 1, *sibling, shadow_host, context, scope, selector_kind, anchor);
        }
        return false;
    case CSS::Selector::Combinator::SubsequentSibling:
        if (context.collect_per_element_selector_involvement_metadata) {
            const_cast<DOM::Element&>(element.element()).set_affected_by_indirect_sibling_combinator(true);
        }
        VERIFY(component_list_index != 0);
        for (auto* sibling = element.element().previous_element_sibling(); sibling; sibling = sibling->previous_element_sibling()) {
            if (context.inside_has_argument_match && context.collect_per_element_selector_involvement_metadata)
                const_cast<DOM::Element&>(*sibling).set_in_has_scope(true);
            if (matches_compound_selector(selector, component_list_index - 1, *sibling, shadow_host, context, scope, selector_kind, anchor))
                return true;
        }
        return false;
    case CSS::Selector::Combinator::Column:
        TODO();
    }
    VERIFY_NOT_REACHED();
}

bool fast_matches(CSS::Selector const& selector, DOM::Element const& element_to_match, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context);

bool matches(CSS::Selector const& selector, DOM::AbstractElement const& target, GC::Ptr<DOM::Element const> shadow_host,
    MatchContext& context, GC::Ptr<DOM::ParentNode const> scope,
    SelectorKind selector_kind, GC::Ptr<DOM::Element const> anchor)
{
    if (selector_kind == SelectorKind::Normal && selector.can_use_fast_matches())
        return fast_matches(selector, target.element(), shadow_host, context);

    VERIFY(!selector.compound_selectors().is_empty());
    if (selector.has_part_pseudo_element()) {
        // For ::part() selectors, find any additional pseudo-element beyond ::part() (e.g., the ::selection in
        // ::part(foo)::selection) and verify it matches the target pseudo-element. A bare ::part(foo) selector has no
        // additional pseudo-element and should only match base element styles.
        Optional<CSS::PseudoElement> target_pseudo;
        for (auto const& simple : selector.compound_selectors().last().simple_selectors) {
            if (simple.type == CSS::Selector::SimpleSelector::Type::PseudoElement
                && simple.pseudo_element().type() != CSS::PseudoElement::Part) {
                target_pseudo = simple.pseudo_element().type();
                break;
            }
        }
        if (target_pseudo != target.pseudo_element())
            return false;
    } else {
        if (target.pseudo_element().has_value() && selector.target_pseudo_element().has_value() && selector.target_pseudo_element().value().type() != target.pseudo_element())
            return false;
        if (!target.pseudo_element().has_value() && selector.target_pseudo_element().has_value())
            return false;
    }

    return matches_compound_selector(selector, selector.compound_selectors().size() - 1, target.element(), shadow_host, context, scope, selector_kind, anchor);
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
