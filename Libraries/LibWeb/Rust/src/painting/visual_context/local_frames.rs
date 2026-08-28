/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::node_values::{border_radii_data, padding_edge_border_radii, piece_border_radii_data};
use super::{
    ClipData, ClipMode, ClipPathData, ContextRef, FrameData, FrameNodeIndex, FrameRole, PatternedEdgeOwner, PieceKey,
    VisualContextNodeSink,
};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::{background_box, line_style, overflow};
use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::host::FfiRootBackgroundSource;
use crate::painting::node_painting;
use crate::painting::paintable_data::{BorderEdge, FfiPixelBox};
use crate::painting::paintable_geometry::{
    absolute_border_box_rect, absolute_rect, committed_border, committed_padding,
    committed_uses_collapsing_borders_model, for_each_rendered_inline_box_piece,
};
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::record::paint::background::{BackgroundBox, background_box_for};
use crate::painting::record::paint::background_resolution::{
    ComputedLayerFrameFacts, background_paint_source_from_style_and_geometry, body_background_is_propagated_to_root,
    computed_background_layer_frame_facts, computed_mask_layer_frame_facts,
};
use crate::painting::record::paint::border::{
    ALL_PIECE_EDGES, BordersDataDevicePixels, patterned_edge_solid_region_paths, style_borders_data,
};
use crate::painting::record::paint::fieldset::{fieldset_borders_data, legend_paintable, visual_border_box_rect};
use crate::painting::record::paint::outline::{outline_border_geometry, outline_borders_data};
use crate::painting::record::paint::svg::svg_image_unquantized_device_rect;
use crate::painting::style_queries;
use libgfx_rust::{
    Color, CompositingAndBlendingOperator, CornerRadii, FloatRect, IntRect, WindingRule, enclosing_int_rect,
};
use std::rc::Rc;

pub(crate) type LocalFrames = Vec<(FrameRole, FrameNodeIndex)>;

pub(crate) struct LocalFrameBuilder<'a, Sink: VisualContextNodeSink, Arena: PaintableRowsRead> {
    tree: &'a mut Sink,
    layout_arena: &'a Arena,
    slot: NodeSlotId,
    kind: NodeKind,
    converter: DevicePixelConverter,
    frames: Option<LocalFrames>,
}

struct FieldsetFrames {
    background_clip: ContextRef,
    legend_cutout: Option<ContextRef>,
    device_border_rect: IntRect,
}

#[derive(Clone, Copy)]
struct BackgroundLayerBoxes {
    border_box: BackgroundBox,
    padding: FfiPixelBox,
    border: FfiPixelBox,
}

fn root_background_source_or_no_propagation(source: Option<FfiRootBackgroundSource>) -> FfiRootBackgroundSource {
    source.unwrap_or(FfiRootBackgroundSource {
        use_body_background_properties: false,
        body_layout_node: NodeSlotId::INVALID,
    })
}

impl<'a, Sink: VisualContextNodeSink, Arena: PaintableRowsRead> LocalFrameBuilder<'a, Sink, Arena> {
    pub(crate) fn new(
        tree: &'a mut Sink,
        layout_arena: &'a Arena,
        slot: NodeSlotId,
        pixel_ratio: f64,
        keeps_frame_list: bool,
    ) -> Self {
        Self {
            tree,
            layout_arena,
            slot,
            kind: layout_arena
                .node_kind_if_live(slot)
                .expect("local frame builder requires a live layout node"),
            converter: DevicePixelConverter::new(pixel_ratio),
            frames: keeps_frame_list.then(LocalFrames::new),
        }
    }

