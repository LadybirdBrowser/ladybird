/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/StackingContext.h>

namespace Web::Painting {

Paintable::Paintable(Layout::Node const& layout_node)
    : m_layout_node(layout_node)
{
    auto& computed_values = layout_node.computed_values();
    if (layout_node.is_grid_item() && computed_values.z_index().has_value()) {
        // https://www.w3.org/TR/css-grid-2/#z-order
        // grid items with z_index should behave as if position were "relative"
        m_positioned = true;
    } else {
        m_positioned = computed_values.position() != CSS::Positioning::Static;
    }

    m_fixed_position = computed_values.position() == CSS::Positioning::Fixed;
    m_sticky_position = computed_values.position() == CSS::Positioning::Sticky;
    m_absolutely_positioned = computed_values.position() == CSS::Positioning::Absolute;
    m_floating = layout_node.is_floating();
    m_inline = layout_node.is_inline();
}

Paintable::~Paintable()
{
}

void Paintable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    TreeNode::visit_edges(visitor);
    visitor.visit(m_dom_node);
    visitor.visit(m_layout_node);
    if (m_containing_block.has_value())
        visitor.visit(m_containing_block.value());
}

bool Paintable::is_visible() const
{
    auto const& computed_values = this->computed_values();
    return computed_values.visibility() == CSS::Visibility::Visible && computed_values.opacity() != 0;
}

DOM::Document const& Paintable::document() const
{
    return layout_node().document();
}

DOM::Document& Paintable::document()
{
    return layout_node().document();
}

CSS::Display Paintable::display() const
{
    return layout_node().display();
}

PaintableBox* Paintable::containing_block() const
{
    if (!m_containing_block.has_value()) {
        auto containing_layout_box = m_layout_node->containing_block();
        if (containing_layout_box)
            m_containing_block = const_cast<PaintableBox*>(containing_layout_box->paintable_box());
        else
            m_containing_block = nullptr;
    }
    return *m_containing_block;
}

CSS::ImmutableComputedValues const& Paintable::computed_values() const
{
    return m_layout_node->computed_values();
}

bool Paintable::visible_for_hit_testing() const
{
    // https://html.spec.whatwg.org/multipage/interaction.html#inert-subtrees
    // When a node is inert:
    // - Hit-testing must act as if the 'pointer-events' CSS property were set to 'none'.
    if (auto dom_node = this->dom_node(); dom_node && dom_node->is_inert())
        return false;

    return computed_values().pointer_events() != CSS::PointerEvents::None;
}

void Paintable::set_dom_node(GC::Ptr<DOM::Node> dom_node)
{
    m_dom_node = dom_node;
}

GC::Ptr<DOM::Node> Paintable::dom_node()
{
    return m_dom_node;
}

GC::Ptr<DOM::Node const> Paintable::dom_node() const
{
    return m_dom_node;
}

GC::Ptr<HTML::Navigable> Paintable::navigable() const
{
    return document().navigable();
}

Paintable::DispatchEventOfSameName Paintable::handle_mousedown(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned)
{
    return DispatchEventOfSameName::Yes;
}

Paintable::DispatchEventOfSameName Paintable::handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned)
{
    return DispatchEventOfSameName::Yes;
}

Paintable::DispatchEventOfSameName Paintable::handle_mousemove(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned)
{
    return DispatchEventOfSameName::Yes;
}

bool Paintable::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, int, int)
{
    return false;
}

TraversalDecision Paintable::hit_test(CSSPixelPoint, HitTestType, Function<TraversalDecision(HitTestResult)> const&) const
{
    return TraversalDecision::Continue;
}

bool Paintable::has_stacking_context() const
{
    if (is_paintable_box())
        return static_cast<PaintableBox const&>(*this).stacking_context();
    return false;
}

StackingContext* Paintable::enclosing_stacking_context()
{
    for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        if (!ancestor->is_paintable_box())
            continue;
        if (auto* stacking_context = static_cast<PaintableBox&>(*ancestor).stacking_context())
            return const_cast<StackingContext*>(stacking_context);
    }
    // We should always reach the viewport's stacking context.
    VERIFY_NOT_REACHED();
}

void Paintable::set_needs_display(InvalidateDisplayList should_invalidate_display_list)
{
    auto& document = const_cast<DOM::Document&>(this->document());
    if (should_invalidate_display_list == InvalidateDisplayList::Yes)
        document.invalidate_display_list();

    auto* containing_block = this->containing_block();
    if (!containing_block)
        return;

    if (!is<Painting::PaintableWithLines>(*containing_block))
        return;
    static_cast<Painting::PaintableWithLines const&>(*containing_block).for_each_fragment([&](auto& fragment) {
        document.set_needs_display(fragment.absolute_rect(), InvalidateDisplayList::No);
        return IterationDecision::Continue;
    });
}

