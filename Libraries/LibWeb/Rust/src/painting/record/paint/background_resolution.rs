/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::{ComputedValuesView, LengthPercentageRef};
use crate::css::css_enums;
use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelRect, CssPixelSize};
use crate::css::style_value::StyleValueData;
use crate::painting::host::FfiLayerImageList;
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::background::{BackgroundBox, background_box_for};
use crate::painting::record::paint::replaced::{Fraction, SizeWithAspectRatio, run_default_sizing_algorithm};
use crate::painting::style_queries;
use crate::painting::visual_context::node_values::mix_blend_mode_to_compositing_and_blending_operator;
use libgfx_rust::CompositingAndBlendingOperator;

#[derive(Clone, Copy)]
pub(crate) struct LayerImageSource<'a> {
    pub value: &'a StyleValueData,
    pub list: FfiLayerImageList,
    pub computed_index: u32,
    pub selected_image_value: Option<&'a StyleValueData>,
}

enum ComputedLayerSize<'a> {
    Contain,
    Cover,
    LengthPercentage {
        x: Option<LengthPercentageRef<'a>>,
        y: Option<LengthPercentageRef<'a>>,
    },
}

struct ComputedLayer<'a> {
    image: Option<LayerImageSource<'a>>,
    attachment: u8,
    clip: u8,
    origin: u8,
    position_x: LengthPercentageRef<'a>,
    position_y: LengthPercentageRef<'a>,
    repeat_x: u8,
    repeat_y: u8,
    size: ComputedLayerSize<'a>,
    blend_mode: u8,
    mask_composite: u8,
}

pub(crate) struct ResolvedBackgroundLayer<'a> {
    pub image: Option<LayerImageSource<'a>>,
    pub attachment: u8,
    pub clip: u8,
    pub position_x: CssPixels,
    pub position_y: CssPixels,
    pub background_positioning_area: CssPixelRect,
    pub image_rect: CssPixelRect,
    pub repeat_x: u8,
    pub repeat_y: u8,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub mask_composite: Option<CompositingAndBlendingOperator>,
}

pub(crate) struct ResolvedBackground<'a> {
    pub color_box: BackgroundBox,
    pub layers: Vec<ResolvedBackgroundLayer<'a>>,
    pub needs_text_clip: bool,
    pub background_rect: CssPixelRect,
    pub color: libgfx_rust::Color,
}

pub(crate) struct BackgroundPaintInputs<'a> {
    pub resolved: ResolvedBackground<'a>,
    pub border_radii: crate::painting::border_radii::BorderRadii,
    pub image_rendering: u8,
    pub is_root_element: bool,
}

fn mask_composite_to_compositing_and_blending_operator(compositing_operator: u8) -> CompositingAndBlendingOperator {
    use css_enums::compositing_operator;
    match compositing_operator {
        compositing_operator::ADD => CompositingAndBlendingOperator::Normal,
        compositing_operator::SUBTRACT => CompositingAndBlendingOperator::SourceOut,
        compositing_operator::INTERSECT => CompositingAndBlendingOperator::SourceIn,
        compositing_operator::EXCLUDE => CompositingAndBlendingOperator::Xor,
        _ => unreachable!("computed mask-composite holds an unknown keyword"),
    }
}

fn cycled<'b>(list: &[&'b StyleValueData], index: usize) -> &'b StyleValueData {
    list[index % list.len()]
}

fn keyword_of(value: &StyleValueData) -> u16 {
    let StyleValueData::Keyword { keyword } = value else {
        unreachable!("computed keyword longhand holds a non-keyword value");
    };
    *keyword
}

fn edge_offset(value: &StyleValueData) -> LengthPercentageRef<'_> {
    let StyleValueData::Edge { offset, .. } = value else {
        unreachable!("computed position component is an edge value");
    };
    LengthPercentageRef::over(offset.data())
}

fn length_percentage_or_auto(value: &StyleValueData) -> Option<LengthPercentageRef<'_>> {
    match value {
        StyleValueData::Keyword { .. } => None,
        other => Some(LengthPercentageRef::over(other)),
    }
}

