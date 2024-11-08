/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Selector.h"
#include <AK/GenericShorthands.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

Selector::Selector(Vector<CompoundSelector>&& compound_selectors)
    : m_compound_selectors(move(compound_selectors))
{
    // FIXME: This assumes that only one pseudo-element is allowed in a selector, and that it appears at the end.
    //        This is not true in Selectors-4!
    if (!m_compound_selectors.is_empty()) {
        for (auto const& simple_selector : m_compound_selectors.last().simple_selectors) {
            if (simple_selector.type == SimpleSelector::Type::PseudoElement) {
                m_pseudo_element = simple_selector.pseudo_element();
                break;
            }
        }
    }

    // https://drafts.csswg.org/css-nesting-1/#contain-the-nesting-selector
    // "A selector is said to contain the nesting selector if, when it was parsed as any type of selector,
    // a <delim-token> with the value "&" (U+0026 AMPERSAND) was encountered."
    for (auto const& compound_selector : m_compound_selectors) {
        for (auto const& simple_selector : compound_selector.simple_selectors) {
            if (simple_selector.type == SimpleSelector::Type::Nesting) {
                m_contains_the_nesting_selector = true;
                break;
            }
            if (simple_selector.type == SimpleSelector::Type::PseudoClass) {
                for (auto const& child_selector : simple_selector.pseudo_class().argument_selector_list) {
                    if (child_selector->contains_the_nesting_selector()) {
                        m_contains_the_nesting_selector = true;
                        break;
                    }
                }
                if (m_contains_the_nesting_selector)
                    break;
            }
        }
        if (m_contains_the_nesting_selector)
            break;
    }

    collect_ancestor_hashes();
}

void Selector::collect_ancestor_hashes()
{
    size_t next_hash_index = 0;
    auto append_unique_hash = [&](u32 hash) -> bool {
        if (next_hash_index >= m_ancestor_hashes.size())
            return true;
        for (size_t i = 0; i < next_hash_index; ++i) {
            if (m_ancestor_hashes[i] == hash)
                return false;
        }
        m_ancestor_hashes[next_hash_index++] = hash;
        return false;
    };

    auto last_combinator = m_compound_selectors.last().combinator;
    for (ssize_t compound_selector_index = static_cast<ssize_t>(m_compound_selectors.size()) - 2; compound_selector_index >= 0; --compound_selector_index) {
        auto const& compound_selector = m_compound_selectors[compound_selector_index];
        if (last_combinator == Combinator::Descendant || last_combinator == Combinator::ImmediateChild) {
            for (auto const& simple_selector : compound_selector.simple_selectors) {
                switch (simple_selector.type) {
                case SimpleSelector::Type::Id:
                case SimpleSelector::Type::Class:
                    if (append_unique_hash(simple_selector.name().hash()))
                        return;
                    break;
                case SimpleSelector::Type::TagName:
                    if (append_unique_hash(simple_selector.qualified_name().name.name.hash()))
                        return;
                    break;
                case SimpleSelector::Type::Attribute:
                    if (append_unique_hash(simple_selector.attribute().qualified_name.name.name.hash()))
                        return;
                    break;
                default:
                    break;
                }
            }
        }
        last_combinator = compound_selector.combinator;
    }

    for (size_t i = next_hash_index; i < m_ancestor_hashes.size(); ++i)
        m_ancestor_hashes[i] = 0;
}

