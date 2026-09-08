/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{
    AnchorScrollShift, BackdropFilterData, BackfaceVisibilityData, ClipData, ClipMode, ClipNode, ClipNodeData,
    ClipNodeIndex, ClipPathData, EffectNode, EffectNodeData, EffectNodeIndex, EffectsData, MaskData, MaskLayerOrigin,
    PerspectiveData, ScrollData, SpatialData, SpatialNode, SpatialNodeIndex, StickyData, TransformData,
    TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree, scroll_state::NO_SCROLL_STATE_SLOT,
};
use crate::layout::node_data::NodeSlotId;
use libgfx_rust::path::OwnedPath;
use libgfx_rust::{
    CompositingAndBlendingOperator, CornerRadii, CornerRadius, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize,
    IntRect, MaskKind, WindingRule,
};
use std::rc::Rc;

const SERIALIZED_TREE_MAGIC: u32 = 0x5443_5641;
const SERIALIZED_TREE_FORMAT: u32 = 3;

const SPATIAL_KIND_SCROLL: u8 = 0;
const SPATIAL_KIND_STICKY: u8 = 1;
const SPATIAL_KIND_TRANSFORM: u8 = 2;
const SPATIAL_KIND_PERSPECTIVE: u8 = 3;
const SPATIAL_KIND_BACKFACE_VISIBILITY: u8 = 4;
const SPATIAL_KIND_ANCHOR_SCROLL_SHIFT: u8 = 5;
const SPATIAL_KIND_DEAD: u8 = 6;

const CLIP_KIND_RECT: u8 = 0;
const CLIP_KIND_PATH: u8 = 1;
const CLIP_KIND_DEAD: u8 = 2;

const EFFECT_KIND_EFFECTS: u8 = 0;
const EFFECT_KIND_MASK: u8 = 1;
const EFFECT_KIND_BACKGROUND_COLOR_ANIMATION: u8 = 2;
const EFFECT_KIND_DEAD: u8 = 3;

