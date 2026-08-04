/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{ClipData, EffectsData, PerspectiveData, TransformData, TransformDataRole};
use crate::css::computed_value_types::{ComputedClipEdge, ComputedStyleValueHandle};
use crate::css::computed_value_views::{ComputedValuesView, LengthPercentageRef};
use crate::css::css_enums;
use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::css::style_value::StyleValueData;
use crate::layout::LayoutNodeArena;
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::host::{FfiVisualContextHostCallbacks, FfiVisualContextTreeInputs};
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::paintable_geometry;
use crate::painting::style_queries;
use libgfx_rust::{
    AffineTransform, CompositingAndBlendingOperator, CornerRadii, FloatPoint, affine_to_matrix, perspective_matrix,
    scale_matrix_for_device_pixels, translation_matrix,
};

pub(crate) fn visual_viewport_transform_data(inputs: &FfiVisualContextTreeInputs) -> TransformData {
    let scaled_offset_x = inputs.visual_viewport_offset_x * inputs.visual_viewport_scale;
    let scaled_offset_y = inputs.visual_viewport_offset_y * inputs.visual_viewport_scale;
    let scale = inputs.visual_viewport_scale as f32;
    let translation_x = (-scaled_offset_x) as f32 + 0.0;
    let translation_y = (-scaled_offset_y) as f32 + 0.0;
    let visual_viewport_affine = AffineTransform {
        values: [scale, 0.0, 0.0, scale, translation_x, translation_y],
    };
    TransformData {
        matrix: scale_matrix_for_device_pixels(
            affine_to_matrix(visual_viewport_affine),
            inputs.device_pixels_per_css_pixel as f32,
        ),
        origin: FloatPoint::default(),
        sorting_context_root_index: None,
        flattens_inherited_transform: false,
        role: TransformDataRole::CssTransform,
        synthetic_plane: false,
    }
}

pub(crate) fn transform_reference_box(
    style: ComputedValuesView<'_>,
    paintables: &PaintableArena,
    callbacks: &FfiVisualContextHostCallbacks,
    slot: PaintableSlotId,
) -> CssPixelRect {
    use css_enums::transform_box::{BORDER_BOX, CONTENT_BOX, FILL_BOX, STROKE_BOX, VIEW_BOX};
    let mut transform_box = style.transform().transform_box;
    let data = paintables.data_ref(slot);
    if paintable_geometry::is_svg_paintable(data.kind) {
        transform_box = match transform_box {
            CONTENT_BOX => FILL_BOX,
            BORDER_BOX => STROKE_BOX,
            other => other,
        };
    } else {
        transform_box = match transform_box {
            FILL_BOX => CONTENT_BOX,
            STROKE_BOX | VIEW_BOX => BORDER_BOX,
            other => other,
        };
    }
    match transform_box {
        CONTENT_BOX | FILL_BOX => paintable_geometry::absolute_rect(paintables, slot),
        VIEW_BOX => callbacks
            .svg_transform_view_box_rect(data.shell)
            .map(CssPixelRect::from)
            .unwrap_or_else(|| paintable_geometry::absolute_border_box_rect(paintables, slot)),
        _ => paintable_geometry::absolute_border_box_rect(paintables, slot),
    }
}

fn affine_is_identity(transform: AffineTransform) -> bool {
    transform.values == [1.0, 0.0, 0.0, 1.0, 0.0, 0.0]
}

fn resolved_translate_axis_px(px: f32, percentage: &ComputedStyleValueHandle, reference: CssPixels) -> f32 {
    let Some(length_percentage) = percentage.length_percentage() else {
        return px;
    };
    if length_percentage.is_calculated() {
        return crate::css::computed_value_views::resolve_calc_to_px(length_percentage.calculated_pointer(), reference)
            .to_float();
    }
    CssPixels::nearest_value_for(reference.to_double() * length_percentage.as_fraction()).to_float()
}

