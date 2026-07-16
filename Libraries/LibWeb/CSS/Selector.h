/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Forward.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/PseudoClassBitmap.h>
#include <LibWeb/CSS/PseudoElement.h>

namespace Web::CSS {

namespace SelectorFFI {

struct RustSelector;

}

using SelectorList = Vector<NonnullRefPtr<class Selector>>;

// This is a <complex-selector> in the spec. https://www.w3.org/TR/selectors-4/#complex
class Selector : public RefCounted<Selector> {
public:
    class PseudoElementSelector {
    public:
        struct PTNameSelector {
            bool is_universal { false };
            Utf16FlyString value {};
        };
        using IdentList = Vector<Utf16FlyString>;

        using Value = Variant<Empty, PTNameSelector, NonnullRefPtr<Selector>, IdentList>;

        explicit PseudoElementSelector(PseudoElement type, Value value = {})
            : m_type(type)
            , m_value(move(value))
        {
            VERIFY(is_known_pseudo_element_type(type));
        }

        PseudoElementSelector(PseudoElement type, Utf16FlyString name, Value value = {})
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

        Utf16String serialize() const;
        void serialize_to(Utf16StringBuilder&) const;

        PseudoElement type() const { return m_type; }

        PTNameSelector const& pt_name_selector() const { return m_value.get<PTNameSelector>(); }

        // NOTE: This can't (currently) be a CompoundSelector due to cyclic dependencies.
        Selector const& compound_selector() const { return m_value.get<NonnullRefPtr<Selector>>(); }

        IdentList const& ident_list() const { return m_value.get<IdentList>(); }

    private:
        PseudoElement m_type;
        Utf16FlyString m_name;
        Value m_value;
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

            Utf16String serialize() const;
            void serialize_to(Utf16StringBuilder&) const;
        };

        struct PseudoClassSelector {
            PseudoClass type;

            // Used for the :nth-*() pseudo-classes
            ANPlusBPattern an_plus_b_pattern {};

            // FIXME: This would make more sense as part of SelectorList but that's currently a `using`
            bool is_forgiving { false };
            SelectorList argument_selector_list {};

            // Used for :lang(en-gb,dk)
            Vector<Utf16FlyString> languages {};

            // Used by :dir()
            struct Ident {
                Keyword keyword;
                Utf16FlyString string_value;
            };
            Optional<Ident> ident {};

            // Used by :heading()
            Vector<i64> levels {};
        };

        struct Name {
            Name(Utf16FlyString n)
                : name(move(n))
                , lowercase_name(name.to_ascii_lowercase())
            {
            }

            Utf16FlyString name;
            Utf16FlyString lowercase_name;
        };

        struct Id {
            Id(Utf16FlyString n)
                : name(move(n))
            {
            }

            Utf16FlyString name;
        };

        struct ClassName {
            ClassName(Utf16FlyString n)
                : name(move(n))
            {
            }

            Utf16FlyString name;
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
            Utf16FlyString namespace_ {};
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
            Utf16String value {};
            CaseType case_type;
        };

        struct Invalid {
            Vector<Parser::ComponentValue> component_values;
        };

        Type type;
        Variant<Empty, Attribute, PseudoClassSelector, PseudoElementSelector, Name, Id, ClassName, QualifiedName, Invalid> value {};

        Attribute const& attribute() const { return value.get<Attribute>(); }
        Attribute& attribute() { return value.get<Attribute>(); }
        PseudoClassSelector const& pseudo_class() const { return value.get<PseudoClassSelector>(); }
        PseudoClassSelector& pseudo_class() { return value.get<PseudoClassSelector>(); }
        PseudoElementSelector const& pseudo_element() const { return value.get<PseudoElementSelector>(); }
        PseudoElementSelector& pseudo_element() { return value.get<PseudoElementSelector>(); }