CSSPixelPoint Paintable::box_type_agnostic_position() const
{
    if (is_paintable_box())
        return static_cast<PaintableBox const*>(this)->absolute_position();

    VERIFY(is_inline());

    CSSPixelPoint position;
    if (auto const* block = containing_block(); block && is<Painting::PaintableWithLines>(*block)) {
        static_cast<Painting::PaintableWithLines const&>(*block).for_each_fragment([&](auto& fragment) {
            position = fragment.absolute_rect().location();
            return IterationDecision::Break;
        });
    }

    return position;
}

Gfx::AffineTransform Paintable::compute_combined_css_transform() const
{
    Gfx::AffineTransform combined_transform;
    if (is_paintable_box()) {
        auto const& paintable_box = static_cast<PaintableBox const&>(*this);
        auto affine_transform = Gfx::extract_2d_affine_transform(paintable_box.transform());
        combined_transform = combined_transform.multiply(affine_transform);
    }
    for (auto const* ancestor = this->containing_block(); ancestor; ancestor = ancestor->containing_block()) {
        auto affine_transform = Gfx::extract_2d_affine_transform(ancestor->transform());
        combined_transform = combined_transform.multiply(affine_transform);
    }
    return combined_transform;
}

Painting::BorderRadiiData normalize_border_radii_data(Layout::Node const& node, CSSPixelRect const& rect, CSS::BorderRadiusData const& top_left_radius, CSS::BorderRadiusData const& top_right_radius, CSS::BorderRadiusData const& bottom_right_radius, CSS::BorderRadiusData const& bottom_left_radius)
{
    Painting::BorderRadiiData radii_px {
        .top_left = {
            top_left_radius.horizontal_radius.to_px(node, rect.width()),
            top_left_radius.vertical_radius.to_px(node, rect.height()) },
        .top_right = { top_right_radius.horizontal_radius.to_px(node, rect.width()), top_right_radius.vertical_radius.to_px(node, rect.height()) },
        .bottom_right = { bottom_right_radius.horizontal_radius.to_px(node, rect.width()), bottom_right_radius.vertical_radius.to_px(node, rect.height()) },
        .bottom_left = { bottom_left_radius.horizontal_radius.to_px(node, rect.width()), bottom_left_radius.vertical_radius.to_px(node, rect.height()) }
    };

    // Scale overlapping curves according to https://www.w3.org/TR/css-backgrounds-3/#corner-overlap
    // Let f = min(Li/Si), where i ∈ {top, right, bottom, left},
    // Si is the sum of the two corresponding radii of the corners on side i,
    // and Ltop = Lbottom = the width of the box, and Lleft = Lright = the height of the box.
    //
    // NOTE: We iterate twice as a form of iterative refinement. A single scaling pass using
    // fixed-point arithmetic can result in small rounding errors, causing the scaled radii to
    // still slightly overflow the box dimensions. A second pass corrects this remaining error.
    for (int iteration = 0; iteration < 2; ++iteration) {
        auto s_top = radii_px.top_left.horizontal_radius + radii_px.top_right.horizontal_radius;
        auto s_right = radii_px.top_right.vertical_radius + radii_px.bottom_right.vertical_radius;
        auto s_bottom = radii_px.bottom_right.horizontal_radius + radii_px.bottom_left.horizontal_radius;
        auto s_left = radii_px.bottom_left.vertical_radius + radii_px.top_left.vertical_radius;

        CSSPixelFraction f = 1;
        if (s_top > rect.width())
            f = min(f, rect.width() / s_top);
        if (s_right > rect.height())
            f = min(f, rect.height() / s_right);
        if (s_bottom > rect.width())
            f = min(f, rect.width() / s_bottom);
        if (s_left > rect.height())
            f = min(f, rect.height() / s_left);

        // If f is 1 or more, the radii fit perfectly and no more scaling is needed
        if (f >= 1)
            break;

        Painting::BorderRadiusData* corners[] = {
            &radii_px.top_left, &radii_px.top_right, &radii_px.bottom_right, &radii_px.bottom_left
        };

        for (auto* corner : corners) {
            corner->horizontal_radius *= f;
            corner->vertical_radius *= f;
        }
    }

    return radii_px;
}

}