fn resolved_transform_to_matrix(
    entry: &crate::css::computed_value_types::ComputedResolvedTransform,
    reference_width: CssPixels,
    reference_height: CssPixels,
) -> libgfx_rust::FloatMatrix4x4 {
    if entry.is_translate {
        return translation_matrix(
            resolved_translate_axis_px(entry.x_px, &entry.x_percentage, reference_width),
            resolved_translate_axis_px(entry.y_px, &entry.y_percentage, reference_height),
            entry.z_px,
        );
    }
    let mut elements = [[0.0f32; 4]; 4];
    for (index, value) in entry.matrix.into_iter().enumerate() {
        elements[index / 4][index % 4] = value;
    }
    libgfx_rust::FloatMatrix4x4 { elements }
}

// https://drafts.csswg.org/css-transforms-2/#ctm
pub(crate) fn compute_transform(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    callbacks: &FfiVisualContextHostCallbacks,
    slot: PaintableSlotId,
    pixel_ratio: f64,
) -> Option<(TransformData, bool)> {
    let data = paintables.data_ref(slot);
    let node = data.layout_node;
    let style = layout_arena.node_style_if_live(node)?;
    let node_kind = layout_arena.node_kind_if_live(node)?;

    let additional_element_transform = if style_queries::kind_is_svg_element_box(node_kind) {
        callbacks.svg_additional_element_transform(data.shell)
    } else {
        None
    };
    let additional_element_transform = additional_element_transform.filter(|transform| !affine_is_identity(*transform));

    let transform_values = style.transform();
    let style_has_transform = transform_values.resolved_transforms.length != 0;
    if (!style_has_transform && additional_element_transform.is_none())
        || !style_queries::is_transformable(layout_arena, node)
    {
        return None;
    }

    // The transformation matrix is computed from the transform, transform-origin, translate, rotate, scale, and
    // offset properties as follows:
    let reference_box = transform_reference_box(style, paintables, callbacks, slot);
    let origin_lp = |handle: &ComputedStyleValueHandle| {
        handle
            .length_percentage()
            .expect("computed transform-origin lost its style value")
    };
    let origin_x = origin_lp(&transform_values.transform_origin_x).to_px(reference_box.width);
    let origin_y = origin_lp(&transform_values.transform_origin_y).to_px(reference_box.height);
    let origin_z = origin_lp(&transform_values.transform_origin_z)
        .to_px(CssPixels::from_raw(0))
        .to_float();

    // 1. Start with the identity matrix.
    // 2. Translate by the computed X, Y, and Z values of transform-origin.
    let mut matrix = translation_matrix(0.0, 0.0, origin_z);

    // 3. Translate by the computed X, Y, and Z values of translate.
    // 4. Rotate by the computed <angle> about the specified axis of rotate.
    // 5. Scale by the computed X, Y, and Z values of scale.
    // FIXME: 6. Translate and rotate by the transform specified by offset.
    // 7. Multiply by each of the transform functions in transform from left to right.
    // NB: The resolved transform list carries translate, rotate, scale, and the
    //     transform functions pre-lowered in exactly that order.
    for entry in transform_values.resolved_transforms.as_slice() {
        matrix = matrix.multiplied(resolved_transform_to_matrix(
            entry,
            reference_box.width,
            reference_box.height,
        ));
    }

    // The x and y properties of <use> define an additional translation applied after any
    // transformations specified with other properties.
    if let Some(additional) = additional_element_transform {
        matrix = matrix.multiplied(affine_to_matrix(additional));
    }

    // 8. Translate by the negated computed X, Y and Z values of transform-origin.
    matrix = matrix.multiplied(translation_matrix(0.0, 0.0, -origin_z));

    let scale = pixel_ratio as f32;
    let device_origin = FloatPoint {
        x: (reference_box.x + origin_x).to_float() * scale,
        y: (reference_box.y + origin_y).to_float() * scale,
    };
    let matrix = scale_matrix_for_device_pixels(matrix, scale);
    let is_invertible = matrix.is_invertible();
    Some((
        TransformData {
            matrix,
            origin: device_origin,
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
        },
        is_invertible,
    ))
}

