/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSNamespaceRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/SelectorRustFFI.h>

namespace Web::CSS {

static_assert(sizeof(Utf16FlyString) == sizeof(uintptr_t));

u8 pseudo_element_to_ffi(Optional<PseudoElement> pseudo_element)
{
    if (!pseudo_element.has_value())
        return NumericLimits<u8>::max();
    if (*pseudo_element == PseudoElement::UnknownWebKit)
        return to_underlying(PseudoElement::KnownPseudoElementCount);
    VERIFY(*pseudo_element < PseudoElement::KnownPseudoElementCount);
    return to_underlying(*pseudo_element);
}

Optional<PseudoElement> pseudo_element_from_ffi(u8 pseudo_element)
{
    if (pseudo_element == NumericLimits<u8>::max())
        return {};
    if (pseudo_element == to_underlying(PseudoElement::KnownPseudoElementCount))
        return PseudoElement::UnknownWebKit;
    VERIFY(pseudo_element < to_underlying(PseudoElement::KnownPseudoElementCount));
    return static_cast<PseudoElement>(pseudo_element);
}

Selector::Selector(SelectorFFI::RustSelector* selector)
    : m_rust_selector(selector)
{
    VERIFY(m_rust_selector);
}

NonnullRefPtr<Selector> Selector::create(SelectorFFI::RustSelector* selector)
{
    return adopt_ref(*new Selector(selector));
}

Selector::~Selector()
{
    SelectorFFI::rust_selector_destroy(m_rust_selector);
}

Optional<PseudoElement> Selector::target_pseudo_element() const
{
    auto value = SelectorFFI::rust_selector_target_pseudo_element(m_rust_selector);
    if (value == NumericLimits<u8>::max())
        return {};
    return pseudo_element_from_ffi(value);
}

bool Selector::contains_the_nesting_selector() const
{
    return SelectorFFI::rust_selector_contains_nesting(m_rust_selector);
}

bool Selector::contains_pseudo_class(PseudoClass pseudo_class) const
{
    return SelectorFFI::rust_selector_contains_pseudo_class(m_rust_selector, to_underlying(pseudo_class));
}

bool Selector::contains_unknown_webkit_pseudo_element() const
{
    return SelectorFFI::rust_selector_contains_unknown_webkit(m_rust_selector);
}

bool Selector::contains_named_namespace() const
{
    return SelectorFFI::rust_selector_contains_named_namespace(m_rust_selector);
}

Selector::Combinator Selector::first_combinator() const
{
    return static_cast<Combinator>(SelectorFFI::rust_selector_first_combinator(m_rust_selector));
}

u32 Selector::specificity() const
{
    return SelectorFFI::rust_selector_specificity(m_rust_selector);
}

static Vector<u16> to_code_units(Utf16View view)
{
    Vector<u16> result;
    result.ensure_capacity(view.length_in_code_units());
    for (size_t i = 0; i < view.length_in_code_units(); ++i)
        result.unchecked_append(view.code_unit_at(i));
    return result;
}

Optional<SelectorList> parse_selector_list_in_rust(Utf16View input, HashTable<Utf16FlyString> const& namespaces, bool is_relative, bool is_forgiving)
{
    Vector<Vector<u16>> namespace_storage;
    Vector<SelectorFFI::StringView> namespace_views;
    namespace_storage.ensure_capacity(namespaces.size());
    namespace_views.ensure_capacity(namespaces.size());
    for (auto const& namespace_ : namespaces)
        namespace_storage.append(to_code_units(namespace_.view()));
    for (auto const& namespace_ : namespace_storage)
        namespace_views.append({ namespace_.data(), namespace_.size() });

    auto* parsed = SelectorFFI::rust_selector_parse(
        {
            .ascii = input.has_ascii_storage() ? reinterpret_cast<u8 const*>(input.ascii_span().data()) : nullptr,
            .utf16 = input.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(input.utf16_span().data()),
            .length = input.length_in_code_units(),
        },
        namespace_views.data(), namespace_views.size(), is_relative, is_forgiving);
    if (!parsed)
        return {};

    Vector<Utf16FlyString> names;
    auto name_count = SelectorFFI::rust_parsed_selector_list_interned_name_count(parsed);
    names.ensure_capacity(name_count);
    for (size_t index = 0; index < name_count; ++index) {
        auto name = SelectorFFI::rust_parsed_selector_list_interned_name(parsed, index);
        names.append(Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(name.data), name.length }));
    }
    Vector<uintptr_t> leaked_name_raws;
    leaked_name_raws.ensure_capacity(names.size());
    for (auto const& name : names)
        leaked_name_raws.unchecked_append(name.to_raw_leaked());
    SelectorFFI::rust_parsed_selector_list_bind_interned_names(parsed, leaked_name_raws.data(), leaked_name_raws.size());

    SelectorList selectors;
    auto selector_count = SelectorFFI::rust_parsed_selector_list_length(parsed);
    selectors.ensure_capacity(selector_count);
    for (size_t index = 0; index < selector_count; ++index)
        selectors.append(Selector::create(SelectorFFI::rust_parsed_selector_list_selector(parsed, index)));
    SelectorFFI::rust_parsed_selector_list_destroy(parsed);
    return selectors;
}