fn computed_layer_size(size_item: &StyleValueData) -> ComputedLayerSize<'_> {
    use css_enums::keyword::{CONTAIN, COVER};
    match size_item {
        StyleValueData::Keyword { keyword } if *keyword == CONTAIN => ComputedLayerSize::Contain,
        StyleValueData::Keyword { keyword } if *keyword == COVER => ComputedLayerSize::Cover,
        StyleValueData::BackgroundSize { size_x, size_y } => ComputedLayerSize::LengthPercentage {
            x: length_percentage_or_auto(size_x.data()),
            y: length_percentage_or_auto(size_y.data()),
        },
        _ => unreachable!("computed background-size holds an unknown value"),
    }
}

fn layer_image_source(
    image_item: &StyleValueData,
    list: FfiLayerImageList,
    computed_index: u32,
) -> Option<LayerImageSource<'_>> {
    style_queries::is_abstract_image(image_item).then_some(LayerImageSource {
        value: image_item,
        list,
        computed_index,
        selected_image_value: None,
    })
}

fn computed_background_layers(style: ComputedValuesView<'_>, image_list: FfiLayerImageList) -> Vec<ComputedLayer<'_>> {
    let background = style.background();
    let items = |handle| {
        style_queries::handle_value(handle)
            .map(style_queries::comma_items)
            .unwrap_or_default()
    };
    let image_items = items(&background.background_image);
    let attachment_items = items(&background.background_attachment);
    let blend_mode_items = items(&background.background_blend_mode);
    let clip_items = items(&background.background_clip);
    let origin_items = items(&background.background_origin);
    let position_x_items = items(&background.background_position_x);
    let position_y_items = items(&background.background_position_y);
    let repeat_items = items(&background.background_repeat);
    let size_items = items(&background.background_size);

    let mut layers = Vec::with_capacity(image_items.len());
    for (index, image_item) in image_items.iter().enumerate() {
        let StyleValueData::RepeatStyle { repeat_x, repeat_y } = cycled(&repeat_items, index) else {
            unreachable!("computed background-repeat holds a repeat-style value");
        };
        layers.push(ComputedLayer {
            image: layer_image_source(image_item, image_list, index as u32),
            attachment: css_enums::keyword_to_background_attachment(keyword_of(cycled(&attachment_items, index)))
                .expect("computed background-attachment holds an attachment keyword"),
            blend_mode: css_enums::keyword_to_mix_blend_mode(keyword_of(cycled(&blend_mode_items, index)))
                .expect("computed background-blend-mode holds a blend keyword"),
            clip: css_enums::keyword_to_background_box(keyword_of(cycled(&clip_items, index)))
                .expect("computed background-clip holds a box keyword"),
            origin: css_enums::keyword_to_background_box(keyword_of(cycled(&origin_items, index)))
                .expect("computed background-origin holds a box keyword"),
            position_x: edge_offset(cycled(&position_x_items, index)),
            position_y: edge_offset(cycled(&position_y_items, index)),
            repeat_x: *repeat_x,
            repeat_y: *repeat_y,
            size: computed_layer_size(cycled(&size_items, index)),
            mask_composite: css_enums::compositing_operator::ADD,
        });
    }
    layers
}

fn computed_mask_layers(style: ComputedValuesView<'_>) -> Vec<ComputedLayer<'_>> {
    use css_enums::mix_blend_mode;
    let mask = style.mask();
    let items = |handle| {
        style_queries::handle_value(handle)
            .map(style_queries::comma_items)
            .unwrap_or_default()
    };
    let image_items = items(&mask.mask_image);
    let clip_items = items(&mask.mask_clip);
    let composite_items = items(&mask.mask_composite);
    let origin_items = items(&mask.mask_origin);
    let position_items = items(&mask.mask_position);
    let repeat_items = items(&mask.mask_repeat);
    let size_items = items(&mask.mask_size);

    let mut layers = Vec::with_capacity(image_items.len());
    for (index, image_item) in image_items.iter().enumerate() {
        let StyleValueData::RepeatStyle { repeat_x, repeat_y } = cycled(&repeat_items, index) else {
            unreachable!("computed mask-repeat holds a repeat-style value");
        };
        let StyleValueData::Position { edge_x, edge_y } = cycled(&position_items, index) else {
            unreachable!("computed mask-position holds a position value");
        };
        let clip = css_enums::keyword_to_background_box(keyword_of(cycled(&clip_items, index)))
            .unwrap_or(css_enums::background_box::BORDER_BOX);
        let origin = css_enums::keyword_to_background_box(keyword_of(cycled(&origin_items, index)))
            .unwrap_or(css_enums::background_box::BORDER_BOX);
        layers.push(ComputedLayer {
            image: layer_image_source(image_item, FfiLayerImageList::Mask, index as u32),
            attachment: css_enums::background_attachment::SCROLL,
            blend_mode: mix_blend_mode::NORMAL,
            clip,
            origin,
            position_x: edge_offset(edge_x.data()),
            position_y: edge_offset(edge_y.data()),
            repeat_x: *repeat_x,
            repeat_y: *repeat_y,
            size: computed_layer_size(cycled(&size_items, index)),
            mask_composite: css_enums::keyword_to_compositing_operator(keyword_of(cycled(&composite_items, index)))
                .expect("computed mask-composite holds a compositing keyword"),
        });
    }
    layers
}

enum LayerType {
    Background,
    Mask,
}

/// Mirrors `resolve_layers()` in BackgroundPainting.cpp.
/// https://drafts.fxtf.org/css-masking-1/#the-mask-image
#[allow(clippy::too_many_arguments)]
fn resolve_layers<'a>(
    recorder: &PaintRecorder<'_>,
    paintable: PaintableSlotId,
    layers: Vec<ComputedLayer<'a>>,
    background_color: libgfx_rust::Color,
    background_color_clip: u8,
    border_rect: CssPixelRect,
    border_radii: crate::painting::border_radii::BorderRadii,
    layer_type: LayerType,
) -> ResolvedBackground<'a> {
    use css_enums::{background_attachment, repetition};
    let border_box = BackgroundBox {
        rect: border_rect,
        radii: border_radii,
    };
    let color_box = background_box_for(background_color_clip, border_box, recorder, paintable);

    let mut resolved_layers: Vec<ResolvedBackgroundLayer<'a>> = Vec::new();
    // A value of none counts as a transparent black image layer.
    // A mask reference that is an empty image (zero width or zero height), that fails to download, is not a reference
    // to an mask element, is non-existent, or that cannot be displayed (e.g. because it is not in a supported image
    // format) still counts as an image layer of transparent black.
    for layer in layers {
        let is_mask = matches!(layer_type, LayerType::Mask);
        let mask_composite = is_mask.then(|| mask_composite_to_compositing_and_blending_operator(layer.mask_composite));
        let blend_mode = mix_blend_mode_to_compositing_and_blending_operator(layer.blend_mode);
        let transparent_mask_layer = |resolved_layers: &mut Vec<ResolvedBackgroundLayer<'a>>| {
            if !is_mask {
                return;
            }
            resolved_layers.push(ResolvedBackgroundLayer {
                image: None,
                attachment: 0,
                clip: layer.clip,
                position_x: CssPixels::from_raw(0),
                position_y: CssPixels::from_raw(0),
                background_positioning_area: CssPixelRect::default(),
                image_rect: CssPixelRect::default(),
                repeat_x: repetition::NO_REPEAT,
                repeat_y: repetition::NO_REPEAT,
                compositing_and_blending_operator: blend_mode,
                mask_composite,
            });
        };

        let Some(mut image) = layer.image else {
            transparent_mask_layer(&mut resolved_layers);
            continue;
        };
        let intrinsics = image_intrinsic_facts(recorder, paintable, &image);
        image.selected_image_value = intrinsics.selected_image_value;
        if !intrinsics.is_paintable {
            transparent_mask_layer(&mut resolved_layers);
            continue;
        }

        let mut background_positioning_area = background_box_for(layer.origin, border_box, recorder, paintable).rect;

        // https://drafts.csswg.org/css-backgrounds-3/#background-origin
        // If the background-attachment value for this layer is fixed, then this property has no effect: in this case
        // the background positioning area is the initial containing block.
        if layer.attachment == background_attachment::FIXED
            && recorder.data(paintable).has_fixed_background_visual_context
        {
            background_positioning_area = CssPixelRect::from_location_and_size(
                crate::css::css_pixels::CssPixelPoint::default(),
                CssPixelRect::from(recorder.inputs.css_viewport_rect).size(),
            );
        }

        let mut specified_width = None;
        let mut specified_height = None;
        if let ComputedLayerSize::LengthPercentage { x, y } = &layer.size {
            if let Some(x) = x {
                specified_width = Some(x.to_px(background_positioning_area.width));
            }
            if let Some(y) = y {
                specified_height = Some(y.to_px(background_positioning_area.height));
            }
        }
        let concrete_image_size = run_default_sizing_algorithm(
            specified_width,
            specified_height,
            &intrinsics.natural,
            background_positioning_area.size(),
        );

        // If the image has no size, there's nothing to paint.
        if concrete_image_size.is_empty() {
            transparent_mask_layer(&mut resolved_layers);
            continue;
        }

        // Size
        let mut image_rect_size = match &layer.size {
            ComputedLayerSize::Contain => {
                let max_width_ratio =
                    background_positioning_area.width.to_double() / concrete_image_size.width.to_double();
                let max_height_ratio =
                    background_positioning_area.height.to_double() / concrete_image_size.height.to_double();
                let ratio = max_width_ratio.min(max_height_ratio);
                CssPixelSize::new(
                    concrete_image_size.width.scaled(ratio),
                    concrete_image_size.height.scaled(ratio),
                )
            }
            ComputedLayerSize::Cover => {
                let max_width_ratio =
                    background_positioning_area.width.to_double() / concrete_image_size.width.to_double();
                let max_height_ratio =
                    background_positioning_area.height.to_double() / concrete_image_size.height.to_double();
                let ratio = max_width_ratio.max(max_height_ratio);
                CssPixelSize::new(
                    concrete_image_size.width.scaled(ratio),
                    concrete_image_size.height.scaled(ratio),
                )
            }
            ComputedLayerSize::LengthPercentage { .. } => concrete_image_size,
        };

        // If after sizing we have a 0px image, we're done. Attempting to paint this would be an infinite loop.
        if image_rect_size.is_empty() {
            transparent_mask_layer(&mut resolved_layers);
            continue;
        }

        // If background-repeat is round for one (or both) dimensions, there is a second step.
        // The UA must scale the image in that dimension (or both dimensions) so that it fits a
        // whole number of times in the background positioning area.
        if layer.repeat_x == repetition::ROUND || layer.repeat_y == repetition::ROUND {
            // If X ≠ 0 is the width of the image after step one and W is the width of the
            // background positioning area, then the rounded width X' = W / round(W / X)
            // where round() is a function that returns the nearest natural number
            // (integer greater than zero).
            let round_to_natural = |value: CssPixels| {
                let rounded = value.round();
                if rounded <= CssPixels::from_raw(0) {
                    return CssPixels::from_integer(1);
                }
                rounded
            };

            if layer.repeat_x == repetition::ROUND {
                image_rect_size.width = background_positioning_area.width.div_as_fraction(round_to_natural(
                    background_positioning_area.width.div_as_fraction(image_rect_size.width),
                ));
            }
            if layer.repeat_y == repetition::ROUND {
                image_rect_size.height = background_positioning_area.height.div_as_fraction(round_to_natural(
                    background_positioning_area
                        .height
                        .div_as_fraction(image_rect_size.height),
                ));
            }

            // If background-repeat is round for one dimension only and if background-size is auto
            // for the other dimension, then there is a third step: that other dimension is scaled
            // so that the original aspect ratio is restored.
            if layer.repeat_x != layer.repeat_y {
                let (size_x_is_auto, size_y_is_auto) = match &layer.size {
                    ComputedLayerSize::LengthPercentage { x, y } => (x.is_none(), y.is_none()),
                    _ => (false, false),
                };
                if size_x_is_auto {
                    image_rect_size.width =
                        image_rect_size
                            .height
                            .mul_by_fraction(crate::css::css_pixels::CssPixelFraction::ratio_of(
                                concrete_image_size.width,
                                concrete_image_size.height,
                            ));
                }
                if size_y_is_auto {
                    image_rect_size.height =
                        image_rect_size
                            .width
                            .mul_by_fraction(crate::css::css_pixels::CssPixelFraction::ratio_of(
                                concrete_image_size.height,
                                concrete_image_size.width,
                            ));
                }
            }
        }

        // If after round adjustments we have a 0px image, we're done.
        if image_rect_size.is_empty() {
            transparent_mask_layer(&mut resolved_layers);
            continue;
        }

        let space_x = background_positioning_area.width - image_rect_size.width;
        let space_y = background_positioning_area.height - image_rect_size.height;

        let position_x = layer.position_x.to_px(space_x);
        let position_y = layer.position_y.to_px(space_y);

        resolved_layers.push(ResolvedBackgroundLayer {
            image: Some(image),
            attachment: layer.attachment,
            clip: layer.clip,
            position_x,
            position_y,
            background_positioning_area,
            image_rect: CssPixelRect::from_location_and_size(
                crate::css::css_pixels::CssPixelPoint::default(),
                image_rect_size,
            ),
            repeat_x: layer.repeat_x,
            repeat_y: layer.repeat_y,
            compositing_and_blending_operator: blend_mode,
            mask_composite,
        });
    }

    ResolvedBackground {
        color_box,
        layers: resolved_layers,
        needs_text_clip: background_color_clip == css_enums::background_box::TEXT,
        background_rect: border_rect,
        color: background_color,
    }
}

