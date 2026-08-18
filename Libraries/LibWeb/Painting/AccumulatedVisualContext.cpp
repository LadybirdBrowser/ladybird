/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/HashTable.h>
#include <AK/StringBuilder.h>
#include <LibGfx/Matrix4x4.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/DevicePixelConverter.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/SVGForeignObjectPaintable.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

AccumulatedVisualContextTree build_accumulated_visual_context_tree(ViewportPaintable&);
bool update_accumulated_visual_context_values(ViewportPaintable&, Paintable&);
void update_visual_viewport_accumulated_visual_context(ViewportPaintable&);

bool ClipData::contains(DevicePixelPoint point) const
{
    return corner_radii.contains(point.to_type<int>(), rect.to_type<int>());
}

static Atomic<u64> s_next_accumulated_visual_context_tree_version { 1 };

static ScrollStateSlot common_ancestor_slot_along_scroll_parent_chain(ScrollState const& scroll_state, ScrollStateSlot a_slot, ScrollStateSlot b_slot)
{
    Vector<ScrollStateSlot, 8> a_slot_and_ancestors;
    for (auto slot = a_slot;; slot = scroll_state.state_at_slot(slot).parent_slot()) {
        a_slot_and_ancestors.append(slot);
        if (slot == NO_SCROLL_STATE_SLOT)
            break;
    }
    for (auto slot = b_slot;; slot = scroll_state.state_at_slot(slot).parent_slot()) {
        if (a_slot_and_ancestors.contains_slow(slot))
            return slot;
        if (slot == NO_SCROLL_STATE_SLOT)
            break;
    }
    return NO_SCROLL_STATE_SLOT;
}

static TransformData identity_visual_viewport_transform()
{
    return { Gfx::FloatMatrix4x4::identity(), { 0.f, 0.f } };
}

// Whole-tree transform root: the visual viewport transform for document trees, the content
// placement for nested display list trees, identity otherwise.
static Vector<AccumulatedVisualContextNode> root_only_nodes(TransformData root_transform)
{
    Vector<AccumulatedVisualContextNode> nodes;
    nodes.append({ move(root_transform), {}, 0, false });
    return nodes;
}