// https://drafts.csswg.org/css-transforms-2/#perspective-matrix
pub(crate) fn compute_perspective_data(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    callbacks: &FfiVisualContextHostCallbacks,
    slot: PaintableSlotId,
    pixel_ratio: f64,
) -> Option<PerspectiveData> {
    let node = paintables.data_ref(slot).layout_node;
    let style = layout_arena.node_style_if_live(node)?;
    let transform_values = style.transform();
    if !transform_values.has_perspective || !style_queries::is_transformable(layout_arena, node) {
        return None;
    }

    // The perspective matrix is computed as follows:

    // 1. Start with the identity matrix.
    // 2. Translate by the computed X and Y values of 'perspective-origin'
    // https://drafts.csswg.org/css-transforms-2/#perspective-origin-property
    // Percentages: refer to the size of the reference box
    let reference_box = transform_reference_box(style, paintables, callbacks, slot);
    let origin_x = transform_values
        .perspective_origin_x
        .length_percentage()
        .expect("computed perspective-origin lost its style value")
        .to_px(reference_box.width);
    let origin_y = transform_values
        .perspective_origin_y
        .length_percentage()
        .expect("computed perspective-origin lost its style value")
        .to_px(reference_box.height);
    let computed_x = (reference_box.x + origin_x).to_float();
    let computed_y = (reference_box.y + origin_y).to_float();
    let mut matrix = translation_matrix(computed_x, computed_y, 0.0);

    // 3. Multiply by the matrix that would be obtained from the 'perspective()' transform function, where the
    //    length is provided by the value of the perspective property
    // https://drafts.csswg.org/css-transforms-2/#funcdef-perspective
    // If the depth value is less than '1px', it must be treated as '1px' for the purpose of rendering, [..]
    let distance = CssPixels::from_raw(transform_values.perspective_px).to_float().max(1.0);
    matrix = matrix.multiplied(perspective_matrix(distance));

    // 4. Translate by the negated computed X and Y values of 'perspective-origin'
    matrix = matrix.multiplied(translation_matrix(-computed_x, -computed_y, 0.0));
    Some(PerspectiveData {
        matrix: scale_matrix_for_device_pixels(matrix, pixel_ratio as f32),
        flattens_inherited_transform: false,
    })
}

pub(crate) fn compute_css_clip_data(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
    pixel_ratio: f64,
) -> Option<ClipData> {
    let node = paintables.data_ref(slot).layout_node;
    let style = layout_arena.node_style_if_live(node)?;
    if !style.effects().clip_is_rect || !style.is_absolutely_positioned() {
        return None;
    }
    let border_box = paintable_geometry::absolute_border_box_rect(paintables, slot);
    let [top_edge, right_edge, bottom_edge, left_edge] = &style.effects().clip_edges;
    let left = border_box.x + resolved_clip_edge_px(left_edge, CssPixels::from_raw(0));
    let top = border_box.y + resolved_clip_edge_px(top_edge, CssPixels::from_raw(0));
    let right = border_box.x + resolved_clip_edge_px(right_edge, border_box.width);
    let bottom = border_box.y + resolved_clip_edge_px(bottom_edge, border_box.height);
    let resolved = CssPixelRect::new(left, top, right - left, bottom - top);
    let effective = if resolved.width < CssPixels::from_raw(0) || resolved.height < CssPixels::from_raw(0) {
        CssPixelRect::new(
            CssPixels::from_raw(0),
            CssPixels::from_raw(0),
            CssPixels::from_raw(0),
            CssPixels::from_raw(0),
        )
    } else {
        resolved
    };
    let converter = DevicePixelConverter::new(pixel_ratio);
    Some(ClipData {
        rect: converter.rounded_device_rect(effective),
        corner_radii: CornerRadii::default(),
    })
}

