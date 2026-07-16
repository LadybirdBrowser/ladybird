/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/AncestorFilter.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/SelectorRustBridge.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>
#include <LibWeb/HTML/HTMLDetailsElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLHeadingElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLMeterElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLProgressElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SelectorRustFFI.h>

namespace Web::SelectorMatching {

static bool fly_string_equals_utf16(Utf16FlyString const& fly_string, Utf16View utf16_string)
{
    return utf16_string == fly_string.view();
}

template<typename... Names>
static bool fly_string_is_one_of_utf16(Utf16View utf16_string, Names const&... names)
{
    return (fly_string_equals_utf16(names, utf16_string) || ...);
}

static u32 salted_tag_name_hash(Utf16FlyString const& tag_name)
{
    return CSS::ancestor_filter_hash_for_tag_name(tag_name.ascii_case_insensitive_hash());
}

static u32 salted_id_hash(Utf16FlyString const& id)
{
    return CSS::ancestor_filter_hash_for_id(id.hash());
}

static u32 salted_class_hash(Utf16FlyString const& class_name)
{
    return CSS::ancestor_filter_hash_for_class(class_name.hash());
}

static u32 salted_attribute_hash(Utf16FlyString const& attribute_name)
{
    return CSS::ancestor_filter_hash_for_attribute(attribute_name.ascii_case_insensitive_hash());
}

void HasFastRejectFilter::add(u32 hash)
{
    auto const first_bit = hash & 4095;
    auto const second_bit = (hash >> 16) & 4095;
    buckets[first_bit / 64] |= 1ull << (first_bit % 64);
    buckets[second_bit / 64] |= 1ull << (second_bit % 64);
}

bool HasFastRejectFilter::may_contain(u32 hash) const
{
    auto const first_bit = hash & 4095;
    auto const second_bit = (hash >> 16) & 4095;
    return (buckets[first_bit / 64] & (1ull << (first_bit % 64)))
        && (buckets[second_bit / 64] & (1ull << (second_bit % 64)));
}

static bool is_excluded_attribute_for_has_filter(Utf16FlyString const& name)
{
    return name == "class"sv
        || name == "id"sv
        || name == "style"sv;
}

static void add_element_identifier_hashes(HasFastRejectFilter& filter, DOM::Element const& element, MatchContext const& context)
{
    filter.add(salted_tag_name_hash(element.local_name()));
    if (element.id().has_value())
        filter.add(salted_id_hash(element.id().value()));
    for (auto const& class_name : element.class_names())
        filter.add(salted_class_hash(class_name));
    element.for_each_attribute([&](auto const& attribute) {
        auto const& name = attribute.name();
        if (is_excluded_attribute_for_has_filter(name))
            return;
        filter.add(salted_attribute_hash(name));
    });

    if (context.inside_has_argument_match && context.collect_per_element_selector_involvement_metadata)
        const_cast<DOM::Element&>(element).set_in_has_scope(true);
}

static void populate_has_fast_reject_filter(HasFastRejectFilter& filter, DOM::Element const& anchor, HasFastRejectFilterTraversalType traversal_type, MatchContext const& context)
{
    // This intentionally mirrors the traversal scope that a matching :has()
    // argument would inspect. The filter is only populated on the second
    // :has() check for the same anchor/scope, so one subtree walk can reject
    // several later arguments without penalizing the single-check case.
    switch (traversal_type) {
    case HasFastRejectFilterTraversalType::Children:
        anchor.for_each_child([&](DOM::Node const& child) {
            if (child.is_element())
                add_element_identifier_hashes(filter, static_cast<DOM::Element const&>(child), context);
            return IterationDecision::Continue;
        });
        break;
    case HasFastRejectFilterTraversalType::Descendants:
        anchor.for_each_in_subtree([&](DOM::Node const& descendant) {
            if (descendant.is_element())
                add_element_identifier_hashes(filter, static_cast<DOM::Element const&>(descendant), context);
            return TraversalDecision::Continue;
        });
        break;
    }
    filter.populated = true;
}

static void collect_has_fast_reject_hashes(CSS::Selector::SimpleSelector const& simple_selector, Vector<u32>& hashes, bool in_quirks_mode)
{
    switch (simple_selector.type) {
    case CSS::Selector::SimpleSelector::Type::TagName:
        hashes.append(salted_tag_name_hash(simple_selector.qualified_name().name.lowercase_name));
        break;
    case CSS::Selector::SimpleSelector::Type::Id:
        hashes.append(salted_id_hash(simple_selector.id_name()));
        break;
    case CSS::Selector::SimpleSelector::Type::Class:
        if (in_quirks_mode)
            break;
        hashes.append(salted_class_hash(simple_selector.class_name()));
        break;
    case CSS::Selector::SimpleSelector::Type::Attribute: {
        auto const& name = simple_selector.attribute().qualified_name.name.lowercase_name;
        if (!is_excluded_attribute_for_has_filter(name))
            hashes.append(salted_attribute_hash(name));
        break;
    }
    case CSS::Selector::SimpleSelector::Type::Universal:
    case CSS::Selector::SimpleSelector::Type::PseudoClass:
    case CSS::Selector::SimpleSelector::Type::PseudoElement:
    case CSS::Selector::SimpleSelector::Type::Nesting:
    case CSS::Selector::SimpleSelector::Type::Invalid:
        break;
    }
}

static Vector<u32> collect_has_fast_reject_hashes(CSS::Selector const& selector, bool in_quirks_mode)
{
    Vector<u32> hashes;
    for (auto const& compound_selector : selector.compound_selectors()) {
        for (auto const& simple_selector : compound_selector.simple_selectors)
            collect_has_fast_reject_hashes(simple_selector, hashes, in_quirks_mode);
    }
    return hashes;
}

static Optional<HasFastRejectFilterTraversalType> has_fast_reject_filter_traversal_type(CSS::Selector const& selector)
{
    if (selector.compound_selectors().is_empty())
        return {};

    switch (selector.compound_selectors().first().combinator) {
    case CSS::Selector::Combinator::ImmediateChild:
        if (selector.compound_selectors().size() == 1)
            return HasFastRejectFilterTraversalType::Children;
        // The argument can still contain later descendant combinators, e.g.
        // `:has(> .wrapper .hit)`. Since we collect hashes from the whole
        // relative selector, use the descendant scope so nested requirements
        // do not cause false rejections.
        return HasFastRejectFilterTraversalType::Descendants;
    case CSS::Selector::Combinator::Descendant:
        return HasFastRejectFilterTraversalType::Descendants;
    case CSS::Selector::Combinator::None:
    case CSS::Selector::Combinator::NextSibling:
    case CSS::Selector::Combinator::SubsequentSibling:
    case CSS::Selector::Combinator::Column:
    case CSS::Selector::Combinator::PseudoElement:
        return {};
    }
    VERIFY_NOT_REACHED();
}

static bool selector_contains_sibling_combinator(CSS::Selector const& selector)
{
    for (auto const& compound_selector : selector.compound_selectors()) {
        if (compound_selector.combinator == CSS::Selector::Combinator::NextSibling
            || compound_selector.combinator == CSS::Selector::Combinator::SubsequentSibling) {
            return true;
        }
    }
    return false;
}

static bool should_reject_with_has_fast_reject_filter(CSS::Selector const& selector, DOM::Element const& anchor, MatchContext& context)
{
    if (!context.has_fast_reject_filter_cache)
        return false;

    if (context.collect_per_element_selector_involvement_metadata && selector_contains_sibling_combinator(selector))
        return false;

    auto traversal_type = has_fast_reject_filter_traversal_type(selector);
    if (!traversal_type.has_value())
        return false;

    auto hashes = collect_has_fast_reject_hashes(selector, anchor.document().in_quirks_mode());
    if (hashes.is_empty())
        return false;

    HasFastRejectFilterKey key {
        .element = &anchor,
        .traversal_type = *traversal_type,
    };
    auto& filter = context.has_fast_reject_filter_cache->ensure(key);
    if (!filter.seen_once) {
        filter.seen_once = true;
        return false;
    }
    if (!filter.populated)
        populate_has_fast_reject_filter(filter, anchor, *traversal_type, context);

    for (auto hash : hashes) {
        if (!filter.may_contain(hash))
            return true;
    }
    return false;
}

static bool language_range_matches_tag(Utf16View language_range, Utf16View language_tag)
{
    // 1. Split both the extended language range and the language tag being compared into a list of subtags by
    //    dividing on the hyphen (%x2D) character.
    auto range_subtags = language_range.split_view('-', SplitBehavior::KeepEmpty);
    auto tag_subtags = language_tag.split_view('-', SplitBehavior::KeepEmpty);

    //    Two subtags match if either they are the same when compared case-insensitively or the language range's subtag
    //    is the wildcard '*'.
    auto subtags_match = [](Utf16View language_range_subtag, Utf16View language_subtag) {
        return language_range_subtag == u"*"sv
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
        if (*range_subtag == u"*"sv) {
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
        if (tag_subtag->length_in_code_units() == 1 && is_ascii_alphanumeric(tag_subtag->code_unit_at(0))) {
            return false;
        }

        // E. Else, move to the next subtag in the language tag's list and continue with the loop.
        ++tag_subtag;
    }

    // 4. When the language range's list has no more subtags, the match succeeds.
    return true;
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
    // - input elements whose type attribute is in the Checkbox state and whose indeterminateness is true
    // FIXME: - input elements whose type attribute is in the Radio Button state and whose radio button group contains no input elements whose checkedness state is true.
    if (auto* input_element = as_if<HTML::HTMLInputElement>(element)) {
        switch (input_element->type_state()) {
        case HTML::HTMLInputElement::TypeAttributeState::Checkbox:
            // https://whatpr.org/html-attr-input-switch/9546/semantics-other.html#selector-indeterminate
            // input elements whose type attribute is in the Checkbox state, whose switch attribute is not set
            return input_element->indeterminate() && !element.has_attribute(HTML::AttributeNames::switch_);
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

static bool matches_optimal_value_pseudo_class(DOM::Element const& element, HTML::HTMLMeterElement::ValueState desired_state)
{
    if (auto* meter = as_if<HTML::HTMLMeterElement>(element))
        return meter->value_state() == desired_state;
    return false;
}

static bool matches_pseudo_class_state(CSS::PseudoClass pseudo_class, DOM::Element const& element)
{
    switch (pseudo_class) {
    case CSS::PseudoClass::Active:
        return element.is_being_activated();
    case CSS::PseudoClass::AnyLink:
    case CSS::PseudoClass::Link:
        // NOTE: AnyLink should match whether the link is visited or not, so if we ever start matching
        //       :visited, we'll need to handle these differently.
        return element.matches_link_pseudo_class();
    case CSS::PseudoClass::Autofill:
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-autofill
        // FIXME: The :autofill and :-webkit-autofill pseudo-classes must match input elements which have been autofilled by
        //        user agent. These pseudo-classes must stop matching if the user edits the autofilled field.
        // NB: We don't support autofilling inputs yet, so this is always false.
        return false;
    case CSS::PseudoClass::Buffering:
        if (auto const* media_element = as_if<HTML::HTMLMediaElement>(element))
            return media_element->blocked();
        return false;
    case CSS::PseudoClass::Checked:
        return element.matches_checked_pseudo_class();
    case CSS::PseudoClass::Default: {
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-default
        if (auto const* form_associated_element = as_if<HTML::FormAssociatedElement>(element)) {
            if (form_associated_element->is_submit_button() && form_associated_element->form() && form_associated_element->form()->default_button() == form_associated_element)
                return true;
            if (auto const* input_element = as_if<HTML::HTMLInputElement>(form_associated_element)) {
                if (input_element->checked_applies() && input_element->has_attribute(HTML::AttributeNames::checked))
                    return true;
            }
            if (auto const* option_element = as_if<HTML::HTMLOptionElement>(form_associated_element)) {
                if (option_element->has_attribute(HTML::AttributeNames::selected))
                    return true;
            }
        }
        return false;
    }
    case CSS::PseudoClass::Defined:
        return element.is_defined();
    case CSS::PseudoClass::Disabled:
        return element.matches_disabled_pseudo_class();
    case CSS::PseudoClass::Enabled:
        return element.matches_enabled_pseudo_class();
    case CSS::PseudoClass::EvenLessGoodValue:
        return matches_optimal_value_pseudo_class(element, HTML::HTMLMeterElement::ValueState::EvenLessGood);
    case CSS::PseudoClass::Focus:
        return element.is_focused();
    case CSS::PseudoClass::FocusVisible:
        return element.is_focused() && element.should_indicate_focus();
    case CSS::PseudoClass::FocusWithin:
        return element.matches_focus_within_pseudo_class();
    case CSS::PseudoClass::Fullscreen:
        return element.is_fullscreen_element();
    case CSS::PseudoClass::HighValue:
        if (auto const* meter = as_if<HTML::HTMLMeterElement>(element))
            return meter->value() > meter->high();
        return false;
    case CSS::PseudoClass::Hover:
        return matches_hover_pseudo_class(element);
    case CSS::PseudoClass::Indeterminate:
        return matches_indeterminate_pseudo_class(element);
    case CSS::PseudoClass::Invalid: {
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-invalid
        if (auto form_associated_element = as_if<HTML::FormAssociatedElement>(element)) {
            if (form_associated_element->is_candidate_for_constraint_validation() && !form_associated_element->satisfies_its_constraints())
                return true;
        }

        if (auto form_element = as_if<HTML::HTMLFormElement>(element)) {
            bool has_invalid_elements = false;
            element.for_each_in_subtree([&](auto& node) {
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

        if (is<HTML::HTMLFieldSetElement>(element)) {
            bool has_invalid_children = false;
            element.for_each_in_subtree([&](auto& node) {
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
    case CSS::PseudoClass::LocalLink:
        return element.matches_local_link_pseudo_class();
    case CSS::PseudoClass::LowValue:
        if (auto const* meter = as_if<HTML::HTMLMeterElement>(element))
            return meter->value() < meter->low();
        return false;
    case CSS::PseudoClass::Modal:
        // https://drafts.csswg.org/selectors/#modal-state
        if (auto const* dialog_element = as_if<HTML::HTMLDialogElement>(element))
            return dialog_element->is_modal();
        // FIXME: fullscreen elements are also modal.
        return false;
    case CSS::PseudoClass::Muted:
        if (auto const* media_element = as_if<HTML::HTMLMediaElement>(element))
            return media_element->muted();
        return false;
    case CSS::PseudoClass::Open:
        return matches_open_state_pseudo_class(element, true);
    case CSS::PseudoClass::OptimalValue:
        return matches_optimal_value_pseudo_class(element, HTML::HTMLMeterElement::ValueState::Optimal);
    case CSS::PseudoClass::Optional:
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-optional
        if (auto const* input_element = as_if<HTML::HTMLInputElement>(element)) {
            if (input_element->required_applies() && !input_element->has_attribute(HTML::AttributeNames::required))
                return true;
            // AD-HOC: Chromium and Webkit also match for hidden inputs (and WPT expects this)
            // See: https://github.com/whatwg/html/issues/11273
            return input_element->type_state() == HTML::HTMLInputElement::TypeAttributeState::Hidden;
        }
        if (auto const* select_element = as_if<HTML::HTMLSelectElement>(element))
            return !select_element->has_attribute(HTML::AttributeNames::required);
        if (auto const* textarea_element = as_if<HTML::HTMLTextAreaElement>(element))
            return !textarea_element->has_attribute(HTML::AttributeNames::required);
        return false;
    case CSS::PseudoClass::Paused:
        if (auto const* media_element = as_if<HTML::HTMLMediaElement>(element))
            return media_element->paused();
        return false;
    case CSS::PseudoClass::PlaceholderShown:
        return element.matches_placeholder_shown_pseudo_class();
    case CSS::PseudoClass::Playing:
        if (auto const* media_element = as_if<HTML::HTMLMediaElement>(element))
            return !media_element->paused();
        return false;
    case CSS::PseudoClass::PopoverOpen:
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-popover-open
        if (auto const* html_element = as_if<HTML::HTMLElement>(element);
            html_element && html_element->has_attribute(HTML::AttributeNames::popover)) {
            return html_element->popover_visibility_state() == HTML::HTMLElement::PopoverVisibilityState::Showing;
        }
        return false;
    case CSS::PseudoClass::ReadOnly:
        return !matches_read_write_pseudo_class(element);
    case CSS::PseudoClass::ReadWrite:
        return matches_read_write_pseudo_class(element);
    case CSS::PseudoClass::Required:
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-required
        if (auto const* input_element = as_if<HTML::HTMLInputElement>(element))
            return input_element->required_applies() && input_element->has_attribute(HTML::AttributeNames::required);
        if (auto const* select_element = as_if<HTML::HTMLSelectElement>(element))
            return select_element->has_attribute(HTML::AttributeNames::required);
        if (auto const* textarea_element = as_if<HTML::HTMLTextAreaElement>(element))
            return textarea_element->has_attribute(HTML::AttributeNames::required);
        return false;
    case CSS::PseudoClass::Seeking:
        if (auto const* media_element = as_if<HTML::HTMLMediaElement>(element))
            return media_element->seeking();
        return false;
    case CSS::PseudoClass::Stalled:
        if (auto const* media_element = as_if<HTML::HTMLMediaElement>(element))
            return media_element->stalled();
        return false;
    case CSS::PseudoClass::SuboptimalValue:
        return matches_optimal_value_pseudo_class(element, HTML::HTMLMeterElement::ValueState::Suboptimal);
    case CSS::PseudoClass::Target:
        return element.is_target();
    case CSS::PseudoClass::Unchecked:
        return element.matches_unchecked_pseudo_class();
    case CSS::PseudoClass::UserInvalid:
    case CSS::PseudoClass::UserValid: {
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-user-valid
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-user-invalid
        bool user_validity = false;
        if (auto const* input_element = as_if<HTML::HTMLInputElement>(element))
            user_validity = input_element->user_validity();
        else if (auto const* select_element = as_if<HTML::HTMLSelectElement>(element))
            user_validity = select_element->user_validity();
        else if (auto const* text_area_element = as_if<HTML::HTMLTextAreaElement>(element))
            user_validity = text_area_element->user_validity();
        if (!user_validity)
            return false;

        auto const& form_associated_element = as<HTML::FormAssociatedElement>(element);
        if (!form_associated_element.is_candidate_for_constraint_validation())
            return false;
        return pseudo_class == CSS::PseudoClass::UserValid
            ? form_associated_element.satisfies_its_constraints()
            : !form_associated_element.satisfies_its_constraints();
    }
    case CSS::PseudoClass::Valid: {
        // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-valid
        if (auto form_associated_element = as_if<HTML::FormAssociatedElement>(element)) {
            if (form_associated_element->is_candidate_for_constraint_validation() && form_associated_element->satisfies_its_constraints())
                return true;
        }

        if (auto form_element = as_if<HTML::HTMLFormElement>(element)) {
            bool has_invalid_elements = false;
            element.for_each_in_subtree([&](auto& node) {
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

        if (is<HTML::HTMLFieldSetElement>(element)) {
            bool has_invalid_children = false;
            element.for_each_in_subtree([&](auto& node) {
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
    case CSS::PseudoClass::Visited:
        return element.matches_visited_pseudo_class();
    case CSS::PseudoClass::VolumeLocked:
        // FIXME: Currently we don't allow the user to specify an override volume, so this is always false.
        //        Once we do, implement this!
        return false;
    case CSS::PseudoClass::__Count:
    case CSS::PseudoClass::Dir:
    case CSS::PseudoClass::Empty:
    case CSS::PseudoClass::FirstChild:
    case CSS::PseudoClass::FirstOfType:
    case CSS::PseudoClass::Has:
    case CSS::PseudoClass::Heading:
    case CSS::PseudoClass::Host:
    case CSS::PseudoClass::Is:
    case CSS::PseudoClass::Lang:
    case CSS::PseudoClass::LastChild:
    case CSS::PseudoClass::LastOfType:
    case CSS::PseudoClass::Not:
    case CSS::PseudoClass::NthChild:
    case CSS::PseudoClass::NthLastChild:
    case CSS::PseudoClass::NthLastOfType:
    case CSS::PseudoClass::NthOfType:
    case CSS::PseudoClass::OnlyChild:
    case CSS::PseudoClass::OnlyOfType:
    case CSS::PseudoClass::Root:
    case CSS::PseudoClass::Scope:
    case CSS::PseudoClass::State:
    case CSS::PseudoClass::Where:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

bool matches(CSS::Selector const& selector, DOM::AbstractElement const& target, GC::Ptr<DOM::Element const> shadow_host,
    MatchContext& context, GC::Ptr<DOM::ParentNode const> scope)
{
    return CSS::SelectorFFI::rust_selector_matches(
        &selector.rust_selector(),
        &target.element(),
        CSS::pseudo_element_to_ffi(target.pseudo_element()),
        shadow_host.ptr(),
        &context,
        scope.ptr(),
        context.collect_per_element_selector_involvement_metadata,
        context.inside_has_argument_match);
}

bool matches_originating_element_for_pseudo_element(CSS::Selector const& selector, CSS::PseudoElement pseudo_element, DOM::AbstractElement const& target, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, GC::Ptr<DOM::ParentNode const> scope)
{
    VERIFY(!target.pseudo_element().has_value());

    return CSS::SelectorFFI::rust_selector_matches_originating_element(
        &selector.rust_selector(),
        CSS::pseudo_element_to_ffi(pseudo_element),
        &target.element(),
        shadow_host.ptr(),
        &context,
        scope.ptr(),
        context.collect_per_element_selector_involvement_metadata,
        context.inside_has_argument_match);
}

static MatchContext& rust_match_context(void* context)
{
    VERIFY(context);
    return *static_cast<MatchContext*>(context);
}

static DOM::Element const& ffi_element(void const* element)
{
    VERIFY(element);
    return *static_cast<DOM::Element const*>(element);
}

static Utf16View ffi_string_view(CSS::SelectorFFI::StringView string)
{
    static_assert(sizeof(u16) == sizeof(char16_t));
    if (string.length == 0)
        return {};
    VERIFY(string.data);
    return { reinterpret_cast<char16_t const*>(string.data), string.length };
}

static CSS::Selector::SimpleSelector const& ffi_simple_selector(void const* simple_selector)
{
    VERIFY(simple_selector);
    return *static_cast<CSS::Selector::SimpleSelector const*>(simple_selector);
}

static bool is_in_null_namespace(DOM::Element const& element)
{
    return !element.namespace_uri().has_value() || element.namespace_uri()->is_empty();
}

static bool matches_namespace(CSS::Selector::SimpleSelector::QualifiedName const& qualified_name, DOM::Element const& element, GC::Ptr<CSS::CSSStyleSheet const> style_sheet_for_rule)
{
    switch (qualified_name.namespace_type) {
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Default:
        // "if no default namespace has been declared for selectors, this is equivalent to *|E."
        if (!style_sheet_for_rule || !style_sheet_for_rule->default_namespace_rule())
            return true;
        // "Otherwise it is equivalent to ns|E where ns is the default namespace."
        if (style_sheet_for_rule->default_namespace_rule()->namespace_uri().is_empty())
            return is_in_null_namespace(element);

        return element.namespace_uri().has_value()
            && style_sheet_for_rule->default_namespace_rule()->namespace_uri() == element.namespace_uri()->view();
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::None:
        // "elements with name E without a namespace"
        return is_in_null_namespace(element);
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Any:
        // "elements with name E in any namespace, including those without a namespace"
        return true;
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Named: {
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
        if (selector_namespace.has_value() && selector_namespace->is_empty())
            return is_in_null_namespace(element);
        return selector_namespace.has_value()
            && element.namespace_uri().has_value()
            && *selector_namespace == element.namespace_uri()->view();
    }
    }
    VERIFY_NOT_REACHED();
}

using CSS::SelectorFFI::AttributeCaseType;
using CSS::SelectorFFI::AttributeMatchType;
using CSS::SelectorFFI::Combinator;
using CSS::SelectorFFI::Direction;
using CSS::SelectorFFI::HasCacheResult;
using CSS::SelectorFFI::NamespaceType;
using CSS::SelectorFFI::StringView;
using CSS::SelectorFFI::TagNameMatchingMode;

#define DECLARE_SELECTOR_FFI_CALLBACK(function) \
    extern "C" decltype(CSS::SelectorFFI::function) function

DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_matches_universal);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_matches_tag_name);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_matches_id);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_matches_class);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_matches_attribute);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_matches_pseudo_class);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_matches_language);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_matches_direction);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_matches_state);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_matches_heading);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_parent_element);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_parent_element_in_light_tree);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_previous_element_sibling);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_next_element_sibling);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_first_element_child);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_first_element_descendant);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_next_element_descendant);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_has_no_element_or_nonempty_text_children);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_has_same_type);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_is_document_root);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_is_shadow_tree_slot);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_slotted_parent);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_part_parent);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_note_structural_pseudo_class);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_note_has_pseudo_class);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_note_sibling_combinator);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_note_has_sibling_combinator_anchor);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_note_has_sibling_combinator_element);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_note_has_scope_element);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_set_inside_has_argument);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_has_cache_get);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_has_cache_set);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_should_reject_has_argument);

