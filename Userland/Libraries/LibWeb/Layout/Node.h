/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <LibGfx/Rect.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/Handle.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/BoxModelMetrics.h>
#include <LibWeb/Painting/PaintContext.h>
#include <LibWeb/TreeNode.h>

namespace Web::Layout {

enum class LayoutMode {
    // Normal layout. No min-content or max-content constraints applied.
    Normal,

    // Intrinsic size determination.
    // Boxes honor min-content and max-content constraints (set via LayoutState::UsedValues::{width,height}_constraint)
    // by considering their containing block to be 0-sized or infinitely large in the relevant axis.
    // https://drafts.csswg.org/css-sizing-3/#intrinsic-sizing
    IntrinsicSizing,
};

class Node
    : public JS::Cell
    , public TreeNode<Node> {
    JS_CELL(Node, JS::Cell);

public:
    virtual ~Node();

    bool is_anonymous() const;
    DOM::Node const* dom_node() const;
    DOM::Node* dom_node();

    DOM::Element const* pseudo_element_generator() const;
    DOM::Element* pseudo_element_generator();

    enum class GeneratedFor {
        NotGenerated,
        PseudoBefore,
        PseudoAfter
    };
    bool is_generated() const { return m_generated_for != GeneratedFor::NotGenerated; }
    bool is_generated_for_before_pseudo_element() const { return m_generated_for == GeneratedFor::PseudoBefore; }
    bool is_generated_for_after_pseudo_element() const { return m_generated_for == GeneratedFor::PseudoAfter; }
    void set_generated_for(GeneratedFor type, DOM::Element& element)
    {
        m_generated_for = type;
        m_pseudo_element_generator = &element;
    }

    Painting::Paintable* paintable() { return m_paintable; }
    Painting::Paintable const* paintable() const { return m_paintable; }
    void set_paintable(JS::GCPtr<Painting::Paintable>);

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const;

    DOM::Document& document();
    DOM::Document const& document() const;

    HTML::BrowsingContext const& browsing_context() const;
    HTML::BrowsingContext& browsing_context();

    JS::GCPtr<HTML::Navigable> navigable() const;

    Viewport const& root() const;
    Viewport& root();

    bool is_root_element() const;

    String debug_description() const;

    bool has_style() const { return m_has_style; }
    bool has_style_or_parent_with_style() const;

    virtual bool can_have_children() const { return true; }

    CSS::Display display() const;

    bool is_inline() const;
    bool is_inline_block() const;
    bool is_inline_table() const;

    bool is_out_of_flow(FormattingContext const&) const;

    // These are used to optimize hot is<T> variants for some classes where dynamic_cast is too slow.
    virtual bool is_box() const { return false; }
    virtual bool is_block_container() const { return false; }
    virtual bool is_break_node() const { return false; }
    virtual bool is_text_node() const { return false; }
    virtual bool is_viewport() const { return false; }
    virtual bool is_svg_box() const { return false; }
    virtual bool is_svg_geometry_box() const { return false; }
    virtual bool is_svg_mask_box() const { return false; }
    virtual bool is_svg_svg_box() const { return false; }
    virtual bool is_label() const { return false; }
    virtual bool is_replaced_box() const { return false; }
    virtual bool is_list_item_box() const { return false; }
    virtual bool is_list_item_marker_box() const { return false; }
    virtual bool is_table_wrapper() const { return false; }
    virtual bool is_node_with_style_and_box_model_metrics() const { return false; }

    template<typename T>
    bool fast_is() const = delete;

    bool is_floating() const;
    bool is_positioned() const;
    bool is_absolutely_positioned() const;
    bool is_fixed_position() const;

    bool is_flex_item() const { return m_is_flex_item; }
    void set_flex_item(bool b) { m_is_flex_item = b; }

    bool is_grid_item() const { return m_is_grid_item; }
    void set_grid_item(bool b) { m_is_grid_item = b; }

    Box const* containing_block() const;
    Box* containing_block() { return const_cast<Box*>(const_cast<Node const*>(this)->containing_block()); }

    [[nodiscard]] Box const* static_position_containing_block() const;
    [[nodiscard]] Box* static_position_containing_block() { return const_cast<Box*>(const_cast<Node const*>(this)->static_position_containing_block()); }

    // Closest non-anonymous ancestor box, to be used when resolving percentage values.
    // Anonymous block boxes are ignored when resolving percentage values that would refer to it:
    // the closest non-anonymous ancestor box is used instead.
    // https://www.w3.org/TR/CSS22/visuren.html#anonymous-block-level
    Box const* non_anonymous_containing_block() const;

    bool establishes_stacking_context() const;

    bool can_contain_boxes_with_position_absolute() const;

    Gfx::Font const& first_available_font() const;
    Gfx::Font const& scaled_font(PaintContext&) const;
    Gfx::Font const& scaled_font(float scale_factor) const;

    CSS::ImmutableComputedValues const& computed_values() const;

    NodeWithStyle* parent();
    NodeWithStyle const* parent() const;

    void inserted_into(Node&) { }
    void removed_from(Node&) { }
    void children_changed() { }

    bool children_are_inline() const { return m_children_are_inline; }
    void set_children_are_inline(bool value) { m_children_are_inline = value; }

    u32 initial_quote_nesting_level() const { return m_initial_quote_nesting_level; }
    void set_initial_quote_nesting_level(u32 value) { m_initial_quote_nesting_level = value; }

    // An element is called out of flow if it is floated, absolutely positioned, or is the root element.
    // https://www.w3.org/TR/CSS22/visuren.html#positioning-scheme
    bool is_out_of_flow() const { return is_floating() || is_absolutely_positioned(); }

    // An element is called in-flow if it is not out-of-flow.
    // https://www.w3.org/TR/CSS22/visuren.html#positioning-scheme
    bool is_in_flow() const { return !is_out_of_flow(); }

protected:
    Node(DOM::Document&, DOM::Node*);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    friend class NodeWithStyle;

    JS::NonnullGCPtr<DOM::Node> m_dom_node;
    JS::GCPtr<Painting::Paintable> m_paintable;

    JS::NonnullGCPtr<HTML::BrowsingContext> m_browsing_context;

    JS::GCPtr<DOM::Element> m_pseudo_element_generator;

    bool m_anonymous { false };
    bool m_has_style { false };
    bool m_children_are_inline { false };

    bool m_is_flex_item { false };
    bool m_is_grid_item { false };

    GeneratedFor m_generated_for { GeneratedFor::NotGenerated };

    u32 m_initial_quote_nesting_level { 0 };
};

class NodeWithStyle : public Node {
    JS_CELL(NodeWithStyle, Node);

public:
    virtual ~NodeWithStyle() override = default;