fn resolved_clip_edge_px(edge: &ComputedClipEdge, auto_value: CssPixels) -> CssPixels {
    if edge.is_auto {
        return auto_value;
    }
    let ratio = crate::css::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS[edge.unit as usize];
    assert!(ratio.is_finite(), "computed clip edge is not an absolute length");
    CssPixels::nearest_value_for(edge.value * ratio)
}

fn border_radius_pair(handle: &ComputedStyleValueHandle) -> (LengthPercentageRef<'_>, LengthPercentageRef<'_>) {
    let Some(value) = style_queries::handle_value(handle) else {
        unreachable!("computed border-radius lost its style value");
    };
    border_radius_pair_of_value(value)
}

pub(crate) fn border_radius_pair_of_value(
    value: &StyleValueData,
) -> (LengthPercentageRef<'_>, LengthPercentageRef<'_>) {
    let StyleValueData::BorderRadius {
        horizontal_radius,
        vertical_radius,
        ..
    } = value
    else {
        unreachable!("computed border-radius is a border-radius value");
    };
    (
        LengthPercentageRef::over(horizontal_radius.data()),
        LengthPercentageRef::over(vertical_radius.data()),
    )
}

fn border_radius_is_initial(handle: &ComputedStyleValueHandle) -> bool {
    let Some(StyleValueData::BorderRadius {
        horizontal_radius,
        vertical_radius,
        ..
    }) = style_queries::handle_value(handle)
    else {
        unreachable!("computed border-radius lost its style value");
    };
    let is_zero_px = |value: &StyleValueData| {
        matches!(value, StyleValueData::Length { value, unit }
            if *value == 0.0 && *unit == crate::css::style_compute::px_length_unit())
    };
    is_zero_px(horizontal_radius.data()) && is_zero_px(vertical_radius.data())
}

pub(crate) fn border_radii_data(
    style: ComputedValuesView<'_>,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
) -> BorderRadii {
    let border = style.border();
    let has_noninitial_radii = !border_radius_is_initial(&border.border_bottom_left_radius)
        || !border_radius_is_initial(&border.border_bottom_right_radius)
        || !border_radius_is_initial(&border.border_top_left_radius)
        || !border_radius_is_initial(&border.border_top_right_radius);
    if !has_noninitial_radii {
        return BorderRadii::default();
    }
    let border_box = paintable_geometry::absolute_border_box_rect(paintables, slot);
    let border_rect = CssPixelRect::new(
        CssPixels::from_raw(0),
        CssPixels::from_raw(0),
        border_box.width,
        border_box.height,
    );
    crate::painting::border_radii::normalize_border_radii_data(
        border_rect,
        border_rect,
        [
            border_radius_pair(&border.border_top_left_radius),
            border_radius_pair(&border.border_top_right_radius),
            border_radius_pair(&border.border_bottom_right_radius),
            border_radius_pair(&border.border_bottom_left_radius),
        ],
    )
}

fn overflow_property_applies(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
) -> bool {
    // https://drafts.csswg.org/css-overflow-3/#overflow-control
    // Overflow properties apply to block containers, flex containers and grid containers.
    // FIXME: Ideally we would check whether overflow applies positively rather than listing exceptions. However,
    // not all elements that should support overflow are currently identifiable that way.
    let data = paintables.data_ref(slot);
    if paintable_geometry::is_svg_paintable(data.kind) {
        return false;
    }
    let display = crate::css::display::FfiDisplay::from_raw(data.display);
    if crate::painting::fragment_ownership::node_is_fragmented_inline(layout_arena, data.layout_node) {
        return false;
    }
    if display.is_ruby_inside() {
        return false;
    }
    if display.is_internal() && !display.is_table_cell() && !display.is_table_caption() {
        return false;
    }
    true
}