#undef DECLARE_SELECTOR_FFI_CALLBACK

extern "C" bool selector_ffi_matches_universal(void* context, void const* element, void const* cxx_simple_selector)
{
    auto& match_context = rust_match_context(context);
    auto const& qualified_name = ffi_simple_selector(cxx_simple_selector).qualified_name();
    return matches_namespace(qualified_name, ffi_element(element), match_context.style_sheet_for_rule);
}

extern "C" bool selector_ffi_matches_tag_name(void* context, void const* element, void const* cxx_simple_selector, TagNameMatchingMode matching_mode)
{
    auto& match_context = rust_match_context(context);
    auto const& target = ffi_element(element);
    auto const& qualified_name = ffi_simple_selector(cxx_simple_selector).qualified_name();
    bool const is_html_element_in_html_document = target.namespace_uri() == Namespace::HTML
        && target.document().document_type() == DOM::Document::Type::HTML;
    auto const& name_to_match = is_html_element_in_html_document ? qualified_name.name.lowercase_name : qualified_name.name.name;
    bool name_matches;
    if (is_html_element_in_html_document || matching_mode == TagNameMatchingMode::Fast)
        name_matches = target.local_name() == name_to_match;
    else
        name_matches = target.local_name().equals_ignoring_ascii_case(name_to_match);
    return name_matches
        && matches_namespace(qualified_name, target, match_context.style_sheet_for_rule);
}

