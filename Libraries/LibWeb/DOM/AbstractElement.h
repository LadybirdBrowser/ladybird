/*
 * Copyright (c) 2025-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibGC/Cell.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class InstalledStyle;

template<typename StyleGroup>
StyleGroup const& style_record_group(void const* record);

}

namespace Web::DOM {

// Either an Element or a PseudoElement
// FIXME: Figure out const-correctness for the Element.
class WEB_API AbstractElement {
public:
    AbstractElement(GC::Ref<Element>, Optional<CSS::PseudoElement> = {}, Utf16FlyString highlight_name = {});
    AbstractElement(Element const&, Optional<CSS::PseudoElement> = {}, Utf16FlyString highlight_name = {});

    Document& document() const;

    Element& element() { return m_element; }
    Element const& element() const { return m_element; }
    Optional<CSS::PseudoElement> pseudo_element() const { return m_pseudo_element; }
    Utf16FlyString const& highlight_name() const { return m_highlight_name; }
    bool is_custom_highlight() const { return m_pseudo_element == CSS::PseudoElement::Highlight && !m_highlight_name.is_empty(); }

    Layout::NodeWithStyle* layout_node(Layout::BegunRead const& read);
    Layout::NodeWithStyle const* layout_node(Layout::BegunRead const& read) const { return const_cast<AbstractElement*>(this)->layout_node(read); }

    Layout::NodeWithStyle* unsafe_layout_node(Layout::BegunRead const& read);
    Layout::NodeWithStyle const* unsafe_layout_node(Layout::BegunRead const& read) const { return const_cast<AbstractElement*>(this)->unsafe_layout_node(read); }

    struct TreeCountingFunctionResolutionContext {
        size_t sibling_count;
        size_t sibling_index;
    };
    TreeCountingFunctionResolutionContext tree_counting_function_resolution_context() const;

    GC::Ptr<Element const> parent_element() const;
    Element* flat_tree_parent_element() const;
    Optional<AbstractElement> element_to_inherit_style_from() const;
    GC::Ptr<Node> root();

    [[nodiscard]] CSS::ComputedStyleRecordView computed_style() const;
    [[nodiscard]] CSS::StyleRecordID style_record_identity() const;
    [[nodiscard]] CSS::InstalledStyle const& installed_style() const;
    [[nodiscard]] bool has_style() const { return !!style_record_identity(); }
    [[nodiscard]] void const* style_record_groups() const;
    template<typename StyleGroup>
    StyleGroup const* style_group() const
    {
        auto const* groups = style_record_groups();
        if (!groups)
            return nullptr;
        return &CSS::style_record_group<StyleGroup>(groups);
    }
    GC::Ptr<CSS::CSSStyleProperties const> inline_style() const;

    void set_custom_property_data(RefPtr<CSS::CustomPropertyData const>);
    void replace_custom_property_data(Badge<CSS::StyleComputer>, RefPtr<CSS::CustomPropertyData const>, CSS::IsAnimationOverlay);
    [[nodiscard]] RefPtr<CSS::CustomPropertyData const> custom_property_data() const;
    // Whether the environment `custom_property_data()` answers is the overlay of what this element's own animations
    // sample. A child that inherits all of its parent's custom properties holds its parent's overlay itself, which is
    // not an animation of the child's.
    [[nodiscard]] bool custom_property_data_is_animation_overlay() const;
    RefPtr<CSS::StyleValue const> get_custom_property(Utf16FlyString const& name) const;

    Vector<GC::Ref<CSS::CSSAnimation>> const* css_defined_animations() const;
    void set_css_defined_animations(Vector<GC::Ref<CSS::CSSAnimation>>&&);

    void visit(GC::Cell::Visitor& visitor) const;

    Utf16String debug_description() const;
    bool operator==(AbstractElement const&) const = default;

    CSS::StyleScope const& style_scope() const;

    void visit_edges(GC::Cell::Visitor&) const;

private:
    GC::Ref<Element> m_element;
    Optional<CSS::PseudoElement> m_pseudo_element;
    Utf16FlyString m_highlight_name;
};

}

template<>
struct AK::Traits<Web::DOM::AbstractElement> : public DefaultTraits<Web::DOM::AbstractElement> {
    static unsigned hash(Web::DOM::AbstractElement const& key)
    {
        return pair_int_hash(pair_int_hash(ptr_hash(&key.element()), key.pseudo_element().has_value() ? to_underlying(key.pseudo_element().value()) : -1), key.highlight_name().hash());
    }
};
