/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/DevicePixelConverter.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/ScrollFrame.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(ViewportPaintable);

GC::Ref<ViewportPaintable> ViewportPaintable::create(Layout::Viewport const& layout_viewport)
{
    return layout_viewport.heap().allocate<ViewportPaintable>(layout_viewport);
}

ViewportPaintable::ViewportPaintable(Layout::Viewport const& layout_viewport)
    : PaintableWithLines(layout_viewport)
{
}

ViewportPaintable::~ViewportPaintable() = default;

void ViewportPaintable::reset_for_relayout()
{
    PaintableWithLines::reset_for_relayout();
    m_scroll_state.clear();
    m_scroll_state_snapshot = {};
    m_needs_to_refresh_scroll_state = true;
    m_paintable_boxes_with_auto_content_visibility.clear();
    m_next_accumulated_visual_context_id = 1;
}

void ViewportPaintable::build_stacking_context_tree_if_needed()
{
    if (stacking_context())
        return;
    build_stacking_context_tree();
}

void ViewportPaintable::build_stacking_context_tree()
{
    set_stacking_context(heap().allocate<StackingContext>(*this, nullptr, 0));

    size_t index_in_tree_order = 1;
    for_each_in_subtree_of_type<PaintableBox>([&](auto& paintable_box) {
        paintable_box.invalidate_stacking_context();
        auto* parent_context = paintable_box.enclosing_stacking_context();
        auto establishes_stacking_context = paintable_box.layout_node().establishes_stacking_context();
        if ((paintable_box.is_positioned() || establishes_stacking_context) && paintable_box.computed_values().z_index().value_or(0) == 0)
            parent_context->m_positioned_descendants_and_stacking_contexts_with_stack_level_0.append(paintable_box);
        if (!paintable_box.is_positioned() && paintable_box.is_floating())
            parent_context->m_non_positioned_floating_descendants.append(paintable_box);
        if (!establishes_stacking_context) {
            VERIFY(!paintable_box.stacking_context());
            return TraversalDecision::Continue;
        }
        VERIFY(parent_context);
        paintable_box.set_stacking_context(heap().allocate<StackingContext>(paintable_box, parent_context, index_in_tree_order++));
        return TraversalDecision::Continue;
    });

    stacking_context()->sort();
}

void ViewportPaintable::paint_all_phases(DisplayListRecordingContext& context)
{
    build_stacking_context_tree_if_needed();
    context.display_list_recorder().save_layer();
    stacking_context()->paint(context);
    context.display_list_recorder().restore();
}