extern "C" bool selector_ffi_matches_id(void const* element, void const* cxx_simple_selector)
{
    return ffi_element(element).id() == ffi_simple_selector(cxx_simple_selector).id_name();
}

extern "C" bool selector_ffi_matches_class(void const* element, void const* cxx_simple_selector)
{
    auto const& target = ffi_element(element);
    auto case_sensitivity = target.document().in_quirks_mode() ? CaseSensitivity::CaseInsensitive : CaseSensitivity::CaseSensitive;
    return target.has_class(ffi_simple_selector(cxx_simple_selector).class_name(), case_sensitivity);
}

static bool matches_attribute_value(CSS::Selector::SimpleSelector::Attribute::MatchType match_type, Utf16View selector_value, Utf16View element_value, CaseSensitivity case_sensitivity)
{
    bool const case_insensitive = case_sensitivity == CaseSensitivity::CaseInsensitive;
    auto values_equal = [&](Utf16View first, Utf16View second) {
        return case_insensitive ? first.equals_ignoring_ascii_case(second) : first == second;
    };

    switch (match_type) {
    case CSS::Selector::SimpleSelector::Attribute::MatchType::HasAttribute:
        return true;
    case CSS::Selector::SimpleSelector::Attribute::MatchType::ExactValueMatch:
        return values_equal(element_value, selector_value);
    case CSS::Selector::SimpleSelector::Attribute::MatchType::ContainsWord:
        if (selector_value.is_empty())
            return false;
        return element_value.split_view(' ', SplitBehavior::Nothing).contains([&](auto value) { return values_equal(value, selector_value); });
    case CSS::Selector::SimpleSelector::Attribute::MatchType::ContainsString:
        return !selector_value.is_empty()
            && (case_insensitive ? element_value.find_code_unit_offset_ignoring_case(selector_value).has_value() : element_value.contains(selector_value));
    case CSS::Selector::SimpleSelector::Attribute::MatchType::StartsWithSegment:
        if (element_value.is_empty())
            return selector_value.is_empty();
        if (selector_value.is_empty() || element_value.length_in_code_units() < selector_value.length_in_code_units())
            return false;
        if (element_value.length_in_code_units() == selector_value.length_in_code_units())
            return values_equal(element_value, selector_value);
        return values_equal(element_value.substring_view(0, selector_value.length_in_code_units()), selector_value)
            && element_value.code_unit_at(selector_value.length_in_code_units()) == '-';
    case CSS::Selector::SimpleSelector::Attribute::MatchType::StartsWithString:
        return !selector_value.is_empty()
            && selector_value.length_in_code_units() <= element_value.length_in_code_units()
            && values_equal(element_value.substring_view(0, selector_value.length_in_code_units()), selector_value);
    case CSS::Selector::SimpleSelector::Attribute::MatchType::EndsWithString:
        return !selector_value.is_empty()
            && selector_value.length_in_code_units() <= element_value.length_in_code_units()
            && values_equal(element_value.substring_view(element_value.length_in_code_units() - selector_value.length_in_code_units()), selector_value);
    }
    VERIFY_NOT_REACHED();
}

