/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/PseudoElement.h>

namespace Web::CSS {

using SelectorList = Vector<NonnullRefPtr<class Selector>>;

// This is a <complex-selector> in the spec. https://www.w3.org/TR/selectors-4/#complex
class Selector : public RefCounted<Selector> {
public:
    class PseudoElementSelector {
    public:
        struct PTNameSelector {
            bool is_universal { false };
            FlyString value {};
        };

        using Value = Variant<Empty, PTNameSelector>;

        explicit PseudoElementSelector(PseudoElement type, Value value = {})
            : m_type(type)
            , m_value(move(value))
        {
            VERIFY(is_known_pseudo_element_type(type));
        }

        PseudoElementSelector(PseudoElement type, String name, Value value = {})
            : m_type(type)
            , m_name(move(name))
            , m_value(move(value))
        {
        }

        bool operator==(PseudoElementSelector const&) const = default;

        [[nodiscard]] static bool is_known_pseudo_element_type(PseudoElement type)
        {
            return to_underlying(type) < to_underlying(PseudoElement::KnownPseudoElementCount);
        }

        String serialize() const;

        PseudoElement type() const { return m_type; }

        PTNameSelector const& pt_name_selector() const { return m_value.get<PTNameSelector>(); }

    private:
        PseudoElement m_type;
        String m_name;
        Variant<Empty, PTNameSelector> m_value;
    };

    struct SimpleSelector {
        enum class Type : u8 {
            Universal,
            TagName,
            Id,
            Class,
            Attribute,
            PseudoClass,
            PseudoElement,
            Nesting,
            Invalid,
        };

        struct ANPlusBPattern {
            int step_size { 0 }; // "A"
            int offset = { 0 };  // "B"

            // https://www.w3.org/TR/css-syntax-3/#serializing-anb
            String serialize() const
            {
                // 1. If A is zero, return the serialization of B.
                if (step_size == 0) {
                    return String::number(offset);
                }

                // 2. Otherwise, let result initially be an empty string.
                StringBuilder result;

                // 3.
                // - A is 1: Append "n" to result.
                if (step_size == 1)
                    result.append('n');
                // - A is -1: Append "-n" to result.
                else if (step_size == -1)
                    result.append("-n"sv);
                // - A is non-zero: Serialize A and append it to result, then append "n" to result.
                else if (step_size != 0)
                    result.appendff("{}n", step_size);

                // 4.
                // - B is greater than zero: Append "+" to result, then append the serialization of B to result.
                if (offset > 0)
                    result.appendff("+{}", offset);
                // - B is less than zero: Append the serialization of B to result.
                if (offset < 0)
                    result.appendff("{}", offset);

                // 5. Return result.
                return MUST(result.to_string());
            }
        };

        struct PseudoClassSelector {
            PseudoClass type;

            // FIXME: We don't need this field on every single SimpleSelector, but it's also annoying to malloc it somewhere.
            // Only used when "pseudo_class" is "NthChild" or "NthLastChild".
            ANPlusBPattern nth_child_pattern {};

            // FIXME: This would make more sense as part of SelectorList but that's currently a `using`
            bool is_forgiving { false };
            SelectorList argument_selector_list {};

            // Used for :lang(en-gb,dk)
            Vector<FlyString> languages {};

            // Used by :dir()
            Optional<Keyword> keyword {};
        };

        struct Name {
            Name(FlyString n)
                : name(move(n))
                , lowercase_name(name.to_string().to_lowercase().release_value_but_fixme_should_propagate_errors())
            {
            }

            FlyString name;
            FlyString lowercase_name;
        };

        // Equivalent to `<wq-name>`
        // https://www.w3.org/TR/selectors-4/#typedef-wq-name
        struct QualifiedName {
            enum class NamespaceType {
                Default, // `E`
                None,    // `|E`
                Any,     // `*|E`
                Named,   // `ns|E`
            };
            NamespaceType namespace_type { NamespaceType::Default };
            FlyString namespace_ {};
            Name name;
        };