// https://drafts.csswg.org/css-overflow-4/#overflow-clip-edge
fn overflow_clip_edge_rect(
    style: ComputedValuesView<'_>,
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
) -> CssPixelRect {
    use crate::css::css_enums::background_box::{BORDER_BOX, CONTENT_BOX};
    let overflow_clip_margin = &style.misc_reset().overflow_clip_margin;
    // https://drafts.csswg.org/css-overflow-4/#overflow-clip-margin
    // Values are defined as follows:
    // '<visual-box>'
    // Specifies the box edge to use as the overflow clip edge origin, i.e. when the specified offset is zero.
    // If omitted, defaults to 'padding-box' on non-replaced elements, or 'content-box' on replaced elements.
    let top_side = &overflow_clip_margin.top;
    let visual_box = if top_side.has_visual_box {
        top_side.visual_box
    } else {
        let node_kind = layout_arena.node_kind_if_live(paintables.data_ref(slot).layout_node);
        if node_kind.is_some_and(crate::layout::kind_is_replaced_box) {
            CONTENT_BOX
        } else {
            crate::css::css_enums::background_box::PADDING_BOX
        }
    };
    let overflow_clip_edge = match visual_box {
        CONTENT_BOX => paintable_geometry::absolute_rect(paintables, slot),
        BORDER_BOX => paintable_geometry::absolute_border_box_rect(paintables, slot),
        _ => paintable_geometry::absolute_padding_box_rect(paintables, slot),
    };
    // '<length [0,∞]>'
    // The specified offset dictates how much the overflow clip edge is expanded from the specified box edge
    // Negative values are invalid. Defaults to zero if omitted.
    overflow_clip_edge.inflated(
        overflow_clip_margin.top.offset,
        overflow_clip_margin.right.offset,
        overflow_clip_margin.bottom.offset,
        overflow_clip_margin.left.offset,
    )
}

pub(crate) fn mix_blend_mode_to_compositing_and_blending_operator(
    mix_blend_mode: u8,
) -> CompositingAndBlendingOperator {
    use crate::css::css_enums::mix_blend_mode as css;
    match mix_blend_mode {
        css::NORMAL => CompositingAndBlendingOperator::Normal,
        css::MULTIPLY => CompositingAndBlendingOperator::Multiply,
        css::SCREEN => CompositingAndBlendingOperator::Screen,
        css::OVERLAY => CompositingAndBlendingOperator::Overlay,
        css::DARKEN => CompositingAndBlendingOperator::Darken,
        css::LIGHTEN => CompositingAndBlendingOperator::Lighten,
        css::COLOR_DODGE => CompositingAndBlendingOperator::ColorDodge,
        css::COLOR_BURN => CompositingAndBlendingOperator::ColorBurn,
        css::HARD_LIGHT => CompositingAndBlendingOperator::HardLight,
        css::SOFT_LIGHT => CompositingAndBlendingOperator::SoftLight,
        css::DIFFERENCE => CompositingAndBlendingOperator::Difference,
        css::EXCLUSION => CompositingAndBlendingOperator::Exclusion,
        css::HUE => CompositingAndBlendingOperator::Hue,
        css::SATURATION => CompositingAndBlendingOperator::Saturation,
        css::COLOR => CompositingAndBlendingOperator::Color,
        css::LUMINOSITY => CompositingAndBlendingOperator::Luminosity,
        css::PLUS_DARKER => CompositingAndBlendingOperator::PlusDarker,
        css::PLUS_LIGHTER => CompositingAndBlendingOperator::PlusLighter,
        _ => unreachable!("computed mix-blend-mode holds an unknown keyword"),
    }
}