extern "C" bool selector_ffi_matches_attribute(void* context, void const* element, void const* cxx_simple_selector)
{
    auto& match_context = rust_match_context(context);
    auto const& target = ffi_element(element);
    auto const& attribute_selector = ffi_simple_selector(cxx_simple_selector).attribute();
    auto const& qualified_name = attribute_selector.qualified_name;
    auto const& attribute_name = qualified_name.name.name;
    auto const& lowercase_attribute_name = qualified_name.name.lowercase_name;
    auto const selector_value = attribute_selector.value.utf16_view();
    auto const match_type = attribute_selector.match_type;

    CaseSensitivity case_sensitivity;
    switch (attribute_selector.case_type) {
    case CSS::Selector::SimpleSelector::Attribute::CaseType::CaseSensitiveMatch:
        case_sensitivity = CaseSensitivity::CaseSensitive;
        break;
    case CSS::Selector::SimpleSelector::Attribute::CaseType::CaseInsensitiveMatch:
        case_sensitivity = CaseSensitivity::CaseInsensitive;
        break;
    case CSS::Selector::SimpleSelector::Attribute::CaseType::DefaultMatch:
        case_sensitivity = target.document().is_html_document()
                && target.namespace_uri() == Namespace::HTML
                && qualified_name.namespace_type == CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Default
                && fly_string_is_one_of_utf16(
                    attribute_name,
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
                    HTML::AttributeNames::valign, HTML::AttributeNames::valuetype, HTML::AttributeNames::vlink)
            ? CaseSensitivity::CaseInsensitive
            : CaseSensitivity::CaseSensitive;
        break;
    }

    auto attribute_matches = [&](DOM::Attr const& attribute) {
        return matches_attribute_value(match_type, selector_value, attribute.value().utf16_view(), case_sensitivity);
    };

    switch (qualified_name.namespace_type) {
    // "In keeping with the Namespaces in the XML recommendation, default namespaces do not apply to attributes,
    //  therefore attribute selectors without a namespace component apply only to attributes that have no namespace (equivalent to "|attr")"
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Default:
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::None: {
        auto const& name_to_match = target.document().is_html_document() && target.namespace_uri() == Namespace::HTML
            ? lowercase_attribute_name
            : attribute_name;
        for (u32 i = 0; i < target.attributes()->length(); ++i) {
            auto const* attribute = target.attributes()->item(i);
            if (!attribute->namespace_uri().has_value() && attribute->local_name() == name_to_match)
                return attribute_matches(*attribute);
        }
        return false;
    }
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Any: {
        bool const use_lowercase_name = target.document().is_html_document() && target.namespace_uri() == Namespace::HTML;
        auto const& name_to_match = use_lowercase_name ? lowercase_attribute_name : attribute_name;
        for (u32 i = 0; i < target.attributes()->length(); ++i) {
            auto const* attribute = target.attributes()->item(i);
            if (attribute->local_name() == name_to_match && attribute_matches(*attribute))
                return true;
        }
        return false;
    }
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Named: {
        if (!match_context.style_sheet_for_rule)
            return false;
        auto selector_namespace = match_context.style_sheet_for_rule->namespace_uri(qualified_name.namespace_);
        if (!selector_namespace.has_value())
            return false;
        for (u32 i = 0; i < target.attributes()->length(); ++i) {
            auto const* attribute = target.attributes()->item(i);
            if (attribute->namespace_uri().has_value()
                && *selector_namespace == attribute->namespace_uri()->view()
                && attribute->local_name() == attribute_name)
                return attribute_matches(*attribute);
        }
        return false;
    }
    }
    VERIFY_NOT_REACHED();
}

