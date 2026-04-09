/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// Either an Element or a PseudoElement
class WEB_API AbstractElement {
public:
    AbstractElement(GC::Ref<Element>, Optional<CSS::PseudoElement> = {});

    Document& document() const;

    Element& element() { return m_element; }
    Element const& element() const { return m_element; }
    Optional<CSS::PseudoElement> pseudo_element() const { return m_pseudo_element; }

    GC::Ptr<Layout::NodeWithStyle> layout_node();
    GC::Ptr<Layout::NodeWithStyle const> layout_node() const { return const_cast<AbstractElement*>(this)->layout_node(); }

    GC::Ptr<Layout::NodeWithStyle> unsafe_layout_node();
    GC::Ptr<Layout::NodeWithStyle const> unsafe_layout_node() const { return const_cast<AbstractElement*>(this)->unsafe_layout_node(); }

    struct TreeCountingFunctionResolutionContext {
        size_t sibling_count;
        size_t sibling_index;
    };
    TreeCountingFunctionResolutionContext tree_counting_function_resolution_context() const;

    GC::Ptr<Element const> parent_element() const;
    Optional<AbstractElement> element_to_inherit_style_from() const;
    Optional<AbstractElement> previous_in_tree_order() { return walk_layout_tree(WalkMethod::Previous); }
    Optional<AbstractElement> previous_sibling_in_tree_order() { return walk_layout_tree(WalkMethod::PreviousSibling); }
    bool is_before(AbstractElement const&) const;

    void set_inheritance_override(GC::Ref<Element> element) { m_inheritance_override = element; }

    GC::Ptr<CSS::ComputedProperties const> computed_properties() const;

    void set_custom_property_data(RefPtr<CSS::CustomPropertyData const>);
    [[nodiscard]] RefPtr<CSS::CustomPropertyData const> custom_property_data() const;
    RefPtr<CSS::StyleValue const> get_custom_property(FlyString const& name) const;

    GC::Ptr<CSS::CascadedProperties> cascaded_properties() const;
    void set_cascaded_properties(GC::Ptr<CSS::CascadedProperties>);

    bool has_non_empty_counters_set() const;
    Optional<CSS::CountersSet const&> counters_set() const;
    CSS::CountersSet& ensure_counters_set();
    void set_counters_set(OwnPtr<CSS::CountersSet>&&);

    HashMap<FlyString, GC::Ref<CSS::CSSAnimation>>* css_defined_animations() const;
    void set_has_css_defined_animations();

    void visit(GC::Cell::Visitor& visitor) const;

    String debug_description() const;
    bool operator==(AbstractElement const&) const = default;

    CSS::StyleScope const& style_scope() const;

private:
    enum class WalkMethod : u8 {
        Previous,
        PreviousSibling,
    };
    Optional<AbstractElement> walk_layout_tree(WalkMethod);

    GC::Ref<Element> m_element;
    Optional<CSS::PseudoElement> m_pseudo_element;

    GC::Ptr<Element> m_inheritance_override;
};

}

template<>
struct AK::Traits<Web::DOM::AbstractElement> : public DefaultTraits<Web::DOM::AbstractElement> {
    static unsigned hash(Web::DOM::AbstractElement const& key)
    {
        return pair_int_hash(ptr_hash(&key.element()), key.pseudo_element().has_value() ? to_underlying(key.pseudo_element().value()) : -1);
    }
};
