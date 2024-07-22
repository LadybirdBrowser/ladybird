/*
 * Copyright (c) 2022-2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/StackingContext.h>

namespace Web::Painting {

Paintable::Paintable(Layout::Node const& layout_node)
    : m_layout_node(layout_node)
    , m_browsing_context(const_cast<HTML::BrowsingContext&>(layout_node.browsing_context()))
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
    visitor.visit(m_browsing_context);
    if (m_containing_block.has_value())
        visitor.visit(m_containing_block.value());
}

bool Paintable::is_visible() const
{
    auto const& computed_values = this->computed_values();
    return computed_values.visibility() == CSS::Visibility::Visible && computed_values.opacity() != 0;
}

void Paintable::set_dom_node(JS::GCPtr<DOM::Node> dom_node)
{
    m_dom_node = dom_node;
}

JS::GCPtr<DOM::Node> Paintable::dom_node()
{
    return m_dom_node;
}

JS::GCPtr<DOM::Node const> Paintable::dom_node() const
{
    return m_dom_node;
}

HTML::BrowsingContext const& Paintable::browsing_context() const
{
    return m_browsing_context;
}

HTML::BrowsingContext& Paintable::browsing_context()
{
    return m_browsing_context;
}

JS::GCPtr<HTML::Navigable> Paintable::navigable() const
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

StackingContext* Paintable::enclosing_stacking_context()
{
    for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        if (auto* stacking_context = ancestor->stacking_context())
            return const_cast<StackingContext*>(stacking_context);
    }
    // We should always reach the viewport's stacking context.
    VERIFY_NOT_REACHED();
}

void Paintable::set_stacking_context(NonnullOwnPtr<StackingContext> stacking_context)
{
    m_stacking_context = move(stacking_context);
}

void Paintable::invalidate_stacking_context()
{
    m_stacking_context = nullptr;
}

void Paintable::set_needs_display() const
{
    auto* containing_block = this->containing_block();
    if (!containing_block)
        return;
    auto navigable = this->navigable();
    if (!navigable)
        return;

    if (is<Painting::InlinePaintable>(*this)) {
        auto const& fragments = static_cast<Painting::InlinePaintable const*>(this)->fragments();
        for (auto const& fragment : fragments)
            navigable->set_needs_display(fragment.absolute_rect());
    }

    if (!is<Painting::PaintableWithLines>(*containing_block))
        return;
    static_cast<Painting::PaintableWithLines const&>(*containing_block).for_each_fragment([&](auto& fragment) {
        navigable->set_needs_display(fragment.absolute_rect());
        return IterationDecision::Continue;
    });
}

CSSPixelPoint Paintable::box_type_agnostic_position() const
{
    if (is_paintable_box())
        return static_cast<PaintableBox const*>(this)->absolute_position();

    VERIFY(is_inline());
    if (is_inline_paintable()) {
        auto const& inline_paintable = static_cast<Painting::InlinePaintable const&>(*this);
        if (!inline_paintable.fragments().is_empty())
            return inline_paintable.fragments().first().absolute_rect().location();
        return inline_paintable.bounding_rect().location();
    }

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
    Painting::BorderRadiusData bottom_left_radius_px {};
    Painting::BorderRadiusData bottom_right_radius_px {};
    Painting::BorderRadiusData top_left_radius_px {};
    Painting::BorderRadiusData top_right_radius_px {};

    bottom_left_radius_px.horizontal_radius = bottom_left_radius.horizontal_radius.to_px(node, rect.width());
    bottom_right_radius_px.horizontal_radius = bottom_right_radius.horizontal_radius.to_px(node, rect.width());
    top_left_radius_px.horizontal_radius = top_left_radius.horizontal_radius.to_px(node, rect.width());
    top_right_radius_px.horizontal_radius = top_right_radius.horizontal_radius.to_px(node, rect.width());

    bottom_left_radius_px.vertical_radius = bottom_left_radius.vertical_radius.to_px(node, rect.height());
    bottom_right_radius_px.vertical_radius = bottom_right_radius.vertical_radius.to_px(node, rect.height());
    top_left_radius_px.vertical_radius = top_left_radius.vertical_radius.to_px(node, rect.height());
    top_right_radius_px.vertical_radius = top_right_radius.vertical_radius.to_px(node, rect.height());

    // Scale overlapping curves according to https://www.w3.org/TR/css-backgrounds-3/#corner-overlap
    // Let f = min(Li/Si), where i ∈ {top, right, bottom, left},
    // Si is the sum of the two corresponding radii of the corners on side i,
    // and Ltop = Lbottom = the width of the box, and Lleft = Lright = the height of the box.
    auto l_top = rect.width();
    auto l_bottom = l_top;
    auto l_left = rect.height();
    auto l_right = l_left;
    auto s_top = (top_left_radius_px.horizontal_radius + top_right_radius_px.horizontal_radius);
    auto s_right = (top_right_radius_px.vertical_radius + bottom_right_radius_px.vertical_radius);
    auto s_bottom = (bottom_left_radius_px.horizontal_radius + bottom_right_radius_px.horizontal_radius);
    auto s_left = (top_left_radius_px.vertical_radius + bottom_left_radius_px.vertical_radius);
    CSSPixelFraction f = 1;
    f = (s_top != 0) ? min(f, l_top / s_top) : f;
    f = (s_right != 0) ? min(f, l_right / s_right) : f;
    f = (s_bottom != 0) ? min(f, l_bottom / s_bottom) : f;
    f = (s_left != 0) ? min(f, l_left / s_left) : f;

    // If f < 1, then all corner radii are reduced by multiplying them by f.
    if (f < 1) {
        top_left_radius_px.horizontal_radius *= f;
        top_left_radius_px.vertical_radius *= f;
        top_right_radius_px.horizontal_radius *= f;
        top_right_radius_px.vertical_radius *= f;
        bottom_right_radius_px.horizontal_radius *= f;
        bottom_right_radius_px.vertical_radius *= f;
        bottom_left_radius_px.horizontal_radius *= f;
        bottom_left_radius_px.vertical_radius *= f;
    }

    return Painting::BorderRadiiData { top_left_radius_px, top_right_radius_px, bottom_right_radius_px, bottom_left_radius_px };
}

}