pub(crate) fn compute_effects_data(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    callbacks: &FfiVisualContextHostCallbacks,
    slot: PaintableSlotId,
) -> Option<EffectsData> {
    use crate::css::css_enums::mix_blend_mode;
    let data = paintables.data_ref(slot);
    // NB: Resolves the box's filter as a side effect, since the effects data embeds the resolved gfx filter.
    let filter_raw = callbacks.resolve_effects_filter(data.shell);
    let filter = if filter_raw.is_null() {
        None
    } else {
        // SAFETY: The host hands over a heap-allocated Gfx::Filter for us to own.
        Some(std::rc::Rc::new(unsafe { super::FilterHandle::adopt(filter_raw) }))
    };
    let style = layout_arena.node_style_if_live(data.layout_node)?;
    let effects_values = style.effects();
    if filter.is_none() && effects_values.opacity == 1.0 && effects_values.mix_blend_mode == mix_blend_mode::NORMAL {
        return None;
    }
    let effects = EffectsData {
        opacity: effects_values.opacity,
        blend_mode: mix_blend_mode_to_compositing_and_blending_operator(effects_values.mix_blend_mode),
        filter,
    };
    let needs_layer = effects.opacity < 1.0
        || effects.blend_mode != CompositingAndBlendingOperator::Normal
        || effects.filter.is_some();
    needs_layer.then_some(effects)
}

fn any_background_layer_has_a_fixed_attachment_image(style: ComputedValuesView<'_>) -> bool {
    use crate::css::css_enums::keyword::FIXED;
    let background = style.background();
    let Some(image_value) = style_queries::handle_value(&background.background_image) else {
        return false;
    };
    let Some(attachment_value) = style_queries::handle_value(&background.background_attachment) else {
        return false;
    };
    let image_items = style_queries::comma_items(image_value);
    let attachment_items = style_queries::comma_items(attachment_value);
    image_items.iter().enumerate().any(|(index, image)| {
        let attachment = attachment_items[index % attachment_items.len()];
        style_queries::is_abstract_image(image)
            && matches!(attachment, StyleValueData::Keyword { keyword } if *keyword == FIXED)
    })
}

pub(crate) fn wants_fixed_background_visual_context(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    root_background_source: crate::painting::host::FfiRootBackgroundSource,
    slot: PaintableSlotId,
    may_be_root_element: bool,
) -> bool {
    let node = paintables.data_ref(slot).layout_node;
    let is_root_element = may_be_root_element && style_queries::node_is_root_element(layout_arena, node);
    let mut background_layers_node = node;
    if is_root_element && root_background_source.use_body_background_properties {
        background_layers_node = root_background_source.body_layout_node;
    }
    let Some(background_style) = layout_arena.node_style_if_live(background_layers_node) else {
        return false;
    };
    if !any_background_layer_has_a_fixed_attachment_image(background_style) {
        return false;
    }

    // https://drafts.csswg.org/css-transforms-1/#transform-rendering
    // For elements that are effected by a transform (i.e. have a transform applied to them, or to any of
    // their ancestor elements) and do not have their background propagated to the canvas, a value of fixed
    // for the background-attachment property is treated as if it had a value of scroll.
    if !is_root_element {
        let mut current = Some(node);
        while let Some(walk_node) = current {
            if layout_arena.node_kind_if_live(walk_node) == Some(crate::layout::node_data::NodeKind::Viewport) {
                break;
            }
            if let Some(style) = layout_arena.node_style_if_live(walk_node)
                && style_queries::has_css_transform(layout_arena, walk_node, style)
            {
                return false;
            }
            current = layout_arena.node_parent_if_live(walk_node);
        }
    }
    true
}

pub(crate) struct MaskLayerPresenceEntry {
    pub origin: super::MaskLayerOrigin,
    pub area: CssPixelRect,
    pub kind: libgfx_rust::MaskKind,
}