void ViewportPaintable::assign_scroll_frames()
{
    auto precompute_sticky_constraints = [](ScrollFrame& sticky_frame, PaintableBox const& paintable_box) {
        auto nearest_scrolling_ancestor_frame = sticky_frame.nearest_scrolling_ancestor();
        if (!nearest_scrolling_ancestor_frame)
            return;

        auto const& scroll_ancestor_paintable = nearest_scrolling_ancestor_frame->paintable_box();
        auto sticky_border_box_rect = paintable_box.absolute_border_box_rect();
        auto const* containing_block_of_sticky = paintable_box.containing_block();

        CSSPixelRect containing_block_region;
        bool needs_parent_offset_adjustment = false;
        if (containing_block_of_sticky == &scroll_ancestor_paintable) {
            containing_block_region = { {}, containing_block_of_sticky->scrollable_overflow_rect()->size() };
        } else {
            containing_block_region = containing_block_of_sticky->absolute_border_box_rect()
                                          .translated(-scroll_ancestor_paintable.absolute_rect().top_left());
            needs_parent_offset_adjustment = true;
        }

        sticky_frame.set_sticky_constraints({
            .position_relative_to_scroll_ancestor = sticky_border_box_rect.top_left() - scroll_ancestor_paintable.absolute_rect().top_left(),
            .border_box_size = sticky_border_box_rect.size(),
            .scrollport_size = scroll_ancestor_paintable.absolute_rect().size(),
            .containing_block_region = containing_block_region,
            .needs_parent_offset_adjustment = needs_parent_offset_adjustment,
            .insets = paintable_box.sticky_insets(),
        });
    };

    for_each_in_inclusive_subtree_of_type<PaintableBox>([&](auto& paintable_box) {
        RefPtr<ScrollFrame> sticky_scroll_frame;
        if (paintable_box.is_sticky_position()) {
            auto parent_scroll_frame = paintable_box.nearest_scroll_frame();
            sticky_scroll_frame = m_scroll_state.create_sticky_frame_for(paintable_box, parent_scroll_frame);
            precompute_sticky_constraints(*sticky_scroll_frame, paintable_box);
            paintable_box.set_enclosing_scroll_frame(sticky_scroll_frame);
            paintable_box.set_own_scroll_frame(sticky_scroll_frame);
        }

        if (paintable_box.has_scrollable_overflow() || is<ViewportPaintable>(paintable_box)) {
            RefPtr<ScrollFrame const> parent_scroll_frame;
            if (sticky_scroll_frame) {
                parent_scroll_frame = sticky_scroll_frame;
            } else {
                parent_scroll_frame = paintable_box.nearest_scroll_frame();
            }
            auto scroll_frame = m_scroll_state.create_scroll_frame_for(paintable_box, parent_scroll_frame);
            paintable_box.set_own_scroll_frame(scroll_frame);
        }

        return TraversalDecision::Continue;
    });

    for_each_in_subtree([&](auto& paintable) {
        if (paintable.is_fixed_position() || paintable.is_sticky_position())
            return TraversalDecision::Continue;

        for (auto block = paintable.containing_block(); block; block = block->containing_block()) {
            if (auto scroll_frame = block->own_scroll_frame(); scroll_frame) {
                if (auto* paintable_box = as_if<PaintableBox>(paintable))
                    paintable_box->set_enclosing_scroll_frame(*scroll_frame);

                return TraversalDecision::Continue;
            }
            if (block->is_fixed_position()) {
                return TraversalDecision::Continue;
            }
        }
        VERIFY_NOT_REACHED();
    });
}

static CSSPixelRect effective_css_clip_rect(CSSPixelRect const& css_clip)
{
    if (css_clip.width() < 0 || css_clip.height() < 0)
        return CSSPixelRect { 0, 0, 0, 0 };
    return css_clip;
}

// Converts a CSS-pixel-space 4x4 matrix to device-pixel-space.
// - Translation column (column 3, rows 0-2) is scaled up by DPR
// - Perspective row (row 3, columns 0-2) is scaled down by DPR
// - All other elements are unaffected (the scale factors cancel out)
static FloatMatrix4x4 scale_matrix_for_device_pixels(FloatMatrix4x4 matrix, float scale)
{
    matrix[0, 3] *= scale;
    matrix[1, 3] *= scale;
    matrix[2, 3] *= scale;
    matrix[3, 0] /= scale;
    matrix[3, 1] /= scale;
    matrix[3, 2] /= scale;
    return matrix;
}

static Optional<TransformData> compute_transform(PaintableBox const& paintable_box, CSS::ComputedValues const& computed_values, double pixel_ratio)
{
    if (!paintable_box.has_css_transform())
        return {};

    auto matrix = Gfx::FloatMatrix4x4::identity();
    if (auto const& translate = computed_values.translate())
        matrix = matrix * translate->to_matrix(paintable_box).release_value();
    if (auto const& rotate = computed_values.rotate())
        matrix = matrix * rotate->to_matrix(paintable_box).release_value();
    if (auto const& scale = computed_values.scale())
        matrix = matrix * scale->to_matrix(paintable_box).release_value();
    for (auto const& transform : computed_values.transformations())
        matrix = matrix * transform->to_matrix(paintable_box).release_value();
    auto const& css_transform_origin = computed_values.transform_origin();
    auto reference_box = paintable_box.transform_reference_box();
    CSSPixelPoint origin {
        reference_box.left() + css_transform_origin.x.to_px(paintable_box.layout_node(), reference_box.width()),
        reference_box.top() + css_transform_origin.y.to_px(paintable_box.layout_node(), reference_box.height()),
    };
    auto scale = static_cast<float>(pixel_ratio);
    auto device_origin = origin.to_type<float>() * scale;
    return TransformData { scale_matrix_for_device_pixels(matrix, scale), device_origin };
}