// https://www.w3.org/TR/selectors-4/#specificity-rules
u32 Selector::specificity() const
{
    if (m_specificity.has_value())
        return *m_specificity;

    constexpr u32 ids_shift = 16;
    constexpr u32 classes_shift = 8;
    constexpr u32 tag_names_shift = 0;
    constexpr u32 ids_mask = 0xff << ids_shift;
    constexpr u32 classes_mask = 0xff << classes_shift;
    constexpr u32 tag_names_mask = 0xff << tag_names_shift;

    u32 ids = 0;
    u32 classes = 0;
    u32 tag_names = 0;

    auto count_specificity_of_most_complex_selector = [&](auto& selector_list) {
        u32 max_selector_list_argument_specificity = 0;
        for (auto const& complex_selector : selector_list) {
            max_selector_list_argument_specificity = max(max_selector_list_argument_specificity, complex_selector->specificity());
        }

        u32 child_ids = (max_selector_list_argument_specificity & ids_mask) >> ids_shift;
        u32 child_classes = (max_selector_list_argument_specificity & classes_mask) >> classes_shift;
        u32 child_tag_names = (max_selector_list_argument_specificity & tag_names_mask) >> tag_names_shift;

        ids += child_ids;
        classes += child_classes;
        tag_names += child_tag_names;
    };

    for (auto& list : m_compound_selectors) {
        for (auto& simple_selector : list.simple_selectors) {
            switch (simple_selector.type) {
            case SimpleSelector::Type::Id:
                // count the number of ID selectors in the selector (= A)
                ++ids;
                break;
            case SimpleSelector::Type::Class:
            case SimpleSelector::Type::Attribute:
                // count the number of class selectors, attributes selectors, and pseudo-classes in the selector (= B)
                ++classes;
                break;
            case SimpleSelector::Type::PseudoClass: {
                auto& pseudo_class = simple_selector.pseudo_class();
                switch (pseudo_class.type) {
                case PseudoClass::Has:
                case PseudoClass::Is:
                case PseudoClass::Not: {
                    // The specificity of an :is(), :not(), or :has() pseudo-class is replaced by the
                    // specificity of the most specific complex selector in its selector list argument.
                    count_specificity_of_most_complex_selector(pseudo_class.argument_selector_list);
                    break;
                }
                case PseudoClass::NthChild:
                case PseudoClass::NthLastChild: {
                    // Analogously, the specificity of an :nth-child() or :nth-last-child() selector
                    // is the specificity of the pseudo class itself (counting as one pseudo-class selector)
                    // plus the specificity of the most specific complex selector in its selector list argument (if any).
                    ++classes;
                    count_specificity_of_most_complex_selector(pseudo_class.argument_selector_list);
                    break;
                }
                case PseudoClass::Where:
                    // The specificity of a :where() pseudo-class is replaced by zero.
                    break;
                default:
                    ++classes;
                    break;
                }
                break;
            }
            case SimpleSelector::Type::TagName:
            case SimpleSelector::Type::PseudoElement:
                // count the number of type selectors and pseudo-elements in the selector (= C)
                ++tag_names;
                break;
            case SimpleSelector::Type::Universal:
                // ignore the universal selector
                break;
            case SimpleSelector::Type::Nesting:
                // "The specificity of the nesting selector is equal to the largest specificity among the complex selectors in the parent style rule’s selector list (identical to the behavior of :is()), or zero if no such selector list exists."
                // - https://drafts.csswg.org/css-nesting/#ref-for-specificity
                // The parented case is handled by replacing & with :is().
                // So if we got here, the specificity is 0.
                break;
            }
        }
    }

    // Due to storage limitations, implementations may have limitations on the size of A, B, or C.
    // If so, values higher than the limit must be clamped to that limit, and not overflow.
    m_specificity = (min(ids, 0xff) << ids_shift)
        + (min(classes, 0xff) << classes_shift)
        + (min(tag_names, 0xff) << tag_names_shift);

    return *m_specificity;
}

