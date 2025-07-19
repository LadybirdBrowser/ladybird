/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// Either an Element or a PseudoElement
class AbstractElement {
public:
    AbstractElement(GC::Ref<Element>, Optional<CSS::PseudoElement> = {});

    Document& document() const;

    Element& element() { return m_element; }
    Element const& element() const { return m_element; }
    Optional<CSS::PseudoElement> pseudo_element() const { return m_pseudo_element; }

    GC::Ptr<Layout::NodeWithStyle> layout_node();
    GC::Ptr<Layout::NodeWithStyle const> layout_node() const { return const_cast<AbstractElement*>(this)->layout_node(); }

    GC::Ptr<Element const> parent_element() const;
    Optional<AbstractElement> previous_in_tree_order() { return walk_layout_tree(WalkMethod::Previous); }
    Optional<AbstractElement> previous_sibling_in_tree_order() { return walk_layout_tree(WalkMethod::PreviousSibling); }
    bool is_before(AbstractElement const&) const;

    GC::Ptr<CSS::ComputedProperties const> computed_properties() const;

    void set_custom_properties(HashMap<FlyString, CSS::StyleProperty>&& custom_properties);
    [[nodiscard]] HashMap<FlyString, CSS::StyleProperty> const& custom_properties() const;
    RefPtr<CSS::CSSStyleValue const> get_custom_property(FlyString const& name) const;

    bool has_non_empty_counters_set() const;
    Optional<CSS::CountersSet const&> counters_set() const;
    CSS::CountersSet& ensure_counters_set();
    void set_counters_set(OwnPtr<CSS::CountersSet>&&);

    void visit(GC::Cell::Visitor& visitor) const;

    String debug_description() const;
    bool operator==(AbstractElement const&) const = default;

private:
    enum class WalkMethod : u8 {
        Previous,
        PreviousSibling,
    };
    Optional<AbstractElement> walk_layout_tree(WalkMethod);

    GC::Ref<Element> m_element;
    Optional<CSS::PseudoElement> m_pseudo_element;
};

}

template<>
struct AK::Traits<Web::DOM::AbstractElement> : public DefaultTraits<Web::DOM::AbstractElement> {
    static unsigned hash(Web::DOM::AbstractElement const& key)
    {
        return pair_int_hash(ptr_hash(&key.element()), key.pseudo_element().has_value() ? to_underlying(key.pseudo_element().value()) : -1);
    }
};
