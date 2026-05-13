/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/StringBuilder.h>
#include <LibGfx/Matrix4x4.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/DevicePixelConverter.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

AccumulatedVisualContextTree build_accumulated_visual_context_tree(ViewportPaintable&);
void update_visual_viewport_accumulated_visual_context(ViewportPaintable&);

bool ClipData::contains(DevicePixelPoint point) const
{
    return corner_radii.contains(point.to_type<int>(), rect.to_type<int>());
}

static Atomic<u64> s_next_accumulated_visual_context_tree_version { 1 };

static TransformData identity_visual_viewport_transform()
{
    return { Gfx::FloatMatrix4x4::identity(), { 0.f, 0.f } };
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create()
{
    return create(identity_visual_viewport_transform());
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create(TransformData visual_viewport_transform)
{
    Vector<AccumulatedVisualContextNode> nodes;
    // Visual viewport transform root. This is identity for trees that are not attached to a document viewport.
    nodes.append({ move(visual_viewport_transform), {}, 0, false });
    return AccumulatedVisualContextTree {
        s_next_accumulated_visual_context_tree_version.fetch_add(1, AK::MemoryOrder::memory_order_relaxed),
        move(nodes)
    };
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

// https://drafts.csswg.org/css-transforms-2/#ctm
static Optional<TransformData> compute_transform(PaintableBox const& paintable_box, CSS::ComputedValues const& computed_values, double pixel_ratio)
{
    if (!paintable_box.has_css_transform())
        return {};

    // The transformation matrix is computed from the transform, transform-origin, translate, rotate, scale, and
    // offset properties as follows:
    auto reference_box = paintable_box.transform_reference_box();
    auto const& css_transform_origin = computed_values.transform_origin();
    auto origin_x = css_transform_origin.x.to_px(reference_box.width());
    auto origin_y = css_transform_origin.y.to_px(reference_box.height());
    auto origin_z = css_transform_origin.z.to_px(0).to_float();

    // 1. Start with the identity matrix.
    // 2. Translate by the computed X, Y, and Z values of transform-origin.
    auto matrix = Gfx::translation_matrix(Vector3 { 0.f, 0.f, origin_z });

    // 3. Translate by the computed X, Y, and Z values of translate.
    if (auto const& translate = computed_values.translate())
        matrix = matrix * translate->to_matrix(paintable_box);

    // 4. Rotate by the computed <angle> about the specified axis of rotate.
    if (auto const& rotate = computed_values.rotate())
        matrix = matrix * rotate->to_matrix(paintable_box);

    // 5. Scale by the computed X, Y, and Z values of scale.
    if (auto const& scale = computed_values.scale())
        matrix = matrix * scale->to_matrix(paintable_box);

    // FIXME: 6. Translate and rotate by the transform specified by offset.

    // 7. Multiply by each of the transform functions in transform from left to right.
    for (auto const& transform : computed_values.transformations())
        matrix = matrix * transform->to_matrix(paintable_box);

    // 8. Translate by the negated computed X, Y and Z values of transform-origin.
    matrix = matrix * Gfx::translation_matrix(Vector3 { 0.f, 0.f, -origin_z });

    auto origin = reference_box.location() + CSSPixelPoint { origin_x, origin_y };
    auto scale = static_cast<float>(pixel_ratio);
    auto device_origin = origin.to_type<float>() * scale;
    return TransformData { scale_matrix_for_device_pixels(matrix, scale), device_origin };
}

// https://drafts.csswg.org/css-transforms-2/#perspective-matrix
static Optional<Gfx::FloatMatrix4x4> compute_perspective_matrix(PaintableBox const& paintable_box, CSS::ComputedValues const& computed_values)
{
    if (!paintable_box.layout_node().is_transformable())
        return {};

    auto perspective = computed_values.perspective();
    if (!perspective.has_value())
        return {};

    // The perspective matrix is computed as follows:

    // 1. Start with the identity matrix.
    // 2. Translate by the computed X and Y values of 'perspective-origin'
    // https://drafts.csswg.org/css-transforms-2/#perspective-origin-property
    // Percentages: refer to the size of the reference box
    auto reference_box = paintable_box.transform_reference_box();
    auto perspective_origin = computed_values.perspective_origin().resolved(reference_box);
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

static Optional<ClipData> compute_css_clip_data(PaintableBox const& paintable_box, DevicePixelConverter const& converter)
{
    if (auto css_clip = paintable_box.get_clip_rect(); css_clip.has_value()) {
        auto effective_rect = effective_css_clip_rect(*css_clip);
        return ClipData { converter.rounded_device_rect(effective_rect), {} };
    }
    return {};
}

static Optional<ClipPathData> compute_basic_shape_clip_path_data(PaintableBox const& paintable_box, CSS::ComputedValues const& computed_values, DevicePixelConverter const& converter, float scale)
{
    // FIXME: Support other geometry boxes. See: https://drafts.fxtf.org/css-masking/#typedef-geometry-box
    auto const& clip_path = computed_values.clip_path();
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

    auto visual_viewport_context_index = VISUAL_VIEWPORT_NODE_INDEX;

    VisualContextIndex viewport_state_for_descendants = visual_viewport_context_index;
    if (viewport_paintable.own_scroll_frame_index().value())
        viewport_state_for_descendants = append_node(visual_viewport_context_index, ScrollData { viewport_paintable.own_scroll_frame_index(), false });
    viewport_paintable.set_accumulated_visual_context(VISUAL_VIEWPORT_NODE_INDEX);
    viewport_paintable.set_accumulated_visual_context_for_descendants(viewport_state_for_descendants);

    struct DescendantVisualContexts {
        VisualContextIndex normal;
        VisualContextIndex absolute_position;
        VisualContextIndex fixed_position;
    };

    auto build_paintable_box = [&](auto& self, PaintableBox& paintable_box, DescendantVisualContexts inherited_contexts) -> void {
        // Resolve filters before make_effects_data reads them.
        auto const& paintable_box_computed_values = paintable_box.computed_values();
        if (paintable_box_computed_values.filter().has_filters())
            paintable_box.set_filter(resolve_css_filter(paintable_box_computed_values.filter(), paintable_box));
        else
            paintable_box.set_filter({});

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

        // Out-of-flow descendants can skip overflow and scroll clips from intermediate ancestors. Keep their visual
        // contexts separate as we descend, and replace them with the normal descendant context only when this box
        // establishes the relevant containing block.
        VisualContextIndex state_for_absolute_position_descendants = inherited_contexts.absolute_position;
        VisualContextIndex state_for_fixed_position_descendants = inherited_contexts.fixed_position;

        auto append_to_own_and_positioned_descendant_contexts = [&](auto const& data) {
            own_state = append_node(own_state, data);
            state_for_absolute_position_descendants = append_node(state_for_absolute_position_descendants, data);
            state_for_fixed_position_descendants = append_node(state_for_fixed_position_descendants, data);
        };

        if (paintable_box.is_sticky_position()) {
            // For sticky elements, use enclosing_scroll_frame which holds the sticky frame.
            // own_scroll_frame may be a different scroll frame if the sticky element also has scrollable overflow.
            if (auto sticky_idx = paintable_box.enclosing_scroll_frame_index(); sticky_idx.value() && viewport_paintable.scroll_state().frame_at(sticky_idx).is_sticky())
                own_state = append_node(own_state, ScrollData { sticky_idx, true });
        }

        auto const& computed_values = paintable_box.computed_values();

        if (auto effects = make_effects_data(paintable_box); effects.has_value())
            append_to_own_and_positioned_descendant_contexts(effects.value());

        if (auto transform_data = compute_transform(paintable_box, computed_values, pixel_ratio); transform_data.has_value()) {
            paintable_box.set_has_non_invertible_css_transform(!transform_data->matrix.is_invertible());
            own_state = append_node(own_state, *transform_data);
        } else {
            paintable_box.set_has_non_invertible_css_transform(false);
        }

        if (auto css_clip = compute_css_clip_data(paintable_box, converter); css_clip.has_value())
            append_to_own_and_positioned_descendant_contexts(css_clip.value());

        if (auto clip_path_data = compute_basic_shape_clip_path_data(paintable_box, computed_values, converter, scale); clip_path_data.has_value())
            append_to_own_and_positioned_descendant_contexts(clip_path_data.value());

        paintable_box.set_accumulated_visual_context(own_state);

        Vector<CSS::BackgroundLayerData> const* background_layers = &computed_values.background_layers();
        if (paintable_box.layout_node_with_style_and_box_metrics().is_root_element()) {
            if (auto* html_element = as_if<HTML::HTMLHtmlElement>(paintable_box.dom_node().ptr())) {
                if (html_element->should_use_body_background_properties())
                    background_layers = paintable_box.document().background_layers();
            }
        }

        if (background_layers) {
            bool has_fixed_background = false;
            for (auto const& layer : *background_layers) {
                if (layer.attachment == CSS::BackgroundAttachment::Fixed) {
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
                if (!paintable_box.layout_node_with_style_and_box_metrics().is_root_element()) {
                    for (auto const* node = &paintable_box.layout_node(); node && !node->is_viewport(); node = node->parent()) {
                        if (node->has_css_transform()) {
                            has_transform_ancestor = true;
                            break;
                        }
                    }
                }

                if (!has_transform_ancestor) {
                    // Build a context that negates all scroll frames in the ancestor chain. This keeps the background
                    // fixed relative to the viewport.
                    auto fixed_background_context = own_state;
                    for (auto index = own_state; index.value(); index = visual_context_tree.node_at(index).parent_index) {
                        auto const& node = visual_context_tree.node_at(index);
                        if (auto const* scroll = node.data.get_pointer<ScrollData>()) {
                            fixed_background_context = append_node(fixed_background_context, ScrollCompensation { scroll->scroll_frame_index });
                        }
                    }
                    paintable_box.set_fixed_background_visual_context(fixed_background_context);
                }
            }
        }

        // Build state for descendants: own state + perspective + clip + scroll.
        VisualContextIndex state_for_descendants = own_state;

        if (auto perspective_matrix = compute_perspective_matrix(paintable_box, computed_values); perspective_matrix.has_value()) {
            auto scaled_matrix = scale_matrix_for_device_pixels(*perspective_matrix, scale);
            state_for_descendants = append_node(state_for_descendants, PerspectiveData { scaled_matrix });
        }

        if (auto clip_data = compute_clip_data(paintable_box, computed_values, converter); clip_data.has_value())
            state_for_descendants = append_node(state_for_descendants, clip_data.value());

        if (paintable_box.own_scroll_frame_index().value()) {
            auto is_sticky_without_scrollable_overflow = paintable_box.is_sticky_position() && paintable_box.enclosing_scroll_frame_index() == paintable_box.own_scroll_frame_index();
            if (!is_sticky_without_scrollable_overflow)
                state_for_descendants = append_node(state_for_descendants, ScrollData { paintable_box.own_scroll_frame_index(), false });
        }

        paintable_box.set_accumulated_visual_context_for_descendants(state_for_descendants);
        if (paintable_box.layout_node().establishes_an_absolute_positioning_containing_block())
            state_for_absolute_position_descendants = state_for_descendants;
        if (paintable_box.layout_node().establishes_a_fixed_positioning_containing_block())
            state_for_fixed_position_descendants = state_for_descendants;

        DescendantVisualContexts child_contexts {
            state_for_descendants,
            state_for_absolute_position_descendants,
            state_for_fixed_position_descendants,
        };
        paintable_box.for_each_child_of_type<PaintableBox>([&](PaintableBox& child) {
            self(self, child, child_contexts);
            return IterationDecision::Continue;
        });
    };

    DescendantVisualContexts viewport_contexts {
        viewport_state_for_descendants,
        viewport_state_for_descendants,
        visual_viewport_context_index,
    };
    viewport_paintable.for_each_child_of_type<PaintableBox>([&](PaintableBox& child) {
        build_paintable_box(build_paintable_box, child, viewport_contexts);
        return IterationDecision::Continue;
    });

    return visual_context_tree;
}

void update_visual_viewport_accumulated_visual_context(ViewportPaintable& viewport_paintable)
{
    viewport_paintable.visual_context_tree().set_visual_viewport_transform(visual_viewport_transform_data(viewport_paintable.document()));
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

VisualContextIndex AccumulatedVisualContextTree::find_common_ancestor(VisualContextIndex a, VisualContextIndex b) const
{
    VERIFY(a.value() < m_nodes.size());
    VERIFY(b.value() < m_nodes.size());
    size_t a_index = a.value();
    size_t b_index = b.value();
    while (m_nodes[a_index].depth > m_nodes[b_index].depth)
        a_index = m_nodes[a_index].parent_index.value();
    while (m_nodes[b_index].depth > m_nodes[a_index].depth)
        b_index = m_nodes[b_index].parent_index.value();
    while (a_index != b_index) {
        a_index = m_nodes[a_index].parent_index.value();
        b_index = m_nodes[b_index].parent_index.value();
    }
    return a_index;
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

Optional<Gfx::FloatPoint> AccumulatedVisualContextTree::transform_point_for_hit_test(VisualContextIndex index, Gfx::FloatPoint screen_point, ScrollStateSnapshot const& scroll_state) const
{
    auto chain = build_ancestor_chain(index);

    auto point = screen_point;
    for (size_t i = chain.size(); i > 0; --i) {
        auto const& node = m_nodes[chain[i - 1]];

        auto result = node.data.visit(
            [&](PerspectiveData const& perspective) -> Optional<Gfx::FloatPoint> {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return {};
                point = inverse->map(point);
                return point;
            },
            [&](ScrollData const& scroll) -> Optional<Gfx::FloatPoint> {
                point.translate_by(-scroll_state.device_offset_for_index(scroll.scroll_frame_index));
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
                // NOTE: The clip rect is in absolute device-pixel coordinates. After inverse-transforming, `point`
                //       is also in device-pixel coordinates, so we compare them directly.
                if (!clip.contains(point.to_type<int>().to_type<DevicePixels>()))
                    return {};
                return point;
            },
            [&](ClipPathData const& clip_path) -> Optional<Gfx::FloatPoint> {
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
            [&](ScrollCompensation const& compensation) -> Optional<Gfx::FloatPoint> {
                point.translate_by(scroll_state.device_offset_for_index(compensation.scroll_frame_index));
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

Gfx::FloatRect AccumulatedVisualContextTree::transform_rect_to_viewport(VisualContextIndex index, Gfx::FloatRect const& source_rect, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    auto rect = source_rect;
    for (size_t i = index.value();; i = m_nodes[i].parent_index.value()) {
        auto const& node = m_nodes[i];
        if (i != VISUAL_VIEWPORT_NODE_INDEX.value() || include_visual_viewport_transform == IncludeVisualViewportTransform::Yes) {
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
                [&](ScrollData const& scroll) {
                    rect.translate_by(scroll_state.device_offset_for_index(scroll.scroll_frame_index));
                },
                [&](ScrollCompensation const& compensation) {
                    auto offset = scroll_state.device_offset_for_index(compensation.scroll_frame_index);
                    rect.translate_by(-offset);
                },
                [&](ClipData const&) { /* clips don't affect rect coordinates */ },
                [&](ClipPathData const&) { /* clip paths don't affect rect coordinates */ },
                [&](EffectsData const&) { /* effects don't affect rect coordinates */ });
        }
        if (i == VISUAL_VIEWPORT_NODE_INDEX.value())
            break;
    }

    return rect;
}

void AccumulatedVisualContextTree::dump(VisualContextIndex index, StringBuilder& builder) const
{
    auto const& node = m_nodes[index.value()];
    node.data.visit(
        [&](PerspectiveData const&) {
            builder.append("perspective"sv);
        },
        [&](ScrollData const& scroll) {
            builder.appendff("scroll_frame_id={}", scroll.scroll_frame_index);
            if (scroll.is_sticky)
                builder.append(" (sticky)"sv);
        },
        [&](TransformData const& transform) {
            auto const& matrix = transform.matrix.elements();
            auto const& origin = transform.origin;
            builder.appendff("transform=[{},{},{},{},{},{}] origin=({},{})", matrix[0][0], matrix[0][1], matrix[1][0], matrix[1][1], matrix[0][3], matrix[1][3], origin.x(), origin.y());
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
        [&](ScrollCompensation const& compensation) {
            builder.appendff("scroll_compensation(frame_id={})", compensation.scroll_frame_index.value());
        });
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ScrollData const& data)
{
    TRY(encoder.encode(data.scroll_frame_index));
    TRY(encoder.encode(data.is_sticky));
    return {};
}

template<>
ErrorOr<Web::Painting::ScrollData> decode(Decoder& decoder)
{
    return Web::Painting::ScrollData {
        .scroll_frame_index = TRY(decoder.decode<Web::Painting::ScrollFrameIndex>()),
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
    return {};
}

template<>
ErrorOr<Web::Painting::TransformData> decode(Decoder& decoder)
{
    return Web::Painting::TransformData {
        .matrix = TRY(decoder.decode<Gfx::FloatMatrix4x4>()),
        .origin = TRY(decoder.decode<Gfx::FloatPoint>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::PerspectiveData const& data)
{
    TRY(encoder.encode(data.matrix));
    return {};
}

template<>
ErrorOr<Web::Painting::PerspectiveData> decode(Decoder& decoder)
{
    return Web::Painting::PerspectiveData {
        .matrix = TRY(decoder.decode<Gfx::FloatMatrix4x4>()),
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
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ScrollCompensation const& data)
{
    TRY(encoder.encode(data.scroll_frame_index));
    return {};
}

template<>
ErrorOr<Web::Painting::ScrollCompensation> decode(Decoder& decoder)
{
    return Web::Painting::ScrollCompensation {
        .scroll_frame_index = TRY(decoder.decode<Web::Painting::ScrollFrameIndex>()),
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
    return {};
}

template<>
ErrorOr<Web::Painting::AccumulatedVisualContextTree> decode(Decoder& decoder)
{
    auto version = TRY(decoder.decode<u64>());
    auto nodes = TRY(decoder.decode<Vector<Web::Painting::AccumulatedVisualContextNode>>());
    if (nodes.is_empty())
        return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree missing visual viewport node");
    if (!nodes[Web::Painting::VISUAL_VIEWPORT_NODE_INDEX.value()].data.has<Web::Painting::TransformData>())
        return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree visual viewport node is not a transform");
    return Web::Painting::AccumulatedVisualContextTree { version, move(nodes) };
}

}