static Vector<SelectorFFI::StringView> namespace_prefixes_mapping_to_default(CSSStyleSheet const& style_sheet, Vector<Vector<u16>>& storage)
{
    Vector<SelectorFFI::StringView> prefixes;
    auto default_namespace = style_sheet.default_namespace();
    if (!default_namespace.has_value())
        return prefixes;
    for (auto const& [prefix, rule] : style_sheet.namespace_rules()) {
        if (rule->namespace_uri() != *default_namespace)
            continue;
        storage.append(to_code_units(prefix.view()));
    }
    prefixes.ensure_capacity(storage.size());
    for (auto const& prefix : storage)
        prefixes.append({ prefix.data(), prefix.size() });
    return prefixes;
}

void Selector::serialize_to(Utf16StringBuilder& builder, GC::Ptr<CSSStyleSheet const> style_sheet) const
{
    Vector<Vector<u16>> prefix_storage;
    Vector<SelectorFFI::StringView> prefixes;
    if (style_sheet)
        prefixes = namespace_prefixes_mapping_to_default(*style_sheet, prefix_storage);
    auto text = SelectorFFI::rust_selector_serialize(
        m_rust_selector, style_sheet && style_sheet->default_namespace().has_value(), prefixes.data(), prefixes.size());
    builder.append(Utf16View { reinterpret_cast<char16_t const*>(text.data), text.length });
    SelectorFFI::rust_selector_serialized_text_release(text.storage);
}

Utf16String Selector::serialize() const
{
    Utf16StringBuilder builder;
    serialize_to(builder);
    return builder.to_string();
}

Utf16String serialize_a_group_of_selectors(SelectorList const& selectors, GC::Ptr<CSSStyleSheet const> style_sheet)
{
    Utf16StringBuilder builder;
    for (size_t index = 0; index < selectors.size(); ++index) {
        if (index != 0)
            builder.append_ascii(", "sv);
        selectors[index]->serialize_to(builder, style_sheet);
    }
    return builder.to_string();
}

Utf16String Selector::PseudoElementSelector::serialize() const
{
    if (!m_serialized.is_empty() || m_type == PseudoElement::UnknownWebKit)
        return m_serialized;
    return Utf16String::formatted("::{}", pseudo_element_name(m_type));
}

static Vector<SelectorFFI::RustSelector const*> selector_handles(SelectorList const& selectors)
{
    Vector<SelectorFFI::RustSelector const*> handles;
    handles.ensure_capacity(selectors.size());
    for (auto const& selector : selectors)
        handles.unchecked_append(&selector->rust_selector());
    return handles;
}

SelectorList adapt_nested_relative_selector_list(SelectorList const& selectors, StyleNestingParent nesting_parent)
{
    SelectorList result;
    result.ensure_capacity(selectors.size());
    for (auto const& selector : selectors) {
        auto first = selector->first_combinator();
        bool insert_nesting = nesting_parent == StyleNestingParent::Style
            && ((!first_is_one_of(first, Selector::Combinator::None, Selector::Combinator::Descendant))
                || !selector->contains_the_nesting_selector());
        if (insert_nesting) {
            result.append(Selector::create(SelectorFFI::rust_selector_relative_to_nesting(&selector->rust_selector())));
        } else if (first == Selector::Combinator::Descendant) {
            result.append(Selector::create(SelectorFFI::rust_selector_with_first_combinator_none(&selector->rust_selector())));
        } else {
            result.append(selector);
        }
    }
    return result;
}

SelectorList adapt_scope_end_selectors_for_matching(SelectorList const& selectors)
{
    SelectorList result;
    result.ensure_capacity(selectors.size());
    for (auto const& selector : selectors) {
        auto first = selector->first_combinator();
        if (!first_is_one_of(first, Selector::Combinator::None, Selector::Combinator::Descendant)
            || (!selector->contains_the_nesting_selector() && !selector->contains_pseudo_class(PseudoClass::Scope))) {
            result.append(Selector::create(SelectorFFI::rust_selector_relative_to_scope(
                &selector->rust_selector(), false, nullptr, 0)));
        } else if (first == Selector::Combinator::Descendant) {
            result.append(Selector::create(SelectorFFI::rust_selector_with_first_combinator_none(&selector->rust_selector())));
        } else {
            result.append(selector);
        }
    }
    return result;
}

SelectorList absolutize_selectors_relative_to(SelectorList const& selectors, GC::Ptr<CSSRule const> parent)
{
    bool parent_is_scope = parent && parent->type() == CSSRule::Type::Scope;
    bool needs_work = selectors.contains([&](auto const& selector) {
        return selector->contains_the_nesting_selector()
            || (parent_is_scope && !first_is_one_of(selector->first_combinator(), Selector::Combinator::None, Selector::Combinator::Descendant));
    });
    if (!needs_work)
        return selectors;

    SelectorList const* parents = nullptr;
    if (auto const* parent_style_rule = as_if<CSSStyleRule const>(parent.ptr()))
        parents = &parent_style_rule->absolutized_selectors();
    auto handles = parents ? selector_handles(*parents) : Vector<SelectorFFI::RustSelector const*> {};

    SelectorList result;
    for (auto const& selector : selectors) {
        bool is_scope_relative = !first_is_one_of(selector->first_combinator(), Selector::Combinator::None, Selector::Combinator::Descendant);
        SelectorFFI::RustSelector* transformed = nullptr;
        if (selector->contains_the_nesting_selector()) {
            transformed = SelectorFFI::rust_selector_absolutize(
                &selector->rust_selector(), parents != nullptr, handles.data(), handles.size());
        } else if (parent_is_scope && is_scope_relative) {
            transformed = SelectorFFI::rust_selector_relative_to_scope(
                &selector->rust_selector(), parents != nullptr, handles.data(), handles.size());
        } else {
            result.append(selector);
            continue;
        }
        if (transformed)
            result.append(Selector::create(transformed));
    }
    return result;
}

}