// https://www.w3.org/TR/cssom/#serialize-a-simple-selector
String Selector::SimpleSelector::serialize() const
{
    StringBuilder s;
    switch (type) {
    case Selector::SimpleSelector::Type::TagName:
    case Selector::SimpleSelector::Type::Universal: {
        auto qualified_name = this->qualified_name();
        // 1. If the namespace prefix maps to a namespace that is not the default namespace and is not the null
        //    namespace (not in a namespace) append the serialization of the namespace prefix as an identifier,
        //    followed by a "|" (U+007C) to s.
        if (qualified_name.namespace_type == QualifiedName::NamespaceType::Named) {
            serialize_an_identifier(s, qualified_name.namespace_);
            s.append('|');
        }

        // 2. If the namespace prefix maps to a namespace that is the null namespace (not in a namespace)
        //    append "|" (U+007C) to s.
        if (qualified_name.namespace_type == QualifiedName::NamespaceType::None)
            s.append('|');

        // 3. If this is a type selector append the serialization of the element name as an identifier to s.
        if (type == Selector::SimpleSelector::Type::TagName)
            serialize_an_identifier(s, qualified_name.name.name);

        // 4. If this is a universal selector append "*" (U+002A) to s.
        if (type == Selector::SimpleSelector::Type::Universal)
            s.append('*');

        break;
    }
    case Selector::SimpleSelector::Type::Attribute: {
        auto& attribute = this->attribute();

        // 1. Append "[" (U+005B) to s.
        s.append('[');

        // 2. If the namespace prefix maps to a namespace that is not the null namespace (not in a namespace)
        //    append the serialization of the namespace prefix as an identifier, followed by a "|" (U+007C) to s.
        if (attribute.qualified_name.namespace_type == QualifiedName::NamespaceType::Named) {
            serialize_an_identifier(s, attribute.qualified_name.namespace_);
            s.append('|');
        }

        // 3. Append the serialization of the attribute name as an identifier to s.
        serialize_an_identifier(s, attribute.qualified_name.name.name);

        // 4. If there is an attribute value specified, append "=", "~=", "|=", "^=", "$=", or "*=" as appropriate (depending on the type of attribute selector),
        //    followed by the serialization of the attribute value as a string, to s.
        if (!attribute.value.is_empty()) {
            switch (attribute.match_type) {
            case Selector::SimpleSelector::Attribute::MatchType::ExactValueMatch:
                s.append("="sv);
                break;
            case Selector::SimpleSelector::Attribute::MatchType::ContainsWord:
                s.append("~="sv);
                break;
            case Selector::SimpleSelector::Attribute::MatchType::ContainsString:
                s.append("*="sv);
                break;
            case Selector::SimpleSelector::Attribute::MatchType::StartsWithSegment:
                s.append("|="sv);
                break;
            case Selector::SimpleSelector::Attribute::MatchType::StartsWithString:
                s.append("^="sv);
                break;
            case Selector::SimpleSelector::Attribute::MatchType::EndsWithString:
                s.append("$="sv);
                break;
            default:
                break;
            }

            serialize_a_string(s, attribute.value);
        }

        // 5. If the attribute selector has the case-insensitivity flag present, append " i" (U+0020 U+0069) to s.
        //    If the attribute selector has the case-insensitivity flag present, append " s" (U+0020 U+0073) to s.
        //    (the line just above is an addition to CSS OM to match Selectors Level 4 last draft)
        switch (attribute.case_type) {
        case Selector::SimpleSelector::Attribute::CaseType::CaseInsensitiveMatch:
            s.append(" i"sv);
            break;
        case Selector::SimpleSelector::Attribute::CaseType::CaseSensitiveMatch:
            s.append(" s"sv);
            break;
        default:
            break;
        }

        // 6. Append "]" (U+005D) to s.
        s.append(']');
        break;
    }

    case Selector::SimpleSelector::Type::Class:
        // Append a "." (U+002E), followed by the serialization of the class name as an identifier to s.
        s.append('.');
        serialize_an_identifier(s, name());
        break;

    case Selector::SimpleSelector::Type::Id:
        // Append a "#" (U+0023), followed by the serialization of the ID as an identifier to s.
        s.append('#');
        serialize_an_identifier(s, name());
        break;

    case Selector::SimpleSelector::Type::PseudoClass: {
        auto& pseudo_class = this->pseudo_class();

        auto metadata = pseudo_class_metadata(pseudo_class.type);
        // HACK: `:host()` has both a function and a non-function form, so handle that first.
        //       It's also not in the spec.
        if (pseudo_class.type == PseudoClass::Host) {
            if (pseudo_class.argument_selector_list.is_empty()) {
                s.append(':');
                s.append(pseudo_class_name(pseudo_class.type));
            } else {
                s.append(':');
                s.append(pseudo_class_name(pseudo_class.type));
                s.append('(');
                s.append(serialize_a_group_of_selectors(pseudo_class.argument_selector_list));
                s.append(')');
            }
        }
        // If the pseudo-class does not accept arguments append ":" (U+003A), followed by the name of the pseudo-class, to s.
        else if (metadata.is_valid_as_identifier) {
            s.append(':');
            s.append(pseudo_class_name(pseudo_class.type));
        }
        // Otherwise, append ":" (U+003A), followed by the name of the pseudo-class, followed by "(" (U+0028),
        // followed by the value of the pseudo-class argument(s) determined as per below, followed by ")" (U+0029), to s.
        else {
            s.append(':');
            s.append(pseudo_class_name(pseudo_class.type));
            s.append('(');
            if (pseudo_class.type == PseudoClass::NthChild
                || pseudo_class.type == PseudoClass::NthLastChild
                || pseudo_class.type == PseudoClass::NthOfType
                || pseudo_class.type == PseudoClass::NthLastOfType) {
                // The result of serializing the value using the rules to serialize an <an+b> value.
                s.append(pseudo_class.nth_child_pattern.serialize());
            } else if (pseudo_class.type == PseudoClass::Not
                || pseudo_class.type == PseudoClass::Is
                || pseudo_class.type == PseudoClass::Where) {
                // The result of serializing the value using the rules for serializing a group of selectors.
                // NOTE: `:is()` and `:where()` aren't in the spec for this yet, but it should be!
                s.append(serialize_a_group_of_selectors(pseudo_class.argument_selector_list));
            } else if (pseudo_class.type == PseudoClass::Lang) {
                // The serialization of a comma-separated list of each argument’s serialization as a string, preserving relative order.
                s.join(", "sv, pseudo_class.languages);
            }
            s.append(')');
        }
        break;
    }
    case Selector::SimpleSelector::Type::PseudoElement:
        // Note: Pseudo-elements are dealt with in Selector::serialize()
        break;
    case Type::Nesting:
        // AD-HOC: Not in spec yet.
        s.append('&');
        break;
    }
    return MUST(s.to_string());
}