static bool is_rust_structural_or_functional_pseudo_class(CSS::PseudoClass pseudo_class)
{
    return first_is_one_of(
        pseudo_class,
        CSS::PseudoClass::Empty,
        CSS::PseudoClass::FirstChild,
        CSS::PseudoClass::FirstOfType,
        CSS::PseudoClass::Has,
        CSS::PseudoClass::Host,
        CSS::PseudoClass::Is,
        CSS::PseudoClass::LastChild,
        CSS::PseudoClass::LastOfType,
        CSS::PseudoClass::Not,
        CSS::PseudoClass::NthChild,
        CSS::PseudoClass::NthLastChild,
        CSS::PseudoClass::NthLastOfType,
        CSS::PseudoClass::NthOfType,
        CSS::PseudoClass::OnlyChild,
        CSS::PseudoClass::OnlyOfType,
        CSS::PseudoClass::Root,
        CSS::PseudoClass::Scope,
        CSS::PseudoClass::Where,
        CSS::PseudoClass::Dir,
        CSS::PseudoClass::Heading,
        CSS::PseudoClass::Lang,
        CSS::PseudoClass::State);
}

extern "C" bool selector_ffi_matches_pseudo_class(void const* element, u8 pseudo_class_value)
{
    auto pseudo_class = static_cast<CSS::PseudoClass>(pseudo_class_value);
    VERIFY(pseudo_class < CSS::PseudoClass::__Count);
    VERIFY(!is_rust_structural_or_functional_pseudo_class(pseudo_class));
    return matches_pseudo_class_state(pseudo_class, ffi_element(element));
}

