/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{
    AnchorScrollShift, BackfaceVisibilityData, ClipData, ClipMode, ClipPathData, EffectsData, FrameData, FrameNode,
    FrameNodeIndex, MaskData, MaskLayerOrigin, PerspectiveData, ScrollData, SpatialData, SpatialNode, SpatialNodeIndex,
    StickyData, TransformData, TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree,
    scroll_state::NO_SCROLL_STATE_SLOT,
};
use crate::layout::node_data::NodeSlotId;
use libgfx_rust::path::OwnedPath;
use libgfx_rust::{
    CompositingAndBlendingOperator, CornerRadii, CornerRadius, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize,
    IntRect, MaskKind, WindingRule,
};
use std::rc::Rc;

const SERIALIZED_TREE_MAGIC: u32 = 0x5443_5641;
const SERIALIZED_TREE_FORMAT: u32 = 1;

const SPATIAL_KIND_SCROLL: u8 = 0;
const SPATIAL_KIND_STICKY: u8 = 1;
const SPATIAL_KIND_TRANSFORM: u8 = 2;
const SPATIAL_KIND_PERSPECTIVE: u8 = 3;
const SPATIAL_KIND_BACKFACE_VISIBILITY: u8 = 4;
const SPATIAL_KIND_ANCHOR_SCROLL_SHIFT: u8 = 5;
const SPATIAL_KIND_DEAD: u8 = 6;

const FRAME_KIND_CLIP: u8 = 0;
const FRAME_KIND_CLIP_PATH: u8 = 1;
const FRAME_KIND_EFFECTS: u8 = 2;
const FRAME_KIND_MASK: u8 = 3;
const FRAME_KIND_DEAD: u8 = 4;

const MINIMUM_SERIALIZED_SPATIAL_NODE_SIZE: usize = 5;
const MINIMUM_SERIALIZED_FRAME_NODE_SIZE: usize = 9;

#[derive(Default)]
struct TreeByteWriter {
    bytes: Vec<u8>,
}

impl TreeByteWriter {
    fn u8(&mut self, value: u8) {
        self.bytes.push(value);
    }
    fn bool(&mut self, value: bool) {
        self.u8(u8::from(value));
    }
    fn u32(&mut self, value: u32) {
        self.bytes.extend_from_slice(&value.to_ne_bytes());
    }
    fn i32(&mut self, value: i32) {
        self.bytes.extend_from_slice(&value.to_ne_bytes());
    }
    fn u64(&mut self, value: u64) {
        self.bytes.extend_from_slice(&value.to_ne_bytes());
    }
    fn f32(&mut self, value: f32) {
        self.bytes.extend_from_slice(&value.to_ne_bytes());
    }
    fn optional_f32(&mut self, value: Option<f32>) {
        self.bool(value.is_some());
        self.f32(value.unwrap_or_default());
    }
    fn spatial_index(&mut self, index: SpatialNodeIndex) {
        self.u32(index.0);
    }
    fn optional_spatial_index(&mut self, index: Option<SpatialNodeIndex>) {
        self.bool(index.is_some());
        self.u32(index.map_or(0, |index| index.0));
    }
    fn frame_index(&mut self, index: FrameNodeIndex) {
        self.u32(index.0);
    }
    fn point(&mut self, point: FloatPoint) {
        self.f32(point.x);
        self.f32(point.y);
    }
    fn size(&mut self, size: FloatSize) {
        self.f32(size.width);
        self.f32(size.height);
    }
    fn float_rect(&mut self, rect: FloatRect) {
        self.f32(rect.x);
        self.f32(rect.y);
        self.f32(rect.width);
        self.f32(rect.height);
    }
    fn int_rect(&mut self, rect: IntRect) {
        self.i32(rect.x);
        self.i32(rect.y);
        self.i32(rect.width);
        self.i32(rect.height);
    }
    fn matrix(&mut self, matrix: FloatMatrix4x4) {
        for row in matrix.elements {
            for element in row {
                self.f32(element);
            }
        }
    }
    fn corner_radius(&mut self, radius: CornerRadius) {
        self.i32(radius.horizontal_radius);
        self.i32(radius.vertical_radius);
    }
    fn corner_radii(&mut self, radii: CornerRadii) {
        self.corner_radius(radii.top_left);
        self.corner_radius(radii.top_right);
        self.corner_radius(radii.bottom_right);
        self.corner_radius(radii.bottom_left);
    }
    fn length_prefixed_bytes(&mut self, bytes: &[u8]) {
        self.u32(bytes.len() as u32);
        self.bytes.extend_from_slice(bytes);
    }
}

struct TreeByteReader<'a> {
    bytes: &'a [u8],
    position: usize,
}

