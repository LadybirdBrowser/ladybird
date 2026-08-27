/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::node_values::{border_radii_data, padding_edge_border_radii, piece_border_radii_data};
use super::{ClipData, ClipMode, ContextRef, FrameData, FrameNodeIndex, FrameRole, PieceKey, VisualContextTree};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::overflow;
use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::NodeSlotId;
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::paintable_data::PaintableKind;
use crate::painting::paintable_geometry::{
    absolute_border_box_rect, absolute_rect, for_each_rendered_inline_box_piece,
};
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::record::paint::fieldset::{legend_paintable, visual_border_box_rect};
use crate::painting::record::paint::svg::svg_image_unquantized_device_rect;
use libgfx_rust::{CornerRadii, FloatRect, IntRect};

pub(crate) type LocalFrames = Vec<(FrameRole, FrameNodeIndex)>;

pub(crate) struct LocalFrameBuilder<'a, Arena: PaintableRowsRead> {
    tree: &'a mut VisualContextTree,
    layout_arena: &'a Arena,
    slot: NodeSlotId,
    kind: PaintableKind,
    converter: DevicePixelConverter,
    frames: Option<LocalFrames>,
}

impl<'a, Arena: PaintableRowsRead> LocalFrameBuilder<'a, Arena> {
    pub(crate) fn new(
        tree: &'a mut VisualContextTree,
        layout_arena: &'a Arena,
        slot: NodeSlotId,
        pixel_ratio: f64,
        keeps_frame_list: bool,
    ) -> Self {
        Self {
            tree,
            layout_arena,
            slot,
            kind: layout_arena.paintable_data(slot).kind,
            converter: DevicePixelConverter::new(pixel_ratio),
            frames: keeps_frame_list.then(LocalFrames::new),
        }
    }

    pub(crate) fn build(mut self, own_state: ContextRef) -> LocalFrames {
        let Some(style) = self.layout_arena.node_style_if_live(self.slot) else {
            return self.finish();
        };
        if self.kind == PaintableKind::InlinePaintable {
            self.append_inline_box_frames(style, own_state);
            return self.finish();
        }

        let border_box_rect = absolute_border_box_rect(self.layout_arena, self.slot);
        let border_radii = border_radii_data(style, self.layout_arena, self.slot);
        self.append_box_shadow_frames(style, own_state, PieceKey::Box, border_box_rect, border_radii);

        match self.kind {
            PaintableKind::ImagePaintable
            | PaintableKind::CanvasPaintable
            | PaintableKind::VideoPaintable
            | PaintableKind::NavigableContainerViewportPaintable => {
                self.append_replaced_content_frames(style, own_state);
            }
            PaintableKind::SVGImagePaintable => self.append_svg_image_frames(style, own_state),
            PaintableKind::FieldSetPaintable => {
                self.append_fieldset_frames(own_state);
            }
            _ => {}
        }
        self.finish()
    }

    fn finish(self) -> LocalFrames {
        self.frames.unwrap_or_default()
    }

    fn append(&mut self, parent: ContextRef, data: FrameData, role: FrameRole) -> ContextRef {
        let frame = self
            .tree
            .append_frame_with_role(data, parent.frame, parent.spatial, role);
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

    fn append_inline_box_frames(&mut self, style: ComputedValuesView<'_>, own_state: ContextRef) {
        let arena = self.layout_arena;
        for_each_rendered_inline_box_piece(arena, self.slot, |position, piece, border_box_rect| {
            let piece_key = PieceKey::Piece(position);
            let border_radii = piece_border_radii_data(
                style,
                piece.border_box_rect.width,
                piece.border_box_rect.height,
                piece.present_edges,
            );
            self.append_box_shadow_frames(style, own_state, piece_key, border_box_rect, border_radii);
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
        let content_always_fills_its_rect = self.kind == PaintableKind::CanvasPaintable;
        if self.kind == PaintableKind::VideoPaintable {
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

    fn append_fieldset_frames(&mut self, own_state: ContextRef) -> ContextRef {
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
            return background_clip;
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
        self.append(
            band_context,
            FrameData::Clip(ClipData {
                rect: cutout.to_float(),
                corner_radii: CornerRadii::default(),
                mode: ClipMode::Difference,
            }),
            FrameRole::LegendCutout,
        );
        background_clip
    }
}
