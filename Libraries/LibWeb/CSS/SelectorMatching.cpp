/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
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

static CSS::SelectorFFI::Element element_to_ffi(DOM::Element const* element)
{
    if (!element)
        return {};

    return {
        .pointer = element,
    };
}

bool matches(CSS::Selector const& selector, DOM::AbstractElement const& target, GC::Ptr<DOM::Element const> shadow_host,
    MatchContext& context, GC::Ptr<DOM::ParentNode const> scope)
{
    return CSS::SelectorFFI::rust_selector_matches(
        &selector.rust_selector(),
        element_to_ffi(&target.element()),
        CSS::pseudo_element_to_ffi(target.pseudo_element()),
        element_to_ffi(shadow_host.ptr()),
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
        element_to_ffi(&target.element()),
        element_to_ffi(shadow_host.ptr()),
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

static bool is_in_null_namespace(DOM::Element const& element)
{
    return !element.namespace_uri().has_value() || element.namespace_uri()->is_empty();
}

using CSS::SelectorFFI::Combinator;
using CSS::SelectorFFI::Direction;
using CSS::SelectorFFI::HasCacheResult;
using CSS::SelectorFFI::NamespaceType;
using CSS::SelectorFFI::StringView;

#define DECLARE_SELECTOR_FFI_CALLBACK(function) \
    extern "C" decltype(CSS::SelectorFFI::function) function

DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_qualified_name);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_id);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_classes);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_class_names_are_case_insensitive);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_namespace_is_null);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_html_element_in_html_document);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_document_root);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_link);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_fullscreen);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_heading_level);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_has_popover_attribute);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_popover_is_showing);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_direction);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_has_custom_state);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_language);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_focused);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_should_indicate_focus);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_has_focus_within);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_active);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_checked);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_defined);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_disabled);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_enabled);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_local_link);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_placeholder_shown);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_target);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_unchecked);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_media_element);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_media_is_blocked);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_media_is_muted);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_media_is_paused);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_media_is_seeking);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_media_is_stalled);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_default);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_meter_value_state);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_meter_value_is_high);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_meter_value_is_low);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_hovered);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_indeterminate);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_validity_state);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_modal);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_open);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_required_state);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_is_read_write);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_user_validity_state);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_local_name);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_class_name);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_attribute_count);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_attribute);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_default_namespace);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_resolve_namespace);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_parent_element);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_parent_element_in_light_tree);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_previous_element_sibling);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_next_element_sibling);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_first_element_child);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_first_element_descendant);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_next_element_descendant);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_has_element_child);
DECLARE_SELECTOR_FFI_CALLBACK(selector_ffi_element_has_nonempty_text_child);
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

// `Utf16FlyString` is its interned one-word representation. The accessors below expose pointers
// to live words only for the duration of the synchronous selector-matching call.
static_assert(sizeof(Utf16FlyString) == sizeof(uintptr_t));
static_assert(alignof(Utf16FlyString) == alignof(uintptr_t));

static uintptr_t interned_string_identity(Utf16FlyString const& string)
{
    uintptr_t identity;
    __builtin_memcpy(&identity, &string, sizeof(identity));
    return identity;
}

static CSS::SelectorFFI::DomStringView dom_string_view(Utf16View string)
{
    if (string.has_ascii_storage()) {
        auto storage = string.ascii_span();
        return {
            .data = storage.data(),
            .length = storage.size(),
            .is_ascii = true,
        };
    }
    auto storage = string.utf16_span();
    return {
        .data = storage.data(),
        .length = storage.size(),
        .is_ascii = false,
    };
}

extern "C" CSS::SelectorFFI::ElementQualifiedName selector_ffi_element_qualified_name(void const* element)
{
    auto const& target = ffi_element(element);
    return {
        .local_name = reinterpret_cast<uintptr_t const*>(&target.local_name()),
        .namespace_ = target.namespace_uri().has_value() ? reinterpret_cast<uintptr_t const*>(&target.namespace_uri().value()) : nullptr,
    };
}

extern "C" uintptr_t const* selector_ffi_element_id(void const* element)
{
    auto const& id = ffi_element(element).id();
    return id.has_value() ? reinterpret_cast<uintptr_t const*>(&id.value()) : nullptr;
}

extern "C" CSS::SelectorFFI::InternedStringList selector_ffi_element_classes(void const* element)
{
    auto const& classes = ffi_element(element).class_names();
    return {
        .data = reinterpret_cast<uintptr_t const*>(classes.data()),
        .count = classes.size(),
    };
}