impl<'a> TreeByteReader<'a> {
    fn remaining(&self) -> usize {
        self.bytes.len() - self.position
    }
    fn take(&mut self, count: usize) -> Option<&'a [u8]> {
        if count > self.remaining() {
            return None;
        }
        let slice = &self.bytes[self.position..self.position + count];
        self.position += count;
        Some(slice)
    }
    fn u8(&mut self) -> Option<u8> {
        self.take(1).map(|bytes| bytes[0])
    }
    fn bool(&mut self) -> Option<bool> {
        match self.u8()? {
            0 => Some(false),
            1 => Some(true),
            _ => None,
        }
    }
    fn u32(&mut self) -> Option<u32> {
        self.take(4)
            .map(|bytes| u32::from_ne_bytes(bytes.try_into().expect("four bytes")))
    }
    fn i32(&mut self) -> Option<i32> {
        self.take(4)
            .map(|bytes| i32::from_ne_bytes(bytes.try_into().expect("four bytes")))
    }
    fn u64(&mut self) -> Option<u64> {
        self.take(8)
            .map(|bytes| u64::from_ne_bytes(bytes.try_into().expect("eight bytes")))
    }
    fn f32(&mut self) -> Option<f32> {
        self.take(4)
            .map(|bytes| f32::from_ne_bytes(bytes.try_into().expect("four bytes")))
    }
    fn optional_f32(&mut self) -> Option<Option<f32>> {
        let has_value = self.bool()?;
        let value = self.f32()?;
        Some(has_value.then_some(value))
    }
    fn spatial_index(&mut self) -> Option<SpatialNodeIndex> {
        self.u32().map(SpatialNodeIndex)
    }
    fn optional_spatial_index(&mut self) -> Option<Option<SpatialNodeIndex>> {
        let has_value = self.bool()?;
        let index = self.u32()?;
        Some(has_value.then_some(SpatialNodeIndex(index)))
    }
    fn frame_index(&mut self) -> Option<FrameNodeIndex> {
        self.u32().map(FrameNodeIndex)
    }
    fn point(&mut self) -> Option<FloatPoint> {
        Some(FloatPoint {
            x: self.f32()?,
            y: self.f32()?,
        })
    }
    fn size(&mut self) -> Option<FloatSize> {
        Some(FloatSize {
            width: self.f32()?,
            height: self.f32()?,
        })
    }
    fn float_rect(&mut self) -> Option<FloatRect> {
        Some(FloatRect::new(self.f32()?, self.f32()?, self.f32()?, self.f32()?))
    }
    fn int_rect(&mut self) -> Option<IntRect> {
        Some(IntRect::new(self.i32()?, self.i32()?, self.i32()?, self.i32()?))
    }
    fn matrix(&mut self) -> Option<FloatMatrix4x4> {
        let mut elements = [[0.0f32; 4]; 4];
        for row in &mut elements {
            for element in row {
                *element = self.f32()?;
            }
        }
        Some(FloatMatrix4x4 { elements })
    }
    fn corner_radius(&mut self) -> Option<CornerRadius> {
        Some(CornerRadius {
            horizontal_radius: self.i32()?,
            vertical_radius: self.i32()?,
        })
    }
    fn corner_radii(&mut self) -> Option<CornerRadii> {
        Some(CornerRadii {
            top_left: self.corner_radius()?,
            top_right: self.corner_radius()?,
            bottom_right: self.corner_radius()?,
            bottom_left: self.corner_radius()?,
        })
    }
    fn length_prefixed_bytes(&mut self) -> Option<&'a [u8]> {
        let length = self.u32()? as usize;
        self.take(length)
    }
    fn winding_rule(&mut self) -> Option<WindingRule> {
        match self.i32()? {
            value if value == WindingRule::Nonzero as i32 => Some(WindingRule::Nonzero),
            value if value == WindingRule::EvenOdd as i32 => Some(WindingRule::EvenOdd),
            _ => None,
        }
    }
    fn mask_kind(&mut self) -> Option<MaskKind> {
        match self.i32()? {
            value if value == MaskKind::Alpha as i32 => Some(MaskKind::Alpha),
            value if value == MaskKind::Luminance as i32 => Some(MaskKind::Luminance),
            _ => None,
        }
    }
    fn compositing_and_blending_operator(&mut self) -> Option<CompositingAndBlendingOperator> {
        CompositingAndBlendingOperator::from_i32(self.i32()?)
    }
    fn clip_mode(&mut self) -> Option<ClipMode> {
        match self.u8()? {
            value if value == ClipMode::Intersect as u8 => Some(ClipMode::Intersect),
            value if value == ClipMode::Difference as u8 => Some(ClipMode::Difference),
            _ => None,
        }
    }
    fn transform_role(&mut self) -> Option<TransformDataRole> {
        match self.u8()? {
            value if value == TransformDataRole::CssTransform as u8 => Some(TransformDataRole::CssTransform),
            value if value == TransformDataRole::SvgViewportTransform as u8 => {
                Some(TransformDataRole::SvgViewportTransform)
            }
            _ => None,
        }
    }
    fn mask_layer_origin(&mut self) -> Option<MaskLayerOrigin> {
        match self.u8()? {
            value if value == MaskLayerOrigin::CssMaskLayers as u8 => Some(MaskLayerOrigin::CssMaskLayers),
            value if value == MaskLayerOrigin::SvgMask as u8 => Some(MaskLayerOrigin::SvgMask),
            value if value == MaskLayerOrigin::SvgClip as u8 => Some(MaskLayerOrigin::SvgClip),
            _ => None,
        }
    }
}

fn write_spatial_data(writer: &mut TreeByteWriter, data: &SpatialData) {
    match data {
        SpatialData::Scroll(_) => writer.u8(SPATIAL_KIND_SCROLL),
        SpatialData::Sticky(sticky) => {
            writer.u8(SPATIAL_KIND_STICKY);
            writer.spatial_index(sticky.scroller);
            writer.optional_spatial_index(sticky.parent_sticky);
            writer.point(sticky.position_relative_to_scroller);
            writer.size(sticky.border_box_size);
            writer.size(sticky.scrollport_size);
            writer.float_rect(sticky.containing_block_region);
            writer.bool(sticky.needs_parent_offset_adjustment);
            writer.optional_f32(sticky.inset_top);
            writer.optional_f32(sticky.inset_right);
            writer.optional_f32(sticky.inset_bottom);
            writer.optional_f32(sticky.inset_left);
        }
        SpatialData::Transform(transform) => {
            writer.u8(SPATIAL_KIND_TRANSFORM);
            writer.matrix(transform.matrix);
            writer.point(transform.origin);
            writer.optional_spatial_index(transform.sorting_context_root_index);
            writer.bool(transform.flattens_inherited_transform);
            writer.u8(transform.role as u8);
            writer.bool(transform.synthetic_plane);
            writer.bool(transform.establishes_sorting_context);
        }
        SpatialData::Perspective(perspective) => {
            writer.u8(SPATIAL_KIND_PERSPECTIVE);
            writer.matrix(perspective.matrix);
            writer.bool(perspective.flattens_inherited_transform);
        }
        SpatialData::BackfaceVisibility(backface) => {
            writer.u8(SPATIAL_KIND_BACKFACE_VISIBILITY);
            writer.spatial_index(backface.plane_root_index);
            writer.bool(backface.flattens_inherited_transform);
        }
        SpatialData::AnchorScrollShift(shift) => {
            writer.u8(SPATIAL_KIND_ANCHOR_SCROLL_SHIFT);
            writer.spatial_index(shift.scroll_node_index);
            writer.bool(shift.negate);
            writer.bool(shift.compensate_horizontal_scroll);
            writer.bool(shift.compensate_vertical_scroll);
        }
        SpatialData::Dead => writer.u8(SPATIAL_KIND_DEAD),
    }
}