    pub(crate) fn build(
        mut self,
        own_state: ContextRef,
        root_background_source: Option<FfiRootBackgroundSource>,
    ) -> LocalFrames {
        let Some(style) = self.layout_arena.node_style_if_live(self.slot) else {
            return self.finish();
        };
        let root_background_source = root_background_source_or_no_propagation(root_background_source);
        if node_painting::is_inline(self.layout_arena, self.slot) {
            self.append_inline_box_frames(style, own_state, root_background_source);
            return self.finish();
        }

        let border_box_rect = absolute_border_box_rect(self.layout_arena, self.slot);
        let border_radii = border_radii_data(style, self.layout_arena, self.slot);
        self.append_box_shadow_frames(style, own_state, PieceKey::Box, border_box_rect, border_radii);

        let mut background_parent = own_state;
        let mut fieldset = None;
        match self.kind {
            NodeKind::ImageBox | NodeKind::CanvasBox | NodeKind::VideoBox | NodeKind::NavigableContainerViewport => {
                self.append_replaced_content_frames(style, own_state);
            }
            NodeKind::SVGImageBox => self.append_svg_image_frames(style, own_state),
            NodeKind::FieldSetBox => {
                let frames = self.append_fieldset_frames(own_state);
                background_parent = frames.background_clip;
                fieldset = Some(frames);
            }
            _ => {}
        }

        if node_painting::paints_box_decorations(self.layout_arena, self.slot) {
            self.append_background_frames(background_parent, root_background_source);
            self.append_border_frames(style, own_state, border_box_rect, border_radii, fieldset.as_ref());
            self.append_outline_frames(style, own_state, PieceKey::Box, border_box_rect, border_radii);
        }
        self.finish()
    }

    pub(crate) fn build_css_mask_layer_frames(
        mut self,
        root_context: ContextRef,
        style: ComputedValuesView<'_>,
        mask_rect: CssPixelRect,
        is_root_element: bool,
    ) -> LocalFrames {
        let layers = computed_mask_layer_frame_facts(style);
        self.append_background_layer_frames(
            PieceKey::Box,
            root_context,
            mask_rect,
            BorderRadii::default(),
            is_root_element,
            false,
            &layers,
        );
        self.finish()
    }

    fn finish(self) -> LocalFrames {
        self.frames.unwrap_or_default()
    }

    fn append(&mut self, parent: ContextRef, data: FrameData, role: FrameRole) -> ContextRef {
        let frame = self.tree.append_frame_node(data, parent.frame, parent.spatial, role);
        if let Some(frames) = &mut self.frames {
            frames.push((role, frame));
        }
        ContextRef { frame, ..parent }
    }

    fn append_clip(
        &mut self,
        parent: ContextRef,
        rect: FloatRect,
        corner_radii: CornerRadii,
        role: FrameRole,
    ) -> ContextRef {
        self.append(
            parent,
            FrameData::Clip(ClipData {
                rect,
                corner_radii,
                mode: ClipMode::Intersect,
            }),
            role,
        )
    }

    fn append_inline_box_frames(
        &mut self,
        style: ComputedValuesView<'_>,
        own_state: ContextRef,
        root_background_source: FfiRootBackgroundSource,
    ) {
        let arena = self.layout_arena;
        let border = committed_border(arena, self.slot);
        let has_borders = style_queries::has_css_borders(style);
        let clips_to_text = style.background().background_color_clip == background_box::TEXT;
        let paints_background = !body_background_is_propagated_to_root(arena, self.slot, root_background_source)
            && (clips_to_text || style_queries::background_layers_have_image(style));
        let background_layers = paints_background.then(|| computed_background_layer_frame_facts(style));
        for_each_rendered_inline_box_piece(arena, self.slot, |position, piece, border_box_rect| {
            let piece_key = PieceKey::Piece(position);
            let border_radii = piece_border_radii_data(
                style,
                piece.border_box_rect.width,
                piece.border_box_rect.height,
                piece.present_edges,
            );
            self.append_box_shadow_frames(style, own_state, piece_key, border_box_rect, border_radii);
            if let Some(layers) = &background_layers {
                let background_rect = if has_borders {
                    border_box_rect
                } else {
                    piece.shrunken_by_present_edges(border_box_rect, border)
                };
                self.append_background_layer_frames(
                    piece_key,
                    own_state,
                    background_rect,
                    border_radii,
                    false,
                    clips_to_text,
                    layers,
                );
            }
            if style_has_patterned_border_edge(style) {
                let borders_data = style_borders_data(style, border, piece.present_edges, &self.converter);
                self.append_patterned_edge_frames(
                    own_state,
                    PatternedEdgeOwner::Border,
                    piece_key,
                    self.converter.rounded_device_rect(border_box_rect),
                    border_radii.as_corners(&self.converter),
                    &borders_data,
                );
            }
            self.append_outline_frames(style, own_state, piece_key, border_box_rect, border_radii);
        });
    }