extern "C" bool selector_ffi_element_class_names_are_case_insensitive(void const* element)
{
    return ffi_element(element).document().in_quirks_mode();
}

extern "C" bool selector_ffi_element_namespace_is_null(void const* element)
{
    return is_in_null_namespace(ffi_element(element));
}

extern "C" bool selector_ffi_element_is_html_element_in_html_document(void const* element)
{
    auto const& target = ffi_element(element);
    return target.namespace_uri() == Namespace::HTML
        && target.document().document_type() == DOM::Document::Type::HTML;
}

extern "C" bool selector_ffi_element_is_document_root(void const* element)
{
    return is<HTML::HTMLHtmlElement>(ffi_element(element));
}

extern "C" bool selector_ffi_element_is_link(void const* element)
{
    return ffi_element(element).matches_link_pseudo_class();
}

extern "C" bool selector_ffi_element_is_fullscreen(void const* element)
{
    return ffi_element(element).is_fullscreen_element();
}

extern "C" i64 selector_ffi_element_heading_level(void const* element)
{
    auto const* heading = as_if<HTML::HTMLHeadingElement>(ffi_element(element));
    return heading ? heading->heading_level() : 0;
}

extern "C" bool selector_ffi_element_has_popover_attribute(void const* element)
{
    return ffi_element(element).has_attribute(HTML::AttributeNames::popover);
}

extern "C" bool selector_ffi_element_popover_is_showing(void const* element)
{
    auto const* html_element = as_if<HTML::HTMLElement>(ffi_element(element));
    return html_element
        && html_element->popover_visibility_state() == HTML::HTMLElement::PopoverVisibilityState::Showing;
}

extern "C" Direction selector_ffi_element_direction(void const* element)
{
    switch (ffi_element(element).directionality()) {
    case DOM::Element::Directionality::Ltr:
        return Direction::LeftToRight;
    case DOM::Element::Directionality::Rtl:
        return Direction::RightToLeft;
    }
    VERIFY_NOT_REACHED();
}

extern "C" bool selector_ffi_element_has_custom_state(void const* element, uintptr_t state)
{
    auto const& target = ffi_element(element);
    if (!target.is_custom())
        return false;
    if (auto custom_state_set = target.custom_state_set())
        return custom_state_set->has_state(*reinterpret_cast<Utf16FlyString const*>(&state));
    return false;
}

extern "C" CSS::SelectorFFI::DomStringView selector_ffi_element_language(void const* element)
{
    auto language = ffi_element(element).lang_view();
    return language.has_value() ? dom_string_view(*language) : CSS::SelectorFFI::DomStringView {};
}

extern "C" bool selector_ffi_element_is_focused(void const* element)
{
    return ffi_element(element).is_focused();
}

extern "C" bool selector_ffi_element_should_indicate_focus(void const* element)
{
    return ffi_element(element).should_indicate_focus();
}

extern "C" bool selector_ffi_element_has_focus_within(void const* element)
{
    return ffi_element(element).matches_focus_within_pseudo_class();
}

extern "C" bool selector_ffi_element_is_active(void const* element)
{
    return ffi_element(element).is_being_activated();
}

extern "C" bool selector_ffi_element_is_checked(void const* element)
{
    return ffi_element(element).matches_checked_pseudo_class();
}

extern "C" bool selector_ffi_element_is_defined(void const* element)
{
    return ffi_element(element).is_defined();
}

extern "C" bool selector_ffi_element_is_disabled(void const* element)
{
    return ffi_element(element).matches_disabled_pseudo_class();
}

extern "C" bool selector_ffi_element_is_enabled(void const* element)
{
    return ffi_element(element).matches_enabled_pseudo_class();
}

extern "C" bool selector_ffi_element_is_local_link(void const* element)
{
    return ffi_element(element).matches_local_link_pseudo_class();
}

extern "C" bool selector_ffi_element_is_placeholder_shown(void const* element)
{
    return ffi_element(element).matches_placeholder_shown_pseudo_class();
}

extern "C" bool selector_ffi_element_is_target(void const* element)
{
    return ffi_element(element).is_target();
}

extern "C" bool selector_ffi_element_is_unchecked(void const* element)
{
    return ffi_element(element).matches_unchecked_pseudo_class();
}

extern "C" bool selector_ffi_element_is_media_element(void const* element)
{
    return is<HTML::HTMLMediaElement>(ffi_element(element));
}

extern "C" bool selector_ffi_element_media_is_blocked(void const* element)
{
    auto const* media_element = as_if<HTML::HTMLMediaElement>(ffi_element(element));
    return media_element && media_element->blocked();
}

