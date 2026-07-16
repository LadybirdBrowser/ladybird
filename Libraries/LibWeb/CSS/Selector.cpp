/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Selector.h"
#include <AK/GenericShorthands.h>
#include <AK/NeverDestroyed.h>
#include <LibWeb/CSS/AncestorFilter.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/SelectorRustBridge.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/SelectorRustFFI.h>

namespace Web::CSS {

static bool component_value_contains_nesting_selector(Parser::ComponentValue const& component_value)
{
    if (component_value.is_delim('&'))
        return true;

    if (component_value.is_block())
        return component_value.block().value.contains(component_value_contains_nesting_selector);

    if (component_value.is_function())
        return component_value.function().value.contains(component_value_contains_nesting_selector);

    return false;
}

Selector::Selector(Vector<CompoundSelector>&& compound_selectors)
    : m_compound_selectors(move(compound_selectors))
{
    for (auto const& compound_selector : m_compound_selectors) {
        for (auto const& simple_selector : compound_selector.simple_selectors) {
            if (simple_selector.type != SimpleSelector::Type::PseudoElement)
                continue;

            if (simple_selector.pseudo_element().type() == PseudoElement::Slotted)
                m_contains_slotted_pseudo_element = true;
            if (simple_selector.pseudo_element().type() == PseudoElement::Part)
                m_contains_part_pseudo_element = true;
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
                m_contained_pseudo_classes.set(simple_selector.pseudo_class().type, true);
                for (auto const& child_selector : simple_selector.pseudo_class().argument_selector_list) {
                    if (child_selector->contains_the_nesting_selector()) {
                        m_contains_the_nesting_selector = true;
                    }
                    m_contained_pseudo_classes |= child_selector->m_contained_pseudo_classes;
                }
                if (m_contains_the_nesting_selector)
                    break;
            }
            if (simple_selector.type == SimpleSelector::Type::Invalid) {
                auto& invalid = simple_selector.value.get<SimpleSelector::Invalid>();
                for (auto& item : invalid.component_values) {
                    if (component_value_contains_nesting_selector(item)) {
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

    m_rust_selector = compile_selector_for_matching(*this);
    VERIFY(m_rust_selector);
    m_target_pseudo_element = pseudo_element_from_ffi(SelectorFFI::rust_selector_target_pseudo_element(m_rust_selector));
}

Selector::~Selector()
{
    SelectorFFI::rust_selector_destroy(m_rust_selector);
}

static void append_integer(Utf16StringBuilder& builder, i64 value)
{
    if (value == 0) {
        builder.append_ascii('0');
        return;
    }

    if (value < 0)
        builder.append_ascii('-');

    u64 magnitude = value < 0 ? 0 - static_cast<u64>(value) : static_cast<u64>(value);
    Array<char, 20> digits;
    size_t digit_count = 0;
    while (magnitude > 0) {
        digits[digit_count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    }

    while (digit_count > 0)
        builder.append_ascii(digits[--digit_count]);
}

static void serialize_a_group_of_selectors_to_builder(Utf16StringBuilder&, SelectorList const&);

void Selector::collect_ancestor_hashes()
{
    if (is_slotted()) {
        // Ancestor filtering is not supported for slotted selectors, because those
        // are supposed to be collected for element inside a slot, while being
        // matched against slot element.
        return;
    }

    size_t next_hash_index = 0;
    struct AncestorHashCollector {
        enum class IncludeRightmostCompound : bool {
            No,
            Yes,
        };

        Array<u32, 8>& ancestor_hashes;
        size_t& next_hash_index;

        static bool contains_hash(Vector<u32> const& hashes, u32 hash)
        {
            for (auto existing_hash : hashes) {
                if (existing_hash == hash)
                    return true;
            }
            return false;
        }

        static void append_unique_hash(Vector<u32>& hashes, u32 hash)
        {
            if (!contains_hash(hashes, hash))
                hashes.append(hash);
        }

        static void intersect_hashes(Vector<u32>& hashes, Vector<u32> const& other_hashes)
        {
            for (size_t i = 0; i < hashes.size();) {
                if (contains_hash(other_hashes, hashes[i])) {
                    ++i;
                    continue;
                }
                hashes.remove(i);
            }
        }

        bool append_unique_hash(u32 hash)
        {
            if (next_hash_index >= ancestor_hashes.size())
                return true;
            for (size_t i = 0; i < next_hash_index; ++i) {
                if (ancestor_hashes[i] == hash)
                    return false;
            }
            ancestor_hashes[next_hash_index++] = hash;
            return false;
        }

        Vector<u32> hashes_from_simple_selector(SimpleSelector const& simple_selector)
        {
            Vector<u32> hashes;
            switch (simple_selector.type) {
            case SimpleSelector::Type::Id:
                hashes.append(ancestor_filter_hash_for_id(simple_selector.id_name().hash()));
                break;
            case SimpleSelector::Type::Class:
                hashes.append(ancestor_filter_hash_for_class(simple_selector.class_name().hash()));
                break;
            case SimpleSelector::Type::TagName:
                hashes.append(ancestor_filter_hash_for_tag_name(simple_selector.qualified_name().name.lowercase_name.hash()));
                break;
            case SimpleSelector::Type::Attribute:
                hashes.append(ancestor_filter_hash_for_attribute(simple_selector.attribute().qualified_name.name.lowercase_name.hash()));
                break;
            case SimpleSelector::Type::PseudoClass: {
                auto const& pseudo_class = simple_selector.pseudo_class();
                if (pseudo_class.type != PseudoClass::Is && pseudo_class.type != PseudoClass::Where)
                    break;

                // The selector's ancestor hashes are all mandatory. For :is()/:where(), only
                // hashes required by every alternative can reject the selector without running it.
                hashes = common_hashes_from_selector_list(pseudo_class.argument_selector_list, IncludeRightmostCompound::Yes);
                break;
            }
            default:
                break;
            }
            return hashes;
        }

        Vector<u32> hashes_from_compound(CompoundSelector const& compound_selector)
        {
            Vector<u32> hashes;
            for (auto const& simple_selector : compound_selector.simple_selectors) {
                for (auto hash : hashes_from_simple_selector(simple_selector))
                    append_unique_hash(hashes, hash);
            }
            return hashes;
        }

        Vector<u32> hashes_from_subject_compound_selector_list_pseudo_classes(CompoundSelector const& compound_selector)
        {
            Vector<u32> hashes;
            for (auto const& simple_selector : compound_selector.simple_selectors) {
                if (simple_selector.type != SimpleSelector::Type::PseudoClass)
                    continue;

                auto const& pseudo_class = simple_selector.pseudo_class();
                if (pseudo_class.type != PseudoClass::Is && pseudo_class.type != PseudoClass::Where)
                    continue;

                for (auto hash : common_hashes_from_selector_list(pseudo_class.argument_selector_list, IncludeRightmostCompound::No))
                    append_unique_hash(hashes, hash);
            }
            return hashes;
        }

        Vector<u32> hashes_from_selector(Selector const& selector, IncludeRightmostCompound include_rightmost_compound)
        {
            Vector<u32> hashes;
            auto const& compound_selectors = selector.compound_selectors();
            if (compound_selectors.is_empty())
                return hashes;

            if (include_rightmost_compound == IncludeRightmostCompound::Yes) {
                // A selector-list pseudo-class in an ancestor compound matches that ancestor.
                // The argument selector's rightmost compound is therefore on the subject's
                // ancestor chain.
                for (auto hash : hashes_from_compound(compound_selectors.last()))
                    append_unique_hash(hashes, hash);
            } else {
                // A selector-list pseudo-class in the subject compound can still contain
                // ancestor requirements inside its alternatives, e.g. `:is(.foo > .bar)`.
                for (auto hash : hashes_from_subject_compound_selector_list_pseudo_classes(compound_selectors.last()))
                    append_unique_hash(hashes, hash);
            }

            auto combinator_to_right = compound_selectors.last().combinator;
            for (ssize_t i = static_cast<ssize_t>(compound_selectors.size()) - 2; i >= 0; --i) {
                auto const& compound_selector = compound_selectors[i];

                switch (combinator_to_right) {
                case Combinator::Descendant:
                case Combinator::ImmediateChild:
                case Combinator::PseudoElement:
                    for (auto hash : hashes_from_compound(compound_selector))
                        append_unique_hash(hashes, hash);
                    break;
                case Combinator::NextSibling:
                case Combinator::SubsequentSibling:
                    break;
                case Combinator::Column:
                default:
                    return hashes;
                }

                combinator_to_right = compound_selector.combinator;
            }

            return hashes;
        }

        Vector<u32> common_hashes_from_selector_list(SelectorList const& selector_list, IncludeRightmostCompound include_rightmost_compound)
        {
            if (selector_list.is_empty())
                return {};

            Optional<Vector<u32>> common_hashes;
            for (auto const& argument_selector : selector_list) {
                auto hashes = hashes_from_selector(*argument_selector, include_rightmost_compound);
                if (!common_hashes.has_value()) {
                    common_hashes = move(hashes);
                    continue;
                }

                intersect_hashes(common_hashes.value(), hashes);
                if (common_hashes->is_empty())
                    break;
            }

            return common_hashes.release_value();
        }

        bool append_hashes_from_simple_selector(SimpleSelector const& simple_selector)
        {
            for (auto hash : hashes_from_simple_selector(simple_selector)) {
                if (append_unique_hash(hash))
                    return true;
            }
            return false;
        }

        bool append_hashes_from_compound(CompoundSelector const& compound_selector)
        {
            for (auto const& simple_selector : compound_selector.simple_selectors) {
                if (append_hashes_from_simple_selector(simple_selector))
                    return true;
            }
            return false;
        }
    };
    AncestorHashCollector ancestor_hash_collector { m_ancestor_hashes, next_hash_index };

    for (auto hash : ancestor_hash_collector.hashes_from_subject_compound_selector_list_pseudo_classes(m_compound_selectors.last())) {
        if (ancestor_hash_collector.append_unique_hash(hash)) {
            m_can_use_ancestor_filter = (next_hash_index > 0);
            return;
        }
    }

    // Walk from the compound immediately to the left of the subject toward the left.
    // The combinator that connects `i` to `i+1` is stored on `i+1`.
    auto combinator_to_right = m_compound_selectors.last().combinator;

    // If we cross a sibling combinator, compounds further to the left are not ancestors of the subject,
    // but they *are* ancestors of that sibling and therefore shared ancestors of the subject (since siblings
    // share all ancestors above their parent). We can safely require tokens from those shared ancestors.
    for (ssize_t i = static_cast<ssize_t>(m_compound_selectors.size()) - 2; i >= 0; --i) {
        auto const& compound_selector = m_compound_selectors[i];

        switch (combinator_to_right) {
        case Combinator::Descendant:
        case Combinator::ImmediateChild:
        case Combinator::PseudoElement:
            // This compound is on the ancestor axis (directly, as the originating element for a pseudo-element,
            // or as a shared ancestor past a sibling boundary).
            if (ancestor_hash_collector.append_hashes_from_compound(compound_selector)) {
                m_can_use_ancestor_filter = (next_hash_index > 0);
                return;
            }
            break;

        case Combinator::NextSibling:
        case Combinator::SubsequentSibling:
            // The compound immediately to the left is a sibling constraint, not an ancestor.
            // Do not collect hashes from it.
            break;

        case Combinator::Column:
        default:
            // Not representable by the ancestor-hash filter.
            next_hash_index = 0;
            m_can_use_ancestor_filter = false;
            for (auto& slot : m_ancestor_hashes)
                slot = 0;
            return;
        }

        combinator_to_right = compound_selector.combinator;
    }

    m_can_use_ancestor_filter = (next_hash_index > 0);

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
                // count the number of type selectors and pseudo-elements in the selector (= C)
                ++tag_names;
                break;
            case SimpleSelector::Type::PseudoElement:
                // https://drafts.csswg.org/css-view-transitions-1/#named-view-transition-pseudo
                // The specificity of a named view transition pseudo-element selector with a <custom-ident> argument is equivalent
                // to a type selector. The specificity of a named view transition pseudo-element selector with a '*' argument is zero.
                // NB: We just break before adding to the type (tag name) specificity in case this is a named view transition pseudo that uses '*'
                if (first_is_one_of(simple_selector.pseudo_element().type(), CSS::PseudoElement::ViewTransitionGroup, CSS::PseudoElement::ViewTransitionImagePair, CSS::PseudoElement::ViewTransitionOld, CSS::PseudoElement::ViewTransitionNew) && simple_selector.pseudo_element().pt_name_selector().is_universal)
                    break;

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
            case SimpleSelector::Type::Invalid:
                // Ignore invalid selectors
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

Utf16String Selector::PseudoElementSelector::serialize() const
{
    Utf16StringBuilder builder;
    serialize_to(builder);
    return builder.to_string();
}

void Selector::PseudoElementSelector::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append_ascii("::"sv);

    if (!m_name.is_empty()) {
        builder.append(m_name.view());
    } else {
        builder.append_ascii(pseudo_element_name(m_type));
    }

    m_value.visit(
        [&builder](NonnullRefPtr<Selector> const& compound_selector) {
            builder.append_ascii('(');
            compound_selector->serialize_to(builder);
            builder.append_ascii(')');
        },
        [&builder](PTNameSelector const& pt_name_selector) {
            builder.append_ascii('(');
            if (pt_name_selector.is_universal)
                builder.append_ascii('*');
            else
                builder.append(pt_name_selector.value.view());
            builder.append_ascii(')');
        },
        [&builder](IdentList const& ident_list) {
            builder.append_ascii('(');
            bool first = true;
            for (auto const& ident : ident_list) {
                if (!first)
                    builder.append_ascii(' ');
                first = false;
                serialize_an_identifier(builder, ident);
            }
            builder.append_ascii(')');
        },
        [](Empty const&) {});
}

// https://www.w3.org/TR/cssom/#serialize-a-simple-selector
Utf16String Selector::SimpleSelector::serialize() const
{
    Utf16StringBuilder builder;
    serialize_to(builder);
    return builder.to_string();
}

void Selector::SimpleSelector::serialize_to(Utf16StringBuilder& s) const
{
    switch (type) {
    case Selector::SimpleSelector::Type::TagName:
    case Selector::SimpleSelector::Type::Universal: {
        auto qualified_name = this->qualified_name();
        // 1. If the namespace prefix maps to a namespace that is not the default namespace and is not the null
        //    namespace (not in a namespace) append the serialization of the namespace prefix as an identifier,
        //    followed by a "|" (U+007C) to s.
        if (qualified_name.namespace_type == QualifiedName::NamespaceType::Named) {
            serialize_an_identifier(s, qualified_name.namespace_);
            s.append_ascii('|');
        }

        // 2. If the namespace prefix maps to a namespace that is the null namespace (not in a namespace)
        //    append "|" (U+007C) to s.
        if (qualified_name.namespace_type == QualifiedName::NamespaceType::None)
            s.append_ascii('|');

        // 3. If this is a type selector append the serialization of the element name as an identifier to s.
        if (type == Selector::SimpleSelector::Type::TagName)
            serialize_an_identifier(s, qualified_name.name.name);

        // 4. If this is a universal selector append "*" (U+002A) to s.
        if (type == Selector::SimpleSelector::Type::Universal)
            s.append_ascii('*');

        break;
    }
    case Selector::SimpleSelector::Type::Attribute: {
        auto& attribute = this->attribute();

        // 1. Append "[" (U+005B) to s.
        s.append_ascii('[');

        // 2. If the namespace prefix maps to a namespace that is not the null namespace (not in a namespace)
        //    append the serialization of the namespace prefix as an identifier, followed by a "|" (U+007C) to s.
        if (attribute.qualified_name.namespace_type == QualifiedName::NamespaceType::Named) {
            serialize_an_identifier(s, attribute.qualified_name.namespace_);
            s.append_ascii('|');
        } else if (attribute.qualified_name.namespace_type == QualifiedName::NamespaceType::Any) {
            s.append_ascii("*|"sv);
        }

        // 3. Append the serialization of the attribute name as an identifier to s.
        serialize_an_identifier(s, attribute.qualified_name.name.name);

        // 4. If there is an attribute value specified, append "=", "~=", "|=", "^=", "$=", or "*=" as appropriate (depending on the type of attribute selector),
        //    followed by the serialization of the attribute value as a string, to s.
        if (!attribute.value.is_empty()) {
            switch (attribute.match_type) {
            case Selector::SimpleSelector::Attribute::MatchType::ExactValueMatch:
                s.append_ascii("="sv);
                break;
            case Selector::SimpleSelector::Attribute::MatchType::ContainsWord:
                s.append_ascii("~="sv);
                break;
            case Selector::SimpleSelector::Attribute::MatchType::ContainsString:
                s.append_ascii("*="sv);
                break;
            case Selector::SimpleSelector::Attribute::MatchType::StartsWithSegment:
                s.append_ascii("|="sv);
                break;
            case Selector::SimpleSelector::Attribute::MatchType::StartsWithString:
                s.append_ascii("^="sv);
                break;
            case Selector::SimpleSelector::Attribute::MatchType::EndsWithString:
                s.append_ascii("$="sv);
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
            s.append_ascii(" i"sv);
            break;
        case Selector::SimpleSelector::Attribute::CaseType::CaseSensitiveMatch:
            s.append_ascii(" s"sv);
            break;
        default:
            break;
        }

        // 6. Append "]" (U+005D) to s.
        s.append_ascii(']');
        break;
    }

    case Selector::SimpleSelector::Type::Class:
        // Append a "." (U+002E), followed by the serialization of the class name as an identifier to s.
        s.append_ascii('.');
        serialize_an_identifier(s, class_name().view());
        break;

    case Selector::SimpleSelector::Type::Id:
        // Append a "#" (U+0023), followed by the serialization of the ID as an identifier to s.
        s.append_ascii('#');
        serialize_an_identifier(s, id_name().view());
        break;

    case Selector::SimpleSelector::Type::PseudoClass: {
        auto& pseudo_class = this->pseudo_class();

        auto metadata = pseudo_class_metadata(pseudo_class.type);
        bool accepts_arguments = [&]() {
            if (!metadata.is_valid_as_function)
                return false;
            if (!metadata.is_valid_as_identifier)
                return true;
            // For pseudo-classes with both a function and identifier form, see if they have arguments.
            switch (pseudo_class.type) {
            case PseudoClass::Heading:
                return !pseudo_class.levels.is_empty();
            case PseudoClass::Host:
                return !pseudo_class.argument_selector_list.is_empty();
            default:
                VERIFY_NOT_REACHED();
            }
        }();

        // If the pseudo-class does not accept arguments append ":" (U+003A), followed by the name of the pseudo-class, to s.
        if (!accepts_arguments) {
            s.append_ascii(':');
            s.append_ascii(pseudo_class_name(pseudo_class.type));
        }
        // Otherwise, append ":" (U+003A), followed by the name of the pseudo-class, followed by "(" (U+0028),
        // followed by the value of the pseudo-class argument(s) determined as per below, followed by ")" (U+0029), to s.
        else {
            s.append_ascii(':');
            s.append_ascii(pseudo_class_name(pseudo_class.type));
            s.append_ascii('(');
            // NB: The spec list is incomplete. For ease of maintenance, we use the data from PseudoClasses.json for
            //     this instead of a hard-coded list.
            switch (metadata.parameter_type) {
            case PseudoClassMetadata::ParameterType::None:
                break;
            case PseudoClassMetadata::ParameterType::ANPlusB:
            case PseudoClassMetadata::ParameterType::ANPlusBOf:
                // The result of serializing the value using the rules to serialize an <an+b> value.
                pseudo_class.an_plus_b_pattern.serialize_to(s);
                break;
            case PseudoClassMetadata::ParameterType::CompoundSelector:
            case PseudoClassMetadata::ParameterType::ForgivingSelectorList:
            case PseudoClassMetadata::ParameterType::ForgivingRelativeSelectorList:
            case PseudoClassMetadata::ParameterType::RelativeSelectorList:
            case PseudoClassMetadata::ParameterType::SelectorList:
                // The result of serializing the value using the rules for serializing a group of selectors.
                serialize_a_group_of_selectors_to_builder(s, pseudo_class.argument_selector_list);
                break;
            case PseudoClassMetadata::ParameterType::Ident:
                serialize_an_identifier(s, pseudo_class.ident->string_value);
                break;
            case PseudoClassMetadata::ParameterType::LanguageRanges:
                // The serialization of a comma-separated list of each argument’s serialization as a string, preserving relative order.
                for (size_t i = 0; i < pseudo_class.languages.size(); ++i) {
                    if (i > 0)
                        s.append_ascii(", "sv);
                    s.append(pseudo_class.languages[i].view());
                }
                break;
            case PseudoClassMetadata::ParameterType::LevelList:
                // AD-HOC: not in the spec.
                for (size_t i = 0; i < pseudo_class.levels.size(); ++i) {
                    if (i > 0)
                        s.append_ascii(", "sv);
                    append_integer(s, pseudo_class.levels[i]);
                }
                break;
            }
            s.append_ascii(')');
        }
        break;
    }
    case Selector::SimpleSelector::Type::PseudoElement:
        // AD-HOC: Spec issue: https://github.com/w3c/csswg-drafts/issues/11997
        this->pseudo_element().serialize_to(s);
        break;
    case Type::Nesting:
        // AD-HOC: Not in spec yet.
        s.append_ascii('&');
        break;
    case Type::Invalid:
        // AD-HOC: We're not told how to do these. Just serialize their component values.
        auto invalid = value.get<Invalid>();
        for (auto const& component_value : invalid.component_values)
            component_value.serialize_to(s);
        break;
    }
}

// https://www.w3.org/TR/cssom/#serialize-a-selector
Utf16String Selector::serialize() const
{
    Utf16StringBuilder builder;
    serialize_to(builder);
    return builder.to_string();
}

void Selector::serialize_to(Utf16StringBuilder& s) const
{
    // AD-HOC: If this is a relative selector, we need to serialize the starting combinator.
    if (!compound_selectors().is_empty()) {
        switch (compound_selectors().first().combinator) {
        case Combinator::ImmediateChild:
            s.append_ascii("> "sv);
            break;
        case Combinator::NextSibling:
            s.append_ascii("+ "sv);
            break;
        case Combinator::SubsequentSibling:
            s.append_ascii("~ "sv);
            break;
        case Combinator::Column:
            s.append_ascii("|| "sv);
            break;
        case Combinator::PseudoElement:
        default:
            break;
        }
    }

    // To serialize a selector let s be the empty string, run the steps below for each part of the chain of the
    // selector, and finally return s:
    for (size_t i = 0; i < compound_selectors().size(); ++i) {
        auto const& compound_selector = compound_selectors()[i];
        // 1. If there is only one simple selector in the compound selectors which is a universal selector, append the
        //    result of serializing the universal selector to s.
        if (compound_selector.simple_selectors.size() == 1
            && compound_selector.simple_selectors.first().type == SimpleSelector::Type::Universal) {
            // NB: Because we've split any compound selectors that contain pseudo-elements, eg `*::before` becomes two
            //     CompoundSelectors, we have to include the following CompoundSelector as well.
            bool should_serialize_universal = !compound_selector.is_implicit_universal_anchor;
            if (should_serialize_universal
                && i != compound_selectors().size() - 1
                && compound_selectors()[i + 1].combinator == Combinator::PseudoElement) {
                auto qualified_name = compound_selector.simple_selectors.first().qualified_name();
                if (qualified_name.namespace_type == SimpleSelector::QualifiedName::NamespaceType::Default
                    || qualified_name.namespace_type == SimpleSelector::QualifiedName::NamespaceType::Any) {
                    should_serialize_universal = false;
                }
            }
            if (should_serialize_universal)
                compound_selector.simple_selectors.first().serialize_to(s);
        }
        // 2. Otherwise, for each simple selector in the compound selectors that is not a universal selector
        //    of which the namespace prefix maps to a namespace that is not the default namespace
        //    serialize the simple selector and append the result to s.
        else {
            for (auto& simple_selector : compound_selector.simple_selectors) {
                if (simple_selector.type == SimpleSelector::Type::Universal) {
                    if (compound_selector.is_implicit_universal_anchor)
                        continue;
                    auto qualified_name = simple_selector.qualified_name();
                    if (qualified_name.namespace_type == SimpleSelector::QualifiedName::NamespaceType::Default
                        || qualified_name.namespace_type == SimpleSelector::QualifiedName::NamespaceType::Any)
                        continue;
                    // FIXME: I *think* if we have a namespace prefix that happens to equal the same as the default namespace,
                    //        we also should skip it. But we don't have access to that here. eg:
                    // <style>
                    //   @namespace "http://example";
                    //   @namespace foo "http://example";
                    //   foo|*.bar { } /* This would skip the `foo|*` when serializing. */
                    // </style>
                }
                simple_selector.serialize_to(s);
            }
        }

        // 3. If this is not the last part of the chain of the selector append a single SPACE (U+0020),
        //    followed by the combinator ">", "+", "~", ">>", "||", as appropriate, followed by another
        //    single SPACE (U+0020) if the combinator was not whitespace, to s.
        if (i != compound_selectors().size() - 1) {
            // NB: The combinator that appears between parts `i` and `i+1` appears with the `i+1` selector,
            //     so we have to check that one.
            switch (compound_selectors()[i + 1].combinator) {
            case Combinator::Descendant:
                s.append_ascii(' ');
                break;
            case Combinator::ImmediateChild:
                s.append_ascii(" > "sv);
                break;
            case Combinator::NextSibling:
                s.append_ascii(" + "sv);
                break;
            case Combinator::SubsequentSibling:
                s.append_ascii(" ~ "sv);
                break;
            case Combinator::Column:
                s.append_ascii(" || "sv);
                break;
            case Combinator::PseudoElement:
            default:
                break;
            }
        } else {
            // 4. If this is the last part of the chain of the selector and there is a pseudo-element,
            // append "::" followed by the name of the pseudo-element, to s.
            // This algorithm has a problem, see https://github.com/w3c/csswg-drafts/issues/11997
            //      serialization of pseudoElements was moved to SimpleSelector::serialize()
        }
    }
}

// https://drafts.csswg.org/selectors-4/#single-colon-pseudos
bool is_legacy_single_colon_pseudo_element(PseudoElement pseudo_element)
{
    // The four Level 2 pseudo-elements (::before, ::after, ::first-line, and ::first-letter) may, for legacy reasons,
    // be written with only a single ":" character at their front, making them resemble a <pseudo-class-selector>.
    switch (pseudo_element) {
    case PseudoElement::After:
    case PseudoElement::Before:
    case PseudoElement::FirstLetter:
    case PseudoElement::FirstLine:
        return true;
    default:
        return false;
    }
}

// https://www.w3.org/TR/cssom/#serialize-a-group-of-selectors
static void serialize_a_group_of_selectors_to_builder(Utf16StringBuilder& builder, SelectorList const& selectors)
{
    // To serialize a group of selectors serialize each selector in the group of selectors and then serialize a comma-separated list of these serializations.
    bool first = true;
    for (auto const& selector : selectors) {
        if (!first)
            builder.append_ascii(", "sv);
        first = false;
        selector->serialize_to(builder);
    }
}

Utf16String serialize_a_group_of_selectors(SelectorList const& selectors)
{
    Utf16StringBuilder builder;
    serialize_a_group_of_selectors_to_builder(builder, selectors);
    return builder.to_string();
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

bool Selector::contains_unknown_webkit_pseudo_element() const
{
    for (auto const& compound_selector : m_compound_selectors) {
        for (auto const& simple_selector : compound_selector.simple_selectors) {
            if (simple_selector.type == SimpleSelector::Type::PseudoClass) {
                auto const& selector_list = simple_selector.pseudo_class().argument_selector_list;
                if (selector_list.contains([](auto const& s) { return s->contains_unknown_webkit_pseudo_element(); }))
                    return true;
            }
            if (simple_selector.type == SimpleSelector::Type::PseudoElement && simple_selector.pseudo_element().type() == PseudoElement::UnknownWebKit)
                return true;
        }
    }
    return false;
}

RefPtr<Selector> Selector::absolutized(Selector::SimpleSelector const& selector_for_nesting) const
{
    if (!contains_the_nesting_selector())
        return fixme_launder_const_through_pointer_cast(*this);

    Vector<CompoundSelector> absolutized_compound_selectors;
    absolutized_compound_selectors.ensure_capacity(m_compound_selectors.size());
    for (auto const& compound_selector : m_compound_selectors) {
        if (auto absolutized = compound_selector.absolutized(selector_for_nesting); absolutized.has_value()) {
            absolutized_compound_selectors.append(absolutized.release_value());
        } else {
            return nullptr;
        }
    }

    return Selector::create(move(absolutized_compound_selectors));
}

Optional<Selector::CompoundSelector> Selector::CompoundSelector::absolutized(Selector::SimpleSelector const& selector_for_nesting) const
{
    // TODO: Cache if it contains the nesting selector?

    Vector<SimpleSelector> absolutized_simple_selectors;
    absolutized_simple_selectors.ensure_capacity(simple_selectors.size());
    for (auto const& simple_selector : simple_selectors) {
        if (auto absolutized = simple_selector.absolutized(selector_for_nesting); absolutized.has_value()) {
            absolutized_simple_selectors.append(absolutized.release_value());
        } else {
            return {};
        }
    }

    return CompoundSelector {
        .combinator = this->combinator,
        .is_implicit_universal_anchor = this->is_implicit_universal_anchor,
        .simple_selectors = absolutized_simple_selectors,
    };
}

static bool contains_invalid_contents_for_has(Selector const& selector)
{
    // :has() has special validity rules:
    // - It can't appear inside itself
    // - It bans most pseudo-elements
    // https://drafts.csswg.org/selectors/#relational

    for (auto const& compound_selector : selector.compound_selectors()) {
        for (auto const& simple_selector : compound_selector.simple_selectors) {
            if (simple_selector.type == Selector::SimpleSelector::Type::PseudoElement) {
                if (!is_has_allowed_pseudo_element(simple_selector.pseudo_element().type()))
                    return true;
            }
            if (simple_selector.type == Selector::SimpleSelector::Type::PseudoClass) {
                if (simple_selector.pseudo_class().type == PseudoClass::Has)
                    return true;
                for (auto& child_selector : simple_selector.pseudo_class().argument_selector_list) {
                    if (contains_invalid_contents_for_has(*child_selector))
                        return true;
                }
            }
        }
    }

    return false;
}

Optional<Selector::SimpleSelector> Selector::SimpleSelector::absolutized(Selector::SimpleSelector const& selector_for_nesting) const
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
            for (auto const& argument_selector : pseudo_class.argument_selector_list) {
                if (auto absolutized = argument_selector->absolutized(selector_for_nesting)) {
                    new_selector_list.append(absolutized.release_nonnull());
                } else if (!pseudo_class.is_forgiving) {
                    return {};
                }
            }
            pseudo_class.argument_selector_list = move(new_selector_list);
        }

        // :has() has special validity rules
        if (pseudo_class.type == PseudoClass::Has) {
            for (auto const& selector : pseudo_class.argument_selector_list) {
                if (contains_invalid_contents_for_has(selector)) {
                    Parser::ErrorReporter::the().report(Parser::InvalidSelectorError {
                        .value_string = selector->serialize().to_utf8(),
                        .description = "After absolutizing, :has() would contain invalid contents."_string,
                    });
                    return {};
                }
            }
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
    case Type::Invalid:
        // Everything else isn't affected
        return *this;
    }

    VERIFY_NOT_REACHED();
}

SelectorList adapt_nested_relative_selector_list(SelectorList const& selectors, StyleNestingParent style_nesting_parent)
{
    // "Nested style rules differ from non-nested rules in the following ways:
    // - A nested style rule accepts a <relative-selector-list> as its prelude (rather than just a <selector-list>).
    //   Any relative selectors are relative to the elements represented by the nesting selector.
    // - If a selector in the <relative-selector-list> does not start with a combinator but does contain the nesting
    //   selector, it is interpreted as a non-relative selector."
    // https://drafts.csswg.org/css-nesting-1/#syntax
    // NOTE: We already parsed the selectors as a <relative-selector-list>

    SelectorList new_list;
    new_list.ensure_capacity(selectors.size());
    for (auto const& selector : selectors) {
        auto first_combinator = selector->compound_selectors().first().combinator;

        // Nested relative selectors get a `&` inserted at the beginning when the nearest nesting parent is a style rule.
        // This is, handily, how the spec wants them serialized:
        // "When serializing a relative selector in a nested style rule, the selector must be absolutized, with the
        // implied nesting selector inserted."
        // - https://drafts.csswg.org/css-nesting-1/#cssom
        // However, relative selectors directly inside a @scope rule stay relative to the scoping root.
        bool insert_leading_ampersand = style_nesting_parent == StyleNestingParent::Style
            && !first_is_one_of(first_combinator, Selector::Combinator::None, Selector::Combinator::Descendant);
        if (!selector->contains_the_nesting_selector() && style_nesting_parent == StyleNestingParent::Style)
            insert_leading_ampersand = true;

        if (insert_leading_ampersand) {
            new_list.append(selector->relative_to(Selector::SimpleSelector { .type = Selector::SimpleSelector::Type::Nesting }));
        } else if (first_combinator == Selector::Combinator::Descendant) {
            // Replace leading descendant combinator (whitespace) with none, because we're not actually relative.
            auto copied_compound_selectors = selector->compound_selectors();
            copied_compound_selectors.first().combinator = Selector::Combinator::None;
            new_list.append(Selector::create(move(copied_compound_selectors)));
        } else {
            new_list.append(selector);
        }
    }
    return new_list;
}

SelectorList absolutize_selectors_relative_to(SelectorList const& selectors, GC::Ptr<CSSRule const> parent)
{
    // NB: We use `:where(:scope)` to avoid adding specificity.
    static NeverDestroyed<Selector::SimpleSelector> where_scope_selector { Selector::SimpleSelector {
        .type = Selector::SimpleSelector::Type::PseudoClass,
        .value = Selector::SimpleSelector::PseudoClassSelector {
            .type = PseudoClass::Where,
            .argument_selector_list = {
                Selector::create({
                    Selector::CompoundSelector {
                        .combinator = Selector::Combinator::None,
                        .simple_selectors = {
                            Selector::SimpleSelector {
                                .type = Selector::SimpleSelector::Type::PseudoClass,
                                .value = Selector::SimpleSelector::PseudoClassSelector {
                                    .type = PseudoClass::Scope,
                                },
                            },
                        },
                    },
                }),
            },
        },
    } };

    auto parent_is_scope_rule = parent && parent->type() == CSSRule::Type::Scope;
    auto selector_is_scope_relative = [](Selector const& selector) {
        return !first_is_one_of(selector.compound_selectors().first().combinator, Selector::Combinator::None, Selector::Combinator::Descendant);
    };

    // Replace all occurrences of `&` with the nearest ancestor style rule's selector list wrapped in `:is(...)`,
    // or if we have no such ancestor, with `:scope`. Selectors directly inside @scope may remain serialized as
    // relative selectors, but need to be absolutized against :scope before matching.

    // If we don't have any nesting selectors, we can just use our selectors as they are.
    if (!any_of(selectors, [&](auto const& selector) { return selector->contains_the_nesting_selector() || (parent_is_scope_rule && selector_is_scope_relative(*selector)); }))
        return selectors;

    // Otherwise, build up a new list of selectors with the `&` replaced.

    // First, figure out what we should replace `&` with.
    // "When used in the selector of a nested style rule, the nesting selector represents the elements matched by the
    // parent rule. When used in any other context, it represents the same elements as :scope in that context (unless
    // otherwise defined)."
    // https://drafts.csswg.org/css-nesting-1/#nest-selector
    auto parent_selector = [&] -> Selector::SimpleSelector {
        if (auto const* parent_style_rule = as_if<CSSStyleRule const>(parent.ptr())) {
            // TODO: If there's only 1, we don't have to use `:is()` for it
            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoClass,
                .value = Selector::SimpleSelector::PseudoClassSelector {
                    .type = PseudoClass::Is,
                    .argument_selector_list = parent_style_rule->absolutized_selectors(),
                },
            };
        }

        return *where_scope_selector;
    }();

    SelectorList absolutized_selectors;
    for (auto const& selector : selectors) {
        if (!selector->contains_the_nesting_selector()) {
            if (parent_is_scope_rule && selector_is_scope_relative(*selector))
                absolutized_selectors.append(selector->relative_to(parent_selector));
            else
                absolutized_selectors.append(selector);
            continue;
        }
        if (auto absolutized = selector->absolutized(parent_selector))
            absolutized_selectors.append(absolutized.release_nonnull());
    }
    return absolutized_selectors;
}

// https://drafts.csswg.org/css-syntax-3/#serializing-anb
Utf16String Selector::SimpleSelector::ANPlusBPattern::serialize() const
{
    Utf16StringBuilder builder;
    serialize_to(builder);
    return builder.to_string();
}

void Selector::SimpleSelector::ANPlusBPattern::serialize_to(Utf16StringBuilder& result) const
{
    // 1. If A is zero, return the serialization of B.
    if (step_size == 0) {
        append_integer(result, offset);
        return;
    }

    // 2. Otherwise, let result initially be an empty string.

    // 3.
    // - A is 1: Append "n" to result.
    if (step_size == 1)
        result.append_ascii('n');
    // - A is -1: Append "-n" to result.
    else if (step_size == -1)
        result.append_ascii("-n"sv);
    // - A is non-zero: Serialize A and append it to result, then append "n" to result.
    else if (step_size != 0) {
        append_integer(result, step_size);
        result.append_ascii('n');
    }

    // 4.
    // - B is greater than zero: Append "+" to result, then append the serialization of B to result.
    if (offset > 0) {
        result.append_ascii('+');
        append_integer(result, offset);
    }
    // - B is less than zero: Append the serialization of B to result.
    else if (offset < 0) {
        append_integer(result, offset);
    }

    // 5. Return result.
}

}