        struct Attribute {
            enum class MatchType {
                HasAttribute,
                ExactValueMatch,
                ContainsWord,      // [att~=val]
                ContainsString,    // [att*=val]
                StartsWithSegment, // [att|=val]
                StartsWithString,  // [att^=val]
                EndsWithString,    // [att$=val]
            };
            enum class CaseType {
                DefaultMatch,
                CaseSensitiveMatch,
                CaseInsensitiveMatch,
            };
            MatchType match_type;
            QualifiedName qualified_name;
            String value {};
            CaseType case_type;
        };

        struct Invalid {
            Vector<Parser::ComponentValue> component_values;
        };

        Type type;
        Variant<Empty, Attribute, PseudoClassSelector, PseudoElementSelector, Name, QualifiedName, Invalid> value {};

        Attribute const& attribute() const { return value.get<Attribute>(); }
        Attribute& attribute() { return value.get<Attribute>(); }
        PseudoClassSelector const& pseudo_class() const { return value.get<PseudoClassSelector>(); }
        PseudoClassSelector& pseudo_class() { return value.get<PseudoClassSelector>(); }
        PseudoElementSelector const& pseudo_element() const { return value.get<PseudoElementSelector>(); }
        PseudoElementSelector& pseudo_element() { return value.get<PseudoElementSelector>(); }

        FlyString const& name() const { return value.get<Name>().name; }
        FlyString& name() { return value.get<Name>().name; }
        FlyString const& lowercase_name() const { return value.get<Name>().lowercase_name; }
        FlyString& lowercase_name() { return value.get<Name>().lowercase_name; }
        QualifiedName const& qualified_name() const { return value.get<QualifiedName>(); }
        QualifiedName& qualified_name() { return value.get<QualifiedName>(); }

        String serialize() const;

        Optional<SimpleSelector> absolutized(SimpleSelector const& selector_for_nesting) const;
    };

    enum class Combinator {
        None,
        ImmediateChild,    // >
        Descendant,        // <whitespace>
        NextSibling,       // +
        SubsequentSibling, // ~
        Column,            // ||
    };

    struct CompoundSelector {
        // Spec-wise, the <combinator> is not part of a <compound-selector>,
        // but it is more understandable to put them together.
        Combinator combinator { Combinator::None };
        Vector<SimpleSelector> simple_selectors;

        Optional<CompoundSelector> absolutized(SimpleSelector const& selector_for_nesting) const;
    };

    static NonnullRefPtr<Selector> create(Vector<CompoundSelector>&& compound_selectors)
    {
        return adopt_ref(*new Selector(move(compound_selectors)));
    }

    ~Selector() = default;

    Vector<CompoundSelector> const& compound_selectors() const { return m_compound_selectors; }
    Optional<PseudoElementSelector> const& pseudo_element() const { return m_pseudo_element; }
    NonnullRefPtr<Selector> relative_to(SimpleSelector const&) const;
    bool contains_the_nesting_selector() const { return m_contains_the_nesting_selector; }
    bool contains_hover_pseudo_class() const { return m_contains_hover_pseudo_class; }
    bool contains_unknown_webkit_pseudo_element() const;
    RefPtr<Selector> absolutized(SimpleSelector const& selector_for_nesting) const;
    u32 specificity() const;
    String serialize() const;

    auto const& ancestor_hashes() const { return m_ancestor_hashes; }

    bool can_use_fast_matches() const { return m_can_use_fast_matches; }
    bool can_use_ancestor_filter() const { return m_can_use_ancestor_filter; }

    size_t sibling_invalidation_distance() const;

private:
    explicit Selector(Vector<CompoundSelector>&&);

    Vector<CompoundSelector> m_compound_selectors;
    mutable Optional<u32> m_specificity;
    Optional<Selector::PseudoElementSelector> m_pseudo_element;
    mutable Optional<size_t> m_sibling_invalidation_distance;
    bool m_can_use_fast_matches { false };
    bool m_can_use_ancestor_filter { false };
    bool m_contains_the_nesting_selector { false };
    bool m_contains_hover_pseudo_class { false };

    void collect_ancestor_hashes();

    Array<u32, 8> m_ancestor_hashes;
};

String serialize_a_group_of_selectors(SelectorList const& selectors);

SelectorList adapt_nested_relative_selector_list(SelectorList const&);

}

namespace AK {

template<>
struct Formatter<Web::CSS::Selector> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Selector const& selector)
    {
        return Formatter<StringView>::format(builder, selector.serialize());
    }
};

}