fn kind_overrides_svg_mask_virtuals(kind: crate::painting::paintable_data::PaintableKind) -> bool {
    use crate::painting::paintable_data::PaintableKind;
    matches!(
        kind,
        PaintableKind::SVGGraphicsPaintable
            | PaintableKind::SVGPathPaintable
            | PaintableKind::SVGImagePaintable
            | PaintableKind::SVGMaskPaintable
            | PaintableKind::SVGForeignObjectPaintable
    )
}

fn mask_kind_from(value: i32) -> libgfx_rust::MaskKind {
    use libgfx_rust::MaskKind;
    if value == MaskKind::Luminance as i32 {
        MaskKind::Luminance
    } else {
        MaskKind::Alpha
    }
}

pub(crate) fn mask_layer_presence(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    callbacks: &FfiVisualContextHostCallbacks,
    slot: PaintableSlotId,
    include_css_mask_layers: bool,
) -> Vec<MaskLayerPresenceEntry> {
    use super::MaskLayerOrigin;
    let mut layers = Vec::new();
    let data = paintables.data_ref(slot);
    if include_css_mask_layers {
        let node = data.layout_node;
        if let Some(style) = layout_arena.node_style_if_live(node)
            && style_queries::mask_layers_have_image(style.mask())
            && layout_arena
                .node_kind_if_live(node)
                .is_some_and(crate::layout::kind_is_box)
            && style_queries::establishes_stacking_context(layout_arena, node)
        {
            layers.push(MaskLayerPresenceEntry {
                origin: MaskLayerOrigin::CssMaskLayers,
                area: paintable_geometry::absolute_border_box_rect(paintables, slot),
                kind: libgfx_rust::MaskKind::Alpha,
            });
        }
    }
    if kind_overrides_svg_mask_virtuals(data.kind) {
        let svg_facts = callbacks.svg_mask_facts(data.shell);
        if svg_facts.has_mask_area {
            layers.push(MaskLayerPresenceEntry {
                origin: MaskLayerOrigin::SvgMask,
                area: CssPixelRect::from(svg_facts.mask_area),
                kind: mask_kind_from(svg_facts.mask_kind),
            });
        }
        if svg_facts.has_clip_area {
            layers.push(MaskLayerPresenceEntry {
                origin: MaskLayerOrigin::SvgClip,
                area: CssPixelRect::from(svg_facts.clip_area),
                kind: libgfx_rust::MaskKind::Alpha,
            });
        }
    }
    layers
}

pub(crate) fn backface_hidden(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
) -> bool {
    use crate::css::css_enums::backface_visibility;
    let node = paintables.data_ref(slot).layout_node;
    let Some(style) = layout_arena.node_style_if_live(node) else {
        return false;
    };
    style.transform().backface_visibility == backface_visibility::HIDDEN
        && style_queries::is_transformable(layout_arena, node)
}

pub(crate) fn may_have_clip(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
) -> bool {
    use crate::css::css_enums::{content_visibility, overflow};
    let node = paintables.data_ref(slot).layout_node;
    let Some(style) = layout_arena.node_style_if_live(node) else {
        return false;
    };
    let box_values = style.box_values();
    box_values.overflow_x != overflow::VISIBLE
        || box_values.overflow_y != overflow::VISIBLE
        || box_values.paint_containment
        || style.content_visibility() == content_visibility::AUTO
}