struct LayerImageIntrinsics<'a> {
    is_paintable: bool,
    natural: SizeWithAspectRatio,
    selected_image_value: Option<&'a StyleValueData>,
}

fn image_intrinsic_facts<'a>(
    recorder: &PaintRecorder<'_>,
    paintable: PaintableSlotId,
    image: &LayerImageSource<'a>,
) -> LayerImageIntrinsics<'a> {
    match image.value {
        StyleValueData::LinearGradient { .. }
        | StyleValueData::ConicGradient { .. }
        | StyleValueData::RadialGradient { .. } => LayerImageIntrinsics {
            is_paintable: true,
            natural: SizeWithAspectRatio {
                width: None,
                height: None,
                aspect_ratio: None,
            },
            selected_image_value: None,
        },
        _ => {
            let facts = recorder.paint_host.image_intrinsic_facts(
                recorder.layout_node_shell(paintable),
                image.list,
                image.computed_index,
            );
            LayerImageIntrinsics {
                is_paintable: facts.is_paintable,
                natural: SizeWithAspectRatio {
                    width: facts
                        .has_natural_width
                        .then(|| CssPixels::from_raw(facts.natural_width)),
                    height: facts
                        .has_natural_height
                        .then(|| CssPixels::from_raw(facts.natural_height)),
                    aspect_ratio: facts.has_natural_aspect_ratio.then(|| Fraction {
                        numerator: CssPixels::from_raw(facts.natural_aspect_ratio_numerator),
                        denominator: CssPixels::from_raw(facts.natural_aspect_ratio_denominator),
                    }),
                },
                // SAFETY: The selected image's retained value is kept alive by the memoized
                // layer vector the host resolves `(list, computed_index)` against, which
                // outlives the recording that borrows `image.value` from the same style.
                selected_image_value: facts
                    .has_selected_image_value
                    .then(|| unsafe { &*facts.selected_image_value.cast::<StyleValueData>() }),
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn resolve_background_layers<'a>(
    recorder: &PaintRecorder<'_>,
    paintable: PaintableSlotId,
    style: ComputedValuesView<'a>,
    image_list: FfiLayerImageList,
    background_color: libgfx_rust::Color,
    background_color_clip: u8,
    border_rect: CssPixelRect,
    border_radii: crate::painting::border_radii::BorderRadii,
) -> ResolvedBackground<'a> {
    let layers = computed_background_layers(style, image_list);
    resolve_layers(
        recorder,
        paintable,
        layers,
        background_color,
        background_color_clip,
        border_rect,
        border_radii,
        LayerType::Background,
    )
}

pub(crate) fn resolve_mask_layers<'a>(
    recorder: &PaintRecorder<'_>,
    paintable: PaintableSlotId,
    style: ComputedValuesView<'a>,
    border_rect: CssPixelRect,
) -> ResolvedBackground<'a> {
    let layers = computed_mask_layers(style);
    resolve_layers(
        recorder,
        paintable,
        layers,
        libgfx_rust::Color(0),
        css_enums::background_box::BORDER_BOX,
        border_rect,
        crate::painting::border_radii::BorderRadii::default(),
        LayerType::Mask,
    )
}