// https://drafts.csswg.org/css-transforms-2/#perspective-matrix
static Optional<Gfx::FloatMatrix4x4> compute_perspective_matrix(PaintableBox const& paintable_box, CSS::ComputedValues const& computed_values)
{
    auto perspective = computed_values.perspective();
    if (!perspective.has_value())
        return {};

    // The perspective matrix is computed as follows:

    // 1. Start with the identity matrix.
    // 2. Translate by the computed X and Y values of 'perspective-origin'
    // https://drafts.csswg.org/css-transforms-2/#perspective-origin-property
    // Percentages: refer to the size of the reference box
    auto reference_box = paintable_box.transform_reference_box();
    auto perspective_origin = computed_values.perspective_origin().resolved(paintable_box.layout_node(), reference_box);
    auto computed_x = perspective_origin.x().to_float();
    auto computed_y = perspective_origin.y().to_float();
    auto perspective_matrix = Gfx::translation_matrix(Vector3<float>(computed_x, computed_y, 0));

    // 3. Multiply by the matrix that would be obtained from the 'perspective()' transform function, where the
    //    length is provided by the value of the perspective property
    // NB: Length values less than 1px being clamped to 1px is handled by the perspective() function already.
    // FIXME: Create the matrix directly.
    perspective_matrix = perspective_matrix * CSS::TransformationStyleValue::create(CSS::PropertyID::Transform, CSS::TransformFunction::Perspective, CSS::StyleValueVector { CSS::LengthStyleValue::create(CSS::Length::make_px(perspective.value())) })->to_matrix({}).release_value();

    // 4. Translate by the negated computed X and Y values of 'perspective-origin'
    perspective_matrix = perspective_matrix * Gfx::translation_matrix(Vector3<float>(-computed_x, -computed_y, 0));
    return perspective_matrix;
}

static Optional<ClipData> compute_clip_data(PaintableBox const& paintable_box, CSS::ComputedValues const& computed_values, DevicePixelConverter const& converter)
{
    auto overflow_x = computed_values.overflow_x();
    auto overflow_y = computed_values.overflow_y();

    // https://drafts.csswg.org/css-contain-2/#paint-containment
    // 1. The contents of the element including any ink or scrollable overflow must be clipped to the overflow clip
    //    edge of the paint containment box, taking corner clipping into account. This does not include the creation of
    //    any mechanism to access or indicate the presence of the clipped content; nor does it inhibit the creation of
    //    any such mechanism through other properties, such as overflow, resize, or text-overflow.
    //    NOTE: This clipping shape respects overflow-clip-margin, allowing an element with paint containment
    //          to still slightly overflow its normal bounds.
    if (paintable_box.layout_node().has_paint_containment()) {
        // NOTE: Note: The behavior is described in this paragraph is equivalent to changing 'overflow-x: visible' into
        //       'overflow-x: clip' and 'overflow-y: visible' into 'overflow-y: clip' at used value time, while leaving other
        //       values of 'overflow-x' and 'overflow-y' unchanged.
        overflow_x = CSS::Overflow::Clip;
        overflow_y = CSS::Overflow::Clip;
    }

    auto has_hidden_overflow = overflow_x != CSS::Overflow::Visible || overflow_y != CSS::Overflow::Visible;

    if (has_hidden_overflow && paintable_box.overflow_property_applies()) {
        auto clip_rect = paintable_box.absolute_padding_box_rect();

        // https://drafts.csswg.org/css-overflow-3/#propdef-overflow
        // 'clip'
        //    This value indicates that the box’s content is clipped to its overflow clip edge
        auto overflow_clip_edge = paintable_box.overflow_clip_edge_rect();
        if (overflow_x == CSS::Overflow::Visible) {
            clip_rect.set_left(0);
            clip_rect.set_right(CSSPixels::max_integer_value);
        } else if (overflow_x == CSS::Overflow::Clip) {
            clip_rect.set_left(overflow_clip_edge.left());
            clip_rect.set_right(overflow_clip_edge.right());
        }
        if (overflow_y == CSS::Overflow::Visible) {
            clip_rect.set_top(0);
            clip_rect.set_bottom(CSSPixels::max_integer_value);
        } else if (overflow_y == CSS::Overflow::Clip) {
            clip_rect.set_top(overflow_clip_edge.top());
            clip_rect.set_bottom(overflow_clip_edge.bottom());
        }

        // https://drafts.csswg.org/css-overflow-3/#corner-clipping
        // As mentioned in CSS Backgrounds 3 § 4.3 Corner Clipping, the clipping region established by 'overflow' can be
        // rounded:
        // - When 'overflow-x' and 'overflow-y' compute to 'hidden', 'scroll', or 'auto', the clipping region is rounded
        //   based on the border radius, adjusted to the padding edge, as described in CSS Backgrounds 3 § 4.2 Corner
        //   Shaping.
        // - When both 'overflow-x' and 'overflow-y' compute to 'clip', the clipping region is rounded as described in § 3.2
        //   Expanding Clipping Bounds: the 'overflow-clip-margin' property.
        // - However, when one of 'overflow-x' or 'overflow-y' computes to 'clip' and the other computes to 'visible', the
        //   clipping region is not rounded.
        // FIXME: Adjust the border radii for the overflow-clip-margin case. (see https://drafts.csswg.org/css-overflow-4/#valdef-overflow-clip-margin-length-0 )
        auto radii = (overflow_x != CSS::Overflow::Visible && overflow_y != CSS::Overflow::Visible) ? paintable_box.normalized_border_radii_data(PaintableBox::ShrinkRadiiForBorders::Yes) : BorderRadiiData {};
        return ClipData { converter.rounded_device_rect(clip_rect), radii.as_corners(converter) };
    }

    return {};
}