    fn append_box_shadow_frames(
        &mut self,
        style: ComputedValuesView<'_>,
        own_state: ContextRef,
        piece: PieceKey,
        border_box_rect: CssPixelRect,
        border_radii: BorderRadii,
    ) {
        let shadows = style.effects().box_shadows.as_slice();
        if !shadows.iter().any(|shadow| !shadow.is_inner()) {
            return;
        }
        let corner_radii = border_radii.corners_unconditionally(&self.converter);
        if !corner_radii.has_any_radius() {
            return;
        }
        self.append(
            own_state,
            FrameData::Clip(ClipData {
                rect: self.converter.rounded_device_rect(border_box_rect).to_float(),
                corner_radii,
                mode: ClipMode::Difference,
            }),
            FrameRole::OuterShadowClip { piece },
        );
    }

    fn append_replaced_content_frames(&mut self, style: ComputedValuesView<'_>, own_state: ContextRef) {
        let content_rect = self
            .converter
            .rounded_device_rect(absolute_rect(self.layout_arena, self.slot))
            .to_float();
        let corner_radii =
            padding_edge_border_radii(style, self.layout_arena, self.slot).corners_unconditionally(&self.converter);
        let has_corner_clip = corner_radii.has_any_radius();
        let content_always_fills_its_rect = self.kind == NodeKind::CanvasBox;
        if self.kind == NodeKind::VideoBox {
            let content = self.append_clip(own_state, content_rect, CornerRadii::default(), FrameRole::ContentClip);
            if has_corner_clip {
                self.append_clip(content, content_rect, corner_radii, FrameRole::ContentCornerClip);
            }
            return;
        }
        let mut parent = own_state;
        if has_corner_clip {
            parent = self.append_clip(parent, content_rect, corner_radii, FrameRole::ContentCornerClip);
        }
        if !content_always_fills_its_rect {
            self.append_clip(parent, content_rect, CornerRadii::default(), FrameRole::ContentClip);
        }
    }

    // https://svgwg.org/svg2-draft/embedded.html#ImageElement
    fn append_svg_image_frames(&mut self, style: ComputedValuesView<'_>, own_state: ContextRef) {
        let overflow_is_visible =
            style.box_values().overflow_x == overflow::VISIBLE && style.box_values().overflow_y == overflow::VISIBLE;
        if overflow_is_visible {
            return;
        }
        let image_rect = svg_image_unquantized_device_rect(
            self.layout_arena,
            self.slot,
            self.converter.device_pixels_per_css_pixel(),
        );
        self.append_clip(own_state, image_rect, CornerRadii::default(), FrameRole::ContentClip);
    }

    fn append_fieldset_frames(&mut self, own_state: ContextRef) -> FieldsetFrames {
        let arena = self.layout_arena;
        let css_border_top = arena
            .node_style_if_live(self.slot)
            .map(|style| style.border_top_width())
            .unwrap_or_default();
        let device_border_rect = self
            .converter
            .rounded_device_rect(visual_border_box_rect(arena, self.slot));
        let background_clip = self.append_clip(
            own_state,
            device_border_rect.to_float(),
            CornerRadii::default(),
            FrameRole::FieldsetBackgroundClip,
        );
        let Some(legend) = legend_paintable(arena, self.slot) else {
            return FieldsetFrames {
                background_clip,
                legend_cutout: None,
                device_border_rect,
            };
        };
        let legend_border_rect = self
            .converter
            .rounded_device_rect(absolute_border_box_rect(arena, legend));
        let top_border = self.converter.enclosing_device_pixels(css_border_top);
        let band = IntRect::new(
            device_border_rect.x,
            device_border_rect.y,
            device_border_rect.width,
            top_border,
        );
        let band_context = self.append_clip(
            own_state,
            band.to_float(),
            CornerRadii::default(),
            FrameRole::FieldsetTopBorderBand,
        );
        let cutout = IntRect::new(
            legend_border_rect.x,
            device_border_rect.y,
            legend_border_rect.width,
            top_border,
        );
        let legend_cutout = self.append(
            band_context,
            FrameData::Clip(ClipData {
                rect: cutout.to_float(),
                corner_radii: CornerRadii::default(),
                mode: ClipMode::Difference,
            }),
            FrameRole::LegendCutout,
        );
        FieldsetFrames {
            background_clip,
            legend_cutout: Some(legend_cutout),
            device_border_rect,
        }
    }