extern "C" bool selector_ffi_matches_language(void const* element, StringView language)
{
    auto element_language = ffi_element(element).lang();
    return element_language.has_value()
        && language_range_matches_tag(ffi_string_view(language), *element_language);
}

extern "C" bool selector_ffi_matches_direction(void const* element, Direction direction)
{
    switch (ffi_element(element).directionality()) {
    case DOM::Element::Directionality::Ltr:
        return direction == Direction::LeftToRight;
    case DOM::Element::Directionality::Rtl:
        return direction == Direction::RightToLeft;
    }
    VERIFY_NOT_REACHED();
}

extern "C" bool selector_ffi_matches_state(void const* element, void const* cxx_simple_selector)
{
    auto const& target = ffi_element(element);
    if (!target.is_custom())
        return false;
    if (auto custom_state_set = target.custom_state_set())
        return custom_state_set->has_state(ffi_simple_selector(cxx_simple_selector).pseudo_class().ident->string_value);
    return false;
}

extern "C" bool selector_ffi_matches_heading(void const* element, i64 const* levels, size_t level_count)
{
    auto const* heading = as_if<HTML::HTMLHeadingElement>(ffi_element(element));
    if (!heading)
        return false;
    if (level_count == 0)
        return true;
    VERIFY(levels);
    return ReadonlySpan<i64> { levels, level_count }.contains_slow(heading->heading_level());
}