static u64 next_accumulated_visual_context_tree_version()
{
    return s_next_accumulated_visual_context_tree_version.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create()
{
    return create(identity_visual_viewport_transform());
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create(TransformData visual_viewport_transform)
{
    return AccumulatedVisualContextTree { next_accumulated_visual_context_tree_version(), root_only_nodes(move(visual_viewport_transform)), true };
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create_with_content_root(TransformData content_transform)
{
    return AccumulatedVisualContextTree { next_accumulated_visual_context_tree_version(), root_only_nodes(move(content_transform)), false };
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create_with_content_offset(Gfx::IntPoint content_offset)
{
    return create_with_content_root(TransformData {
        Gfx::translation_matrix(Vector3<float>(static_cast<float>(content_offset.x()), static_cast<float>(content_offset.y()), 0)),
        {},
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

static TransformData visual_viewport_transform_data(DOM::Document& document)
{
    auto scale = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto matrix = scale_matrix_for_device_pixels(document.visual_viewport()->transform().to_matrix(), scale);
    return TransformData { matrix, { 0.f, 0.f } };
}

// Content below a viewport node records in the viewport's user units scaled by the device pixel
// ratio, mirroring how ordinary content records in CSS pixels scaled by it; the node folds the
// viewport box's position in its own recorded space together with the viewBox transform.
static TransformData compute_svg_viewport_transform_data(Paintable const& viewport_box, Gfx::AffineTransform const& viewbox_transform, double pixel_ratio)
{
    auto matrix = Gfx::AffineTransform {}
                      .translate(viewport_box.absolute_rect().location().to_type<float>())
                      .multiply(viewbox_transform);
    return TransformData {
        scale_matrix_for_device_pixels(matrix.to_matrix(), static_cast<float>(pixel_ratio)),
        { 0.f, 0.f },
        false,
        TransformDataRole::SvgViewportTransform,
    };
}

static bool style_has_transform(Layout::NodeWithStyle const& style_source)
{
    return style_source.has_resolved_transforms();
}

static Gfx::AffineTransform svg_additional_element_transform(Layout::NodeWithStyle const& style_source)
{
    if (auto const* graphics_element = as_if<SVG::SVGGraphicsElement>(style_source.dom_node()))
        return graphics_element->additional_element_transform();
    return {};
}

// https://drafts.csswg.org/css-transforms-2/#ctm
Optional<TransformData> compute_transform(Paintable const& paintable_box, double pixel_ratio)
{
    auto const& style_source = paintable_box.layout_node();
    auto additional_element_transform = svg_additional_element_transform(style_source);
    if ((!style_has_transform(style_source) && additional_element_transform.is_identity()) || !style_source.is_transformable())
        return {};

    // The transformation matrix is computed from the transform, transform-origin, translate, rotate, scale, and
    // offset properties as follows:
    auto reference_box = paintable_box.transform_reference_box();
    auto const& css_transform_origin = style_source.transform_origin();
    auto origin_x = css_transform_origin.x.to_px(reference_box.width());
    auto origin_y = css_transform_origin.y.to_px(reference_box.height());
    auto origin_z = css_transform_origin.z.to_px(0).to_float();

    // 1. Start with the identity matrix.
    // 2. Translate by the computed X, Y, and Z values of transform-origin.
    auto matrix = Gfx::translation_matrix(Vector3 { 0.f, 0.f, origin_z });

    // 3. Translate by the computed X, Y, and Z values of translate.
    // 4. Rotate by the computed <angle> about the specified axis of rotate.
    // 5. Scale by the computed X, Y, and Z values of scale.
    // FIXME: 6. Translate and rotate by the transform specified by offset.
    // 7. Multiply by each of the transform functions in transform from left to right.
    // NB: The resolved transform list carries translate, rotate, scale, and the
    //     transform functions pre-lowered in exactly that order.
    style_source.for_each_resolved_transform([&](auto const& transform) {
        matrix = matrix * transform.to_matrix(reference_box.width(), reference_box.height());
    });

    // The x and y properties of <use> define an additional translation applied after any
    // transformations specified with other properties.
    if (!additional_element_transform.is_identity())
        matrix = matrix * additional_element_transform.to_matrix();

    // 8. Translate by the negated computed X, Y and Z values of transform-origin.
    matrix = matrix * Gfx::translation_matrix(Vector3 { 0.f, 0.f, -origin_z });

    auto origin = reference_box.location() + CSSPixelPoint { origin_x, origin_y };
    auto scale = static_cast<float>(pixel_ratio);
    auto device_origin = origin.to_type<float>() * scale;
    return TransformData { scale_matrix_for_device_pixels(matrix, scale), device_origin };
}

// https://drafts.csswg.org/css-transforms-2/#perspective-matrix
static Optional<Gfx::FloatMatrix4x4> compute_perspective_matrix(Paintable const& paintable_box)
{
    auto const& style_source = paintable_box.layout_node();
    auto perspective = style_source.perspective();
    if (!perspective.has_value() || !paintable_box.layout_node().is_transformable())
        return {};

    // The perspective matrix is computed as follows:

    // 1. Start with the identity matrix.
    // 2. Translate by the computed X and Y values of 'perspective-origin'
    // https://drafts.csswg.org/css-transforms-2/#perspective-origin-property
    // Percentages: refer to the size of the reference box
    auto reference_box = paintable_box.transform_reference_box();
    auto perspective_origin = style_source.perspective_origin().resolved(reference_box);
    auto computed_x = perspective_origin.x().to_float();
    auto computed_y = perspective_origin.y().to_float();
    auto perspective_matrix = Gfx::translation_matrix(Vector3<float>(computed_x, computed_y, 0));

    // 3. Multiply by the matrix that would be obtained from the 'perspective()' transform function, where the
    //    length is provided by the value of the perspective property
    // https://drafts.csswg.org/css-transforms-2/#funcdef-perspective
    // If the depth value is less than '1px', it must be treated as '1px' for the purpose of rendering, [..]
    auto distance = max(perspective->to_float(), 1.f);
    perspective_matrix = perspective_matrix * Gfx::perspective_matrix(distance);

    // 4. Translate by the negated computed X and Y values of 'perspective-origin'
    return perspective_matrix * Gfx::translation_matrix(Vector3 { -computed_x, -computed_y, 0.f });
}

static Optional<ClipData> compute_clip_data(Paintable const& paintable_box, DevicePixelConverter const& converter)
{
    auto const& style_source = paintable_box.layout_node();
    auto overflow_x = style_source.overflow_x();
    auto overflow_y = style_source.overflow_y();

    // https://drafts.csswg.org/css-contain-2/#paint-containment
    // 1. The contents of the element including any ink or scrollable overflow must be clipped to the overflow clip
    //    edge of the paint containment box, taking corner clipping into account. This does not include the creation of
    //    any mechanism to access or indicate the presence of the clipped content; nor does it inhibit the creation of
    //    any such mechanism through other properties, such as overflow, resize, or text-overflow.
    //    NOTE: This clipping shape respects overflow-clip-margin, allowing an element with paint containment
    //          to still slightly overflow its normal bounds.
    auto has_paint_containment = style_source.contain().paint_containment
        || style_source.content_visibility() == CSS::ContentVisibility::Auto;
    if (has_paint_containment && paintable_box.layout_node().has_paint_containment()) {
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
        auto radii = (overflow_x != CSS::Overflow::Visible && overflow_y != CSS::Overflow::Visible) ? paintable_box.normalized_border_radii_data(Paintable::ShrinkRadiiForBorders::Yes) : BorderRadiiData {};
        return ClipData { converter.rounded_device_rect(clip_rect), radii.as_corners(converter) };
    }

    return {};
}

static Optional<ClipData> compute_css_clip_data(Paintable const& paintable_box, DevicePixelConverter const& converter)
{
    if (!paintable_box.layout_node().clip().is_rect())
        return {};
    if (auto css_clip = paintable_box.get_clip_rect(); css_clip.has_value()) {
        auto effective_rect = effective_css_clip_rect(*css_clip);
        return ClipData { converter.rounded_device_rect(effective_rect), {} };
    }
    return {};
}

static Optional<ClipPathData> compute_basic_shape_clip_path_data(Paintable const& paintable_box, DevicePixelConverter const& converter, float scale)
{
    // FIXME: Support other geometry boxes. See: https://drafts.fxtf.org/css-masking/#typedef-geometry-box
    auto const& clip_path = paintable_box.layout_node().clip_path();
    if (!clip_path.has_value() || !clip_path->is_basic_shape())
        return {};

    auto masking_area = paintable_box.absolute_border_box_rect();
    auto reference_box = CSSPixelRect { {}, masking_area.size() };
    auto const& basic_shape = clip_path->basic_shape();
    auto path = basic_shape.to_path(reference_box);
    path.offset(masking_area.top_left().template to_type<float>());
    auto fill_rule = basic_shape.basic_shape().visit(
        [](CSS::Polygon const& polygon) { return polygon.fill_rule; },
        [](CSS::Path const& path) { return path.fill_rule; },
        [](auto const&) { return Gfx::WindingRule::Nonzero; });
    auto device_path = path.copy_transformed(Gfx::AffineTransform {}.set_scale(scale, scale));
    auto device_bounding_rect = converter.rounded_device_rect(masking_area);
    return ClipPathData { move(device_path), device_bounding_rect, fill_rule };
}

static Optional<PerspectiveData> compute_perspective_data(Paintable const& paintable_box, float scale)
{
    auto perspective_matrix = compute_perspective_matrix(paintable_box);
    if (!perspective_matrix.has_value())
        return {};
    return PerspectiveData { scale_matrix_for_device_pixels(*perspective_matrix, scale) };
}

// NB: Resolves the box's filter as a side effect, since the effects data embeds the resolved gfx filter.
static Optional<EffectsData> compute_effects_data(Paintable& box, double pixel_ratio)
{
    auto const& style_source = box.layout_node();
    if (style_source.filter().has_filters())
        box.set_filter(resolve_css_filter(style_source.filter(), box));
    else if (box.filter().has_filters() || box.filter().svg_filter_bounds.has_value())
        box.set_filter({});

    if (!box.filter().has_filters() && style_source.opacity() == 1 && style_source.mix_blend_mode() == CSS::MixBlendMode::Normal)
        return {};

    Optional<Gfx::Filter> gfx_filter;
    if (box.filter().has_filters())
        gfx_filter = to_gfx_filter(box.filter(), pixel_ratio);
    EffectsData effects {
        style_source.opacity(),
        mix_blend_mode_to_compositing_and_blending_operator(style_source.mix_blend_mode()),
        move(gfx_filter)
    };
    if (!effects.needs_layer())
        return {};
    return effects;
}

AccumulatedVisualContextTree build_accumulated_visual_context_tree(ViewportPaintable& viewport_paintable)
{
    auto& document = viewport_paintable.document();
    auto visual_context_tree = AccumulatedVisualContextTree::create(visual_viewport_transform_data(document));
    auto pixel_ratio = document.page().client().device_pixels_per_css_pixel();
    DevicePixelConverter converter { pixel_ratio };
    auto scale = static_cast<float>(pixel_ratio);

    auto append_node = [&](VisualContextIndex parent_index, VisualContextData data) -> VisualContextIndex {
        return visual_context_tree.append(move(data), parent_index);
    };

    auto visual_viewport_context_index = VISUAL_VIEWPORT_NODE_INDEX;

    viewport_paintable.set_enclosing_scroll_node_index({});
    auto viewport_state_for_descendants = append_node(visual_viewport_context_index, ScrollData { .is_sticky = false });
    viewport_paintable.register_scroll_node(visual_context_tree, viewport_state_for_descendants, viewport_paintable, {});
    viewport_paintable.set_own_scroll_node_index(viewport_state_for_descendants);
    viewport_paintable.set_accumulated_visual_context(VISUAL_VIEWPORT_NODE_INDEX);
    viewport_paintable.set_accumulated_visual_context_for_descendants(viewport_state_for_descendants);

    // Nearest ancestor scroll node resolved along the containing block chain, drilled down alongside
    // the visual context indices. A fixed-position ancestor decouples its subtree from all outer
    // scrollers, but sticky boxes must still reference a scrollport through fixed-position ancestors
    // for their sticky offset computation, so both resolutions are carried.
    struct NearestScrollNodeIndices {
        VisualContextIndex stopping_at_fixed_position_ancestors;
        VisualContextIndex continuing_through_fixed_position_ancestors;
    };

    struct DescendantVisualContexts {
        VisualContextIndex normal;
        VisualContextIndex absolute_position;
        VisualContextIndex fixed_position;
        NearestScrollNodeIndices normal_nearest_scroll_nodes;
        NearestScrollNodeIndices absolute_position_nearest_scroll_nodes;
        NearestScrollNodeIndices fixed_position_nearest_scroll_nodes;
        VisualContextIndex normal_plane_root;
        VisualContextIndex absolute_position_plane_root;
        VisualContextIndex fixed_position_plane_root;
        bool flattens_inherited_transform { false };
    };

    auto build_paintable_box = [&](Paintable& paintable_box, DescendantVisualContexts inherited_contexts, bool may_be_root_element) -> DescendantVisualContexts {
        auto first_visual_context_node_index = visual_context_tree.nodes().size();
        auto& layout_node = paintable_box.layout_node();

        paintable_box.set_enclosing_scroll_node_index({});
        paintable_box.set_own_scroll_node_index({});

        auto nearest_scroll_nodes_for_descendants = [&]() -> NearestScrollNodeIndices {
            if (paintable_box.is_fixed_position())
                return { {}, inherited_contexts.fixed_position_nearest_scroll_nodes.continuing_through_fixed_position_ancestors };
            if (paintable_box.is_absolutely_positioned())
                return inherited_contexts.absolute_position_nearest_scroll_nodes;
            return inherited_contexts.normal_nearest_scroll_nodes;
        }();
        auto nearest_ancestor_scroll_node_index = paintable_box.is_sticky_position()
            ? nearest_scroll_nodes_for_descendants.continuing_through_fixed_position_ancestors
            : nearest_scroll_nodes_for_descendants.stopping_at_fixed_position_ancestors;
        if (!paintable_box.is_fixed_position() && !paintable_box.is_sticky_position())
            paintable_box.set_enclosing_scroll_node_index(nearest_ancestor_scroll_node_index);

        bool creates_sticky_scroll_node = paintable_box.is_sticky_position() && paintable_box.has_sticky_insets();

        VisualContextIndex inherited_state;

        if (paintable_box.is_fixed_position()) {
            inherited_state = inherited_contexts.fixed_position;
        } else if (paintable_box.is_absolutely_positioned()) {
            inherited_state = inherited_contexts.absolute_position;
        } else {
            // In-flow and relatively positioned boxes inherit the normal descendant context from their visual parent.
            inherited_state = inherited_contexts.normal;
        }

        // Build this element's own state from inherited state.
        VisualContextIndex own_state = inherited_state;

        // https://drafts.csswg.org/css-anchor-position-1/#default-scroll-shift
        // After layout has been performed for abspos, it is additionally shifted by the default scroll shift, as if
        // affected by a transform (before any other transforms).
        // NB: The shift is the scroll movement of the frames between the box's containing block and its default anchor
        //     box. When the anchor is itself an anchor-positioned box, its layout position does not include its own
        //     paint-time shift, so each chained anchor's shift is emitted as well, masked to the axes that every link
        //     below it compensates in. The visited set and depth cap guard against malformed anchor chains.
        if (auto const* box = as_if<Layout::Box>(&layout_node)) {
            auto const& scroll_state = viewport_paintable.scroll_state();
            bool compensate_horizontal_scroll = true;
            bool compensate_vertical_scroll = true;
            Vector<Layout::Box const*, 8> visited;
            constexpr size_t max_anchor_chain_depth = 32;
            while (box && !visited.contains_slow(box) && visited.size() < max_anchor_chain_depth) {
                auto const* anchor_box = as_if<Layout::Box>(box->default_scroll_shift_anchor());
                if (!anchor_box)
                    break;
                auto box_paintable = box->paintable_box();
                auto anchor_paintable = anchor_box->paintable_box();
                if (!box_paintable || !anchor_paintable)
                    break;
                visited.append(box);
                compensate_horizontal_scroll = compensate_horizontal_scroll && box->compensates_for_horizontal_scroll();
                compensate_vertical_scroll = compensate_vertical_scroll && box->compensates_for_vertical_scroll();
                auto anchor_scroll_slot = visual_context_tree.scroll_state_slot_for_node(anchor_paintable->enclosing_scroll_node_index());
                auto base_scroll_slot = visual_context_tree.scroll_state_slot_for_node(box_paintable->enclosing_scroll_node_index());
                auto shared_scroll_slot = common_ancestor_slot_along_scroll_parent_chain(scroll_state, anchor_scroll_slot, base_scroll_slot);
                for (auto slot = anchor_scroll_slot; slot != NO_SCROLL_STATE_SLOT && slot != shared_scroll_slot; slot = scroll_state.state_at_slot(slot).parent_slot())
                    own_state = append_node(own_state, AnchorScrollShift { scroll_state.node_index_for_slot(slot), false, compensate_horizontal_scroll, compensate_vertical_scroll });
                for (auto slot = base_scroll_slot; slot != NO_SCROLL_STATE_SLOT && slot != shared_scroll_slot; slot = scroll_state.state_at_slot(slot).parent_slot())
                    own_state = append_node(own_state, AnchorScrollShift { scroll_state.node_index_for_slot(slot), true, compensate_horizontal_scroll, compensate_vertical_scroll });
                box = anchor_box;
            }
        }

        // Out-of-flow descendants can skip overflow and scroll clips from intermediate ancestors. Keep their visual
        // contexts separate as we descend, and replace them with the normal descendant context only when this box
        // establishes the relevant containing block. A chain this box replaces below gets no copies appended:
        // they would be orphaned nodes that nothing in the built tree ever references.
        auto positioning_containing_blocks = layout_node.establishes_positioning_containing_blocks();
        VisualContextIndex state_for_absolute_position_descendants = inherited_contexts.absolute_position;
        VisualContextIndex state_for_fixed_position_descendants = inherited_contexts.fixed_position;

        auto append_to_own_and_positioned_descendant_contexts = [&](auto const& data) {
            own_state = append_node(own_state, data);
            if (!positioning_containing_blocks.absolute)
                state_for_absolute_position_descendants = append_node(state_for_absolute_position_descendants, data);
            if (!positioning_containing_blocks.fixed)
                state_for_fixed_position_descendants = append_node(state_for_fixed_position_descendants, data);
        };

        VisualContextIndex sticky_scroll_node_index;
        if (creates_sticky_scroll_node) {
            sticky_scroll_node_index = append_node(own_state, ScrollData { .is_sticky = true });
            own_state = sticky_scroll_node_index;
            viewport_paintable.register_sticky_node(visual_context_tree, sticky_scroll_node_index, paintable_box, nearest_ancestor_scroll_node_index);
            paintable_box.set_enclosing_scroll_node_index(sticky_scroll_node_index);
            paintable_box.set_own_scroll_node_index(sticky_scroll_node_index);
            nearest_scroll_nodes_for_descendants = { sticky_scroll_node_index, sticky_scroll_node_index };
        }

        auto transform_data = compute_transform(paintable_box, pixel_ratio);

        if (auto effects = compute_effects_data(paintable_box, pixel_ratio); effects.has_value())
            append_to_own_and_positioned_descendant_contexts(effects.value());

        auto flattens_inherited_transform = inherited_contexts.flattens_inherited_transform;

        bool appended_transform_node = false;
        if (transform_data.has_value()) {
            transform_data->flattens_inherited_transform = flattens_inherited_transform;
            paintable_box.set_has_non_invertible_css_transform(!transform_data->matrix.is_invertible());
            own_state = append_node(own_state, *transform_data);
            appended_transform_node = true;
        } else {
            paintable_box.set_has_non_invertible_css_transform(false);
        }

        // https://drafts.csswg.org/css-transforms-2/#backface-visibility-property
        // NB: Whether the element's backface is visible depends on its accumulated 3D transformation matrix, which
        //     is only known at replay time once scroll offsets have been applied. The node recorded below marks the
        //     content to skip and stores the plane root from which that matrix is accumulated. The plane root bounds
        //     the accumulation to the element's 3D rendering context.
        // AD-HOC: The spec determines visibility from the sign of m33 in the accumulated matrix. That is wrong for
        //         matrices with a perspective component, so we test the z-component of the transformed plane normal
        //         instead. See: https://github.com/w3c/csswg-drafts/issues/917.
        auto inherited_plane_root = [&] {
            if (paintable_box.is_fixed_position())
                return inherited_contexts.fixed_position_plane_root;
            if (paintable_box.is_absolutely_positioned())
                return inherited_contexts.absolute_position_plane_root;
            return inherited_contexts.normal_plane_root;
        }();
        bool appended_backface_marker = false;
        if (layout_node.style_group<CSS::ComputedValues::TransformValues>().backface_visibility_value() == CSS::BackfaceVisibility::Hidden && layout_node.is_transformable()) {
            own_state = append_node(own_state, BackfaceVisibilityData { inherited_plane_root, !appended_transform_node && flattens_inherited_transform });
            appended_backface_marker = true;
        }

        // https://drafts.csswg.org/css-transforms-2/#3d-rendering-contexts
        // An element participates in a 3D rendering context if its parent establishes or extends a 3D rendering
        // context. The position of each element in that three-dimensional space is determined by accumulating the
        // transformation matrices up from the given element to the element that establishes the 3D rendering context.
        // NB: Children of an element that neither establishes nor extends a 3D rendering context start a new plane
        //     below the element's own transform. Anonymous boxes carry no element style and are invisible to 3D
        //     rendering contexts, so they pass the inherited plane through unchanged. Pseudo-element boxes carry
        //     their own style and participate normally. An inherited flatten is only materialized by an appended
        //     node, so an element that appends none keeps it pending for its descendants.
        auto establishes_or_extends_3d_rendering_context = layout_node.establishes_or_extends_a_3d_rendering_context();
        bool invisible_to_3d_rendering_contexts = layout_node.is_anonymous() && !layout_node.is_generated_for_pseudo_element();
        auto plane_root_for_descendants = establishes_or_extends_3d_rendering_context || invisible_to_3d_rendering_contexts ? inherited_plane_root : own_state;
        bool inherited_flatten_still_pending = flattens_inherited_transform && !appended_transform_node && !appended_backface_marker;
        auto descendants_flatten_inherited_transform = invisible_to_3d_rendering_contexts ? flattens_inherited_transform : (!establishes_or_extends_3d_rendering_context || inherited_flatten_still_pending);

        if (layout_node.clip().is_rect()) {
            if (auto css_clip = compute_css_clip_data(paintable_box, converter); css_clip.has_value())
                append_to_own_and_positioned_descendant_contexts(css_clip.value());
        }

        if (auto clip_path_data = compute_basic_shape_clip_path_data(paintable_box, converter, scale); clip_path_data.has_value())
            append_to_own_and_positioned_descendant_contexts(clip_path_data.value());

        auto mask_layer_presence = paintable_box.mask_layer_presence(MaskLayerSet::CssAndSvg);
        if (!mask_layer_presence.is_empty())
            viewport_paintable.register_paintable_with_mask_nodes(paintable_box);
        for (auto const& mask_layer : mask_layer_presence)
            append_to_own_and_positioned_descendant_contexts(MaskData { converter.enclosing_device_rect(mask_layer.area), mask_layer.kind, mask_layer.origin });

        paintable_box.set_accumulated_visual_context(own_state);

        Vector<CSS::BackgroundLayerData> const* background_layers = &layout_node.background_layers();
        auto is_root_element = may_be_root_element && layout_node.is_root_element();
        if (is_root_element) {
            if (auto* html_element = as_if<HTML::HTMLHtmlElement>(paintable_box.dom_node().ptr())) {
                if (html_element->should_use_body_background_properties())
                    background_layers = paintable_box.document().background_layers();
            }
        }

        if (background_layers) {
            bool has_fixed_background = false;
            for (auto const& layer : *background_layers) {
                if (layer.background_image && layer.attachment == CSS::BackgroundAttachment::Fixed) {
                    has_fixed_background = true;
                    break;
                }
            }

            if (has_fixed_background) {
                // https://drafts.csswg.org/css-transforms-1/#transform-rendering
                // For elements that are effected by a transform (i.e. have a transform applied to them, or to any of
                // their ancestor elements) and do not have their background propagated to the canvas, a value of fixed
                // for the background-attachment property is treated as if it had a value of scroll.
                auto has_transform_ancestor = false;
                if (!is_root_element) {
                    for (Layout::NodeWithStyle const* node = &layout_node; node && !node->is_viewport(); node = node->parent()) {
                        if (node->has_css_transform()) {
                            has_transform_ancestor = true;
                            break;
                        }
                    }
                }

                if (!has_transform_ancestor) {
                    // Build a context that negates all scroll nodes in the ancestor chain. This keeps the background
                    // fixed relative to the viewport.
                    auto fixed_background_context = own_state;
                    for (auto index = own_state; index.value(); index = visual_context_tree.node_at(index).parent_index) {
                        auto const& node = visual_context_tree.node_at(index);
                        if (node.data.has<ScrollData>())
                            fixed_background_context = append_node(fixed_background_context, ScrollCompensation { index });
                    }
                    paintable_box.set_fixed_background_visual_context(fixed_background_context);
                }
            }
        }

        // Build state for descendants: own state + perspective + clip + scroll.
        VisualContextIndex state_for_descendants = own_state;

        if (layout_node.perspective().has_value()) {
            if (auto perspective_data = compute_perspective_data(paintable_box, scale); perspective_data.has_value()) {
                perspective_data->flattens_inherited_transform = descendants_flatten_inherited_transform;
                descendants_flatten_inherited_transform = false;
                state_for_descendants = append_node(state_for_descendants, *perspective_data);
            }
        }

        auto may_have_clip = layout_node.overflow_x() != CSS::Overflow::Visible
            || layout_node.overflow_y() != CSS::Overflow::Visible
            || layout_node.contain().paint_containment
            || layout_node.content_visibility() == CSS::ContentVisibility::Auto;
        if (may_have_clip) {
            if (auto clip_data = compute_clip_data(paintable_box, converter); clip_data.has_value())
                state_for_descendants = append_node(state_for_descendants, clip_data.value());
        }

        if (paintable_box.has_scrollable_overflow()) {
            auto parent_index = creates_sticky_scroll_node ? sticky_scroll_node_index : nearest_ancestor_scroll_node_index;
            auto scroll_node_index = append_node(state_for_descendants, ScrollData { .is_sticky = false });
            state_for_descendants = scroll_node_index;
            viewport_paintable.register_scroll_node(visual_context_tree, scroll_node_index, paintable_box, parent_index);
            paintable_box.set_own_scroll_node_index(scroll_node_index);
            nearest_scroll_nodes_for_descendants = { scroll_node_index, scroll_node_index };
        }

        // Positioned descendants that escape into a viewport-establishing containing block lay out
        // in the box's own coordinate space, not the viewport's user units, so they hang above the
        // viewport transform node.
        auto state_for_positioned_descendants = state_for_descendants;
        if (auto svg_viewport_transform = paintable_box.svg_viewport_transform(); svg_viewport_transform.has_value()) {
            auto viewport_transform_data = compute_svg_viewport_transform_data(paintable_box, *svg_viewport_transform, pixel_ratio);
            viewport_transform_data.flattens_inherited_transform = descendants_flatten_inherited_transform;
            descendants_flatten_inherited_transform = false;
            state_for_descendants = append_node(state_for_descendants, viewport_transform_data);
        }

        paintable_box.set_accumulated_visual_context_for_descendants(state_for_descendants);
        paintable_box.set_visual_context_node_range(first_visual_context_node_index, visual_context_tree.nodes().size());
        auto absolute_position_nearest_scroll_nodes = inherited_contexts.absolute_position_nearest_scroll_nodes;
        auto fixed_position_nearest_scroll_nodes = inherited_contexts.fixed_position_nearest_scroll_nodes;
        auto absolute_position_plane_root = inherited_contexts.absolute_position_plane_root;
        auto fixed_position_plane_root = inherited_contexts.fixed_position_plane_root;
        if (positioning_containing_blocks.absolute) {
            state_for_absolute_position_descendants = state_for_positioned_descendants;
            absolute_position_nearest_scroll_nodes = nearest_scroll_nodes_for_descendants;
            absolute_position_plane_root = plane_root_for_descendants;
        }
        if (positioning_containing_blocks.fixed) {
            state_for_fixed_position_descendants = state_for_positioned_descendants;
            fixed_position_nearest_scroll_nodes = nearest_scroll_nodes_for_descendants;
            fixed_position_plane_root = plane_root_for_descendants;
        }

        return DescendantVisualContexts {
            state_for_descendants,
            state_for_absolute_position_descendants,
            state_for_fixed_position_descendants,
            nearest_scroll_nodes_for_descendants,
            absolute_position_nearest_scroll_nodes,
            fixed_position_nearest_scroll_nodes,
            plane_root_for_descendants,
            absolute_position_plane_root,
            fixed_position_plane_root,
            descendants_flatten_inherited_transform,
        };
    };

    NearestScrollNodeIndices viewport_nearest_scroll_nodes { viewport_state_for_descendants, viewport_state_for_descendants };
    DescendantVisualContexts viewport_contexts {
        viewport_state_for_descendants,
        viewport_state_for_descendants,
        visual_viewport_context_index,
        viewport_nearest_scroll_nodes,
        viewport_nearest_scroll_nodes,
        viewport_nearest_scroll_nodes,
        viewport_state_for_descendants,
        viewport_state_for_descendants,
        visual_viewport_context_index,
        true,
    };

    struct PendingPaintable {
        Paintable* paintable;
        DescendantVisualContexts inherited_contexts;
        bool may_be_root_element;
    };

    auto has_default_scroll_shift_anchor = [](Paintable const& paintable_box) {
        auto const* box = as_if<Layout::Box>(paintable_box.layout_node());
        return box && box->default_scroll_shift_anchor();
    };

    // Anchor-positioned boxes emit AnchorScrollShift nodes by reading the enclosing scroll nodes of their
    // anchors, and an acceptable anchor may come later in tree order than the positioned box. Building such
    // boxes' subtrees is deferred until their anchors have been built; the hash table mirrors the queue so
    // readiness checks stay cheap across rounds.
    Vector<PendingPaintable> deferred_anchor_positioned_paintables;
    HashTable<Paintable const*> deferred_paintables_awaiting_build;

    auto build_paintables_deferring_anchor_positioned = [&](Vector<PendingPaintable, 64>& stack, Paintable const* paintable_exempt_from_deferral) {
        while (!stack.is_empty()) {
            auto pending = stack.take_last();
            if (pending.paintable != paintable_exempt_from_deferral && has_default_scroll_shift_anchor(*pending.paintable)) {
                deferred_anchor_positioned_paintables.append(pending);
                deferred_paintables_awaiting_build.set(pending.paintable);
                continue;
            }
            auto child_contexts = build_paintable_box(*pending.paintable, pending.inherited_contexts, pending.may_be_root_element);
            for (auto* child = pending.paintable->last_child_ptr(); child; child = child->previous_sibling_ptr())
                stack.append({ child, child_contexts, false });
        }
    };

    Vector<PendingPaintable, 64> pending_paintables;
    for (auto* child = viewport_paintable.last_child_ptr(); child; child = child->previous_sibling_ptr())
        pending_paintables.append({ child, viewport_contexts, true });
    build_paintables_deferring_anchor_positioned(pending_paintables, nullptr);

    auto anchor_is_awaiting_build = [&](Paintable const& paintable_box) {
        auto const* box = as_if<Layout::Box>(paintable_box.layout_node());
        auto const* anchor_box = box ? as_if<Layout::Box>(box->default_scroll_shift_anchor()) : nullptr;
        auto anchor_paintable = anchor_box ? anchor_box->paintable_box() : nullptr;
        for (auto const* paintable = anchor_paintable.ptr(); paintable; paintable = paintable->parent_ptr()) {
            if (deferred_paintables_awaiting_build.contains(paintable))
                return true;
        }
        return false;
    };

    auto build_deferred_subtree = [&](PendingPaintable& entry) {
        deferred_paintables_awaiting_build.remove(entry.paintable);
        pending_paintables.clear_with_capacity();
        pending_paintables.append(entry);
        build_paintables_deferring_anchor_positioned(pending_paintables, entry.paintable);
    };

    while (!deferred_anchor_positioned_paintables.is_empty()) {
        auto entries = move(deferred_anchor_positioned_paintables);
        Vector<PendingPaintable> entries_whose_anchor_is_still_deferred;
        for (auto& entry : entries) {
            if (anchor_is_awaiting_build(*entry.paintable))
                entries_whose_anchor_is_still_deferred.append(entry);
            else
                build_deferred_subtree(entry);
        }

        bool no_entry_was_ready = entries_whose_anchor_is_still_deferred.size() == entries.size();
        if (no_entry_was_ready) {
            // Cyclic or otherwise malformed anchor chains can leave every remaining entry waiting on another;
            // build them in queue order then — the anchor chain walk's visited set and depth cap bound the
            // damage the same way they do for cycles discovered mid-walk.
            for (auto& entry : entries_whose_anchor_is_still_deferred)
                build_deferred_subtree(entry);
        } else {
            deferred_anchor_positioned_paintables.extend(move(entries_whose_anchor_is_still_deferred));
        }
    }

    return visual_context_tree;
}

static void build_nested_svg_visual_context_tree_for_subtree(AccumulatedVisualContextTree& visual_context_tree, NestedVisualContextAssignments& assignments, DevicePixelConverter const& converter, Paintable& paintable_box, VisualContextIndex inherited_state, double pixel_ratio, bool include_element_transform)
{
    auto own_state = inherited_state;
    if (auto effects = compute_effects_data(paintable_box, pixel_ratio); effects.has_value())
        own_state = visual_context_tree.append(effects.release_value(), inherited_state);

    if (include_element_transform) {
        if (auto transform_data = compute_transform(paintable_box, pixel_ratio); transform_data.has_value())
            own_state = visual_context_tree.append(*transform_data, own_state);
    }

    for (auto const& mask_layer : paintable_box.mask_layer_presence(MaskLayerSet::SvgOnly)) {
        own_state = visual_context_tree.append(MaskData { converter.enclosing_device_rect(mask_layer.area), mask_layer.kind, mask_layer.origin }, own_state);
        assignments.mask_node_indices.ensure(&paintable_box).append(own_state);
    }

    auto state_for_descendants = own_state;

    auto const& style_source = paintable_box.layout_node();
    auto may_have_clip = style_source.overflow_x() != CSS::Overflow::Visible
        || style_source.overflow_y() != CSS::Overflow::Visible
        || style_source.contain().paint_containment
        || style_source.content_visibility() == CSS::ContentVisibility::Auto;
    if (may_have_clip) {
        if (auto clip_data = compute_clip_data(paintable_box, converter); clip_data.has_value())
            state_for_descendants = visual_context_tree.append(clip_data.value(), state_for_descendants);
    }

    if (auto svg_viewport_transform = paintable_box.svg_viewport_transform(); svg_viewport_transform.has_value()) {
        auto viewport_transform_data = compute_svg_viewport_transform_data(paintable_box, *svg_viewport_transform, pixel_ratio);
        state_for_descendants = visual_context_tree.append(viewport_transform_data, state_for_descendants);
    }

    assignments.paintable_indices.set(&paintable_box, { own_state, state_for_descendants });

    paintable_box.for_each_child_of_type<Paintable>([&](Paintable& child) {
        build_nested_svg_visual_context_tree_for_subtree(visual_context_tree, assignments, converter, child, state_for_descendants, pixel_ratio, true);
        return IterationDecision::Continue;
    });
}

AccumulatedVisualContextTree build_nested_svg_visual_context_tree(Paintable& root_paintable, TransformData root_transform, NestedVisualContextAssignments& assignments, IncludeRootElementTransform include_root_element_transform)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create_with_content_root(move(root_transform));
    auto pixel_ratio = root_paintable.document().page().client().device_pixels_per_css_pixel();
    DevicePixelConverter converter(pixel_ratio);
    build_nested_svg_visual_context_tree_for_subtree(visual_context_tree, assignments, converter, root_paintable, {}, pixel_ratio, include_root_element_transform == IncludeRootElementTransform::Yes);
    return visual_context_tree;
}

// Patches the transform/effects/perspective values of the box's existing visual context nodes in place.
// Returns false if the box's node structure no longer matches; the caller must then do a full rebuild.
bool update_accumulated_visual_context_values(ViewportPaintable& viewport_paintable, Paintable& paintable_box)
{
    auto& visual_context_tree = viewport_paintable.visual_context_tree();
    auto begin = paintable_box.visual_context_nodes_begin();
    auto end = paintable_box.visual_context_nodes_end();
    if (end > visual_context_tree.nodes().size())
        return false;

    auto pixel_ratio = viewport_paintable.document().page().client().device_pixels_per_css_pixel();
    auto transform = compute_transform(paintable_box, pixel_ratio);
    auto effects = compute_effects_data(paintable_box, pixel_ratio);
    auto perspective = compute_perspective_data(paintable_box, static_cast<float>(pixel_ratio));

    Optional<TransformData> svg_viewport_transform_data;
    if (auto svg_viewport_transform = paintable_box.svg_viewport_transform(); svg_viewport_transform.has_value())
        svg_viewport_transform_data = compute_svg_viewport_transform_data(paintable_box, *svg_viewport_transform, pixel_ratio);

    paintable_box.set_has_non_invertible_css_transform(transform.has_value() && !transform->matrix.is_invertible());

    // The builder duplicates a box's EffectsData into the positioned-descendant chains, so every
    // node of a kind is patched with the same recomputed payload.
    bool found_css_transform = false;
    bool found_svg_viewport_transform = false;
    bool found_effects = false;
    bool found_perspective = false;
    for (size_t i = begin; i < end; ++i) {
        auto& node = visual_context_tree.node_at(VisualContextIndex { i });
        if (auto* transform_data = node.data.get_pointer<TransformData>()) {
            if (transform_data->role == TransformDataRole::SvgViewportTransform) {
                if (!svg_viewport_transform_data.has_value())
                    return false;
                svg_viewport_transform_data->flattens_inherited_transform = transform_data->flattens_inherited_transform;
                *transform_data = *svg_viewport_transform_data;
                found_svg_viewport_transform = true;
                continue;
            }
            if (!transform.has_value())
                return false;
            transform->flattens_inherited_transform = transform_data->flattens_inherited_transform;
            *transform_data = *transform;
            found_css_transform = true;
        } else if (auto* effects_data = node.data.get_pointer<EffectsData>()) {
            if (!effects.has_value())
                return false;
            *effects_data = *effects;
            found_effects = true;
        } else if (auto* perspective_data = node.data.get_pointer<PerspectiveData>()) {
            if (!perspective.has_value())
                return false;
            perspective->flattens_inherited_transform = perspective_data->flattens_inherited_transform;
            *perspective_data = *perspective;
            found_perspective = true;
        }
    }

    return transform.has_value() == found_css_transform
        && effects.has_value() == found_effects
        && perspective.has_value() == found_perspective
        && svg_viewport_transform_data.has_value() == found_svg_viewport_transform;
}

void update_visual_viewport_accumulated_visual_context(ViewportPaintable& viewport_paintable)
{
    VERIFY(viewport_paintable.m_visual_context_tree.has_value());
    viewport_paintable.m_visual_context_tree->set_visual_viewport_transform(visual_viewport_transform_data(viewport_paintable.document()));
}

VisualContextIndex AccumulatedVisualContextTree::append(VisualContextData data, VisualContextIndex parent_index)
{
    VERIFY(parent_index.value() < m_nodes.size());
    size_t depth = m_nodes[parent_index.value()].depth + 1;

    bool empty_clip = false;
    if (m_nodes[parent_index.value()].has_empty_effective_clip) {
        empty_clip = true;
    } else if (data.has<ClipData>()) {
        empty_clip = data.get<ClipData>().rect.is_empty();
    } else if (data.has<ClipPathData>()) {
        empty_clip = data.get<ClipPathData>().path.bounding_box().is_empty();
    } else if (data.has<MaskData>()) {
        empty_clip = data.get<MaskData>().rect.is_empty();
    }

    auto index = VisualContextIndex(m_nodes.size());
    m_nodes.append({ move(data), parent_index, depth, empty_clip });
    return index;
}

void AccumulatedVisualContextTree::set_visual_viewport_transform(TransformData transform)
{
    VERIFY(!m_nodes.is_empty());
    VERIFY(m_nodes[VISUAL_VIEWPORT_NODE_INDEX.value()].data.has<TransformData>());
    m_nodes[VISUAL_VIEWPORT_NODE_INDEX.value()].data = move(transform);
}

bool AccumulatedVisualContextTree::is_compatible_with(AccumulatedVisualContextTree const& other) const
{
    if (m_nodes.size() != other.m_nodes.size())
        return false;

    for (size_t i = 0; i < m_nodes.size(); ++i) {
        auto const& node = m_nodes[i];
        auto const& other_node = other.m_nodes[i];
        if (node.parent_index != other_node.parent_index)
            return false;
        if (node.has_empty_effective_clip != other_node.has_empty_effective_clip)
            return false;
        if (!node.data.visit([&](auto const& data) {
                using DataType = RemoveCVReference<decltype(data)>;
                return other_node.data.has<DataType>();
            }))
            return false;
    }

    return true;
}

void AccumulatedVisualContextTree::reuse_version_from(AccumulatedVisualContextTree const& other)
{
    VERIFY(is_compatible_with(other));
    m_version = other.m_version;
}

Vector<size_t, 8> AccumulatedVisualContextTree::build_ancestor_chain(VisualContextIndex index) const
{
    VERIFY(index.value() < m_nodes.size());
    auto const& node = m_nodes[index.value()];
    Vector<size_t, 8> chain;
    chain.ensure_capacity(node.depth + 1);
    for (size_t i = index.value();; i = m_nodes[i].parent_index.value()) {
        chain.append(i);
        if (i == VISUAL_VIEWPORT_NODE_INDEX.value())
            break;
    }
    return chain;
}

struct LocalSpatialMatrix {
    Gfx::FloatMatrix4x4 matrix;
    bool flattens_inherited_transform { false };
};

static LocalSpatialMatrix local_spatial_matrix(AccumulatedVisualContextNode const& node, VisualContextIndex node_index, ScrollStateSnapshot const& scroll_state)
{
    return node.data.visit(
        [&](TransformData const& transform) {
            return LocalSpatialMatrix { transform.matrix_including_origin(), transform.flattens_inherited_transform };
        },
        [&](PerspectiveData const& perspective) {
            return LocalSpatialMatrix { perspective.matrix, perspective.flattens_inherited_transform };
        },
        [&](ScrollData const&) {
            auto offset = scroll_state.device_offset_for_index(node_index);
            return LocalSpatialMatrix { Gfx::translation_matrix(Vector3 { offset.x(), offset.y(), 0.f }) };
        },
        [&](ScrollCompensation const& compensation) {
            auto offset = scroll_state.device_offset_for_index(compensation.scroll_node_index);
            return LocalSpatialMatrix { Gfx::translation_matrix(Vector3 { -offset.x(), -offset.y(), 0.f }) };
        },
        [&](AnchorScrollShift const& shift) {
            auto offset = shift.masked_offset(scroll_state);
            return LocalSpatialMatrix { Gfx::translation_matrix(Vector3 { offset.x(), offset.y(), 0.f }) };
        },
        [&](BackfaceVisibilityData const& backface) { return LocalSpatialMatrix { Gfx::FloatMatrix4x4::identity(), backface.flattens_inherited_transform }; },
        [&](ClipData const&) { return LocalSpatialMatrix { Gfx::FloatMatrix4x4::identity() }; },
        [&](ClipPathData const&) { return LocalSpatialMatrix { Gfx::FloatMatrix4x4::identity() }; },
        [&](EffectsData const&) { return LocalSpatialMatrix { Gfx::FloatMatrix4x4::identity() }; },
        [&](MaskData const&) { return LocalSpatialMatrix { Gfx::FloatMatrix4x4::identity() }; });
}

bool AccumulatedVisualContextTree::chain_contains_3d_transform(VisualContextIndex index) const
{
    for (size_t i = index.value();; i = m_nodes[i].parent_index.value()) {
        auto const& node = m_nodes[i];
        if (auto const* transform = node.data.get_pointer<TransformData>()) {
            if (!Gfx::is_2d_affine_transform(transform->matrix))
                return true;
        } else if (node.data.has<PerspectiveData>()) {
            return true;
        }
        if (i == VISUAL_VIEWPORT_NODE_INDEX.value())
            break;
    }
    return false;
}

// Homogeneous coordinates this close to the eye plane have no meaningful projection.
static constexpr float minimum_projection_w = 0.0001f;

static Gfx::FloatRect map_rect_through_matrix(Gfx::FloatMatrix4x4 const& matrix, Gfx::FloatRect const& rect)
{
    auto map_corner = [&](Gfx::FloatPoint point) {
        return matrix * Gfx::FloatVector4 { point.x(), point.y(), 0, 1 };
    };
    Array<Gfx::FloatVector4, 4> mapped_corners {
        map_corner(rect.top_left()),
        map_corner(rect.top_right()),
        map_corner(rect.bottom_left()),
        map_corner(rect.bottom_right()),
    };
    bool all_corners_behind_eye_plane = all_of(mapped_corners, [](auto const& corner) { return corner.w() <= 0; });
    // A rect entirely behind the eye plane divides through its negative homogeneous coordinates, matching the
    // geometry other engines report. A rect that crosses the eye plane projects without bound, so the corners
    // behind it are clamped to keep the result conservatively covering the rendered region.
    auto project_corner = [&](Gfx::FloatVector4 const& corner) -> Gfx::FloatPoint {
        auto w = all_corners_behind_eye_plane ? min(corner.w(), -minimum_projection_w) : max(corner.w(), minimum_projection_w);
        return { corner.x() / w, corner.y() / w };
    };
    auto top_left = project_corner(mapped_corners[0]);
    auto top_right = project_corner(mapped_corners[1]);
    auto bottom_left = project_corner(mapped_corners[2]);
    auto bottom_right = project_corner(mapped_corners[3]);
    auto left = min(min(top_left.x(), top_right.x()), min(bottom_left.x(), bottom_right.x()));
    auto right = max(max(top_left.x(), top_right.x()), max(bottom_left.x(), bottom_right.x()));
    auto top = min(min(top_left.y(), top_right.y()), min(bottom_left.y(), bottom_right.y()));
    auto bottom = max(max(top_left.y(), top_right.y()), max(bottom_left.y(), bottom_right.y()));
    return { left, top, right - left, bottom - top };
}

Optional<Gfx::FloatPoint> AccumulatedVisualContextTree::transform_point_for_hit_test(VisualContextIndex index, Gfx::FloatPoint screen_point, ScrollStateSnapshot const& scroll_state, ClipBehavior clip_behavior) const
{
    auto chain = build_ancestor_chain(index);

    // The backface test needs forward matrices, but this walk only applies inverses. When the chain contains
    // backface markers, we accumulate the forward matrices as we walk, from the root down, so a marker can look up
    // the matrix at its plane root by depth.
    bool chain_has_backface_marker = any_of(chain, [&](size_t chain_index) {
        return m_nodes[chain_index].data.has<BackfaceVisibilityData>();
    });
    bool chain_has_3d_transform = chain_contains_3d_transform(index);
    bool needs_accumulated_matrices = chain_has_backface_marker || chain_has_3d_transform;
    Vector<Gfx::FloatMatrix4x4, 8> accumulated_matrices;
    if (needs_accumulated_matrices)
        accumulated_matrices.ensure_capacity(chain.size());

    auto point = screen_point;
    for (size_t i = chain.size(); i > 0; --i) {
        auto node_index = VisualContextIndex { chain[i - 1] };
        auto const& node = m_nodes[node_index.value()];

        if (needs_accumulated_matrices) {
            auto local = local_spatial_matrix(node, node_index, scroll_state);
            if (i == chain.size()) {
                accumulated_matrices.unchecked_append(local.matrix);
            } else {
                auto const& parent_matrix = accumulated_matrices.last();
                accumulated_matrices.unchecked_append((local.flattens_inherited_transform ? Gfx::flattened(parent_matrix) : parent_matrix) * local.matrix);
            }
        }

        if (chain_has_3d_transform
            && (node.data.has<TransformData>() || node.data.has<PerspectiveData>() || node.data.has<ScrollData>()
                || node.data.has<ScrollCompensation>() || node.data.has<AnchorScrollShift>())) {
            auto inverse = Gfx::flattened(accumulated_matrices.last()).inverse();
            if (!inverse.has_value())
                return {};
            auto mapped = *inverse * Gfx::FloatVector4 { screen_point.x(), screen_point.y(), 0, 1 };
            if (mapped.w() < minimum_projection_w)
                return {};
            point = { mapped.x() / mapped.w(), mapped.y() / mapped.w() };
            continue;
        }

        auto result = node.data.visit(
            [&](PerspectiveData const& perspective) -> Optional<Gfx::FloatPoint> {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return {};
                point = inverse->map(point);
                return point;
            },
            [&](BackfaceVisibilityData const& backface) -> Optional<Gfx::FloatPoint> {
                auto plane_root_depth = m_nodes[backface.plane_root_index.value()].depth;
                VERIFY(chain[chain.size() - 1 - plane_root_depth] == backface.plane_root_index.value());
                if (should_cull_back_face(accumulated_matrices.last(), accumulated_matrices[plane_root_depth]))
                    return {};
                return point;
            },
            [&](ScrollData const&) -> Optional<Gfx::FloatPoint> {
                point.translate_by(-scroll_state.device_offset_for_index(node_index));
                return point;
            },
            [&](TransformData const& transform) -> Optional<Gfx::FloatPoint> {
                auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return {};

                auto offset_point = point - transform.origin;
                auto transformed = inverse->map(offset_point);
                point = transformed + transform.origin;
                return point;
            },
            [&](ClipData const& clip) -> Optional<Gfx::FloatPoint> {
                if (clip_behavior == ClipBehavior::Ignore)
                    return point;
                // NOTE: The clip rect is in absolute device-pixel coordinates. After inverse-transforming, `point`
                //       is also in device-pixel coordinates, so we compare them directly.
                if (!clip.contains(point.to_type<int>().to_type<DevicePixels>()))
                    return {};
                return point;
            },
            [&](ClipPathData const& clip_path) -> Optional<Gfx::FloatPoint> {
                if (clip_behavior == ClipBehavior::Ignore)
                    return point;
                // NOTE: The clip path is in absolute device-pixel coordinates. After inverse-transforming, `point`
                //       is also in device-pixel coordinates, so we compare them directly.
                if (!clip_path.bounding_rect.contains(point.to_type<int>().to_type<DevicePixels>()))
                    return {};
                if (!clip_path.path.contains(point, clip_path.fill_rule))
                    return {};
                return point;
            },
            [&](EffectsData const&) -> Optional<Gfx::FloatPoint> {
                // Effects don't affect coordinate transforms
                return point;
            },
            [&](MaskData const&) -> Optional<Gfx::FloatPoint> {
                return point;
            },
            [&](ScrollCompensation const& compensation) -> Optional<Gfx::FloatPoint> {
                point.translate_by(scroll_state.device_offset_for_index(compensation.scroll_node_index));
                return point;
            },
            [&](AnchorScrollShift const& shift) -> Optional<Gfx::FloatPoint> {
                point.translate_by(-shift.masked_offset(scroll_state));
                return point;
            });

        if (!result.has_value())
            return {};
    }

    return point;
}

Gfx::FloatPoint AccumulatedVisualContextTree::inverse_transform_point(VisualContextIndex index, Gfx::FloatPoint screen_point) const
{
    auto chain = build_ancestor_chain(index);

    // This walk deliberately skips translation-only nodes. Callers resolve offsets and scroll positions
    // themselves. The per-node inverses below are only exact for chains of 2D transforms, so a chain containing a
    // 3D transform inverts the flattened accumulated matrix, mapping the screen point onto the plane the content
    // was rendered into.
    if (chain_contains_3d_transform(index)) {
        auto matrix = Gfx::FloatMatrix4x4::identity();
        for (size_t i = chain.size(); i > 0; --i) {
            auto const& node = m_nodes[chain[i - 1]];
            node.data.visit(
                [&](TransformData const& transform) {
                    matrix = (transform.flattens_inherited_transform ? Gfx::flattened(matrix) : matrix) * transform.matrix_including_origin();
                },
                [&](PerspectiveData const& perspective) {
                    matrix = (perspective.flattens_inherited_transform ? Gfx::flattened(matrix) : matrix) * perspective.matrix;
                },
                [&](auto const&) {});
        }
        auto inverse = Gfx::flattened(matrix).inverse();
        if (!inverse.has_value())
            return screen_point;
        auto mapped = *inverse * Gfx::FloatVector4 { screen_point.x(), screen_point.y(), 0, 1 };
        if (mapped.w() < minimum_projection_w)
            return screen_point;
        return { mapped.x() / mapped.w(), mapped.y() / mapped.w() };
    }

    auto point = screen_point;
    for (size_t i = chain.size(); i > 0; --i) {
        auto const& node = m_nodes[chain[i - 1]];

        node.data.visit(
            [&](PerspectiveData const& perspective) {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                auto inverse = affine.inverse();
                if (inverse.has_value())
                    point = inverse->map(point);
            },
            [&](TransformData const& transform) {
                auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                auto inverse = affine.inverse();
                if (inverse.has_value()) {
                    auto offset_point = point - transform.origin;
                    auto transformed = inverse->map(offset_point);
                    point = transformed + transform.origin;
                }
            },
            [&](auto const&) {});
    }

    return point;
}

Gfx::FloatMatrix4x4 AccumulatedVisualContextTree::accumulated_matrix(VisualContextIndex index, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    auto chain = build_ancestor_chain(index);
    auto matrix = Gfx::FloatMatrix4x4::identity();
    for (size_t i = chain.size(); i > 0; --i) {
        auto node_index = VisualContextIndex { chain[i - 1] };
        if (node_index == VISUAL_VIEWPORT_NODE_INDEX && m_root_is_visual_viewport && include_visual_viewport_transform == IncludeVisualViewportTransform::No)
            continue;
        auto local = local_spatial_matrix(m_nodes[node_index.value()], node_index, scroll_state);
        matrix = (local.flattens_inherited_transform ? Gfx::flattened(matrix) : matrix) * local.matrix;
    }
    return matrix;
}

Gfx::FloatSize AccumulatedVisualContextTree::accumulated_2d_scale(VisualContextIndex index, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    auto affine = Gfx::extract_2d_affine_transform(accumulated_matrix(index, scroll_state, include_visual_viewport_transform));
    return { affine.x_scale(), affine.y_scale() };
}

Gfx::FloatRect AccumulatedVisualContextTree::transform_rect_to_viewport(VisualContextIndex index, Gfx::FloatRect const& source_rect, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    // A chain with three-dimensional transforms cannot be applied one two-dimensional projection at a time.
    if (chain_contains_3d_transform(index)) {
        return map_rect_through_matrix(accumulated_matrix(index, scroll_state, include_visual_viewport_transform), source_rect);
    }

    auto rect = source_rect;
    for (size_t i = index.value();; i = m_nodes[i].parent_index.value()) {
        auto const& node = m_nodes[i];
        if (i != VISUAL_VIEWPORT_NODE_INDEX.value() || !m_root_is_visual_viewport || include_visual_viewport_transform == IncludeVisualViewportTransform::Yes) {
            node.data.visit(
                [&](TransformData const& transform) {
                    auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                    rect.translate_by(-transform.origin);
                    rect = affine.map(rect);
                    rect.translate_by(transform.origin);
                },
                [&](PerspectiveData const& perspective) {
                    auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                    rect = affine.map(rect);
                },
                [&](ScrollData const&) {
                    rect.translate_by(scroll_state.device_offset_for_index(VisualContextIndex { i }));
                },
                [&](ScrollCompensation const& compensation) {
                    rect.translate_by(-scroll_state.device_offset_for_index(compensation.scroll_node_index));
                },
                [&](AnchorScrollShift const& shift) {
                    rect.translate_by(shift.masked_offset(scroll_state));
                },
                [&](BackfaceVisibilityData const&) {},
                [&](ClipData const&) { /* clips don't affect rect coordinates */ },
                [&](ClipPathData const&) { /* clip paths don't affect rect coordinates */ },
                [&](EffectsData const&) { /* effects don't affect rect coordinates */ },
                [&](MaskData const&) {});
        }
        if (i == VISUAL_VIEWPORT_NODE_INDEX.value())
            break;
    }

    return rect;
}

Gfx::FloatMatrix4x4 TransformData::matrix_including_origin() const
{
    auto origin_translation = Gfx::translation_matrix(Gfx::Vector3<float> { origin.x(), origin.y(), 0 });
    auto inverse_origin_translation = Gfx::translation_matrix(Gfx::Vector3<float> { -origin.x(), -origin.y(), 0 });
    return origin_translation * matrix * inverse_origin_translation;
}

bool should_cull_back_face(Gfx::FloatMatrix4x4 const& accumulated_matrix, Gfx::FloatMatrix4x4 const& plane_root_matrix)
{
    auto inverse_plane_root_matrix = Gfx::flattened(plane_root_matrix).inverse();
    if (!inverse_plane_root_matrix.has_value())
        return false;
    return Gfx::is_back_face_visible(*inverse_plane_root_matrix * accumulated_matrix);
}

Gfx::FloatPoint AnchorScrollShift::masked_offset(ScrollStateSnapshot const& scroll_state) const
{
    auto offset = scroll_state.device_offset_for_index(scroll_node_index);
    if (!compensate_horizontal_scroll)
        offset.set_x(0);
    if (!compensate_vertical_scroll)
        offset.set_y(0);
    return negate ? -offset : offset;
}

void AccumulatedVisualContextTree::dump(VisualContextIndex index, StringBuilder& builder) const
{
    auto const& node = m_nodes[index.value()];
    node.data.visit(
        [&](PerspectiveData const&) {
            builder.append("perspective"sv);
        },
        [&](BackfaceVisibilityData const& backface) {
            builder.appendff("backface-hidden plane_root={}", backface.plane_root_index.value());
        },
        [&](ScrollData const& scroll) {
            builder.append("scroll"sv);
            if (scroll.is_sticky)
                builder.append(" (sticky)"sv);
        },
        [&](TransformData const& transform) {
            auto const& matrix = transform.matrix.elements();
            auto const& origin = transform.origin;
            builder.appendff("{}=[{},{},{},{},{},{}] origin=({},{})",
                transform.role == TransformDataRole::SvgViewportTransform ? "svg-viewport-transform"sv : "transform"sv,
                matrix[0][0], matrix[0][1], matrix[1][0], matrix[1][1], matrix[0][3], matrix[1][3], origin.x(), origin.y());
        },
        [&](ClipData const& clip) {
            auto const& rect = clip.rect;
            builder.appendff("clip=[{},{} {}x{}]", rect.x(), rect.y(), rect.width(), rect.height());

            if (clip.corner_radii.has_any_radius()) {
                auto const& corner_radii = clip.corner_radii;
                builder.appendff(" radii=({},{},{},{})", corner_radii.top_left.horizontal_radius, corner_radii.top_right.horizontal_radius, corner_radii.bottom_right.horizontal_radius, corner_radii.bottom_left.horizontal_radius);
            }
        },
        [&](ClipPathData const& clip_path) {
            auto const& rect = clip_path.bounding_rect;
            builder.appendff("clip_path=[bounds: {},{} {}x{}, path: {}]", rect.x(), rect.y(), rect.width(), rect.height(), clip_path.path.to_svg_string());
        },
        [&](EffectsData const& effects) {
            builder.append("effects=["sv);
            bool has_content = false;
            if (effects.opacity < 1.0f) {
                builder.appendff("opacity={}", effects.opacity);
                has_content = true;
            }
            if (effects.blend_mode != Gfx::CompositingAndBlendingOperator::Normal) {
                if (has_content)
                    builder.append(' ');
                builder.appendff("blend_mode={}", static_cast<int>(effects.blend_mode));
                has_content = true;
            }
            if (effects.gfx_filter.has_value()) {
                if (has_content)
                    builder.append(' ');
                builder.append("filter"sv);
                has_content = true;
            }
            builder.append("]"sv);
        },
        [&](MaskData const& mask) {
            auto const& rect = mask.rect;
            auto kind = mask.kind == Gfx::MaskKind::Alpha ? "alpha"sv : "luminance"sv;
            auto origin = [&] {
                switch (mask.origin) {
                case MaskLayerOrigin::CssMaskLayers:
                    return "css-mask-layers"sv;
                case MaskLayerOrigin::SvgMask:
                    return "svg-mask"sv;
                case MaskLayerOrigin::SvgClip:
                    return "svg-clip"sv;
                }
                VERIFY_NOT_REACHED();
            }();
            builder.appendff("mask=[{},{} {}x{}] kind={} origin={}", rect.x(), rect.y(), rect.width(), rect.height(), kind, origin);
        },
        [&](ScrollCompensation const& compensation) {
            builder.appendff("scroll_compensation(node_index={})", compensation.scroll_node_index.value());
        },
        [&](AnchorScrollShift const& shift) {
            builder.appendff("anchor_scroll_shift(node_index={}{}{}{})", shift.scroll_node_index.value(),
                shift.negate ? ", negate"sv : ""sv,
                shift.compensate_horizontal_scroll ? ""sv : ", no-x"sv,
                shift.compensate_vertical_scroll ? ""sv : ", no-y"sv);
        });
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ScrollData const& data)
{
    TRY(encoder.encode(data.is_sticky));
    return {};
}

template<>
ErrorOr<Web::Painting::ScrollData> decode(Decoder& decoder)
{
    return Web::Painting::ScrollData {
        .is_sticky = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ClipData const& data)
{
    TRY(encoder.encode(data.rect));
    TRY(encoder.encode(data.corner_radii));
    return {};
}

template<>
ErrorOr<Web::Painting::ClipData> decode(Decoder& decoder)
{
    return Web::Painting::ClipData {
        TRY(decoder.decode<Web::DevicePixelRect>()),
        TRY(decoder.decode<Gfx::CornerRadii>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::TransformData const& data)
{
    TRY(encoder.encode(data.matrix));
    TRY(encoder.encode(data.origin));
    TRY(encoder.encode(data.flattens_inherited_transform));
    TRY(encoder.encode(data.role));
    return {};
}

template<>
ErrorOr<Web::Painting::TransformData> decode(Decoder& decoder)
{
    return Web::Painting::TransformData {
        .matrix = TRY(decoder.decode<Gfx::FloatMatrix4x4>()),
        .origin = TRY(decoder.decode<Gfx::FloatPoint>()),
        .flattens_inherited_transform = TRY(decoder.decode<bool>()),
        .role = TRY(decoder.decode<Web::Painting::TransformDataRole>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::PerspectiveData const& data)
{
    TRY(encoder.encode(data.matrix));
    TRY(encoder.encode(data.flattens_inherited_transform));
    return {};
}

template<>
ErrorOr<Web::Painting::PerspectiveData> decode(Decoder& decoder)
{
    return Web::Painting::PerspectiveData {
        .matrix = TRY(decoder.decode<Gfx::FloatMatrix4x4>()),
        .flattens_inherited_transform = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::BackfaceVisibilityData const& data)
{
    TRY(encoder.encode(data.plane_root_index));
    TRY(encoder.encode(data.flattens_inherited_transform));
    return {};
}

template<>
ErrorOr<Web::Painting::BackfaceVisibilityData> decode(Decoder& decoder)
{
    return Web::Painting::BackfaceVisibilityData {
        .plane_root_index = TRY(decoder.decode<Web::Painting::VisualContextIndex>()),
        .flattens_inherited_transform = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ClipPathData const& data)
{
    TRY(encoder.encode(data.path));
    TRY(encoder.encode(data.bounding_rect));
    TRY(encoder.encode(data.fill_rule));
    return {};
}

template<>
ErrorOr<Web::Painting::ClipPathData> decode(Decoder& decoder)
{
    return Web::Painting::ClipPathData {
        .path = TRY(decoder.decode<Gfx::Path>()),
        .bounding_rect = TRY(decoder.decode<Web::DevicePixelRect>()),
        .fill_rule = TRY(decoder.decode<Gfx::WindingRule>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::EffectsData const& data)
{
    TRY(encoder.encode(data.opacity));
    TRY(encoder.encode(data.blend_mode));
    TRY(encoder.encode(data.gfx_filter));
    return {};
}

template<>
ErrorOr<Web::Painting::EffectsData> decode(Decoder& decoder)
{
    return Web::Painting::EffectsData {
        .opacity = TRY(decoder.decode<float>()),
        .blend_mode = TRY(decoder.decode<Gfx::CompositingAndBlendingOperator>()),
        .gfx_filter = TRY(decoder.decode<Optional<Gfx::Filter>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::MaskData const& data)
{
    TRY(encoder.encode(data.rect));
    TRY(encoder.encode(data.kind));
    TRY(encoder.encode(data.origin));
    return {};
}

template<>
ErrorOr<Web::Painting::MaskData> decode(Decoder& decoder)
{
    return Web::Painting::MaskData {
        .rect = TRY(decoder.decode<Web::DevicePixelRect>()),
        .kind = TRY(decoder.decode<Gfx::MaskKind>()),
        .origin = TRY(decoder.decode<Web::Painting::MaskLayerOrigin>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ScrollCompensation const& data)
{
    TRY(encoder.encode(data.scroll_node_index));
    return {};
}

template<>
ErrorOr<Web::Painting::ScrollCompensation> decode(Decoder& decoder)
{
    return Web::Painting::ScrollCompensation {
        .scroll_node_index = TRY(decoder.decode<Web::Painting::VisualContextIndex>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::AnchorScrollShift const& data)
{
    TRY(encoder.encode(data.scroll_node_index));
    TRY(encoder.encode(data.negate));
    TRY(encoder.encode(data.compensate_horizontal_scroll));
    TRY(encoder.encode(data.compensate_vertical_scroll));
    return {};
}

template<>
ErrorOr<Web::Painting::AnchorScrollShift> decode(Decoder& decoder)
{
    return Web::Painting::AnchorScrollShift {
        .scroll_node_index = TRY(decoder.decode<Web::Painting::VisualContextIndex>()),
        .negate = TRY(decoder.decode<bool>()),
        .compensate_horizontal_scroll = TRY(decoder.decode<bool>()),
        .compensate_vertical_scroll = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::AccumulatedVisualContextNode const& node)
{
    TRY(encoder.encode(node.data));
    TRY(encoder.encode(node.parent_index));
    TRY(encoder.encode(node.depth));
    TRY(encoder.encode(node.has_empty_effective_clip));
    return {};
}

template<>
ErrorOr<Web::Painting::AccumulatedVisualContextNode> decode(Decoder& decoder)
{
    return Web::Painting::AccumulatedVisualContextNode {
        .data = TRY(decoder.decode<Web::Painting::VisualContextData>()),
        .parent_index = TRY(decoder.decode<Web::Painting::VisualContextIndex>()),
        .depth = TRY(decoder.decode<size_t>()),
        .has_empty_effective_clip = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::AccumulatedVisualContextTree const& tree)
{
    TRY(encoder.encode(tree.m_version));
    TRY(encoder.encode(tree.m_nodes));
    TRY(encoder.encode(tree.m_root_is_visual_viewport));
    return {};
}

template<>
ErrorOr<Web::Painting::AccumulatedVisualContextTree> decode(Decoder& decoder)
{
    auto version = TRY(decoder.decode<u64>());
    auto nodes = TRY(decoder.decode<Vector<Web::Painting::AccumulatedVisualContextNode>>());
    auto root_is_visual_viewport = TRY(decoder.decode<bool>());
    if (nodes.is_empty())
        return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree missing visual viewport node");
    if (!nodes[Web::Painting::VISUAL_VIEWPORT_NODE_INDEX.value()].data.has<Web::Painting::TransformData>())
        return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree visual viewport node is not a transform");
    return Web::Painting::AccumulatedVisualContextTree { version, move(nodes), root_is_visual_viewport };
}

}
