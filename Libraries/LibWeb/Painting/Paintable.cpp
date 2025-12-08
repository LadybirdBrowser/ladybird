/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
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

String Paintable::debug_description() const
{
    return MUST(String::formatted("{}({})", class_name(), layout_node().debug_description()));
}

void Paintable::resolve_paint_properties()
{
    m_visible_for_hit_testing = true;
    if (auto dom_node = this->dom_node(); dom_node && dom_node->is_inert()) {
        // https://html.spec.whatwg.org/multipage/interaction.html#inert-subtrees
        // When a node is inert:
        // - Hit-testing must act as if the 'pointer-events' CSS property were set to 'none'.
        m_visible_for_hit_testing = false;
    }
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
    return m_containing_block.ensure([&] -> GC::Ptr<PaintableBox> {
        auto containing_layout_box = m_layout_node->containing_block();
        if (!containing_layout_box)
            return nullptr;
        return const_cast<PaintableBox*>(containing_layout_box->paintable_box());
    });
}

CSS::ImmutableComputedValues const& Paintable::computed_values() const
{
    return m_layout_node->computed_values();
}

bool Paintable::visible_for_hit_testing() const
{
    return m_visible_for_hit_testing && computed_values().pointer_events() != CSS::PointerEvents::None;
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

void Paintable::paint_inspector_overlay(DisplayListRecordingContext& context) const
{
    Vector<Painting::Paintable const*> self_and_ancestors {};
    for (Paintable const* paintable = this; paintable; paintable = paintable->parent()) {
        self_and_ancestors.append(paintable);
    }

    for (auto const* paintable : self_and_ancestors.in_reverse()) {
        if (auto const* box = as_if<PaintableBox>(paintable)) {
            box->apply_scroll_offset(context);
            if (box->stacking_context()) {
                auto to_device_pixels_scale = float(context.device_pixels_per_css_pixel());
                auto transform_matrix = box->transform();
                auto transform_origin = box->transform_origin().to_type<float>();
                Optional<Gfx::FloatMatrix4x4> parent_perspective_matrix;
                if (auto const* parent = as_if<PaintableBox>(box->parent()))
                    parent_perspective_matrix = parent->perspective_matrix();
                // We only want the transform here, everything else undesirable for the inspector overlay
                DisplayListRecorder::PushStackingContextParams push_stacking_context_params {
                    .opacity = 1.0,
                    .compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::Normal,
                    .isolate = false,
                    .transform = StackingContextTransform(transform_origin, transform_matrix, parent_perspective_matrix, to_device_pixels_scale),
                };
                context.display_list_recorder().push_stacking_context(push_stacking_context_params);
            }
        }
    }

    paint_inspector_overlay_internal(context);

    for (auto const* paintable : self_and_ancestors) {
        if (auto const* box = as_if<PaintableBox>(paintable)) {
            if (box->stacking_context()) {
                context.display_list_recorder().pop_stacking_context();
            }
            box->reset_scroll_offset(context);
        }
    }
}

void Paintable::set_needs_display(InvalidateDisplayList should_invalidate_display_list)
{
    auto& document = this->document();
    if (should_invalidate_display_list == InvalidateDisplayList::Yes)
        document.invalidate_display_list();

    auto* containing_block = this->containing_block();
    if (!containing_block)
        return;

    if (!is<PaintableWithLines>(*containing_block))
        return;
    for (auto const& fragment : as<PaintableWithLines>(*containing_block).fragments())
        document.set_needs_display(fragment.absolute_rect(), InvalidateDisplayList::No);
}

CSSPixelPoint Paintable::box_type_agnostic_position() const
{
    if (is_paintable_box())
        return static_cast<PaintableBox const*>(this)->absolute_position();

    VERIFY(is_inline());

    CSSPixelPoint position;
    if (auto const* block = containing_block(); block && is<Painting::PaintableWithLines>(*block)) {
        auto const& fragments = static_cast<Painting::PaintableWithLines const&>(*block).fragments();
        if (!fragments.is_empty()) {
            position = fragments[0].absolute_rect().location();
        }
    }

    return position;
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

void Paintable::set_needs_paint_only_properties_update(bool needs_update)
{
    if (needs_update == m_needs_paint_only_properties_update)
        return;

    m_needs_paint_only_properties_update = needs_update;

    if (needs_update) {
        document().set_needs_to_resolve_paint_only_properties();
    }
}

}
