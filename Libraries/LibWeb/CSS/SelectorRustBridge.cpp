/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorRustBridge.h>
#include <LibWeb/SelectorRustFFI.h>

namespace Web::CSS {

static SelectorFFI::Combinator combinator_to_ffi(Selector::Combinator combinator)
{
    switch (combinator) {
    case Selector::Combinator::None:
        return SelectorFFI::Combinator::None;
    case Selector::Combinator::ImmediateChild:
        return SelectorFFI::Combinator::ImmediateChild;
    case Selector::Combinator::Descendant:
        return SelectorFFI::Combinator::Descendant;
    case Selector::Combinator::NextSibling:
        return SelectorFFI::Combinator::NextSibling;
    case Selector::Combinator::SubsequentSibling:
        return SelectorFFI::Combinator::SubsequentSibling;
    case Selector::Combinator::Column:
        return SelectorFFI::Combinator::Column;
    case Selector::Combinator::PseudoElement:
        return SelectorFFI::Combinator::PseudoElement;
    }
    VERIFY_NOT_REACHED();
}

static SelectorFFI::NamespaceType namespace_type_to_ffi(Selector::SimpleSelector::QualifiedName::NamespaceType namespace_type)
{
    switch (namespace_type) {
    case Selector::SimpleSelector::QualifiedName::NamespaceType::Default:
        return SelectorFFI::NamespaceType::Default;
    case Selector::SimpleSelector::QualifiedName::NamespaceType::None:
        return SelectorFFI::NamespaceType::None;
    case Selector::SimpleSelector::QualifiedName::NamespaceType::Any:
        return SelectorFFI::NamespaceType::Any;
    case Selector::SimpleSelector::QualifiedName::NamespaceType::Named:
        return SelectorFFI::NamespaceType::Named;
    }
    VERIFY_NOT_REACHED();
}

static SelectorFFI::AttributeMatchType attribute_match_type_to_ffi(Selector::SimpleSelector::Attribute::MatchType match_type)
{
    switch (match_type) {
    case Selector::SimpleSelector::Attribute::MatchType::HasAttribute:
        return SelectorFFI::AttributeMatchType::HasAttribute;
    case Selector::SimpleSelector::Attribute::MatchType::ExactValueMatch:
        return SelectorFFI::AttributeMatchType::ExactValue;
    case Selector::SimpleSelector::Attribute::MatchType::ContainsWord:
        return SelectorFFI::AttributeMatchType::ContainsWord;
    case Selector::SimpleSelector::Attribute::MatchType::ContainsString:
        return SelectorFFI::AttributeMatchType::ContainsString;
    case Selector::SimpleSelector::Attribute::MatchType::StartsWithSegment:
        return SelectorFFI::AttributeMatchType::StartsWithSegment;
    case Selector::SimpleSelector::Attribute::MatchType::StartsWithString:
        return SelectorFFI::AttributeMatchType::StartsWithString;
    case Selector::SimpleSelector::Attribute::MatchType::EndsWithString:
        return SelectorFFI::AttributeMatchType::EndsWithString;
    }
    VERIFY_NOT_REACHED();
}

static SelectorFFI::AttributeCaseType attribute_case_type_to_ffi(Selector::SimpleSelector::Attribute::CaseType case_type)
{
    switch (case_type) {
    case Selector::SimpleSelector::Attribute::CaseType::DefaultMatch:
        return SelectorFFI::AttributeCaseType::Default;
    case Selector::SimpleSelector::Attribute::CaseType::CaseSensitiveMatch:
        return SelectorFFI::AttributeCaseType::Sensitive;
    case Selector::SimpleSelector::Attribute::CaseType::CaseInsensitiveMatch:
        return SelectorFFI::AttributeCaseType::Insensitive;
    }
    VERIFY_NOT_REACHED();
}

u8 pseudo_element_to_ffi(Optional<PseudoElement> pseudo_element)
{
    if (!pseudo_element.has_value())
        return NumericLimits<u8>::max();
    // NB: The Rust PseudoElementType enum is generated from PseudoElements.json with UnknownWebKit
    //     appended after the known pseudo-elements, matching this encoding.
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

class SelectorCompiler {
public:
    SelectorFFI::RustSelector* compile(Selector const& selector)
    {
        m_simple_selector_lists.ensure_capacity(selector.compound_selectors().size());
        m_compound_selectors.ensure_capacity(selector.compound_selectors().size());

        for (auto const& compound_selector : selector.compound_selectors()) {
            Vector<SelectorFFI::SimpleSelector> simple_selectors;
            simple_selectors.ensure_capacity(compound_selector.simple_selectors.size());
            for (auto const& simple_selector : compound_selector.simple_selectors)
                simple_selectors.append(compile_simple_selector(simple_selector));

            m_simple_selector_lists.append(move(simple_selectors));
            auto const& stored_simple_selectors = m_simple_selector_lists.last();
            m_compound_selectors.append({
                .combinator = combinator_to_ffi(compound_selector.combinator),
                .simple_selectors = stored_simple_selectors.data(),
                .simple_selector_count = stored_simple_selectors.size(),
            });
        }

        SelectorFFI::Selector ffi_selector {
            .cxx_selector = &selector,
            .compound_selectors = m_compound_selectors.data(),
            .compound_selector_count = m_compound_selectors.size(),
        };
        return SelectorFFI::rust_selector_create(&ffi_selector);
    }

private:
    SelectorFFI::StringView store_string(Utf16View string)
    {
        Vector<u16> code_units;
        code_units.ensure_capacity(string.length_in_code_units());
        for (size_t i = 0; i < string.length_in_code_units(); ++i)
            code_units.unchecked_append(string.code_unit_at(i));
        m_strings.append(move(code_units));
        auto const& stored_string = m_strings.last();
        return { stored_string.data(), stored_string.size() };
    }

    void compile_qualified_name(SelectorFFI::SimpleSelector& output, Selector::SimpleSelector::QualifiedName const& qualified_name)
    {
        output.namespace_type = namespace_type_to_ffi(qualified_name.namespace_type);
        output.namespace_ = store_string(qualified_name.namespace_);
        output.name = store_string(qualified_name.name.name);
        output.lowercase_name = store_string(qualified_name.name.lowercase_name);
    }

    SelectorFFI::SimpleSelector compile_simple_selector(Selector::SimpleSelector const& simple_selector)
    {
        SelectorFFI::SimpleSelector output {};
        // NB: The C++ simple selector outlives the compiled Rust selector, so matching callbacks
        //     can use this pointer to compare interned strings without copying them.
        output.cxx_simple_selector = &simple_selector;

        switch (simple_selector.type) {
        case Selector::SimpleSelector::Type::Universal:
            output.selector_type = SelectorFFI::SimpleSelectorType::Universal;
            compile_qualified_name(output, simple_selector.qualified_name());
            break;
        case Selector::SimpleSelector::Type::TagName:
            output.selector_type = SelectorFFI::SimpleSelectorType::TagName;
            compile_qualified_name(output, simple_selector.qualified_name());
            break;
        case Selector::SimpleSelector::Type::Id:
            output.selector_type = SelectorFFI::SimpleSelectorType::Id;
            output.name = store_string(simple_selector.id_name());
            break;
        case Selector::SimpleSelector::Type::Class:
            output.selector_type = SelectorFFI::SimpleSelectorType::Class;
            output.name = store_string(simple_selector.class_name());
            break;
        case Selector::SimpleSelector::Type::Attribute: {
            output.selector_type = SelectorFFI::SimpleSelectorType::Attribute;
            auto const& attribute = simple_selector.attribute();
            compile_qualified_name(output, attribute.qualified_name);
            output.attribute_match_type = attribute_match_type_to_ffi(attribute.match_type);
            output.attribute_case_type = attribute_case_type_to_ffi(attribute.case_type);
            output.attribute_value = store_string(attribute.value);
            break;
        }
        case Selector::SimpleSelector::Type::PseudoClass:
            output.selector_type = SelectorFFI::SimpleSelectorType::PseudoClass;
            compile_pseudo_class(output, simple_selector.pseudo_class());
            break;
        case Selector::SimpleSelector::Type::PseudoElement:
            output.selector_type = SelectorFFI::SimpleSelectorType::PseudoElement;
            compile_pseudo_element(output, simple_selector.pseudo_element());
            break;
        case Selector::SimpleSelector::Type::Nesting:
            output.selector_type = SelectorFFI::SimpleSelectorType::Nesting;
            break;
        case Selector::SimpleSelector::Type::Invalid:
            output.selector_type = SelectorFFI::SimpleSelectorType::Invalid;
            break;
        }

        return output;
    }

    void compile_pseudo_class(SelectorFFI::SimpleSelector& output, Selector::SimpleSelector::PseudoClassSelector const& pseudo_class)
    {
        output.pseudo_class = to_underlying(pseudo_class.type);
        output.an_plus_b_step_size = pseudo_class.an_plus_b_pattern.step_size;
        output.an_plus_b_offset = pseudo_class.an_plus_b_pattern.offset;

        Vector<SelectorFFI::RustSelector const*> argument_selectors;
        argument_selectors.ensure_capacity(pseudo_class.argument_selector_list.size());
        for (auto const& selector : pseudo_class.argument_selector_list)
            argument_selectors.unchecked_append(&selector->rust_selector());
        m_selector_lists.append(move(argument_selectors));
        output.argument_selectors = m_selector_lists.last().data();
        output.argument_selector_count = m_selector_lists.last().size();

        Vector<SelectorFFI::StringView> languages;
        languages.ensure_capacity(pseudo_class.languages.size());
        for (auto const& language : pseudo_class.languages)
            languages.unchecked_append(store_string(language));
        m_string_lists.append(move(languages));
        output.languages = m_string_lists.last().data();
        output.language_count = m_string_lists.last().size();

        if (pseudo_class.ident.has_value()) {
            output.identifier = store_string(pseudo_class.ident->string_value);
            if (pseudo_class.ident->keyword == Keyword::Ltr)
                output.direction = SelectorFFI::Direction::LeftToRight;
            else if (pseudo_class.ident->keyword == Keyword::Rtl)
                output.direction = SelectorFFI::Direction::RightToLeft;
            else
                output.direction = SelectorFFI::Direction::Other;
        }

        output.levels = pseudo_class.levels.data();
        output.level_count = pseudo_class.levels.size();
    }

    void compile_pseudo_element(SelectorFFI::SimpleSelector& output, Selector::PseudoElementSelector const& pseudo_element)
    {
        output.pseudo_element = pseudo_element_to_ffi(pseudo_element.type());

        switch (pseudo_element.type()) {
        case PseudoElement::Slotted:
            output.pseudo_element_value_type = SelectorFFI::PseudoElementValueType::CompoundSelector;
            output.pseudo_element_selector = &pseudo_element.compound_selector().rust_selector();
            break;
        case PseudoElement::Part: {
            output.pseudo_element_value_type = SelectorFFI::PseudoElementValueType::Identifiers;
            Vector<SelectorFFI::StringView> identifiers;
            identifiers.ensure_capacity(pseudo_element.ident_list().size());
            for (auto const& identifier : pseudo_element.ident_list())
                identifiers.unchecked_append(store_string(identifier));
            m_string_lists.append(move(identifiers));
            output.pseudo_element_identifiers = m_string_lists.last().data();
            output.pseudo_element_identifier_count = m_string_lists.last().size();
            break;
        }
        case PseudoElement::ViewTransitionGroup:
        case PseudoElement::ViewTransitionImagePair:
        case PseudoElement::ViewTransitionNew:
        case PseudoElement::ViewTransitionOld: {
            output.pseudo_element_value_type = SelectorFFI::PseudoElementValueType::TransitionName;
            auto const& transition_name = pseudo_element.pt_name_selector();
            output.transition_name_is_universal = transition_name.is_universal;
            output.transition_name = store_string(transition_name.value);
            break;
        }
        default:
            output.pseudo_element_value_type = SelectorFFI::PseudoElementValueType::None;
            break;
        }
    }

    Vector<Vector<u16>> m_strings;
    Vector<Vector<SelectorFFI::StringView>> m_string_lists;
    Vector<Vector<SelectorFFI::RustSelector const*>> m_selector_lists;
    Vector<Vector<SelectorFFI::SimpleSelector>> m_simple_selector_lists;
    Vector<SelectorFFI::CompoundSelector> m_compound_selectors;
};

SelectorFFI::RustSelector* compile_selector_for_matching(Selector const& selector)
{
    return SelectorCompiler {}.compile(selector);
}

}