        Utf16FlyString const& name() const { return value.get<Name>().name; }
        Utf16FlyString& name() { return value.get<Name>().name; }
        Utf16FlyString const& id_name() const { return value.get<Id>().name; }
        Utf16FlyString& id_name() { return value.get<Id>().name; }
        Utf16FlyString const& class_name() const { return value.get<ClassName>().name; }
        Utf16FlyString& class_name() { return value.get<ClassName>().name; }
        Utf16FlyString const& lowercase_name() const { return value.get<Name>().lowercase_name; }
        Utf16FlyString& lowercase_name() { return value.get<Name>().lowercase_name; }
        QualifiedName const& qualified_name() const { return value.get<QualifiedName>(); }
        QualifiedName& qualified_name() { return value.get<QualifiedName>(); }

        Utf16String serialize() const;
        void serialize_to(Utf16StringBuilder&) const;

        Optional<SimpleSelector> absolutized(SimpleSelector const& selector_for_nesting) const;
    };

    enum class Combinator {
        None,
        ImmediateChild,    // >
        Descendant,        // <whitespace>
        NextSibling,       // +
        SubsequentSibling, // ~
        Column,            // ||
        PseudoElement,     // Internal-only transition to a different AbstractElement
    };

    struct CompoundSelector {
        // Spec-wise, the <combinator> is not part of a <compound-selector>,
        // but it is more understandable to put them together.
        Combinator combinator { Combinator::None };
        bool is_implicit_universal_anchor { false };
        Vector<SimpleSelector> simple_selectors;

        Optional<CompoundSelector> absolutized(SimpleSelector const& selector_for_nesting) const;
    };

    static NonnullRefPtr<Selector> create(Vector<CompoundSelector>&& compound_selectors)
    {
        return adopt_ref(*new Selector(move(compound_selectors)));
    }

    ~Selector();

    Vector<CompoundSelector> const& compound_selectors() const { return m_compound_selectors; }
    Optional<PseudoElement> target_pseudo_element() const { return m_target_pseudo_element; }
    NonnullRefPtr<Selector> relative_to(SimpleSelector const&) const;
    bool contains_the_nesting_selector() const { return m_contains_the_nesting_selector; }
    bool contains_pseudo_class(PseudoClass pseudo_class) const { return m_contained_pseudo_classes.get(pseudo_class); }
    bool contains_unknown_webkit_pseudo_element() const;
    RefPtr<Selector> absolutized(SimpleSelector const& selector_for_nesting) const;
    u32 specificity() const;
    Utf16String serialize() const;
    void serialize_to(Utf16StringBuilder&) const;

    auto const& ancestor_hashes() const { return m_ancestor_hashes; }

    bool can_use_ancestor_filter() const { return m_can_use_ancestor_filter; }

    bool is_slotted() const { return m_contains_slotted_pseudo_element; }
    bool has_part_pseudo_element() const { return m_contains_part_pseudo_element; }

    SelectorFFI::RustSelector const& rust_selector() const
    {
        VERIFY(m_rust_selector);
        return *m_rust_selector;
    }

private:
    explicit Selector(Vector<CompoundSelector>&&);

    Vector<CompoundSelector> m_compound_selectors;
    mutable Optional<u32> m_specificity;
    Optional<PseudoElement> m_target_pseudo_element;
    bool m_can_use_ancestor_filter { false };
    bool m_contains_the_nesting_selector { false };
    bool m_contains_slotted_pseudo_element { false };
    bool m_contains_part_pseudo_element { false };

    PseudoClassBitmap m_contained_pseudo_classes;

    void collect_ancestor_hashes();

    Array<u32, 8> m_ancestor_hashes;
    SelectorFFI::RustSelector* m_rust_selector { nullptr };
};

bool is_legacy_single_colon_pseudo_element(PseudoElement);

Utf16String serialize_a_group_of_selectors(SelectorList const& selectors);

enum class StyleNestingParent : u8 {
    None,
    Style,
    Scope,
};
SelectorList adapt_nested_relative_selector_list(SelectorList const&, StyleNestingParent);

SelectorList absolutize_selectors_relative_to(SelectorList const&, GC::Ptr<CSSRule const> parent);

}

namespace AK {

template<>
struct Formatter<Web::CSS::Selector> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Selector const& selector)
    {
        return Formatter<StringView>::format(builder, selector.serialize().to_utf8());
    }
};

}