fn read_spatial_data(reader: &mut TreeByteReader<'_>) -> Option<SpatialData> {
    Some(match reader.u8()? {
        SPATIAL_KIND_SCROLL => SpatialData::Scroll(ScrollData {
            state_slot: NO_SCROLL_STATE_SLOT,
            owner_paintable: NodeSlotId::INVALID,
            registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
        }),
        SPATIAL_KIND_STICKY => {
            let scroller = reader.spatial_index()?;
            let parent_sticky = reader.optional_spatial_index()?;
            SpatialData::Sticky(StickyData {
                scroller,
                parent_sticky,
                position_relative_to_scroller: reader.point()?,
                border_box_size: reader.size()?,
                scrollport_size: reader.size()?,
                containing_block_region: reader.float_rect()?,
                needs_parent_offset_adjustment: reader.bool()?,
                inset_top: reader.optional_f32()?,
                inset_right: reader.optional_f32()?,
                inset_bottom: reader.optional_f32()?,
                inset_left: reader.optional_f32()?,
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: parent_sticky.unwrap_or(scroller),
            })
        }
        SPATIAL_KIND_TRANSFORM => SpatialData::Transform(TransformData {
            matrix: reader.matrix()?,
            origin: reader.point()?,
            sorting_context_root_index: reader.optional_spatial_index()?,
            flattens_inherited_transform: reader.bool()?,
            role: reader.transform_role()?,
            synthetic_plane: reader.bool()?,
            establishes_sorting_context: reader.bool()?,
        }),
        SPATIAL_KIND_PERSPECTIVE => SpatialData::Perspective(PerspectiveData {
            matrix: reader.matrix()?,
            flattens_inherited_transform: reader.bool()?,
        }),
        SPATIAL_KIND_BACKFACE_VISIBILITY => SpatialData::BackfaceVisibility(BackfaceVisibilityData {
            plane_root_index: reader.spatial_index()?,
            flattens_inherited_transform: reader.bool()?,
        }),
        SPATIAL_KIND_ANCHOR_SCROLL_SHIFT => SpatialData::AnchorScrollShift(AnchorScrollShift {
            scroll_node_index: reader.spatial_index()?,
            negate: reader.bool()?,
            compensate_horizontal_scroll: reader.bool()?,
            compensate_vertical_scroll: reader.bool()?,
        }),
        SPATIAL_KIND_DEAD => SpatialData::Dead,
        _ => return None,
    })
}

fn write_frame_data(writer: &mut TreeByteWriter, data: &FrameData) {
    match data {
        FrameData::Clip(clip) => {
            writer.u8(FRAME_KIND_CLIP);
            writer.float_rect(clip.rect);
            writer.corner_radii(clip.corner_radii);
            writer.u8(clip.mode as u8);
        }
        FrameData::ClipPath(clip_path) => {
            writer.u8(FRAME_KIND_CLIP_PATH);
            writer.int_rect(clip_path.bounding_rect);
            writer.i32(clip_path.fill_rule as i32);
            writer.length_prefixed_bytes(&clip_path.path.serialize_to_bytes());
        }
        FrameData::Effects(effects) => {
            writer.u8(FRAME_KIND_EFFECTS);
            writer.f32(effects.opacity);
            writer.i32(effects.blend_mode as i32);
            writer.bool(effects.filter.is_some());
            writer.length_prefixed_bytes(effects.filter.as_deref().map_or(&[], Vec::as_slice));
        }
        FrameData::Mask(mask) => {
            writer.u8(FRAME_KIND_MASK);
            writer.int_rect(mask.rect);
            writer.i32(mask.kind as i32);
            writer.u8(mask.origin as u8);
        }
        FrameData::Dead => writer.u8(FRAME_KIND_DEAD),
    }
}

fn read_frame_data(reader: &mut TreeByteReader<'_>) -> Option<FrameData> {
    Some(match reader.u8()? {
        FRAME_KIND_CLIP => FrameData::Clip(ClipData {
            rect: reader.float_rect()?,
            corner_radii: reader.corner_radii()?,
            mode: reader.clip_mode()?,
        }),
        FRAME_KIND_CLIP_PATH => {
            let bounding_rect = reader.int_rect()?;
            let fill_rule = reader.winding_rule()?;
            let path = OwnedPath::from_serialized_bytes(reader.length_prefixed_bytes()?);
            FrameData::ClipPath(ClipPathData {
                path: Rc::new(path),
                bounding_rect,
                fill_rule,
            })
        }
        FRAME_KIND_EFFECTS => {
            let opacity = reader.f32()?;
            let blend_mode = reader.compositing_and_blending_operator()?;
            let has_filter = reader.bool()?;
            let filter_bytes = reader.length_prefixed_bytes()?;
            FrameData::Effects(EffectsData {
                opacity,
                blend_mode,
                filter: has_filter.then(|| Rc::new(filter_bytes.to_vec())),
            })
        }
        FRAME_KIND_MASK => FrameData::Mask(MaskData {
            rect: reader.int_rect()?,
            kind: reader.mask_kind()?,
            origin: reader.mask_layer_origin()?,
        }),
        FRAME_KIND_DEAD => FrameData::Dead,
        _ => return None,
    })
}

fn spatial_node_has_ancestor(nodes: &[SpatialNode], mut node: usize, ancestor: SpatialNodeIndex) -> bool {
    while node != VISUAL_VIEWPORT_NODE_INDEX.0 as usize {
        node = nodes[node].parent.0 as usize;
        if node == ancestor.0 as usize {
            return true;
        }
    }
    false
}