pub(crate) fn compute_clip_data(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
    pixel_ratio: f64,
) -> Option<ClipData> {
    use crate::css::css_enums::{content_visibility, overflow};
    let node = paintables.data_ref(slot).layout_node;
    let style = layout_arena.node_style_if_live(node)?;
    let box_values = style.box_values();
    let mut overflow_x = box_values.overflow_x;
    let mut overflow_y = box_values.overflow_y;

    let style_has_paint_containment =
        box_values.paint_containment || style.content_visibility() == content_visibility::AUTO;
    // https://drafts.csswg.org/css-contain-2/#paint-containment
    // 1. The contents of the element including any ink or scrollable overflow must be clipped to the overflow clip
    // edge of the paint containment box, taking corner clipping into account. This does not include the creation of
    // any mechanism to access or indicate the presence of the clipped content; nor does it inhibit the creation of
    // any such mechanism through other properties, such as overflow, resize, or text-overflow.
    // NOTE: This clipping shape respects overflow-clip-margin, allowing an element with paint containment
    // to still slightly overflow its normal bounds.
    if style_has_paint_containment && style_queries::has_paint_containment(layout_arena, node, style) {
        // NOTE: The behavior described in this paragraph is equivalent to changing 'overflow-x: visible' into
        // 'overflow-x: clip' and 'overflow-y: visible' into 'overflow-y: clip' at used value time, while leaving
        // other values of 'overflow-x' and 'overflow-y' unchanged.
        overflow_x = overflow::CLIP;
        overflow_y = overflow::CLIP;
    }

    // https://drafts.csswg.org/css-overflow-3/#propdef-overflow
    // 'clip'
    // This value indicates that the box’s content is clipped to its overflow clip edge
    let has_hidden_overflow = overflow_x != overflow::VISIBLE || overflow_y != overflow::VISIBLE;
    if !has_hidden_overflow || !overflow_property_applies(layout_arena, paintables, slot) {
        return None;
    }

    let padding_box = paintable_geometry::absolute_padding_box_rect(paintables, slot);
    let overflow_clip_edge = overflow_clip_edge_rect(style, layout_arena, paintables, slot);
    let extent_limit = CssPixels::from_integer(crate::css::css_pixels::MAX_INTEGER_VALUE as i64);
    let (left, right) = match overflow_x {
        overflow::VISIBLE => (CssPixels::from_raw(0), extent_limit),
        overflow::CLIP => (overflow_clip_edge.x, overflow_clip_edge.x + overflow_clip_edge.width),
        _ => (padding_box.x, padding_box.x + padding_box.width),
    };
    let (top, bottom) = match overflow_y {
        overflow::VISIBLE => (CssPixels::from_raw(0), extent_limit),
        overflow::CLIP => (overflow_clip_edge.y, overflow_clip_edge.y + overflow_clip_edge.height),
        _ => (padding_box.y, padding_box.y + padding_box.height),
    };
    let clip_rect = CssPixelRect::new(left, top, right - left, bottom - top);

    // https://drafts.csswg.org/css-overflow-3/#corner-clipping
    // As mentioned in CSS Backgrounds 3 § 4.3 Corner Clipping, the clipping region established by 'overflow' can be
    // rounded:
    // - When 'overflow-x' and 'overflow-y' compute to 'hidden', 'scroll', or 'auto', the clipping region is rounded
    //   based on the border radius, adjusted to the padding edge, as described in CSS Backgrounds 3 § 4.2 Corner
    //   Shaping.
    // - When both 'overflow-x' and 'overflow-y' compute to 'clip', the clipping region is rounded as described in § 3.2
    //   Expanding Clipping Bounds: the 'overflow-clip-margin' property.
    // - However, when one of 'overflow-x' or 'overflow-y' computes to 'clip' and the other computes to 'visible', the
    //   clipping region is not rounded.
    // FIXME: Adjust the border radii for the overflow-clip-margin case.
    //        (see https://drafts.csswg.org/css-overflow-4/#valdef-overflow-clip-margin-length-0 )
    let radii = if overflow_x != overflow::VISIBLE && overflow_y != overflow::VISIBLE {
        border_radii_data(style, paintables, slot).shrunken(
            style.border_top_width(),
            style.border_right_width(),
            style.border_bottom_width(),
            style.border_left_width(),
        )
    } else {
        BorderRadii::default()
    };
    let converter = DevicePixelConverter::new(pixel_ratio);
    Some(ClipData {
        rect: converter.rounded_device_rect(clip_rect),
        corner_radii: radii.as_corners(&converter),
    })
}