void ViewportPaintable::assign_accumulated_visual_contexts()
{
    m_next_accumulated_visual_context_id = 1;

    auto pixel_ratio = document().page().client().device_pixels_per_css_pixel();
    DevicePixelConverter converter { pixel_ratio };
    auto scale = static_cast<float>(pixel_ratio);

    auto append_node = [&](RefPtr<AccumulatedVisualContext const> parent, VisualContextData data) {
        return AccumulatedVisualContext::create(allocate_accumulated_visual_context_id(), move(data), parent);
    };

    auto make_effects_data = [&](PaintableBox const& box) -> Optional<EffectsData> {
        auto const& computed_values = box.computed_values();
        auto gfx_filter = to_gfx_filter(box.filter(), pixel_ratio);
        EffectsData effects {
            computed_values.opacity(),
            mix_blend_mode_to_compositing_and_blending_operator(computed_values.mix_blend_mode()),
            move(gfx_filter)
        };
        if (!effects.needs_layer())
            return {};
        return effects;
    };

    // Create visual viewport transform as root (if not identity)
    m_visual_viewport_context = nullptr;
    auto transform = document().visual_viewport()->transform();
    if (!transform.is_identity()) {
        auto matrix = scale_matrix_for_device_pixels(transform.to_matrix(), scale);
        m_visual_viewport_context = append_node(nullptr, TransformData { matrix, { 0.f, 0.f } });
    }

    RefPtr<AccumulatedVisualContext const> viewport_state_for_descendants = m_visual_viewport_context;
    if (own_scroll_frame())
        viewport_state_for_descendants = append_node(m_visual_viewport_context, ScrollData { own_scroll_frame()->id(), false });
    set_accumulated_visual_context(nullptr);
    set_accumulated_visual_context_for_descendants(viewport_state_for_descendants);

    for_each_in_subtree_of_type<PaintableBox>([&](auto& paintable_box) {
        auto* visual_parent = as_if<PaintableBox>(paintable_box.parent());
        if (!visual_parent)
            return TraversalDecision::Continue;

        RefPtr<AccumulatedVisualContext const> inherited_state;

        if (paintable_box.is_fixed_position()) {
            inherited_state = m_visual_viewport_context;
        } else if (paintable_box.is_absolutely_positioned()) {
            // For position: absolute, use containing block's state to correctly escape scroll containers.
            auto* containing = paintable_box.containing_block();
            inherited_state = containing->accumulated_visual_context_for_descendants();

            // Abspos elements escape scroll containers and overflow clips of non-positioned
            // ancestors, but cannot escape stacking contexts created by intermediate effects
            // (opacity, mix-blend-mode, isolation). Walk from visual parent to containing
            // block and collect these intermediate effects.
            // NOTE: transforms/perspectives/filters establish containing blocks for abspos,
            //       so they cannot appear as intermediates.
            Vector<VisualContextData, 4> intermediate_effects;
            for (Paintable* paintable = visual_parent; paintable && paintable != containing; paintable = paintable->parent()) {
                auto* ancestor_box = as_if<PaintableBox>(paintable);
                if (!ancestor_box)
                    continue;
                if (auto effects = make_effects_data(*ancestor_box); effects.has_value())
                    intermediate_effects.append(effects.release_value());
            }
            for (auto& effects : intermediate_effects.in_reverse())
                inherited_state = append_node(inherited_state, move(effects));
        } else {
            // For position: relative/static, use visual parent's state directly.
            // This avoids duplicate transform/perspective allocations that would occur with
            // the containing block + intermediate walk approach.
            inherited_state = visual_parent->accumulated_visual_context_for_descendants();
        }

        // Build this element's own state from inherited state.
        RefPtr<AccumulatedVisualContext const> own_state = inherited_state;

        if (paintable_box.is_sticky_position()) {
            // For sticky elements, use enclosing_scroll_frame which holds the sticky frame.
            // own_scroll_frame may be a different scroll frame if the sticky element also has scrollable overflow.
            if (auto sticky_frame = paintable_box.enclosing_scroll_frame(); sticky_frame && sticky_frame->is_sticky())
                own_state = append_node(own_state, ScrollData { sticky_frame->id(), true });
        }

        auto const& computed_values = paintable_box.computed_values();

        if (auto effects = make_effects_data(paintable_box); effects.has_value())
            own_state = append_node(own_state, effects.release_value());

        if (auto transform_data = compute_transform(paintable_box, computed_values, pixel_ratio); transform_data.has_value()) {
            paintable_box.set_has_non_invertible_css_transform(!transform_data->matrix.is_invertible());
            own_state = append_node(own_state, *transform_data);
        } else {
            paintable_box.set_has_non_invertible_css_transform(false);
        }

        if (auto css_clip = paintable_box.get_clip_rect(); css_clip.has_value()) {
            auto effective_rect = effective_css_clip_rect(*css_clip);
            own_state = append_node(own_state, ClipData { converter.rounded_device_rect(effective_rect), {} });
        }

        // FIXME: Support other geometry boxes. See: https://drafts.fxtf.org/css-masking/#typedef-geometry-box
        if (auto const& clip_path = computed_values.clip_path(); clip_path.has_value() && clip_path->is_basic_shape()) {
            auto masking_area = paintable_box.absolute_border_box_rect();
            auto reference_box = CSSPixelRect { {}, masking_area.size() };
            auto const& basic_shape = clip_path->basic_shape();
            auto path = basic_shape.to_path(reference_box, paintable_box.layout_node());
            path.offset(masking_area.top_left().template to_type<float>());
            auto fill_rule = basic_shape.basic_shape().visit(
                [](CSS::Polygon const& polygon) { return polygon.fill_rule; },
                [](CSS::Path const& path) { return path.fill_rule; },
                [](auto const&) { return Gfx::WindingRule::Nonzero; });
            auto device_path = path.copy_transformed(Gfx::AffineTransform {}.set_scale(scale, scale));
            auto device_bounding_rect = converter.rounded_device_rect(masking_area);
            own_state = append_node(own_state, ClipPathData { move(device_path), device_bounding_rect, fill_rule });
        }

        paintable_box.set_accumulated_visual_context(own_state);

        // Build state for descendants: own state + perspective + clip + scroll.
        RefPtr<AccumulatedVisualContext const> state_for_descendants = own_state;

        if (auto perspective_matrix = compute_perspective_matrix(paintable_box, computed_values); perspective_matrix.has_value()) {
            auto scaled_matrix = scale_matrix_for_device_pixels(*perspective_matrix, scale);
            state_for_descendants = append_node(state_for_descendants, PerspectiveData { scaled_matrix });
        }

        if (auto clip_data = compute_clip_data(paintable_box, computed_values, converter); clip_data.has_value())
            state_for_descendants = append_node(state_for_descendants, clip_data.value());

        if (paintable_box.own_scroll_frame()) {
            auto is_sticky_without_scrollable_overflow = paintable_box.is_sticky_position() && paintable_box.enclosing_scroll_frame() == paintable_box.own_scroll_frame();
            if (!is_sticky_without_scrollable_overflow)
                state_for_descendants = append_node(state_for_descendants, ScrollData { paintable_box.own_scroll_frame()->id(), false });
        }

        paintable_box.set_accumulated_visual_context_for_descendants(state_for_descendants);

        return TraversalDecision::Continue;
    });
}