const MINIMUM_SERIALIZED_SPATIAL_NODE_SIZE: usize = 5;
const MINIMUM_SERIALIZED_CLIP_NODE_SIZE: usize = 9;
const MINIMUM_SERIALIZED_EFFECT_NODE_SIZE: usize = 13;

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
    fn clip_index(&mut self, index: ClipNodeIndex) {
        self.u32(index.0);
    }
    fn effect_index(&mut self, index: EffectNodeIndex) {
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
    fn clip_index(&mut self) -> Option<ClipNodeIndex> {
        self.u32().map(ClipNodeIndex)
    }
    fn effect_index(&mut self) -> Option<EffectNodeIndex> {
        self.u32().map(EffectNodeIndex)
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

fn write_clip_data(writer: &mut TreeByteWriter, data: &ClipNodeData) {
    match data {
        ClipNodeData::Rect(clip) => {
            writer.u8(CLIP_KIND_RECT);
            writer.float_rect(clip.rect);
            writer.corner_radii(clip.corner_radii);
            writer.u8(clip.mode as u8);
        }
        ClipNodeData::Path(clip_path) => {
            writer.u8(CLIP_KIND_PATH);
            writer.int_rect(clip_path.bounding_rect);
            writer.i32(clip_path.fill_rule as i32);
            writer.length_prefixed_bytes(&clip_path.path.serialize_to_bytes());
        }
        ClipNodeData::Dead => writer.u8(CLIP_KIND_DEAD),
    }
}

fn read_clip_data(reader: &mut TreeByteReader<'_>) -> Option<ClipNodeData> {
    Some(match reader.u8()? {
        CLIP_KIND_RECT => ClipNodeData::Rect(ClipData {
            rect: reader.float_rect()?,
            corner_radii: reader.corner_radii()?,
            mode: reader.clip_mode()?,
        }),
        CLIP_KIND_PATH => {
            let bounding_rect = reader.int_rect()?;
            let fill_rule = reader.winding_rule()?;
            let path = OwnedPath::from_serialized_bytes(reader.length_prefixed_bytes()?);
            ClipNodeData::Path(ClipPathData {
                path: Rc::new(path),
                bounding_rect,
                fill_rule,
            })
        }
        CLIP_KIND_DEAD => ClipNodeData::Dead,
        _ => return None,
    })
}

fn write_effect_data(writer: &mut TreeByteWriter, data: &EffectNodeData) {
    match data {
        EffectNodeData::Effects(effects) => {
            writer.u8(EFFECT_KIND_EFFECTS);
            writer.f32(effects.opacity);
            writer.i32(effects.blend_mode as i32);
            writer.bool(effects.filter.is_some());
            writer.length_prefixed_bytes(effects.filter.as_deref().map_or(&[], Vec::as_slice));
            writer.bool(effects.backdrop_filter.is_some());
            if let Some(backdrop_filter) = &effects.backdrop_filter {
                writer.int_rect(backdrop_filter.region);
                writer.corner_radii(backdrop_filter.corner_radii);
                writer.length_prefixed_bytes(&backdrop_filter.filter);
            }
        }
        EffectNodeData::Mask(mask) => {
            writer.u8(EFFECT_KIND_MASK);
            writer.int_rect(mask.rect);
            writer.i32(mask.kind as i32);
            writer.u8(mask.origin as u8);
        }
        EffectNodeData::BackgroundColorAnimation => writer.u8(EFFECT_KIND_BACKGROUND_COLOR_ANIMATION),
        EffectNodeData::Dead => writer.u8(EFFECT_KIND_DEAD),
    }
}

fn read_effect_data(reader: &mut TreeByteReader<'_>) -> Option<EffectNodeData> {
    Some(match reader.u8()? {
        EFFECT_KIND_EFFECTS => {
            let opacity = reader.f32()?;
            let blend_mode = reader.compositing_and_blending_operator()?;
            let has_filter = reader.bool()?;
            let filter_bytes = reader.length_prefixed_bytes()?;
            let backdrop_filter = if reader.bool()? {
                Some(BackdropFilterData {
                    region: reader.int_rect()?,
                    corner_radii: reader.corner_radii()?,
                    filter: Rc::new(reader.length_prefixed_bytes()?.to_vec()),
                })
            } else {
                None
            };
            EffectNodeData::Effects(EffectsData {
                opacity,
                blend_mode,
                filter: has_filter.then(|| Rc::new(filter_bytes.to_vec())),
                backdrop_filter,
            })
        }
        EFFECT_KIND_MASK => EffectNodeData::Mask(MaskData {
            rect: reader.int_rect()?,
            kind: reader.mask_kind()?,
            origin: reader.mask_layer_origin()?,
        }),
        EFFECT_KIND_BACKGROUND_COLOR_ANIMATION => EffectNodeData::BackgroundColorAnimation,
        EFFECT_KIND_DEAD => EffectNodeData::Dead,
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
    pub(crate) fn node_references_are_consistent(&self) -> bool {
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
        // A node's spatial node is in range, and live where the node is live.
        let spatial_reference_is_consistent = |spatial: SpatialNodeIndex, is_live: bool| {
            (spatial.0 as usize) < spatial_nodes.len() && (!is_live || self.spatial_is_live(spatial))
        };
        if !self
            .clip_nodes
            .iter()
            .all(|node| spatial_reference_is_consistent(node.spatial, node.data.is_live()))
        {
            return false;
        }
        let clip_order = self.clip_dependency_order_with_back_edges();
        if !clip_order.back_edges.is_empty() || !clip_order.dangling_references.is_empty() {
            return false;
        }
        for node in &self.effect_nodes {
            if !spatial_reference_is_consistent(node.spatial, node.data.is_live()) {
                return false;
            }
            if node.data.is_live()
                && node
                    .resolved_output_clip
                    .is_none_or(|clip| !self.clip_is_none_or_live(clip))
            {
                return false;
            }
        }
        let effect_order = self.effect_dependency_order_with_back_edges();
        if !effect_order.back_edges.is_empty() || !effect_order.dangling_references.is_empty() {
            return false;
        }
        // Replay pushes each effect's layer inside its output clip and the remaining clips inside
        // the layer, which needs the output clips nested along the effect chain. Display-list
        // contexts are validated against these clips separately. The clip tree has
        // just been checked for cycles.
        for node in &self.effect_nodes {
            if !node.data.is_live() || node.parent.is_none() {
                continue;
            }
            let parent_output_clip = self.effect_nodes[node.parent.0 as usize].output_clip();
            if !self.clip_is_ancestor_or_self(parent_output_clip, node.output_clip()) {
                return false;
            }
        }
        // Plane clips are pushed immediately above this root isolation layer.
        if let Some(effect) = self.root_isolation_effect {
            let Some(node) = self.effect_nodes.get(effect.0 as usize) else {
                return false;
            };
            if !matches!(node.data, EffectNodeData::Effects(_))
                || !node.parent.is_none()
                || !node.output_clip().is_none()
            {
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
        writer.effect_index(self.root_isolation_effect.unwrap_or(EffectNodeIndex::NONE));
        writer.u32(self.spatial_nodes.len() as u32);
        writer.u32(self.clip_nodes.len() as u32);
        writer.u32(self.effect_nodes.len() as u32);
        for node in &self.spatial_nodes {
            writer.spatial_index(node.parent);
            write_spatial_data(&mut writer, &node.data);
        }
        for node in &self.clip_nodes {
            writer.clip_index(node.parent);
            writer.spatial_index(node.spatial);
            write_clip_data(&mut writer, &node.data);
        }
        for node in &self.effect_nodes {
            writer.effect_index(node.parent);
            writer.spatial_index(node.spatial);
            writer.clip_index(node.output_clip());
            write_effect_data(&mut writer, &node.data);
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
        let root_isolation_effect = reader.effect_index()?;
        let spatial_count = reader.u32()? as usize;
        let clip_count = reader.u32()? as usize;
        let effect_count = reader.u32()? as usize;
        if spatial_count == 0
            || spatial_count > reader.remaining() / MINIMUM_SERIALIZED_SPATIAL_NODE_SIZE
            || clip_count > reader.remaining() / MINIMUM_SERIALIZED_CLIP_NODE_SIZE
            || effect_count > reader.remaining() / MINIMUM_SERIALIZED_EFFECT_NODE_SIZE
        {
            return None;
        }

        let mut spatial_nodes = Vec::with_capacity(spatial_count);
        for _ in 0..spatial_count {
            let parent = reader.spatial_index()?;
            let data = read_spatial_data(&mut reader)?;
            spatial_nodes.push(SpatialNode { data, parent });
        }

        let mut clip_nodes = Vec::with_capacity(clip_count);
        for _ in 0..clip_count {
            let parent = reader.clip_index()?;
            let spatial = reader.spatial_index()?;
            let data = read_clip_data(&mut reader)?;
            clip_nodes.push(ClipNode::new(data, parent, spatial));
        }

        let mut effect_nodes = Vec::with_capacity(effect_count);
        for _ in 0..effect_count {
            let parent = reader.effect_index()?;
            let spatial = reader.spatial_index()?;
            let output_clip = reader.clip_index()?;
            let data = read_effect_data(&mut reader)?;
            effect_nodes.push(EffectNode {
                data,
                parent,
                spatial,
                resolved_output_clip: Some(output_clip),
            });
        }

        if reader.remaining() != 0 {
            return None;
        }

        let tree = Self::from_nodes(
            spatial_nodes,
            clip_nodes,
            effect_nodes,
            root_is_visual_viewport,
            (!root_isolation_effect.is_none()).then_some(root_isolation_effect),
            structural_epoch,
        );
        tree.node_references_are_consistent().then_some(tree)
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

        let clip = tree.append_clip(
            ClipNodeData::Rect(ClipData {
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
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let clip_path = tree.append_clip(
            ClipNodeData::Path(ClipPathData {
                path: Rc::new(OwnedPath::from_serialized_bytes(&[])),
                bounding_rect: IntRect::new(5, 6, 7, 8),
                fill_rule: WindingRule::EvenOdd,
            }),
            clip,
            scroll_node,
        );
        let root_effect = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let effects = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                opacity: 0.25,
                blend_mode: CompositingAndBlendingOperator::PlusLighter,
                filter: Some(Rc::new(vec![1, 2, 3, 4])),
                backdrop_filter: Some(BackdropFilterData {
                    filter: Rc::new(vec![5, 6, 7]),
                    region: IntRect::new(9, 10, 11, 12),
                    corner_radii: CornerRadii {
                        top_left: CornerRadius {
                            horizontal_radius: 1,
                            vertical_radius: 2,
                        },
                        ..CornerRadii::default()
                    },
                }),
            }),
            root_effect,
            scroll_node,
            clip,
        );
        let mask = tree.append_effect(
            EffectNodeData::Mask(MaskData {
                rect: IntRect::new(1, 2, 3, 4),
                kind: MaskKind::Luminance,
                origin: MaskLayerOrigin::SvgClip,
            }),
            effects,
            sorting_root,
            clip_path,
        );
        tree.append_effect(EffectNodeData::BackgroundColorAnimation, mask, sorting_root, clip_path);
        tree.root_isolation_effect = Some(root_effect);
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

    fn clip_data_matches(a: &ClipNodeData, b: &ClipNodeData) -> bool {
        match (a, b) {
            (ClipNodeData::Rect(a), ClipNodeData::Rect(b)) => a == b,
            (ClipNodeData::Path(a), ClipNodeData::Path(b)) => {
                a.bounding_rect == b.bounding_rect && a.fill_rule == b.fill_rule
            }
            (ClipNodeData::Dead, ClipNodeData::Dead) => true,
            _ => false,
        }
    }

    fn effect_data_matches(a: &EffectNodeData, b: &EffectNodeData) -> bool {
        match (a, b) {
            (EffectNodeData::Effects(a), EffectNodeData::Effects(b)) => {
                a.opacity == b.opacity
                    && a.blend_mode == b.blend_mode
                    && a.filter == b.filter
                    && a.backdrop_filter == b.backdrop_filter
            }
            (EffectNodeData::Mask(a), EffectNodeData::Mask(b)) => a == b,
            (EffectNodeData::BackgroundColorAnimation, EffectNodeData::BackgroundColorAnimation) => true,
            (EffectNodeData::Dead, EffectNodeData::Dead) => true,
            _ => false,
        }
    }

    fn assert_trees_match(a: &VisualContextTree, b: &VisualContextTree) {
        assert_eq!(a.structural_epoch, b.structural_epoch);
        assert_eq!(a.root_is_visual_viewport, b.root_is_visual_viewport);
        assert_eq!(a.root_isolation_effect, b.root_isolation_effect);
        assert_eq!(a.spatial_nodes.len(), b.spatial_nodes.len());
        assert_eq!(a.clip_nodes.len(), b.clip_nodes.len());
        assert_eq!(a.effect_nodes.len(), b.effect_nodes.len());
        assert_eq!(a.live_spatial_node_count(), b.live_spatial_node_count());
        assert_eq!(a.live_clip_node_count(), b.live_clip_node_count());
        assert_eq!(a.live_effect_node_count(), b.live_effect_node_count());
        for (node, other) in a.spatial_nodes.iter().zip(&b.spatial_nodes) {
            assert_eq!(node.parent, other.parent);
            assert!(spatial_data_matches(&node.data, &other.data));
        }
        for (node, other) in a.clip_nodes.iter().zip(&b.clip_nodes) {
            assert_eq!(node.parent, other.parent);
            assert_eq!(node.spatial, other.spatial);
            assert_eq!(node.clips_everything, other.clips_everything);
            assert!(clip_data_matches(&node.data, &other.data));
        }
        for (node, other) in a.effect_nodes.iter().zip(&b.effect_nodes) {
            assert_eq!(node.parent, other.parent);
            assert_eq!(node.spatial, other.spatial);
            assert_eq!(node.output_clip(), other.output_clip());
            assert!(effect_data_matches(&node.data, &other.data));
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
        let stale_effect = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            scroll_node,
            ClipNodeIndex::NONE,
        );
        let stale_clip = tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(0.0, 0.0, 1.0, 1.0)),
            ClipNodeIndex::NONE,
            scroll_node,
        );
        assert!(tree.tombstone_spatial_slot(stale_transform));
        assert!(tree.tombstone_effect_slot(stale_effect));
        assert!(tree.tombstone_clip_slot(stale_clip));
        let bytes = tree.to_bytes();
        let decoded = VisualContextTree::from_bytes(&bytes).expect("a serialized tree decodes");
        assert_trees_match(&tree, &decoded);
        assert!(!decoded.spatial_is_live(stale_transform));
        assert!(!decoded.clip_is_live(stale_clip));
        assert!(!decoded.effect_is_live(stale_effect));
        assert_eq!(decoded.dead_node_count(), 3);
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
            EffectNodeData::Effects(EffectsData {
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            })
        };
        let rect = || ClipNodeData::rect_clip(FloatRect::new(0.0, 0.0, 1.0, 1.0));
        let fresh = || VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        let rejected = |tree: &VisualContextTree| {
            assert!(!tree.node_references_are_consistent());
            assert!(VisualContextTree::from_bytes(&encode_tree(tree)).is_none());
        };

        let mut effect_under_tombstone = fresh();
        let parent_effect = effect_under_tombstone.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        effect_under_tombstone.append_effect(
            effects(),
            parent_effect,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        assert!(effect_under_tombstone.tombstone_effect_slot(parent_effect));
        rejected(&effect_under_tombstone);

        let mut clip_under_tombstone = fresh();
        let parent_clip = clip_under_tombstone.append_clip(rect(), ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        clip_under_tombstone.append_clip(rect(), parent_clip, VISUAL_VIEWPORT_NODE_INDEX);
        assert!(clip_under_tombstone.tombstone_clip_slot(parent_clip));
        rejected(&clip_under_tombstone);

        let mut nodes_in_tombstone = fresh();
        let spatial = nodes_in_tombstone.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let mut clip_in_tombstone = nodes_in_tombstone.clone();
        clip_in_tombstone.append_clip(rect(), ClipNodeIndex::NONE, spatial);
        assert!(clip_in_tombstone.tombstone_spatial_slot(spatial));
        rejected(&clip_in_tombstone);
        nodes_in_tombstone.append_effect(effects(), EffectNodeIndex::NONE, spatial, ClipNodeIndex::NONE);
        assert!(nodes_in_tombstone.tombstone_spatial_slot(spatial));
        rejected(&nodes_in_tombstone);

        let mut effect_under_tombstoned_output_clip = fresh();
        let output_clip =
            effect_under_tombstoned_output_clip.append_clip(rect(), ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        effect_under_tombstoned_output_clip.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            output_clip,
        );
        assert!(effect_under_tombstoned_output_clip.tombstone_clip_slot(output_clip));
        rejected(&effect_under_tombstoned_output_clip);

        let mut tombstoned_isolation_effect = fresh();
        let isolation_effect = tombstoned_isolation_effect.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        assert!(tombstoned_isolation_effect.tombstone_effect_slot(isolation_effect));
        tombstoned_isolation_effect.root_isolation_effect = Some(isolation_effect);
        rejected(&tombstoned_isolation_effect);
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
        // Clips: c0 at the root, c1 under it in s2. Effects: e0 at the root under no clip, e1 under
        // e0 with output clip c0.
        tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(0.0, 0.0, 10.0, 10.0)),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.append_clip(
            ClipNodeData::Rect(ClipData {
                rect: FloatRect::default(),
                corner_radii: CornerRadii::default(),
                mode: ClipMode::Intersect,
            }),
            ClipNodeIndex(0),
            SpatialNodeIndex(2),
        );
        tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex(0),
            SpatialNodeIndex(2),
            ClipNodeIndex(0),
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
        child_stored_below_its_parent.clip_nodes[0].parent = ClipNodeIndex(1);
        child_stored_below_its_parent.clip_nodes[1].parent = ClipNodeIndex::NONE;
        child_stored_below_its_parent.effect_nodes[0].parent = EffectNodeIndex(1);
        child_stored_below_its_parent.effect_nodes[1].parent = EffectNodeIndex::NONE;
        child_stored_below_its_parent.effect_nodes[1].resolved_output_clip = Some(ClipNodeIndex::NONE);
        child_stored_below_its_parent.effect_nodes[0].resolved_output_clip = Some(ClipNodeIndex(1));
        let decoded = VisualContextTree::from_bytes(&encode_tree(&child_stored_below_its_parent))
            .expect("acyclic forward references decode");
        assert_eq!(decoded.spatial_dependency_order(), vec![0, 2, 1]);
        assert_eq!(decoded.clip_dependency_order(), vec![1, 0]);
        assert_eq!(decoded.effect_dependency_order(), vec![1, 0]);
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
    fn clip_and_effect_reference_cycles_and_ranges_are_rejected() {
        let rejected = |tree: &VisualContextTree| assert!(VisualContextTree::from_bytes(&encode_tree(tree)).is_none());

        let mut clip_parent_cycle = hostile_tree();
        clip_parent_cycle.clip_nodes[0].parent = ClipNodeIndex(1);
        rejected(&clip_parent_cycle);

        let mut clip_parent_out_of_range = hostile_tree();
        clip_parent_out_of_range.clip_nodes[1].parent = ClipNodeIndex(4);
        rejected(&clip_parent_out_of_range);

        let mut clip_spatial_out_of_range = hostile_tree();
        clip_spatial_out_of_range.clip_nodes[1].spatial = SpatialNodeIndex(3);
        rejected(&clip_spatial_out_of_range);

        let mut effect_parent_cycle = hostile_tree();
        effect_parent_cycle.effect_nodes[0].parent = EffectNodeIndex(1);
        rejected(&effect_parent_cycle);

        let mut effect_parent_out_of_range = hostile_tree();
        effect_parent_out_of_range.effect_nodes[1].parent = EffectNodeIndex(4);
        rejected(&effect_parent_out_of_range);

        let mut effect_spatial_out_of_range = hostile_tree();
        effect_spatial_out_of_range.effect_nodes[1].spatial = SpatialNodeIndex(3);
        rejected(&effect_spatial_out_of_range);

        let mut output_clip_out_of_range = hostile_tree();
        output_clip_out_of_range.effect_nodes[1].resolved_output_clip = Some(ClipNodeIndex(9));
        rejected(&output_clip_out_of_range);

        // e0's output clip moves below e1's.
        let mut output_clip_outside_the_parents = hostile_tree();
        output_clip_outside_the_parents.effect_nodes[0].resolved_output_clip = Some(ClipNodeIndex(1));
        rejected(&output_clip_outside_the_parents);

        let mut isolation_effect_out_of_range = hostile_tree();
        isolation_effect_out_of_range.root_isolation_effect = Some(EffectNodeIndex(2));
        rejected(&isolation_effect_out_of_range);

        let mut isolation_effect_is_a_mask = hostile_tree();
        isolation_effect_is_a_mask.effect_nodes[0].data = EffectNodeData::Mask(MaskData {
            rect: IntRect::new(0, 0, 1, 1),
            kind: MaskKind::Alpha,
            origin: MaskLayerOrigin::CssMaskLayers,
        });
        isolation_effect_is_a_mask.root_isolation_effect = Some(EffectNodeIndex(0));
        rejected(&isolation_effect_is_a_mask);

        // The isolation layer must be a root effect.
        let mut isolation_effect_has_a_parent = hostile_tree();
        isolation_effect_has_a_parent.root_isolation_effect = Some(EffectNodeIndex(1));
        rejected(&isolation_effect_has_a_parent);

        let mut isolation_effect_at_the_root = hostile_tree();
        isolation_effect_at_the_root.root_isolation_effect = Some(EffectNodeIndex(0));
        assert!(VisualContextTree::from_bytes(&encode_tree(&isolation_effect_at_the_root)).is_some());
    }

    #[test]
    fn out_of_range_discriminants_and_absurd_counts_are_rejected() {
        let tree = hostile_tree();
        let bytes = tree.to_bytes();
        let header_size = 4 + 4 + 8 + 1 + 4 + 4 + 4 + 4;
        let root_spatial_kind_offset = header_size + 4;
        let mut bad_spatial_kind = bytes.clone();
        bad_spatial_kind[root_spatial_kind_offset] = 200;
        assert!(VisualContextTree::from_bytes(&bad_spatial_kind).is_none());

        let mut bad_bool = bytes.clone();
        bad_bool[4 + 4 + 8] = 2;
        assert!(VisualContextTree::from_bytes(&bad_bool).is_none());

        let count_offset = |slot: usize| header_size - 12 + slot * 4;
        for slot in 0..3 {
            let mut absurd_count = bytes.clone();
            absurd_count[count_offset(slot)..count_offset(slot) + 4].copy_from_slice(&u32::MAX.to_ne_bytes());
            assert!(
                VisualContextTree::from_bytes(&absurd_count).is_none(),
                "an absurd count in slot {slot} decoded"
            );
        }

        // The first clip node starts right after the spatial nodes; its kind byte follows its
        // parent and spatial indices.
        let spatial_nodes_size = {
            let mut probe = tree.clone();
            probe.clip_nodes.clear();
            probe.effect_nodes.clear();
            probe.root_isolation_effect = None;
            probe.to_bytes().len() - header_size
        };
        let first_clip_kind_offset = header_size + spatial_nodes_size + 4 + 4;
        let mut bad_clip_kind = bytes.clone();
        bad_clip_kind[first_clip_kind_offset] = 200;
        assert!(VisualContextTree::from_bytes(&bad_clip_kind).is_none());
    }
}