// https://www.w3.org/TR/cssom/#serialize-a-selector
String Selector::serialize() const
{
    StringBuilder s;

    // To serialize a selector let s be the empty string, run the steps below for each part of the chain of the selector, and finally return s:
    for (size_t i = 0; i < compound_selectors().size(); ++i) {
        auto const& compound_selector = compound_selectors()[i];
        // 1. If there is only one simple selector in the compound selectors which is a universal selector, append the result of serializing the universal selector to s.
        if (compound_selector.simple_selectors.size() == 1
            && compound_selector.simple_selectors.first().type == Selector::SimpleSelector::Type::Universal) {
            s.append(compound_selector.simple_selectors.first().serialize());
        }
        // 2. Otherwise, for each simple selector in the compound selectors that is not a universal selector
        //    of which the namespace prefix maps to a namespace that is not the default namespace
        //    serialize the simple selector and append the result to s.
        else {
            for (auto& simple_selector : compound_selector.simple_selectors) {
                if (simple_selector.type == SimpleSelector::Type::Universal) {
                    auto qualified_name = simple_selector.qualified_name();
                    if (qualified_name.namespace_type == SimpleSelector::QualifiedName::NamespaceType::Default)
                        continue;
                    // FIXME: I *think* if we have a namespace prefix that happens to equal the same as the default namespace,
                    //        we also should skip it. But we don't have access to that here. eg:
                    // <style>
                    //   @namespace "http://example";
                    //   @namespace foo "http://example";
                    //   foo|*.bar { } /* This would skip the `foo|*` when serializing. */
                    // </style>
                }
                s.append(simple_selector.serialize());
            }
        }

        // 3. If this is not the last part of the chain of the selector append a single SPACE (U+0020),
        //    followed by the combinator ">", "+", "~", ">>", "||", as appropriate, followed by another
        //    single SPACE (U+0020) if the combinator was not whitespace, to s.
        if (i != compound_selectors().size() - 1) {
            s.append(' ');
            // Note: The combinator that appears between parts `i` and `i+1` appears with the `i+1` selector,
            //       so we have to check that one.
            switch (compound_selectors()[i + 1].combinator) {
            case Selector::Combinator::ImmediateChild:
                s.append("> "sv);
                break;
            case Selector::Combinator::NextSibling:
                s.append("+ "sv);
                break;
            case Selector::Combinator::SubsequentSibling:
                s.append("~ "sv);
                break;
            case Selector::Combinator::Column:
                s.append("|| "sv);
                break;
            default:
                break;
            }
        } else {
            // 4. If this is the last part of the chain of the selector and there is a pseudo-element,
            // append "::" followed by the name of the pseudo-element, to s.
            if (compound_selector.simple_selectors.last().type == Selector::SimpleSelector::Type::PseudoElement) {
                s.append("::"sv);
                s.append(compound_selector.simple_selectors.last().pseudo_element().name());
            }
        }
    }

    return MUST(s.to_string());
}

// https://www.w3.org/TR/cssom/#serialize-a-group-of-selectors
String serialize_a_group_of_selectors(SelectorList const& selectors)
{
    // To serialize a group of selectors serialize each selector in the group of selectors and then serialize a comma-separated list of these serializations.
    return MUST(String::join(", "sv, selectors));
}

