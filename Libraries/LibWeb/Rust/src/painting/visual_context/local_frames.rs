/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::node_values::padding_edge_border_radii;
use super::{ClipData, ClipMode, ContextRef, FrameData, FrameNodeIndex, FrameRole, VisualContextTree};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::overflow;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::paintable_data::PaintableKind;
use crate::painting::paintable_geometry::absolute_rect;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::record::paint::svg::svg_image_unquantized_device_rect;
use libgfx_rust::{CornerRadii, FloatRect};

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
        match self.kind {
            PaintableKind::ImagePaintable
            | PaintableKind::CanvasPaintable
            | PaintableKind::VideoPaintable
            | PaintableKind::NavigableContainerViewportPaintable => {
                self.append_replaced_content_frames(style, own_state);
            }
            PaintableKind::SVGImagePaintable => self.append_svg_image_frames(style, own_state),
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
}