    fn append_background_frames(
        &mut self,
        background_parent: ContextRef,
        root_background_source: FfiRootBackgroundSource,
    ) {
        let Some(source) =
            background_paint_source_from_style_and_geometry(self.layout_arena, self.slot, root_background_source)
        else {
            return;
        };
        let Some(layers_style) = source.layers_style_if_live else {
            return;
        };
        let clips_to_text = !source.is_root_element && source.background_color_clip == background_box::TEXT;
        if !clips_to_text && !style_queries::background_layers_have_image(layers_style) {
            return;
        }
        let layers = computed_background_layer_frame_facts(layers_style);
        self.append_background_layer_frames(
            PieceKey::Box,
            background_parent,
            source.background_rect,
            source.border_radii,
            source.is_root_element,
            clips_to_text,
            &layers,
        );
    }

    // https://drafts.csswg.org/css-backgrounds-4/#valdef-background-clip-text
    // https://drafts.fxtf.org/compositing/#background-blend-mode
    #[allow(clippy::too_many_arguments)]
    fn append_background_layer_frames(
        &mut self,
        piece: PieceKey,
        background_parent: ContextRef,
        background_rect: CssPixelRect,
        border_radii: BorderRadii,
        is_root_element: bool,
        clips_to_text: bool,
        layers: &[ComputedLayerFrameFacts],
    ) {
        let mut parent = background_parent;
        if clips_to_text {
            let text_clip = self.append_clip(
                parent,
                self.converter.rounded_device_rect(background_rect).to_float(),
                CornerRadii::default(),
                FrameRole::BackgroundTextClip { piece },
            );
            parent = self.append(
                text_clip,
                FrameData::layer_blending_with(CompositingAndBlendingOperator::Normal),
                FrameRole::BackgroundTextContentLayer { piece },
            );
            self.append(
                parent,
                FrameData::layer_blending_with(CompositingAndBlendingOperator::DestinationIn),
                FrameRole::BackgroundTextMask { piece },
            );
        }
        let boxes = BackgroundLayerBoxes {
            border_box: BackgroundBox {
                rect: background_rect,
                radii: border_radii,
            },
            padding: committed_padding(self.layout_arena, self.slot),
            border: committed_border(self.layout_arena, self.slot),
        };
        let some_layer_blends = layers
            .iter()
            .any(|layer| layer.may_be_painted && layer.blend_mode != CompositingAndBlendingOperator::Normal);
        if !some_layer_blends {
            self.append_background_layer_chain(piece, parent, boxes, is_root_element, layers, false);
            return;
        }
        let isolation = self.append(
            parent,
            FrameData::layer_blending_with(CompositingAndBlendingOperator::Normal),
            FrameRole::BackgroundIsolation { piece },
        );
        self.append_background_layer_chain(piece, isolation, boxes, is_root_element, layers, true);
        if layers.iter().filter(|layer| layer.may_be_painted).count() == 1 {
            self.append_background_layer_chain(piece, parent, boxes, is_root_element, layers, false);
        }
    }

    fn append_background_layer_chain(
        &mut self,
        piece: PieceKey,
        chain_parent: ContextRef,
        boxes: BackgroundLayerBoxes,
        is_root_element: bool,
        layers: &[ComputedLayerFrameFacts],
        isolated: bool,
    ) {
        for (index, layer) in layers.iter().enumerate() {
            if !layer.may_be_painted {
                continue;
            }
            let layer_index = index as u16;
            let mut layer_parent = chain_parent;
            if !is_root_element {
                let clip_box = background_box_for(layer.clip, boxes.border_box, boxes.padding, boxes.border);
                let clip_rect = self.converter.rounded_device_rect(clip_box.rect).to_float();
                let corner_radii = clip_box.radii.corners_unconditionally(&self.converter);
                if corner_radii.has_any_radius() {
                    layer_parent = self.append_clip(
                        layer_parent,
                        clip_rect,
                        corner_radii,
                        FrameRole::BackgroundLayerCornerClip {
                            piece,
                            layer: layer_index,
                            isolated,
                        },
                    );
                }
                layer_parent = self.append_clip(
                    layer_parent,
                    clip_rect,
                    CornerRadii::default(),
                    FrameRole::BackgroundLayerClip {
                        piece,
                        layer: layer_index,
                        isolated,
                    },
                );
            }
            let blend_layer_operator = layer.blend_layer_operator();
            if blend_layer_operator != CompositingAndBlendingOperator::Normal {
                self.append(
                    layer_parent,
                    FrameData::layer_blending_with(blend_layer_operator),
                    FrameRole::BackgroundLayerBlend {
                        piece,
                        layer: layer_index,
                        isolated,
                    },
                );
            }
        }
    }