impl VisualContextTree {
    fn decoded_references_are_consistent(&self) -> bool {
        let spatial_nodes = &self.spatial_nodes;
        let root = &spatial_nodes[VISUAL_VIEWPORT_NODE_INDEX.0 as usize];
        if root.parent != VISUAL_VIEWPORT_NODE_INDEX || !matches!(root.data, SpatialData::Transform(_)) {
            return false;
        }
        let spatial_order = self.spatial_dependency_order_with_back_edges();
        if !spatial_order.back_edges.is_empty() || !spatial_order.dangling_references.is_empty() {
            return false;
        }
        for (index, node) in spatial_nodes.iter().enumerate() {
            match &node.data {
                // The hit-test walk looks the plane root up on the marker's root path, so a node on
                // another branch must be rejected here rather than fail that lookup.
                SpatialData::BackfaceVisibility(backface) => {
                    if !spatial_node_has_ancestor(spatial_nodes, index, backface.plane_root_index) {
                        return false;
                    }
                }
                // resolve_sticky_offsets() reads the referenced nodes by kind, so a hostile tree must not
                // get past this point with references of the wrong kind.
                SpatialData::Sticky(sticky) => {
                    let scroller_is_valid = sticky.scroller == VISUAL_VIEWPORT_NODE_INDEX
                        || matches!(spatial_nodes[sticky.scroller.0 as usize].data, SpatialData::Scroll(_));
                    let parent_sticky_is_valid = sticky
                        .parent_sticky
                        .is_none_or(|parent| matches!(spatial_nodes[parent.0 as usize].data, SpatialData::Sticky(_)));
                    if !scroller_is_valid || !parent_sticky_is_valid {
                        return false;
                    }
                }
                _ => {}
            }
        }
        for node in &self.frame_nodes {
            let spatial_is_out_of_range = node.spatial.0 as usize >= spatial_nodes.len();
            let live_frame_in_a_tombstone = node.data.is_live() && !self.spatial_is_live(node.spatial);
            if spatial_is_out_of_range || live_frame_in_a_tombstone {
                return false;
            }
        }
        let frame_order = self.frame_dependency_order_with_back_edges();
        if !frame_order.back_edges.is_empty() || !frame_order.dangling_references.is_empty() {
            return false;
        }
        if let Some(root_isolation_frame) = self.root_isolation_frame {
            let index = root_isolation_frame.0 as usize;
            if index >= self.frame_nodes.len() || !matches!(self.frame_nodes[index].data, FrameData::Effects(_)) {
                return false;
            }
        }
        true
    }
}