StringView Selector::PseudoElement::name(Selector::PseudoElement::Type pseudo_element)
{
    switch (pseudo_element) {
    case Selector::PseudoElement::Type::Before:
        return "before"sv;
    case Selector::PseudoElement::Type::After:
        return "after"sv;
    case Selector::PseudoElement::Type::FirstLine:
        return "first-line"sv;
    case Selector::PseudoElement::Type::FirstLetter:
        return "first-letter"sv;
    case Selector::PseudoElement::Type::Marker:
        return "marker"sv;
    case Selector::PseudoElement::Type::MeterBar:
        return "-webkit-meter-bar"sv;
    case Selector::PseudoElement::Type::MeterEvenLessGoodValue:
        return "-webkit-meter-even-less-good-value"sv;
    case Selector::PseudoElement::Type::MeterOptimumValue:
        return "-webkit-meter-optimum-value"sv;
    case Selector::PseudoElement::Type::MeterSuboptimumValue:
        return "-webkit-meter-suboptimum-value"sv;
    case Selector::PseudoElement::Type::ProgressBar:
        return "-webkit-progress-bar"sv;
    case Selector::PseudoElement::Type::ProgressValue:
        return "-webkit-progress-value"sv;
    case Selector::PseudoElement::Type::Placeholder:
        return "placeholder"sv;
    case Selector::PseudoElement::Type::Selection:
        return "selection"sv;
    case Selector::PseudoElement::Type::SliderRunnableTrack:
        return "-webkit-slider-runnable-track"sv;
    case Selector::PseudoElement::Type::SliderThumb:
        return "-webkit-slider-thumb"sv;
    case Selector::PseudoElement::Type::Backdrop:
        return "backdrop"sv;
    case Selector::PseudoElement::Type::KnownPseudoElementCount:
        break;
    case Selector::PseudoElement::Type::UnknownWebKit:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

Optional<Selector::PseudoElement> Selector::PseudoElement::from_string(FlyString const& name)
{
    if (name.equals_ignoring_ascii_case("after"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::After };
    } else if (name.equals_ignoring_ascii_case("before"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::Before };
    } else if (name.equals_ignoring_ascii_case("first-letter"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::FirstLetter };
    } else if (name.equals_ignoring_ascii_case("first-line"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::FirstLine };
    } else if (name.equals_ignoring_ascii_case("marker"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::Marker };
    } else if (name.equals_ignoring_ascii_case("-webkit-meter-bar"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::MeterBar };
    } else if (name.equals_ignoring_ascii_case("-webkit-meter-even-less-good-value"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::MeterEvenLessGoodValue };
    } else if (name.equals_ignoring_ascii_case("-webkit-meter-optimum-value"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::MeterOptimumValue };
    } else if (name.equals_ignoring_ascii_case("-webkit-meter-suboptimum-value"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::MeterSuboptimumValue };
    } else if (name.equals_ignoring_ascii_case("-webkit-progress-bar"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::ProgressBar };
    } else if (name.equals_ignoring_ascii_case("-webkit-progress-value"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::ProgressValue };
    } else if (name.equals_ignoring_ascii_case("placeholder"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::Placeholder };
    } else if (name.equals_ignoring_ascii_case("selection"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::Selection };
    } else if (name.equals_ignoring_ascii_case("backdrop"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::Backdrop };
    } else if (name.equals_ignoring_ascii_case("-webkit-slider-runnable-track"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::SliderRunnableTrack };
    } else if (name.equals_ignoring_ascii_case("-webkit-slider-thumb"sv)) {
        return Selector::PseudoElement { Selector::PseudoElement::Type::SliderThumb };
    }
    return {};
}

NonnullRefPtr<Selector> Selector::relative_to(SimpleSelector const& parent) const
{
    // To make us relative to the parent, prepend it to the list of compound selectors,
    // and ensure the next compound selector starts with a combinator.
    Vector<CompoundSelector> copied_compound_selectors;
    copied_compound_selectors.ensure_capacity(compound_selectors().size() + 1);
    copied_compound_selectors.empend(CompoundSelector { .simple_selectors = { parent } });

    bool first = true;
    for (auto compound_selector : compound_selectors()) {
        if (first) {
            if (compound_selector.combinator == Combinator::None)
                compound_selector.combinator = Combinator::Descendant;
            first = false;
        }

        copied_compound_selectors.append(move(compound_selector));
    }

    return Selector::create(move(copied_compound_selectors));
}

NonnullRefPtr<Selector> Selector::absolutized(Selector::SimpleSelector const& selector_for_nesting) const
{
    if (!contains_the_nesting_selector())
        return *this;

    Vector<CompoundSelector> absolutized_compound_selectors;
    absolutized_compound_selectors.ensure_capacity(m_compound_selectors.size());
    for (auto const& compound_selector : m_compound_selectors)
        absolutized_compound_selectors.append(compound_selector.absolutized(selector_for_nesting));

    return Selector::create(move(absolutized_compound_selectors));
}

Selector::CompoundSelector Selector::CompoundSelector::absolutized(Selector::SimpleSelector const& selector_for_nesting) const
{
    // TODO: Cache if it contains the nesting selector?

    Vector<SimpleSelector> absolutized_simple_selectors;
    absolutized_simple_selectors.ensure_capacity(simple_selectors.size());
    for (auto const& simple_selector : simple_selectors)
        absolutized_simple_selectors.append(simple_selector.absolutized(selector_for_nesting));

    return CompoundSelector {
        .combinator = this->combinator,
        .simple_selectors = absolutized_simple_selectors,
    };
}

Selector::SimpleSelector Selector::SimpleSelector::absolutized(Selector::SimpleSelector const& selector_for_nesting) const
{
    switch (type) {
    case Type::Nesting:
        // Nesting selectors get replaced directly.
        return selector_for_nesting;

    case Type::PseudoClass: {
        // Pseudo-classes may contain other selectors, so we need to absolutize them.
        // Copy the PseudoClassSelector, and then replace its argument selector list.
        auto pseudo_class = this->pseudo_class();
        if (!pseudo_class.argument_selector_list.is_empty()) {
            SelectorList new_selector_list;
            new_selector_list.ensure_capacity(pseudo_class.argument_selector_list.size());
            for (auto const& argument_selector : pseudo_class.argument_selector_list)
                new_selector_list.append(argument_selector->absolutized(selector_for_nesting));
            pseudo_class.argument_selector_list = move(new_selector_list);
        }
        return SimpleSelector {
            .type = Type::PseudoClass,
            .value = move(pseudo_class),
        };
    }

    case Type::Universal:
    case Type::TagName:
    case Type::Id:
    case Type::Class:
    case Type::Attribute:
    case Type::PseudoElement:
        // Everything else isn't affected
        return *this;
    }

    VERIFY_NOT_REACHED();
}

SelectorList adapt_nested_relative_selector_list(SelectorList const& selectors)
{
    // "Nested style rules differ from non-nested rules in the following ways:
    // - A nested style rule accepts a <relative-selector-list> as its prelude (rather than just a <selector-list>).
    //   Any relative selectors are relative to the elements represented by the nesting selector.
    // - If a selector in the <relative-selector-list> does not start with a combinator but does contain the nesting
    //   selector, it is interpreted as a non-relative selector."
    // https://drafts.csswg.org/css-nesting-1/#syntax
    // NOTE: We already parsed the selectors as a <relative-selector-list>

    // Nested relative selectors get a `&` inserted at the beginning.
    // This is, handily, how the spec wants them serialized:
    // "When serializing a relative selector in a nested style rule, the selector must be absolutized,
    // with the implied nesting selector inserted."
    // - https://drafts.csswg.org/css-nesting-1/#cssom

    CSS::SelectorList new_list;
    new_list.ensure_capacity(selectors.size());
    for (auto const& selector : selectors) {
        auto first_combinator = selector->compound_selectors().first().combinator;
        if (!first_is_one_of(first_combinator, CSS::Selector::Combinator::None, CSS::Selector::Combinator::Descendant)
            || !selector->contains_the_nesting_selector()) {
            new_list.append(selector->relative_to(CSS::Selector::SimpleSelector { .type = CSS::Selector::SimpleSelector::Type::Nesting }));
        } else if (first_combinator == CSS::Selector::Combinator::Descendant) {
            // Replace leading descendant combinator (whitespace) with none, because we're not actually relative.
            auto copied_compound_selectors = selector->compound_selectors();
            copied_compound_selectors.first().combinator = CSS::Selector::Combinator::None;
            new_list.append(CSS::Selector::create(move(copied_compound_selectors)));
        } else {
            new_list.append(selector);
        }
    }
    return new_list;
}

}