void ViewportPaintable::refresh_scroll_state()
{
    if (!m_needs_to_refresh_scroll_state)
        return;
    m_needs_to_refresh_scroll_state = false;

    m_scroll_state.for_each_sticky_frame([&](auto& scroll_frame) {
        auto nearest_scrolling_ancestor_frame = scroll_frame->nearest_scrolling_ancestor();
        if (!nearest_scrolling_ancestor_frame || !scroll_frame->has_sticky_constraints())
            return;

        auto const& sticky_data = scroll_frame->sticky_constraints();
        auto const& sticky_insets = sticky_data.insets;
        auto const& scroll_ancestor_paintable = nearest_scrolling_ancestor_frame->paintable_box();

        // For nested sticky elements, the parent sticky's offset is applied via cumulative_offset.
        // We need to adjust all position calculations to account for this, so we work in the
        // coordinate space where the parent sticky is at its current (offset) position.
        CSSPixelPoint parent_sticky_offset;
        if (auto parent = scroll_frame->parent(); parent && parent->is_sticky())
            parent_sticky_offset = parent->cumulative_offset();

        auto sticky_position_in_ancestor = sticky_data.position_relative_to_scroll_ancestor + parent_sticky_offset;

        auto containing_block_region = sticky_data.containing_block_region;
        if (sticky_data.needs_parent_offset_adjustment)
            containing_block_region.translate_by(parent_sticky_offset);
        CSSPixelPoint min_offset_within_containing_block = containing_block_region.top_left();
        CSSPixelPoint max_offset_within_containing_block = {
            containing_block_region.right() - sticky_data.border_box_size.width(),
            containing_block_region.bottom() - sticky_data.border_box_size.height()
        };

        CSSPixelRect scrollport_rect { scroll_ancestor_paintable.scroll_offset(), sticky_data.scrollport_size };
        CSSPixelPoint sticky_offset;

        if (sticky_insets.top.has_value()) {
            if (scrollport_rect.top() > sticky_position_in_ancestor.y() - *sticky_insets.top)
                sticky_offset.set_y(min(scrollport_rect.top() + *sticky_insets.top, max_offset_within_containing_block.y()) - sticky_position_in_ancestor.y());
        }
        if (sticky_insets.left.has_value()) {
            if (scrollport_rect.left() > sticky_position_in_ancestor.x() - *sticky_insets.left)
                sticky_offset.set_x(min(scrollport_rect.left() + *sticky_insets.left, max_offset_within_containing_block.x()) - sticky_position_in_ancestor.x());
        }
        if (sticky_insets.bottom.has_value()) {
            if (scrollport_rect.bottom() < sticky_position_in_ancestor.y() + sticky_data.border_box_size.height() + *sticky_insets.bottom)
                sticky_offset.set_y(max(scrollport_rect.bottom() - sticky_data.border_box_size.height() - *sticky_insets.bottom, min_offset_within_containing_block.y()) - sticky_position_in_ancestor.y());
        }
        if (sticky_insets.right.has_value()) {
            if (scrollport_rect.right() < sticky_position_in_ancestor.x() + sticky_data.border_box_size.width() + *sticky_insets.right)
                sticky_offset.set_x(max(scrollport_rect.right() - sticky_data.border_box_size.width() - *sticky_insets.right, min_offset_within_containing_block.x()) - sticky_position_in_ancestor.x());
        }

        scroll_frame->set_own_offset(sticky_offset);
    });

    m_scroll_state.for_each_scroll_frame([&](auto& scroll_frame) {
        scroll_frame->set_own_offset(-scroll_frame->paintable_box().scroll_offset());
    });

    m_scroll_state_snapshot = m_scroll_state.snapshot(document().page().client().device_pixels_per_css_pixel());
}