    CSS::ImmutableComputedValues const& computed_values() const { return static_cast<CSS::ImmutableComputedValues const&>(*m_computed_values); }
    CSS::MutableComputedValues& mutable_computed_values() { return static_cast<CSS::MutableComputedValues&>(*m_computed_values); }

    void apply_style(const CSS::StyleProperties&);

    Gfx::Font const& first_available_font() const;
    Vector<CSS::BackgroundLayerData> const& background_layers() const { return computed_values().background_layers(); }
    const CSS::AbstractImageStyleValue* list_style_image() const { return m_list_style_image; }

    JS::NonnullGCPtr<NodeWithStyle> create_anonymous_wrapper() const;

    void transfer_table_box_computed_values_to_wrapper_computed_values(CSS::ComputedValues& wrapper_computed_values);

    virtual void visit_edges(Cell::Visitor& visitor) override;

protected:
    NodeWithStyle(DOM::Document&, DOM::Node*, NonnullRefPtr<CSS::StyleProperties>);
    NodeWithStyle(DOM::Document&, DOM::Node*, NonnullOwnPtr<CSS::ComputedValues>);

private:
    void reset_table_box_computed_values_used_by_wrapper_to_init_values();
    void propagate_style_to_anonymous_wrappers();

    NonnullOwnPtr<CSS::ComputedValues> m_computed_values;
    RefPtr<CSS::AbstractImageStyleValue const> m_list_style_image;
};

class NodeWithStyleAndBoxModelMetrics : public NodeWithStyle {
    JS_CELL(NodeWithStyleAndBoxModelMetrics, NodeWithStyle);

public:
    BoxModelMetrics& box_model() { return m_box_model; }
    BoxModelMetrics const& box_model() const { return m_box_model; }

protected:
    NodeWithStyleAndBoxModelMetrics(DOM::Document& document, DOM::Node* node, NonnullRefPtr<CSS::StyleProperties> style)
        : NodeWithStyle(document, node, move(style))
    {
    }

    NodeWithStyleAndBoxModelMetrics(DOM::Document& document, DOM::Node* node, NonnullOwnPtr<CSS::ComputedValues> computed_values)
        : NodeWithStyle(document, node, move(computed_values))
    {
    }

private:
    virtual bool is_node_with_style_and_box_model_metrics() const final { return true; }

    BoxModelMetrics m_box_model;
};

template<>
inline bool Node::fast_is<NodeWithStyleAndBoxModelMetrics>() const { return is_node_with_style_and_box_model_metrics(); }

inline bool Node::has_style_or_parent_with_style() const
{
    return m_has_style || (parent() != nullptr && parent()->has_style_or_parent_with_style());
}

inline Gfx::Font const& Node::first_available_font() const
{
    VERIFY(has_style_or_parent_with_style());
    if (m_has_style)
        return static_cast<NodeWithStyle const*>(this)->first_available_font();
    return parent()->first_available_font();
}

inline Gfx::Font const& Node::scaled_font(PaintContext& context) const
{
    return scaled_font(context.device_pixels_per_css_pixel());
}

inline Gfx::Font const& Node::scaled_font(float scale_factor) const
{
    auto const& font = first_available_font();
    return font.with_size(font.point_size() * scale_factor);
}

inline const CSS::ImmutableComputedValues& Node::computed_values() const
{
    VERIFY(has_style_or_parent_with_style());

    if (m_has_style)
        return static_cast<NodeWithStyle const*>(this)->computed_values();
    return parent()->computed_values();
}

inline NodeWithStyle const* Node::parent() const
{
    return static_cast<NodeWithStyle const*>(TreeNode<Node>::parent());
}

inline NodeWithStyle* Node::parent()
{
    return static_cast<NodeWithStyle*>(TreeNode<Node>::parent());
}

inline Gfx::Font const& NodeWithStyle::first_available_font() const
{
    // https://drafts.csswg.org/css-fonts/#first-available-font
    // First font for which the character U+0020 (space) is not excluded by a unicode-range
    return computed_values().font_list().font_for_code_point(' ');
}

}