pub(crate) fn resolve_background_for_paint<'a>(
    recorder: &PaintRecorder<'a>,
    paintable: PaintableSlotId,
) -> Option<BackgroundPaintInputs<'a>> {
    let layout_arena = recorder.layout_arena;
    let data = recorder.data(paintable);
    let node = data.layout_node;
    let style = layout_arena.node_style_if_live(node)?;

    let node_flags = layout_arena.node_flags_if_live(node);
    let node_is_body = node_flags & crate::layout::node_data::NodeFlag::IsBody as u32 != 0;
    // If the body's background properties were propagated to the root element, do not re-paint the body's background.
    if node_is_body
        && recorder
            .paint_host
            .root_background_source()
            .use_body_background_properties
    {
        return None;
    }

    // https://drafts.csswg.org/css-backgrounds/#root-background
    if style_queries::node_is_root_element(layout_arena, node) {
        let root_background_source = recorder.paint_host.root_background_source();
        let background_rect =
            crate::painting::paintable_geometry::absolute_border_box_rect(recorder.paintables, paintable);
        let border_radii =
            crate::painting::visual_context::node_values::border_radii_data(style, recorder.paintables, paintable);

        let own_color = libgfx_rust::Color(style.background().background_color);
        let body_style = root_background_source
            .use_body_background_properties
            .then(|| layout_arena.node_style_if_live(root_background_source.body_layout_node))
            .flatten();
        let (layers_style, background_color, image_rendering) = if root_background_source.use_body_background_properties
        {
            let background_color = if own_color.alpha() != 0 {
                own_color
            } else {
                body_style.map_or(libgfx_rust::Color(0), |body_style| {
                    libgfx_rust::Color(body_style.background().background_color)
                })
            };
            // If the body's background was propagated to the root element, use the body's image-rendering value.
            let image_rendering =
                body_style.map_or(css_enums::image_rendering::AUTO, ComputedValuesView::image_rendering);
            (body_style, background_color, image_rendering)
        } else {
            (Some(style), own_color, style.image_rendering())
        };

        let mut resolved_background = match layers_style {
            Some(layers_style) => resolve_background_layers(
                recorder,
                paintable,
                layers_style,
                if root_background_source.use_body_background_properties {
                    FfiLayerImageList::DocumentBackground
                } else {
                    FfiLayerImageList::Background
                },
                background_color,
                style.background().background_color_clip,
                background_rect,
                border_radii,
            ),
            None => ResolvedBackground {
                color_box: BackgroundBox {
                    rect: CssPixelRect::default(),
                    radii: crate::painting::border_radii::BorderRadii::default(),
                },
                layers: Vec::new(),
                needs_text_clip: false,
                background_rect: CssPixelRect::default(),
                color: libgfx_rust::Color(0),
            },
        };

        let mut canvas_rect = CssPixelRect::from(recorder.inputs.css_viewport_rect);
        if let Some(overflow_rect) =
            crate::painting::paintable_geometry::scrollable_overflow_rect(recorder.paintables, paintable)
        {
            canvas_rect.unite(overflow_rect);
        }
        resolved_background.background_rect.unite(canvas_rect);
        resolved_background.color_box.rect.unite(canvas_rect);

        return Some(BackgroundPaintInputs {
            resolved: resolved_background,
            border_radii,
            image_rendering,
            is_root_element: true,
        });
    }

    if libgfx_rust::Color(style.background().background_color).alpha() == 0
        && !style_queries::background_layers_have_image(style)
    {
        return None;
    }

    // HACK: If the Box has a border, use the bordered_rect to paint the background.
    //       This way if we have a border-radius there will be no gap between the filling and actual border.
    let zero = CssPixels::from_raw(0);
    let has_css_borders = style.border_top_width() != zero
        || style.border_right_width() != zero
        || style.border_bottom_width() != zero
        || style.border_left_width() != zero;
    let background_rect = if has_css_borders {
        crate::painting::paintable_geometry::absolute_border_box_rect(recorder.paintables, paintable)
    } else {
        crate::painting::paintable_geometry::absolute_padding_box_rect(recorder.paintables, paintable)
    };
    let border_radii =
        crate::painting::visual_context::node_values::border_radii_data(style, recorder.paintables, paintable);
    let resolved_background = resolve_background_layers(
        recorder,
        paintable,
        style,
        FfiLayerImageList::Background,
        libgfx_rust::Color(style.background().background_color),
        style.background().background_color_clip,
        background_rect,
        border_radii,
    );
    Some(BackgroundPaintInputs {
        resolved: resolved_background,
        border_radii,
        image_rendering: style.image_rendering(),
        is_root_element: false,
    })
}