impl VisualContextTree {
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut writer = TreeByteWriter::default();
        writer.u32(SERIALIZED_TREE_MAGIC);
        writer.u32(SERIALIZED_TREE_FORMAT);
        writer.u64(self.structural_epoch);
        writer.bool(self.root_is_visual_viewport);
        writer.frame_index(self.root_isolation_frame.unwrap_or(FrameNodeIndex::NONE));
        writer.u32(self.spatial_nodes.len() as u32);
        writer.u32(self.frame_nodes.len() as u32);
        for node in &self.spatial_nodes {
            writer.spatial_index(node.parent);
            write_spatial_data(&mut writer, &node.data);
        }
        for node in &self.frame_nodes {
            writer.frame_index(node.parent);
            writer.spatial_index(node.spatial);
            write_frame_data(&mut writer, &node.data);
        }
        writer.bytes
    }

    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        let mut reader = TreeByteReader { bytes, position: 0 };
        if reader.u32()? != SERIALIZED_TREE_MAGIC || reader.u32()? != SERIALIZED_TREE_FORMAT {
            return None;
        }
        let structural_epoch = reader.u64()?;
        let root_is_visual_viewport = reader.bool()?;
        let root_isolation_frame = reader.frame_index()?;
        let spatial_count = reader.u32()? as usize;
        let frame_count = reader.u32()? as usize;
        if spatial_count == 0
            || spatial_count > reader.remaining() / MINIMUM_SERIALIZED_SPATIAL_NODE_SIZE
            || frame_count > reader.remaining() / MINIMUM_SERIALIZED_FRAME_NODE_SIZE
        {
            return None;
        }

        let mut spatial_nodes = Vec::with_capacity(spatial_count);
        for _ in 0..spatial_count {
            let parent = reader.spatial_index()?;
            let data = read_spatial_data(&mut reader)?;
            spatial_nodes.push(SpatialNode { data, parent });
        }

        let mut frame_nodes = Vec::with_capacity(frame_count);
        for _ in 0..frame_count {
            let parent = reader.frame_index()?;
            let spatial = reader.spatial_index()?;
            let data = read_frame_data(&mut reader)?;
            frame_nodes.push(FrameNode::new(data, parent, spatial));
        }

        if reader.remaining() != 0 {
            return None;
        }

        let live_spatial_node_count = spatial_nodes.iter().filter(|node| node.data.is_live()).count() as u32;
        let live_frame_node_count = frame_nodes.iter().filter(|node| node.data.is_live()).count() as u32;
        let tree = Self {
            spatial_nodes,
            frame_nodes,
            root_is_visual_viewport,
            root_isolation_frame: (!root_isolation_frame.is_none()).then_some(root_isolation_frame),
            structural_epoch,
            live_spatial_node_count,
            live_frame_node_count,
            free_spatial_slots: Vec::new(),
            free_frame_slots: Vec::new(),
            quarantined_spatial_slots: Vec::new(),
            quarantined_frame_slots: Vec::new(),
        };
        tree.decoded_references_are_consistent().then_some(tree)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libgfx_rust::{perspective_matrix, translation_matrix};

    fn transform(matrix: FloatMatrix4x4) -> TransformData {
        TransformData {
            matrix,
            origin: FloatPoint { x: 1.0, y: 2.0 },
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        }
    }

    fn tree_with_every_node_kind() -> VisualContextTree {
        let mut tree = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        tree.structural_epoch = 42;
        let scroll_node = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let sorting_root = tree.append_spatial(
            SpatialData::Transform(TransformData {
                sorting_context_root_index: Some(VISUAL_VIEWPORT_NODE_INDEX),
                flattens_inherited_transform: true,
                role: TransformDataRole::SvgViewportTransform,
                synthetic_plane: true,
                establishes_sorting_context: false,
                ..transform(translation_matrix(3.0, 4.0, 5.0))
            }),
            scroll_node,
        );
        let outer_sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData {
                scroller: scroll_node,
                parent_sticky: None,
                position_relative_to_scroller: FloatPoint { x: 1.0, y: 100.0 },
                border_box_size: FloatSize {
                    width: 800.0,
                    height: 50.0,
                },
                scrollport_size: FloatSize {
                    width: 800.0,
                    height: 600.0,
                },
                containing_block_region: FloatRect::new(0.0, 0.0, 800.0, 2000.0),
                needs_parent_offset_adjustment: true,
                inset_top: Some(0.0),
                inset_right: None,
                inset_bottom: Some(2.5),
                inset_left: None,
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: scroll_node,
            }),
            scroll_node,
        );
        tree.append_spatial(
            SpatialData::Sticky(StickyData {
                scroller: scroll_node,
                parent_sticky: Some(outer_sticky),
                position_relative_to_scroller: FloatPoint { x: 0.0, y: 110.0 },
                border_box_size: FloatSize {
                    width: 800.0,
                    height: 10.0,
                },
                scrollport_size: FloatSize {
                    width: 800.0,
                    height: 600.0,
                },
                containing_block_region: FloatRect::new(0.0, 100.0, 800.0, 50.0),
                needs_parent_offset_adjustment: false,
                inset_top: None,
                inset_right: Some(1.0),
                inset_bottom: None,
                inset_left: Some(3.0),
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: scroll_node,
            }),
            outer_sticky,
        );
        let perspective = tree.append_spatial(
            SpatialData::Perspective(PerspectiveData {
                matrix: perspective_matrix(1000.0),
                flattens_inherited_transform: true,
            }),
            sorting_root,
        );
        tree.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: sorting_root,
                flattens_inherited_transform: false,
            }),
            perspective,
        );
        tree.append_spatial(
            SpatialData::AnchorScrollShift(AnchorScrollShift {
                scroll_node_index: scroll_node,
                negate: true,
                compensate_horizontal_scroll: false,
                compensate_vertical_scroll: true,
            }),
            scroll_node,
        );

        let clip = tree.append_frame(
            FrameData::Clip(ClipData {
                rect: FloatRect::new(0.5, 1.0, 20.0, 30.0),
                corner_radii: CornerRadii {
                    top_left: CornerRadius {
                        horizontal_radius: 1,
                        vertical_radius: 2,
                    },
                    top_right: CornerRadius {
                        horizontal_radius: 3,
                        vertical_radius: 4,
                    },
                    bottom_right: CornerRadius {
                        horizontal_radius: 5,
                        vertical_radius: 6,
                    },
                    bottom_left: CornerRadius {
                        horizontal_radius: 7,
                        vertical_radius: 8,
                    },
                },
                mode: ClipMode::Difference,
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let effects = tree.append_frame(
            FrameData::Effects(EffectsData {
                opacity: 0.25,
                blend_mode: CompositingAndBlendingOperator::PlusLighter,
                filter: Some(Rc::new(vec![1, 2, 3, 4])),
            }),
            clip,
            scroll_node,
        );
        tree.append_frame(
            FrameData::Effects(EffectsData {
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
            }),
            effects,
            scroll_node,
        );
        tree.append_frame(
            FrameData::Mask(MaskData {
                rect: IntRect::new(1, 2, 3, 4),
                kind: MaskKind::Luminance,
                origin: MaskLayerOrigin::SvgClip,
            }),
            effects,
            sorting_root,
        );
        tree.append_frame(
            FrameData::ClipPath(ClipPathData {
                path: Rc::new(OwnedPath::from_serialized_bytes(&[])),
                bounding_rect: IntRect::new(5, 6, 7, 8),
                fill_rule: WindingRule::EvenOdd,
            }),
            FrameNodeIndex::NONE,
            scroll_node,
        );
        tree.root_isolation_frame = Some(effects);
        tree
    }

    fn spatial_data_matches(a: &SpatialData, b: &SpatialData) -> bool {
        match (a, b) {
            (SpatialData::Scroll(_), SpatialData::Scroll(_)) => true,
            (SpatialData::Sticky(a), SpatialData::Sticky(b)) => {
                StickyData {
                    state_slot: NO_SCROLL_STATE_SLOT,
                    owner_paintable: b.owner_paintable,
                    registry_parent_node: b.registry_parent_node,
                    ..*a
                } == *b
            }
            (SpatialData::Transform(a), SpatialData::Transform(b)) => a == b,
            (SpatialData::Perspective(a), SpatialData::Perspective(b)) => a == b,
            (SpatialData::BackfaceVisibility(a), SpatialData::BackfaceVisibility(b)) => a == b,
            (SpatialData::AnchorScrollShift(a), SpatialData::AnchorScrollShift(b)) => a == b,
            (SpatialData::Dead, SpatialData::Dead) => true,
            _ => false,
        }
    }

    fn frame_data_matches(a: &FrameData, b: &FrameData) -> bool {
        match (a, b) {
            (FrameData::Clip(a), FrameData::Clip(b)) => a == b,
            (FrameData::ClipPath(a), FrameData::ClipPath(b)) => {
                a.bounding_rect == b.bounding_rect && a.fill_rule == b.fill_rule
            }
            (FrameData::Effects(a), FrameData::Effects(b)) => {
                a.opacity == b.opacity && a.blend_mode == b.blend_mode && a.filter == b.filter
            }
            (FrameData::Mask(a), FrameData::Mask(b)) => a == b,
            (FrameData::Dead, FrameData::Dead) => true,
            _ => false,
        }
    }

    fn assert_trees_match(a: &VisualContextTree, b: &VisualContextTree) {
        assert_eq!(a.structural_epoch, b.structural_epoch);
        assert_eq!(a.root_is_visual_viewport, b.root_is_visual_viewport);
        assert_eq!(a.root_isolation_frame, b.root_isolation_frame);
        assert_eq!(a.spatial_nodes.len(), b.spatial_nodes.len());
        assert_eq!(a.frame_nodes.len(), b.frame_nodes.len());
        assert_eq!(a.live_spatial_node_count, b.live_spatial_node_count);
        assert_eq!(a.live_frame_node_count, b.live_frame_node_count);
        for (node, other) in a.spatial_nodes.iter().zip(&b.spatial_nodes) {
            assert_eq!(node.parent, other.parent);
            assert!(spatial_data_matches(&node.data, &other.data));
        }
        for (node, other) in a.frame_nodes.iter().zip(&b.frame_nodes) {
            assert_eq!(node.parent, other.parent);
            assert_eq!(node.spatial, other.spatial);
            assert!(frame_data_matches(&node.data, &other.data));
        }
    }

    #[test]
    fn a_tree_with_every_node_kind_round_trips() {
        let tree = tree_with_every_node_kind();
        let bytes = tree.to_bytes();
        let decoded = VisualContextTree::from_bytes(&bytes).expect("a serialized tree decodes");
        assert_trees_match(&tree, &decoded);
        assert_eq!(decoded.to_bytes(), bytes);
    }

    #[test]
    fn a_tree_with_tombstones_round_trips_and_keeps_its_live_counts() {
        let mut tree = tree_with_every_node_kind();
        let scroll_node = SpatialNodeIndex(1);
        let stale_transform = tree.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            scroll_node,
        );
        let stale_frame = tree.append_frame(
            FrameData::Effects(EffectsData {
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
            }),
            FrameNodeIndex::NONE,
            scroll_node,
        );
        assert!(tree.tombstone_spatial_slot(stale_transform));
        assert!(tree.tombstone_frame_slot(stale_frame));
        let bytes = tree.to_bytes();
        let decoded = VisualContextTree::from_bytes(&bytes).expect("a serialized tree decodes");
        assert_trees_match(&tree, &decoded);
        assert!(!decoded.spatial_is_live(stale_transform));
        assert!(!decoded.frame_is_live(stale_frame));
        assert_eq!(decoded.dead_node_count(), 2);
        assert_eq!(decoded.to_bytes(), bytes);
    }

    #[test]
    fn a_live_node_referencing_a_tombstone_is_rejected() {
        let tree_with_tombstoned_target = |make_referencing_data: &dyn Fn(SpatialNodeIndex) -> SpatialData| {
            let mut tree = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
            let target = tree.append_spatial(
                SpatialData::Scroll(ScrollData {
                    state_slot: NO_SCROLL_STATE_SLOT,
                    owner_paintable: NodeSlotId::INVALID,
                    registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
                }),
                VISUAL_VIEWPORT_NODE_INDEX,
            );
            tree.append_spatial(make_referencing_data(target), VISUAL_VIEWPORT_NODE_INDEX);
            assert!(tree.tombstone_spatial_slot(target));
            tree
        };
        let plane_root = tree_with_tombstoned_target(&|target| {
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: target,
                flattens_inherited_transform: false,
            })
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&plane_root)).is_none());
        let sorting_root = tree_with_tombstoned_target(&|target| {
            SpatialData::Transform(TransformData {
                sorting_context_root_index: Some(target),
                ..transform(FloatMatrix4x4::identity())
            })
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&sorting_root)).is_none());
        let anchor = tree_with_tombstoned_target(&|target| {
            SpatialData::AnchorScrollShift(AnchorScrollShift {
                scroll_node_index: target,
                negate: false,
                compensate_horizontal_scroll: true,
                compensate_vertical_scroll: true,
            })
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&anchor)).is_none());
        let scroller = tree_with_tombstoned_target(&|target| {
            SpatialData::Sticky(StickyData::unconstrained(
                target,
                None,
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
                target,
            ))
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&scroller)).is_none());
    }

    #[test]
    fn a_live_node_under_a_tombstone_is_rejected() {
        let mut child_under_tombstone = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        let parent = child_under_tombstone.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        child_under_tombstone.append_spatial(SpatialData::Transform(transform(FloatMatrix4x4::identity())), parent);
        assert!(child_under_tombstone.tombstone_spatial_slot(parent));
        assert!(VisualContextTree::from_bytes(&encode_tree(&child_under_tombstone)).is_none());

        let effects = || {
            FrameData::Effects(EffectsData {
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
            })
        };
        let mut frame_under_tombstone = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        let parent_frame =
            frame_under_tombstone.append_frame(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        frame_under_tombstone.append_frame(effects(), parent_frame, VISUAL_VIEWPORT_NODE_INDEX);
        assert!(frame_under_tombstone.tombstone_frame_slot(parent_frame));
        assert!(VisualContextTree::from_bytes(&encode_tree(&frame_under_tombstone)).is_none());

        let mut frame_in_tombstone = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        let spatial = frame_in_tombstone.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        frame_in_tombstone.append_frame(effects(), FrameNodeIndex::NONE, spatial);
        assert!(frame_in_tombstone.tombstone_spatial_slot(spatial));
        assert!(VisualContextTree::from_bytes(&encode_tree(&frame_in_tombstone)).is_none());

        let mut tombstoned_isolation_frame = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        let isolation_frame =
            tombstoned_isolation_frame.append_frame(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        assert!(tombstoned_isolation_frame.tombstone_frame_slot(isolation_frame));
        tombstoned_isolation_frame.root_isolation_frame = Some(isolation_frame);
        assert!(VisualContextTree::from_bytes(&encode_tree(&tombstoned_isolation_frame)).is_none());
    }

    #[test]
    fn a_content_root_tree_round_trips() {
        let mut tree = VisualContextTree::create_with_content_root(transform(translation_matrix(-8.0, -9.0, 0.0)));
        tree.structural_epoch = 7;
        let decoded = VisualContextTree::from_bytes(&tree.to_bytes()).expect("a serialized tree decodes");
        assert_trees_match(&tree, &decoded);
    }

    #[test]
    fn truncated_bytes_are_rejected_without_panicking() {
        let bytes = tree_with_every_node_kind().to_bytes();
        for length in 0..bytes.len() {
            assert!(
                VisualContextTree::from_bytes(&bytes[..length]).is_none(),
                "a prefix of {length} bytes decoded"
            );
        }
    }

    #[test]
    fn trailing_bytes_are_rejected() {
        let mut bytes = tree_with_every_node_kind().to_bytes();
        bytes.push(0);
        assert!(VisualContextTree::from_bytes(&bytes).is_none());
    }

    #[test]
    fn a_wrong_magic_or_format_is_rejected() {
        let bytes = tree_with_every_node_kind().to_bytes();
        let mut wrong_magic = bytes.clone();
        wrong_magic[0] ^= 0xff;
        assert!(VisualContextTree::from_bytes(&wrong_magic).is_none());
        let mut wrong_format = bytes;
        wrong_format[4] ^= 0xff;
        assert!(VisualContextTree::from_bytes(&wrong_format).is_none());
    }

    fn encode_tree(tree: &VisualContextTree) -> Vec<u8> {
        tree.to_bytes()
    }

    fn hostile_tree() -> VisualContextTree {
        let mut tree = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.append_spatial(
            SpatialData::Sticky(StickyData::unconstrained(
                SpatialNodeIndex(1),
                None,
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
                SpatialNodeIndex(1),
            )),
            SpatialNodeIndex(1),
        );
        tree.append_frame(
            FrameData::Effects(EffectsData {
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.append_frame(
            FrameData::Clip(ClipData {
                rect: FloatRect::default(),
                corner_radii: CornerRadii::default(),
                mode: ClipMode::Intersect,
            }),
            FrameNodeIndex(0),
            SpatialNodeIndex(2),
        );
        tree
    }

    #[test]
    fn references_to_nodes_stored_after_the_node_are_accepted() {
        let mut child_stored_below_its_parent = hostile_tree();
        child_stored_below_its_parent.spatial_nodes[1].parent = SpatialNodeIndex(2);
        child_stored_below_its_parent.spatial_nodes[2] = SpatialNode {
            data: SpatialData::Transform(transform(translation_matrix(5.0, 5.0, 0.0))),
            parent: VISUAL_VIEWPORT_NODE_INDEX,
        };
        child_stored_below_its_parent.frame_nodes[0].parent = FrameNodeIndex(1);
        child_stored_below_its_parent.frame_nodes[1].parent = FrameNodeIndex::NONE;
        let decoded = VisualContextTree::from_bytes(&encode_tree(&child_stored_below_its_parent))
            .expect("acyclic forward references decode");
        assert_eq!(decoded.spatial_dependency_order(), vec![0, 2, 1]);
        assert_eq!(decoded.frame_dependency_order(), vec![1, 0]);
        assert_eq!(
            decoded.transform_rect_to_viewport(
                SpatialNodeIndex(1),
                FloatRect::new(0.0, 0.0, 10.0, 10.0),
                &[],
                super::super::IncludeVisualViewportTransform::Yes
            ),
            FloatRect::new(5.0, 5.0, 10.0, 10.0)
        );
    }

    #[test]
    fn reference_cycles_self_references_and_wrong_kinds_are_rejected() {
        let mut parent_cycle = hostile_tree();
        parent_cycle.spatial_nodes[1].parent = SpatialNodeIndex(2);
        assert!(VisualContextTree::from_bytes(&encode_tree(&parent_cycle)).is_none());

        let mut self_parented = hostile_tree();
        self_parented.spatial_nodes[1].parent = SpatialNodeIndex(1);
        assert!(VisualContextTree::from_bytes(&encode_tree(&self_parented)).is_none());

        let mut root_with_a_parent = hostile_tree();
        root_with_a_parent.spatial_nodes[0].parent = SpatialNodeIndex(1);
        assert!(VisualContextTree::from_bytes(&encode_tree(&root_with_a_parent)).is_none());

        let mut sticky_parent_cycle = hostile_tree();
        sticky_parent_cycle.spatial_nodes[1] = SpatialNode {
            data: SpatialData::Sticky(StickyData::unconstrained(
                VISUAL_VIEWPORT_NODE_INDEX,
                Some(SpatialNodeIndex(2)),
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
                VISUAL_VIEWPORT_NODE_INDEX,
            )),
            parent: VISUAL_VIEWPORT_NODE_INDEX,
        };
        sticky_parent_cycle.spatial_nodes[2] = SpatialNode {
            data: SpatialData::Sticky(StickyData::unconstrained(
                VISUAL_VIEWPORT_NODE_INDEX,
                Some(SpatialNodeIndex(1)),
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
                VISUAL_VIEWPORT_NODE_INDEX,
            )),
            parent: VISUAL_VIEWPORT_NODE_INDEX,
        };
        assert!(VisualContextTree::from_bytes(&encode_tree(&sticky_parent_cycle)).is_none());

        let mut parent_out_of_range = hostile_tree();
        parent_out_of_range.spatial_nodes[1].parent = SpatialNodeIndex(7);
        assert!(VisualContextTree::from_bytes(&encode_tree(&parent_out_of_range)).is_none());

        let mut plane_root_out_of_range = hostile_tree();
        plane_root_out_of_range.spatial_nodes[1].data = SpatialData::BackfaceVisibility(BackfaceVisibilityData {
            plane_root_index: SpatialNodeIndex(9),
            flattens_inherited_transform: false,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&plane_root_out_of_range)).is_none());

        let mut root_is_not_a_transform = hostile_tree();
        root_is_not_a_transform.spatial_nodes[0].data = SpatialData::Scroll(ScrollData {
            state_slot: NO_SCROLL_STATE_SLOT,
            owner_paintable: NodeSlotId::INVALID,
            registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&root_is_not_a_transform)).is_none());

        let mut sorting_root_cycle = hostile_tree();
        sorting_root_cycle.spatial_nodes[0].data = SpatialData::Transform(TransformData {
            sorting_context_root_index: Some(SpatialNodeIndex(1)),
            ..transform(FloatMatrix4x4::identity())
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&sorting_root_cycle)).is_none());

        let mut sticky_scroller_is_not_a_scroll_node = hostile_tree();
        sticky_scroller_is_not_a_scroll_node.spatial_nodes[1].data = SpatialData::Perspective(PerspectiveData {
            matrix: FloatMatrix4x4::identity(),
            flattens_inherited_transform: false,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&sticky_scroller_is_not_a_scroll_node)).is_none());

        let mut sticky_parent_is_not_sticky = hostile_tree();
        sticky_parent_is_not_sticky.spatial_nodes[2].data = SpatialData::Sticky(StickyData::unconstrained(
            SpatialNodeIndex(1),
            Some(SpatialNodeIndex(1)),
            NO_SCROLL_STATE_SLOT,
            NodeSlotId::INVALID,
            SpatialNodeIndex(1),
        ));
        assert!(VisualContextTree::from_bytes(&encode_tree(&sticky_parent_is_not_sticky)).is_none());

        let mut plane_root_cycle = hostile_tree();
        plane_root_cycle.spatial_nodes[1].data = SpatialData::BackfaceVisibility(BackfaceVisibilityData {
            plane_root_index: SpatialNodeIndex(2),
            flattens_inherited_transform: false,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&plane_root_cycle)).is_none());

        let mut plane_root_on_another_branch = hostile_tree();
        plane_root_on_another_branch.spatial_nodes[2].parent = VISUAL_VIEWPORT_NODE_INDEX;
        plane_root_on_another_branch.spatial_nodes[2].data = SpatialData::BackfaceVisibility(BackfaceVisibilityData {
            plane_root_index: SpatialNodeIndex(1),
            flattens_inherited_transform: false,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&plane_root_on_another_branch)).is_none());

        let mut plane_root_on_the_root_path = hostile_tree();
        plane_root_on_the_root_path.spatial_nodes[2].data = SpatialData::BackfaceVisibility(BackfaceVisibilityData {
            plane_root_index: SpatialNodeIndex(1),
            flattens_inherited_transform: false,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&plane_root_on_the_root_path)).is_some());

        let mut anchor_node_names_itself = hostile_tree();
        anchor_node_names_itself.spatial_nodes[1].data = SpatialData::AnchorScrollShift(AnchorScrollShift {
            scroll_node_index: SpatialNodeIndex(1),
            negate: false,
            compensate_horizontal_scroll: true,
            compensate_vertical_scroll: true,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&anchor_node_names_itself)).is_none());
    }

    #[test]
    fn frame_reference_cycles_and_ranges_are_rejected() {
        let mut frame_parent_cycle = hostile_tree();
        frame_parent_cycle.frame_nodes[0].parent = FrameNodeIndex(1);
        assert!(VisualContextTree::from_bytes(&encode_tree(&frame_parent_cycle)).is_none());

        let mut frame_parent_out_of_range = hostile_tree();
        frame_parent_out_of_range.frame_nodes[1].parent = FrameNodeIndex(4);
        assert!(VisualContextTree::from_bytes(&encode_tree(&frame_parent_out_of_range)).is_none());

        let mut frame_spatial_out_of_range = hostile_tree();
        frame_spatial_out_of_range.frame_nodes[1].spatial = SpatialNodeIndex(3);
        assert!(VisualContextTree::from_bytes(&encode_tree(&frame_spatial_out_of_range)).is_none());

        let mut isolation_frame_out_of_range = hostile_tree();
        isolation_frame_out_of_range.root_isolation_frame = Some(FrameNodeIndex(2));
        assert!(VisualContextTree::from_bytes(&encode_tree(&isolation_frame_out_of_range)).is_none());

        let mut isolation_frame_is_a_clip = hostile_tree();
        isolation_frame_is_a_clip.root_isolation_frame = Some(FrameNodeIndex(1));
        assert!(VisualContextTree::from_bytes(&encode_tree(&isolation_frame_is_a_clip)).is_none());

        let mut isolation_frame_is_effects = hostile_tree();
        isolation_frame_is_effects.root_isolation_frame = Some(FrameNodeIndex(0));
        assert!(VisualContextTree::from_bytes(&encode_tree(&isolation_frame_is_effects)).is_some());
    }

    #[test]
    fn out_of_range_discriminants_and_absurd_counts_are_rejected() {
        let tree = hostile_tree();
        let bytes = tree.to_bytes();
        let header_size = 4 + 4 + 8 + 1 + 4 + 4 + 4;
        let root_spatial_kind_offset = header_size + 4;
        let mut bad_spatial_kind = bytes.clone();
        bad_spatial_kind[root_spatial_kind_offset] = 200;
        assert!(VisualContextTree::from_bytes(&bad_spatial_kind).is_none());

        let mut bad_bool = bytes.clone();
        bad_bool[header_size - 4 - 4 - 4] = 2;
        assert!(VisualContextTree::from_bytes(&bad_bool).is_none());

        let mut absurd_spatial_count = bytes.clone();
        absurd_spatial_count[header_size - 8..header_size - 4].copy_from_slice(&u32::MAX.to_ne_bytes());
        assert!(VisualContextTree::from_bytes(&absurd_spatial_count).is_none());

        let mut absurd_frame_count = bytes;
        absurd_frame_count[header_size - 4..header_size].copy_from_slice(&u32::MAX.to_ne_bytes());
        assert!(VisualContextTree::from_bytes(&absurd_frame_count).is_none());
    }
}
