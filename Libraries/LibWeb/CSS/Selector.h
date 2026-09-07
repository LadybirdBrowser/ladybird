/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class RustRule;
class RustNamespaceContext;

namespace SelectorFFI {

struct RustSelector;
struct RustParsedSelectorList;

}

using SelectorList = Vector<NonnullRefPtr<class Selector>>;

class Selector final : public RefCounted<Selector> {
public:
    enum class Combinator : u8 {
        None,
        ImmediateChild,
        Descendant,
        NextSibling,
        SubsequentSibling,
        Column,
        PseudoElement,
    };

    class PseudoElementSelector {
    public:
        explicit PseudoElementSelector(PseudoElement type)
            : m_type(type)
        {
        }

        PseudoElementSelector(PseudoElement type, Utf16String serialized)
            : m_type(type)
            , m_serialized(move(serialized))
        {
        }

        bool operator==(PseudoElementSelector const& other) const
        {
            return m_type == other.m_type && serialize() == other.serialize();
        }
        [[nodiscard]] static bool is_known_pseudo_element_type(PseudoElement type)
        {
            return to_underlying(type) < to_underlying(PseudoElement::KnownPseudoElementCount);
        }

        Utf16String serialize() const;
        PseudoElement type() const { return m_type; }

    private:
        PseudoElement m_type;
        Utf16String m_serialized;
    };

    static NonnullRefPtr<Selector> create(SelectorFFI::RustSelector*);
    ~Selector();

    Optional<PseudoElement> target_pseudo_element() const;
    bool contains_the_nesting_selector() const;
    bool contains_pseudo_class(PseudoClass) const;
    bool contains_named_namespace() const;
    Combinator first_combinator() const;
    u32 specificity() const;
    Utf16String serialize() const;
    void serialize_to(Utf16StringBuilder&, StyleSheetState const* = nullptr) const;

    SelectorFFI::RustSelector const& rust_selector() const { return *m_rust_selector; }

private:
    explicit Selector(SelectorFFI::RustSelector*);

    SelectorFFI::RustSelector* m_rust_selector { nullptr };
};

SelectorList selector_list_from_rust(SelectorFFI::RustParsedSelectorList const*);
SelectorList matching_selectors_for_rule(RustRule const&);
Optional<SelectorList> scope_start_selectors_for_rule(RustRule const&);
Optional<SelectorList> scope_end_selectors_for_rule(RustRule const&);
Utf16String serialize_a_group_of_selectors(SelectorList const&, StyleSheetState const* = nullptr);
u8 pseudo_element_to_ffi(Optional<PseudoElement>);
Optional<PseudoElement> pseudo_element_from_ffi(u8);

Optional<SelectorList> parse_selector_list_in_rust(Utf16View, RustNamespaceContext const&, bool is_relative, bool is_forgiving);

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