extern "C" void const* selector_ffi_parent_element(void const* element, void const* shadow_host)
{
    auto const& target = ffi_element(element);
    if (!shadow_host)
        return target.parent_element();
    if (element == shadow_host)
        return nullptr;
    return target.parent_or_shadow_host_element();
}

extern "C" void const* selector_ffi_parent_element_in_light_tree(void const* element)
{
    return ffi_element(element).parent_element();
}

extern "C" void const* selector_ffi_previous_element_sibling(void const* element)
{
    return ffi_element(element).previous_element_sibling();
}

extern "C" void const* selector_ffi_next_element_sibling(void const* element)
{
    return ffi_element(element).next_element_sibling();
}

extern "C" void const* selector_ffi_first_element_child(void const* element)
{
    return ffi_element(element).first_child_of_type<DOM::Element>();
}

extern "C" void const* selector_ffi_first_element_descendant(void const* element)
{
    auto const& root = ffi_element(element);
    for (auto const* node = root.first_child(); node; node = node->next_in_pre_order(&root)) {
        if (node->is_element())
            return static_cast<DOM::Element const*>(node);
    }
    return nullptr;
}

extern "C" void const* selector_ffi_next_element_descendant(void const* element, void const* root)
{
    auto const& root_element = ffi_element(root);
    for (auto const* node = static_cast<DOM::Node const*>(&ffi_element(element))->next_in_pre_order(&root_element); node; node = node->next_in_pre_order(&root_element)) {
        if (node->is_element())
            return static_cast<DOM::Element const*>(node);
    }
    return nullptr;
}