extern "C" bool selector_ffi_element_media_is_muted(void const* element)
{
    auto const* media_element = as_if<HTML::HTMLMediaElement>(ffi_element(element));
    return media_element && media_element->muted();
}

extern "C" bool selector_ffi_element_media_is_paused(void const* element)
{
    auto const* media_element = as_if<HTML::HTMLMediaElement>(ffi_element(element));
    return media_element && media_element->paused();
}

extern "C" bool selector_ffi_element_media_is_seeking(void const* element)
{
    auto const* media_element = as_if<HTML::HTMLMediaElement>(ffi_element(element));
    return media_element && media_element->seeking();
}

extern "C" bool selector_ffi_element_media_is_stalled(void const* element)
{
    auto const* media_element = as_if<HTML::HTMLMediaElement>(ffi_element(element));
    return media_element && media_element->stalled();
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-default
extern "C" bool selector_ffi_element_is_default(void const* element)
{
    auto const& target = ffi_element(element);
    auto const* form_associated_element = as_if<HTML::FormAssociatedElement>(target);
    if (!form_associated_element)
        return false;
    if (form_associated_element->is_submit_button() && form_associated_element->form() && form_associated_element->form()->default_button() == form_associated_element)
        return true;
    if (auto const* input_element = as_if<HTML::HTMLInputElement>(form_associated_element))
        return input_element->checked_applies() && input_element->has_attribute(HTML::AttributeNames::checked);
    if (auto const* option_element = as_if<HTML::HTMLOptionElement>(form_associated_element))
        return option_element->has_attribute(HTML::AttributeNames::selected);
    return false;
}

extern "C" CSS::SelectorFFI::FfiMeterValueState selector_ffi_element_meter_value_state(void const* element)
{
    auto const* meter = as_if<HTML::HTMLMeterElement>(ffi_element(element));
    if (!meter)
        return CSS::SelectorFFI::FfiMeterValueState::NotMeter;
    switch (meter->value_state()) {
    case HTML::HTMLMeterElement::ValueState::EvenLessGood:
        return CSS::SelectorFFI::FfiMeterValueState::EvenLessGood;
    case HTML::HTMLMeterElement::ValueState::Suboptimal:
        return CSS::SelectorFFI::FfiMeterValueState::Suboptimal;
    case HTML::HTMLMeterElement::ValueState::Optimal:
        return CSS::SelectorFFI::FfiMeterValueState::Optimal;
    }
    VERIFY_NOT_REACHED();
}

extern "C" bool selector_ffi_element_meter_value_is_high(void const* element)
{
    auto const* meter = as_if<HTML::HTMLMeterElement>(ffi_element(element));
    return meter && meter->value() > meter->high();
}

extern "C" bool selector_ffi_element_meter_value_is_low(void const* element)
{
    auto const* meter = as_if<HTML::HTMLMeterElement>(ffi_element(element));
    return meter && meter->value() < meter->low();
}

extern "C" bool selector_ffi_element_is_hovered(void const* element)
{
    return matches_hover_pseudo_class(ffi_element(element));
}

extern "C" bool selector_ffi_element_is_indeterminate(void const* element)
{
    return matches_indeterminate_pseudo_class(ffi_element(element));
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-invalid
// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-valid
extern "C" CSS::SelectorFFI::FfiValidityState selector_ffi_element_validity_state(void const* element)
{
    auto const& target = ffi_element(element);
    if (auto const* form_associated_element = as_if<HTML::FormAssociatedElement>(target)) {
        if (form_associated_element->is_candidate_for_constraint_validation()) {
            return form_associated_element->satisfies_its_constraints()
                ? CSS::SelectorFFI::FfiValidityState::Valid
                : CSS::SelectorFFI::FfiValidityState::Invalid;
        }
    }

    auto const* form_element = as_if<HTML::HTMLFormElement>(target);
    if (!form_element && !is<HTML::HTMLFieldSetElement>(target))
        return CSS::SelectorFFI::FfiValidityState::NotApplicable;

    bool has_invalid_elements = false;
    target.for_each_in_subtree([&](auto& node) {
        auto const* form_associated_element = as_if<HTML::FormAssociatedElement>(&node);
        if (!form_associated_element)
            return TraversalDecision::Continue;
        if (form_element && form_associated_element->form() != form_element)
            return TraversalDecision::Continue;
        if (form_associated_element->is_candidate_for_constraint_validation() && !form_associated_element->satisfies_its_constraints()) {
            has_invalid_elements = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return has_invalid_elements
        ? CSS::SelectorFFI::FfiValidityState::Invalid
        : CSS::SelectorFFI::FfiValidityState::Valid;
}

// https://drafts.csswg.org/selectors/#modal-state
extern "C" bool selector_ffi_element_is_modal(void const* element)
{
    auto const* dialog_element = as_if<HTML::HTMLDialogElement>(ffi_element(element));
    // FIXME: Fullscreen elements are also modal.
    return dialog_element && dialog_element->is_modal();
}

extern "C" bool selector_ffi_element_is_open(void const* element)
{
    return matches_open_state_pseudo_class(ffi_element(element), true);
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-optional
// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-required
extern "C" CSS::SelectorFFI::FfiRequiredState selector_ffi_element_required_state(void const* element)
{
    auto const& target = ffi_element(element);
    if (auto const* input_element = as_if<HTML::HTMLInputElement>(target)) {
        if (input_element->required_applies())
            return input_element->has_attribute(HTML::AttributeNames::required)
                ? CSS::SelectorFFI::FfiRequiredState::Required
                : CSS::SelectorFFI::FfiRequiredState::Optional;
        // AD-HOC: Chromium and WebKit also match :optional for hidden inputs.
        return input_element->type_state() == HTML::HTMLInputElement::TypeAttributeState::Hidden
            ? CSS::SelectorFFI::FfiRequiredState::Optional
            : CSS::SelectorFFI::FfiRequiredState::NotApplicable;
    }
    if (is<HTML::HTMLSelectElement>(target) || is<HTML::HTMLTextAreaElement>(target))
        return target.has_attribute(HTML::AttributeNames::required)
            ? CSS::SelectorFFI::FfiRequiredState::Required
            : CSS::SelectorFFI::FfiRequiredState::Optional;
    return CSS::SelectorFFI::FfiRequiredState::NotApplicable;
}

extern "C" bool selector_ffi_element_is_read_write(void const* element)
{
    return matches_read_write_pseudo_class(ffi_element(element));
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-user-valid
// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-user-invalid
extern "C" CSS::SelectorFFI::FfiValidityState selector_ffi_element_user_validity_state(void const* element)
{
    auto const& target = ffi_element(element);
    bool user_validity = false;
    if (auto const* input_element = as_if<HTML::HTMLInputElement>(target))
        user_validity = input_element->user_validity();
    else if (auto const* select_element = as_if<HTML::HTMLSelectElement>(target))
        user_validity = select_element->user_validity();
    else if (auto const* text_area_element = as_if<HTML::HTMLTextAreaElement>(target))
        user_validity = text_area_element->user_validity();
    else
        return CSS::SelectorFFI::FfiValidityState::NotApplicable;
    if (!user_validity)
        return CSS::SelectorFFI::FfiValidityState::NotApplicable;

    auto const& form_associated_element = as<HTML::FormAssociatedElement>(target);
    if (!form_associated_element.is_candidate_for_constraint_validation())
        return CSS::SelectorFFI::FfiValidityState::NotApplicable;
    return form_associated_element.satisfies_its_constraints()
        ? CSS::SelectorFFI::FfiValidityState::Valid
        : CSS::SelectorFFI::FfiValidityState::Invalid;
}

extern "C" CSS::SelectorFFI::DomStringView selector_ffi_element_local_name(void const* element)
{
    return dom_string_view(ffi_element(element).local_name());
}

extern "C" CSS::SelectorFFI::DomStringView selector_ffi_element_class_name(void const* element, size_t index)
{
    auto const& classes = ffi_element(element).class_names();
    VERIFY(index < classes.size());
    return dom_string_view(classes[index]);
}

extern "C" size_t selector_ffi_element_attribute_count(void const* element)
{
    auto count = ffi_element(element).attribute_list_size();
    VERIFY(count <= NumericLimits<u32>::max());
    return count;
}

extern "C" CSS::SelectorFFI::DomAttribute selector_ffi_element_attribute(void const* element, size_t index)
{
    auto const& target = ffi_element(element);
    VERIFY(index < target.attribute_list_size());
    auto const* attribute = target.attributes()->item(static_cast<u32>(index));
    VERIFY(attribute);
    return {
        .local_name = interned_string_identity(attribute->local_name()),
        .namespace_ = attribute->namespace_uri().has_value() ? interned_string_identity(*attribute->namespace_uri()) : 0,
        .has_namespace = attribute->namespace_uri().has_value(),
        .value = dom_string_view(attribute->value()),
    };
}

extern "C" CSS::SelectorFFI::ResolvedNamespace selector_ffi_default_namespace(void* context)
{
    auto& match_context = rust_match_context(context);
    if (!match_context.style_sheet_for_rule || !match_context.style_sheet_for_rule->default_namespace_rule()) {
        return {
            .namespace_type = CSS::SelectorFFI::ResolvedNamespaceType::Missing,
            .namespace_ = 0,
        };
    }

    auto const& namespace_ = match_context.style_sheet_for_rule->default_namespace_rule()->namespace_uri();
    if (namespace_.is_empty()) {
        return {
            .namespace_type = CSS::SelectorFFI::ResolvedNamespaceType::Null,
            .namespace_ = 0,
        };
    }
    return {
        .namespace_type = CSS::SelectorFFI::ResolvedNamespaceType::Named,
        .namespace_ = interned_string_identity(namespace_),
    };
}

extern "C" CSS::SelectorFFI::ResolvedNamespace selector_ffi_resolve_namespace(void* context, StringView prefix)
{
    auto& match_context = rust_match_context(context);
    if (!match_context.style_sheet_for_rule) {
        return {
            .namespace_type = CSS::SelectorFFI::ResolvedNamespaceType::Missing,
            .namespace_ = 0,
        };
    }

    auto namespace_ = match_context.style_sheet_for_rule->namespace_uri(ffi_string_view(prefix));
    if (!namespace_.has_value()) {
        return {
            .namespace_type = CSS::SelectorFFI::ResolvedNamespaceType::Missing,
            .namespace_ = 0,
        };
    }
    if (namespace_->is_empty()) {
        return {
            .namespace_type = CSS::SelectorFFI::ResolvedNamespaceType::Null,
            .namespace_ = 0,
        };
    }
    return {
        .namespace_type = CSS::SelectorFFI::ResolvedNamespaceType::Named,
        .namespace_ = interned_string_identity(*namespace_),
    };
}

extern "C" CSS::SelectorFFI::Element selector_ffi_parent_element(void const* element, void const* shadow_host)
{
    auto const& target = ffi_element(element);
    if (!shadow_host)
        return element_to_ffi(target.parent_element());
    if (element == shadow_host)
        return {};
    return element_to_ffi(target.parent_or_shadow_host_element());
}

extern "C" CSS::SelectorFFI::Element selector_ffi_parent_element_in_light_tree(void const* element)
{
    return element_to_ffi(ffi_element(element).parent_element());
}

extern "C" CSS::SelectorFFI::Element selector_ffi_previous_element_sibling(void const* element)
{
    return element_to_ffi(ffi_element(element).previous_element_sibling());
}

extern "C" CSS::SelectorFFI::Element selector_ffi_next_element_sibling(void const* element)
{
    return element_to_ffi(ffi_element(element).next_element_sibling());
}

extern "C" CSS::SelectorFFI::Element selector_ffi_first_element_child(void const* element)
{
    return element_to_ffi(ffi_element(element).first_child_of_type<DOM::Element>());
}

extern "C" CSS::SelectorFFI::Element selector_ffi_first_element_descendant(void const* element)
{
    auto const& root = ffi_element(element);
    for (auto const* node = root.first_child(); node; node = node->next_in_pre_order(&root)) {
        if (node->is_element())
            return element_to_ffi(static_cast<DOM::Element const*>(node));
    }
    return {};
}

extern "C" CSS::SelectorFFI::Element selector_ffi_next_element_descendant(void const* element, void const* root)
{
    auto const& root_element = ffi_element(root);
    for (auto const* node = static_cast<DOM::Node const*>(&ffi_element(element))->next_in_pre_order(&root_element); node; node = node->next_in_pre_order(&root_element)) {
        if (node->is_element())
            return element_to_ffi(static_cast<DOM::Element const*>(node));
    }
    return {};
}

extern "C" bool selector_ffi_element_has_element_child(void const* element)
{
    return ffi_element(element).first_child_of_type<DOM::Element>();
}

extern "C" bool selector_ffi_element_has_nonempty_text_child(void const* element)
{
    bool has_nonempty_text_child = false;
    ffi_element(element).for_each_child_of_type<DOM::Text>([&](auto const& text) {
        if (!text.data().is_empty()) {
            has_nonempty_text_child = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return has_nonempty_text_child;
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
            .element = element_to_ffi(slot),
            .shadow_host = element_to_ffi(slot_shadow_root ? slot_shadow_root->host() : nullptr),
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
        return { .element = element_to_ffi(&host), .shadow_host = element_to_ffi(next_shadow_host) };
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