static void resolve_paint_only_properties_in_subtree(Paintable& root)
{
    root.for_each_in_inclusive_subtree([&](auto& paintable) {
        paintable.resolve_paint_properties();
        paintable.set_needs_paint_only_properties_update(false);
        return TraversalDecision::Continue;
    });
}

void ViewportPaintable::resolve_paint_only_properties()
{
    // Resolves layout-dependent properties not handled during layout and stores them in the paint tree.
    // Properties resolved include:
    // - Border radii
    // - Box shadows
    // - Text shadows
    // - Transforms
    // - Transform origins
    // - Outlines
    for_each_in_inclusive_subtree([&](Paintable& paintable) {
        if (paintable.needs_paint_only_properties_update()) {
            resolve_paint_only_properties_in_subtree(paintable);
            return TraversalDecision::SkipChildrenAndContinue;
        }
        return TraversalDecision::Continue;
    });
}

GC::Ptr<Selection::Selection> ViewportPaintable::selection() const
{
    return document().get_selection();
}

void ViewportPaintable::recompute_selection_states(DOM::Range& range)
{
    // 1. Start by resetting the selection state of all layout nodes to None.
    for_each_in_inclusive_subtree([&](auto& layout_node) {
        layout_node.set_selection_state(SelectionState::None);
        return TraversalDecision::Continue;
    });

    auto start_container = range.start_container();
    auto end_container = range.end_container();

    // 2. If the selection starts and ends in the same node:
    if (start_container == end_container) {
        // 1. If the selection starts and ends at the same offset, return.
        if (range.start_offset() == range.end_offset()) {
            // NOTE: A zero-length selection should not be visible.
            return;
        }

        // 2. If it's a text node, mark it as StartAndEnd and return.
        if (is<DOM::Text>(*start_container) && !range.start().node->is_inert()) {
            if (auto* paintable = start_container->paintable())
                paintable->set_selection_state(SelectionState::StartAndEnd);
            return;
        }
    }

    // 3. Mark the selection start node as Start (if text) or Full (if anything else).
    if (auto* paintable = start_container->paintable(); paintable && !range.start().node->is_inert()) {
        if (is<DOM::Text>(*start_container))
            paintable->set_selection_state(SelectionState::Start);
        else
            paintable->set_selection_state(SelectionState::Full);
    }

    // 4. Mark the nodes between the start and end of the selection as Full.
    auto* start_at = start_container->child_at_index(range.start_offset());
    // If the start container has no child at that index, we need to start on the node right after the start container.
    if (!start_at) {
        if (auto* last_child = start_container->last_child()) {
            start_at = last_child->next_in_pre_order();
        } else {
            start_at = start_container->next_in_pre_order();
        }
    }

    DOM::Node* stop_at = end_container->child_at_index(range.end_offset());
    // Only stop at the end container if it has no children that may need to be included.
    for (auto* node = start_at; node && (node != stop_at && !(node == end_container && !end_container->has_children())); node = node->next_in_pre_order(end_container)) {
        if (node->is_inert())
            continue;
        if (auto* paintable = node->paintable())
            paintable->set_selection_state(SelectionState::Full);
    }

    // 5. Mark the selection end node as End if it is a text node.
    if (auto* paintable = end_container->paintable(); paintable && !range.end().node->is_inert() && is<DOM::Text>(*end_container)) {
        paintable->set_selection_state(SelectionState::End);
    }
}

bool ViewportPaintable::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, int, int)
{
    return false;
}

void ViewportPaintable::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_paintable_boxes_with_auto_content_visibility);
}

}