    fn append_border_frames(
        &mut self,
        style: ComputedValuesView<'_>,
        own_state: ContextRef,
        border_box_rect: CssPixelRect,
        border_radii: BorderRadii,
        fieldset: Option<&FieldsetFrames>,
    ) {
        if !style_has_patterned_border_edge(style) {
            return;
        }
        let corner_radii = border_radii.as_corners(&self.converter);
        if let Some(FieldsetFrames {
            legend_cutout: Some(legend_cutout),
            device_border_rect,
            ..
        }) = fieldset
        {
            let borders = fieldset_borders_data(style, &self.converter);
            self.append_patterned_edge_frames(
                own_state,
                PatternedEdgeOwner::Border,
                PieceKey::Box,
                *device_border_rect,
                corner_radii,
                &borders.without_top,
            );
            self.append_patterned_edge_frames(
                *legend_cutout,
                PatternedEdgeOwner::Border,
                PieceKey::Box,
                *device_border_rect,
                corner_radii,
                &borders.top_only,
            );
        } else if !committed_uses_collapsing_borders_model(self.layout_arena, self.slot) {
            let border = committed_border(self.layout_arena, self.slot);
            let borders_data = style_borders_data(style, border, ALL_PIECE_EDGES, &self.converter);
            self.append_patterned_edge_frames(
                own_state,
                PatternedEdgeOwner::Border,
                PieceKey::Box,
                self.converter.rounded_device_rect(border_box_rect),
                corner_radii,
                &borders_data,
            );
        }
    }

    fn append_outline_frames(
        &mut self,
        style: ComputedValuesView<'_>,
        own_state: ContextRef,
        piece: PieceKey,
        border_box_rect: CssPixelRect,
        border_radii: BorderRadii,
    ) {
        let Some(outline) = style_queries::outline_geometry(style) else {
            return;
        };
        if !is_patterned(outline.line_style) {
            return;
        }
        let outline_offset = style.misc_reset().outline_offset;
        let (borders_rect, border_radius_data) =
            outline_border_geometry(outline.width, outline_offset, border_box_rect, border_radii);
        let borders_data = outline_borders_data(outline, Color::TRANSPARENT, &self.converter);
        self.append_patterned_edge_frames(
            own_state,
            PatternedEdgeOwner::Outline,
            piece,
            self.converter.rounded_device_rect(borders_rect),
            border_radius_data.as_corners(&self.converter),
            &borders_data,
        );
    }

    fn append_patterned_edge_frames(
        &mut self,
        parent: ContextRef,
        owner: PatternedEdgeOwner,
        piece: PieceKey,
        border_rect: IntRect,
        corner_radii: CornerRadii,
        borders_data: &BordersDataDevicePixels,
    ) {
        let paths = patterned_edge_solid_region_paths(border_rect, corner_radii, borders_data);
        for (edge, path) in BorderEdge::ALL.into_iter().zip(paths) {
            let Some(path) = path else {
                continue;
            };
            let bounding_rect = enclosing_int_rect(FloatRect::from_array(path.bounding_box()));
            self.append(
                parent,
                FrameData::ClipPath(ClipPathData {
                    path: Rc::new(path),
                    bounding_rect,
                    fill_rule: WindingRule::EvenOdd,
                }),
                FrameRole::PatternedEdge { owner, piece, edge },
            );
        }
    }
}

fn is_patterned(line_style: u8) -> bool {
    matches!(line_style, line_style::DASHED | line_style::DOTTED)
}

fn style_has_patterned_border_edge(style: ComputedValuesView<'_>) -> bool {
    is_patterned(style.border_top_style())
        || is_patterned(style.border_right_style())
        || is_patterned(style.border_bottom_style())
        || is_patterned(style.border_left_style())
}