extern "C" bool selector_ffi_has_no_element_or_nonempty_text_children(void const* element)
{
    auto const& target = ffi_element(element);
    if (!target.has_children())
        return true;
    if (target.first_child_of_type<DOM::Element>())
        return false;
    bool has_nonempty_text_child = false;
    target.for_each_child_of_type<DOM::Text>([&](auto const& text) {
        if (!text.data().is_empty()) {
            has_nonempty_text_child = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return !has_nonempty_text_child;
}

extern "C" bool selector_ffi_has_same_type(void const* first, void const* second)
{
    auto const& first_element = ffi_element(first);
    auto const& second_element = ffi_element(second);
    return first_element.local_name() == second_element.local_name()
        && first_element.namespace_uri() == second_element.namespace_uri();
}

extern "C" bool selector_ffi_is_document_root(void const* element)
{
    return is<HTML::HTMLHtmlElement>(ffi_element(element));
}

extern "C" bool selector_ffi_is_shadow_tree_slot(void const* element)
{
    auto const* slot = as_if<HTML::HTMLSlotElement>(ffi_element(element));
    return slot && slot->root().is_shadow_root();
}

extern "C" CSS::SelectorFFI::ElementAndShadowHost selector_ffi_slotted_parent(void* context, void const* element)
{
    auto& match_context = rust_match_context(context);
    auto const& target = ffi_element(element);
    for (auto slot = target.assigned_slot_internal(); slot; slot = slot->assigned_slot_internal()) {
        auto const* slot_shadow_root = as_if<DOM::ShadowRoot>(slot->root());
        if (slot_shadow_root != match_context.rule_shadow_root)
            continue;
        return {
            .element = slot,
            .shadow_host = slot_shadow_root ? slot_shadow_root->host() : nullptr,
        };
    }
    return {};
}

extern "C" CSS::SelectorFFI::ElementAndShadowHost selector_ffi_part_parent(void* context, void const* element, StringView const* identifiers, size_t identifier_count, bool allow_same_shadow_root_scope, void const* shadow_host)
{
    auto& match_context = rust_match_context(context);
    auto const& target_element = ffi_element(element);
    DOM::AbstractElement target { target_element };

    for (auto ancestor_shadow_root = target_element.containing_shadow_root();
        ancestor_shadow_root;
        ancestor_shadow_root = ancestor_shadow_root->containing_shadow_root()) {
        bool const is_direct_child_scope = ancestor_shadow_root->host()->containing_shadow_root() == match_context.rule_shadow_root;
        bool const is_host_part_own_scope = allow_same_shadow_root_scope && ancestor_shadow_root == match_context.rule_shadow_root;
        if (!is_direct_child_scope && !is_host_part_own_scope)
            continue;

        bool all_part_names_match = true;
        for (size_t i = 0; i < identifier_count; ++i) {
            auto part_name = Utf16FlyString::from_utf16(ffi_string_view(identifiers[i]));
            auto matching_parts = ancestor_shadow_root->part_element_map().get(part_name);
            if (!matching_parts.has_value() || !matching_parts->contains(target)) {
                all_part_names_match = false;
                break;
            }
        }
        if (!all_part_names_match)
            continue;

        auto const& host = *ancestor_shadow_root->host();
        auto const* next_shadow_host = static_cast<DOM::Element const*>(shadow_host);
        bool const is_internal_part = match_context.rule_shadow_root
            && match_context.rule_shadow_root == host.shadow_root();
        if (!is_internal_part) {
            if (auto containing_shadow_root = host.containing_shadow_root())
                next_shadow_host = containing_shadow_root->host();
            else
                next_shadow_host = nullptr;
        }
        return { .element = &host, .shadow_host = next_shadow_host };
    }
    return {};
}

static void note_structural_pseudo_class_match_attempt(MatchContext& match_context, DOM::Element& element)
{
    if (&element != match_context.subject)
        element.set_affected_by_structural_pseudo_class_in_non_subject_position();
}

extern "C" void selector_ffi_note_structural_pseudo_class(void* context, void const* element, u8 pseudo_class_value)
{
    auto& match_context = rust_match_context(context);
    if (!match_context.collect_per_element_selector_involvement_metadata)
        return;
    auto& target = const_cast<DOM::Element&>(ffi_element(element));
    auto pseudo_class = static_cast<CSS::PseudoClass>(pseudo_class_value);
    switch (pseudo_class) {
    case CSS::PseudoClass::FirstChild:
        target.set_affected_by_first_child_pseudo_class(true);
        break;
    case CSS::PseudoClass::LastChild:
        target.set_affected_by_last_child_pseudo_class(true);
        break;
    case CSS::PseudoClass::OnlyChild:
        target.set_affected_by_first_child_pseudo_class(true);
        target.set_affected_by_last_child_pseudo_class(true);
        break;
    case CSS::PseudoClass::FirstOfType:
    case CSS::PseudoClass::NthChild:
    case CSS::PseudoClass::NthOfType:
        target.set_affected_by_forward_positional_pseudo_class(true);
        break;
    case CSS::PseudoClass::LastOfType:
    case CSS::PseudoClass::NthLastChild:
    case CSS::PseudoClass::NthLastOfType:
        target.set_affected_by_backward_positional_pseudo_class(true);
        break;
    case CSS::PseudoClass::OnlyOfType:
        target.set_affected_by_forward_positional_pseudo_class(true);
        target.set_affected_by_backward_positional_pseudo_class(true);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    note_structural_pseudo_class_match_attempt(match_context, target);
}

extern "C" void selector_ffi_note_has_pseudo_class(void* context, void const* element)
{
    auto& match_context = rust_match_context(context);
    if (!match_context.collect_per_element_selector_involvement_metadata)
        return;
    auto& target = const_cast<DOM::Element&>(ffi_element(element));
    if (&target == match_context.subject)
        target.set_affected_by_has_pseudo_class_in_subject_position(true);
    else
        target.set_affected_by_has_pseudo_class_in_non_subject_position();
}

extern "C" void selector_ffi_note_sibling_combinator(void* context, void const* element, Combinator combinator, size_t sibling_invalidation_distance)
{
    auto& match_context = rust_match_context(context);
    if (!match_context.collect_per_element_selector_involvement_metadata)
        return;
    auto& target = const_cast<DOM::Element&>(ffi_element(element));
    if (combinator == Combinator::NextSibling) {
        target.set_affected_by_direct_sibling_combinator(true);
        target.set_sibling_invalidation_distance(max(sibling_invalidation_distance, target.sibling_invalidation_distance()));
    } else {
        VERIFY(combinator == Combinator::SubsequentSibling);
        target.set_affected_by_indirect_sibling_combinator(true);
    }
    if (&target != match_context.subject)
        target.set_affected_by_sibling_combinator_in_non_subject_position();
}

extern "C" void selector_ffi_note_has_sibling_combinator_anchor(void* context, void const* anchor)
{
    auto& match_context = rust_match_context(context);
    if (match_context.collect_per_element_selector_involvement_metadata)
        const_cast<DOM::Element&>(ffi_element(anchor)).set_affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator(true);
}

extern "C" void selector_ffi_note_has_sibling_combinator_element(void* context, void const* element)
{
    auto& match_context = rust_match_context(context);
    if (!match_context.collect_per_element_selector_involvement_metadata)
        return;
    auto& target = const_cast<DOM::Element&>(ffi_element(element));
    target.set_in_has_scope(true);
    target.set_in_subtree_of_has_pseudo_class_relative_selector_with_sibling_combinator(true);
}

extern "C" void selector_ffi_note_has_scope_element(void* context, void const* element)
{
    auto& match_context = rust_match_context(context);
    if (match_context.inside_has_argument_match
        && match_context.collect_per_element_selector_involvement_metadata)
        const_cast<DOM::Element&>(ffi_element(element)).set_in_has_scope(true);
}

extern "C" void selector_ffi_set_inside_has_argument(void* context, bool value)
{
    rust_match_context(context).inside_has_argument_match = value;
}

extern "C" HasCacheResult selector_ffi_has_cache_get(void* context, u64 selector_id, void const* anchor)
{
    auto& match_context = rust_match_context(context);
    auto& counters = ffi_element(anchor).document().style_invalidation_counters();
    ++counters.has_match_invocations;
    if (!match_context.has_result_cache)
        return HasCacheResult::NotCached;
    auto cached = match_context.has_result_cache->get({ selector_id, &ffi_element(anchor) });
    if (!cached.has_value()) {
        ++counters.has_result_cache_misses;
        return HasCacheResult::NotCached;
    }
    ++counters.has_result_cache_hits;
    return cached.value() == HasMatchResult::Matched ? HasCacheResult::Matched : HasCacheResult::NotMatched;
}

extern "C" void selector_ffi_has_cache_set(void* context, u64 selector_id, void const* anchor, bool result)
{
    auto& match_context = rust_match_context(context);
    if (match_context.has_result_cache)
        match_context.has_result_cache->set({ selector_id, &ffi_element(anchor) }, result ? HasMatchResult::Matched : HasMatchResult::NotMatched);
}

extern "C" bool selector_ffi_should_reject_has_argument(void* context, void const* selector, void const* anchor)
{
    VERIFY(selector);
    return should_reject_with_has_fast_reject_filter(
        *static_cast<CSS::Selector const*>(selector),
        ffi_element(anchor),
        rust_match_context(context));
}

}
