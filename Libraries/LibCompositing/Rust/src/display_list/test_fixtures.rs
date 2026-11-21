/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the unit tests of the layer planner, of the wire form of its plans and of tile keying build their pages from.

use crate::display_list::builder::DisplayListBuilder;
use crate::display_list::commands::{
    CanvasId, ClipNodeIndex, CompositorContextId, ContextRef, DisplayListCommand, DrawCanvas, EffectNodeIndex,
    FillRect, ImageFrameResourceId, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX, VideoSinkHandle,
};
use crate::display_list::layers::LayerStack;
use crate::display_list::layers::planning::{LayerStackInputs, PlannedLayers, plan_layer_stack};
use crate::display_list::storage::{DisplayListStorage, ListChunk};
use crate::display_list::tile_keys::{ContentHash, FrameStateReadByReplay, LayerRasterSettings, LiveContentVersions};
use crate::host::{FfiVisualAnimationTargetKind, FfiVisualAnimationTransformOperationKind};
use crate::node_slot_id::NodeSlotId;
use crate::visual_animation::{
    VisualAnimation, VisualAnimationKeyframe, VisualAnimationTransformOperation, VisualAnimationValue,
};
use crate::visual_context::scroll_state::NO_SCROLL_STATE_SLOT;
use crate::visual_context::{
    BackdropFilterData, CompositorScrollNodeRecord, CompositorScrollTable, EffectNodeData, EffectsData, ScrollData,
    SpatialData, TransformData, TransformDataRole, VisualContextTree,
};
use libgfx_rust::{
    Color, CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, IntRect, ScalingMode,
};
use std::sync::Arc;

pub const VIEWPORT: IntRect = IntRect::new(0, 0, 800, 600);
pub const GREEN: Color = Color::from_rgb(0, 128, 0);

pub struct LiveContentThatNeverChanges;

impl LiveContentVersions for LiveContentThatNeverChanges {
    fn canvas_content_generation(&self, _: CanvasId) -> u64 {
        0
    }
    fn video_frame_id(&self, _: VideoSinkHandle) -> Option<u64> {
        None
    }
    fn image_frame_pixel_generation(&self, _: ImageFrameResourceId) -> Option<u64> {
        None
    }
    fn nested_context_frame_generation(&self, _: CompositorContextId) -> Option<u64> {
        None
    }
}

pub const NO_FRAME_STATE: FrameStateReadByReplay<'static> = FrameStateReadByReplay {
    scrollbars: &[],
    time_ns: 0,
    live_content: &LiveContentThatNeverChanges,
};

pub fn raster_settings_hash_by_layer(stack: &LayerStack) -> Vec<ContentHash> {
    let raster_settings = LayerRasterSettings {
        raster_space_identity: 0,
        clear_color: Color::TRANSPARENT,
        root_group_color: Color::TRANSPARENT,
        viewport_to_raster_transform: FloatMatrix4x4::identity(),
        allows_lcd_text: false,
    };
    vec![raster_settings.content_hash(); stack.layers.len()]
}

pub fn effects(opacity: f32, blend_mode: CompositingAndBlendingOperator) -> EffectNodeData {
    EffectNodeData::Effects(EffectsData {
        isolation: false,
        opacity,
        blend_mode,
        filter: None,
        backdrop_filter: None,
    })
}

pub fn transform(bounds: FloatRect) -> TransformData {
    TransformData {
        matrix: FloatMatrix4x4::identity(),
        origin: FloatPoint {
            x: bounds.x + bounds.width / 2.0,
            y: bounds.y + bounds.height / 2.0,
        },
        sorting_context_root_index: None,
        flattens_inherited_transform: false,
        role: TransformDataRole::CssTransform,
        synthetic_plane: false,
        establishes_sorting_context: false,
        bounds,
    }
}

/// A tree whose root isolation effect has `root_opacity`, and the context of the viewport inside that effect.
pub fn tree_with_root_isolation_effect(root_opacity: f32) -> (VisualContextTree, ContextRef) {
    let mut tree = VisualContextTree::create_identity();
    let root = tree.append_effect(
        effects(root_opacity, CompositingAndBlendingOperator::Normal),
        EffectNodeIndex::NONE,
        VISUAL_VIEWPORT_NODE_INDEX,
        ClipNodeIndex::NONE,
    );
    tree.root_isolation_effect = Some(root);
    let viewport = ContextRef {
        effect: root,
        ..ContextRef::default()
    };
    (tree, viewport)
}

pub fn scroll_node(tree: &mut VisualContextTree, parent: SpatialNodeIndex) -> SpatialNodeIndex {
    tree.append_spatial(
        SpatialData::Scroll(ScrollData {
            state_slot: NO_SCROLL_STATE_SLOT,
            owner_paintable: NodeSlotId::INVALID,
        }),
        parent,
    )
}

/// The record of a scroller that a wheel scrolls down by up to `max_scroll_offset_y`.
pub fn scroll_record(
    node: SpatialNodeIndex,
    parent: SpatialNodeIndex,
    scrollport_rect: IntRect,
    max_scroll_offset_y: f32,
    is_viewport: bool,
) -> CompositorScrollNodeRecord {
    CompositorScrollNodeRecord {
        scroll_node_index: node,
        parent_scroll_node_index: parent,
        scrollport_rect,
        max_scroll_offset: FloatPoint {
            x: 0.0,
            y: max_scroll_offset_y,
        },
        is_viewport,
        can_be_wheel_scrolled_vertically: true,
        ..CompositorScrollNodeRecord::default()
    }
}

/// A tree whose document scrolls by up to 2000 px down the viewport, with the context of the viewport, and those of the
/// document and of a scroller in it per rect of `nested_scrollers`, which scrolls by up to 500 px.
pub fn tree_with_document_scroller(nested_scrollers: &[IntRect]) -> (VisualContextTree, ContextRef, Vec<ContextRef>) {
    let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
    let document_node = scroll_node(&mut tree, VISUAL_VIEWPORT_NODE_INDEX);
    let mut records = vec![scroll_record(
        document_node,
        VISUAL_VIEWPORT_NODE_INDEX,
        VIEWPORT,
        2000.0,
        true,
    )];
    let mut contexts = vec![ContextRef {
        spatial: document_node,
        ..viewport
    }];
    for &scrollport_rect in nested_scrollers {
        let nested_node = scroll_node(&mut tree, document_node);
        records.push(scroll_record(nested_node, document_node, scrollport_rect, 500.0, false));
        contexts.push(ContextRef {
            spatial: nested_node,
            ..viewport
        });
    }
    assert!(tree.set_compositor_scroll_table(CompositorScrollTable {
        nodes: records.into(),
        ..CompositorScrollTable::default()
    }));
    (tree, viewport, contexts)
}

/// A tree whose document scrolls by up to 2000 px down the viewport, with the contexts of the viewport and of the
/// document.
pub fn tree_with_document_scroll() -> (VisualContextTree, ContextRef, ContextRef) {
    let (tree, viewport, contexts) = tree_with_document_scroller(&[]);
    (tree, viewport, contexts[0])
}

pub fn fill(rect: IntRect, color: Color) -> FillRect {
    FillRect {
        rect,
        color,
        compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
        background_color_animation_effect: EffectNodeIndex::NONE,
    }
}

pub fn canvas(rect: IntRect, id: u64) -> DrawCanvas {
    DrawCanvas {
        dst_rect: rect,
        canvas_id: CanvasId(id),
        scaling_mode: ScalingMode::NearestNeighbor,
    }
}

pub fn frosted_glass(tree: &mut VisualContextTree, parent: ContextRef, region: IntRect) -> ContextRef {
    tree.append_effect_node_under(
        parent,
        EffectNodeData::Effects(EffectsData {
            isolation: false,
            opacity: 1.0,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
            backdrop_filter: Some(BackdropFilterData {
                filter: Arc::new(libgfx_rust::filter::Filter::blur(4.0, 4.0, None).serialize()),
                region,
                corner_radii: CornerRadii::default(),
            }),
        }),
    )
}

#[derive(Default)]
pub struct ListOfOneChunk {
    pub builder: DisplayListBuilder,
}

impl ListOfOneChunk {
    pub fn with<C: DisplayListCommand>(mut self, command: C, context: ContextRef) -> Self {
        self.builder.append(&command, &[], context);
        self
    }

    pub fn storage(&self) -> Arc<DisplayListStorage> {
        DisplayListStorage::of_builder(&self.builder)
    }

    pub fn planned(&self, tree: &VisualContextTree, scroll_offsets: &[FloatPoint]) -> PlannedLayers {
        plan_layer_stack(&LayerStackInputs {
            list: &self.storage(),
            tree,
            scroll_offsets,
            viewport_rect: VIEWPORT,
        })
    }

    pub fn stack(&self, tree: &VisualContextTree, scroll_offsets: &[FloatPoint]) -> LayerStack {
        self.planned(tree, scroll_offsets).stack
    }
}

pub fn chunk_of<C: DisplayListCommand>(command: C, context: ContextRef) -> Arc<ListChunk> {
    let mut builder = DisplayListBuilder::new();
    builder.append(&command, &[], context);
    Arc::new(ListChunk::copied(builder.bytes(), builder.command_runs(), 0))
}

/// An animation of `node_index` that takes one second.
pub fn animation_of(
    target_kind: FfiVisualAnimationTargetKind,
    node_index: u32,
    keyframes: Vec<VisualAnimationKeyframe>,
) -> VisualAnimation {
    VisualAnimation {
        target_kind,
        node_indices: vec![node_index],
        timing: crate::animation_timing::AnimationEffectTiming {
            iteration_duration: crate::animation_timing::TimeValue::milliseconds(1000.0),
            ..VisualAnimation::default().timing
        },
        keyframes,
        ..VisualAnimation::default()
    }
}

fn transform_animation_keyframe(
    offset: f64,
    kind: FfiVisualAnimationTransformOperationKind,
    value: f32,
) -> VisualAnimationKeyframe {
    VisualAnimationKeyframe {
        offset,
        easing: crate::easing::Easing::default(),
        value: VisualAnimationValue::Transform(vec![VisualAnimationTransformOperation::new(kind, &[value]).unwrap()]),
    }
}

pub fn transform_animation_of(
    node: SpatialNodeIndex,
    kind: FfiVisualAnimationTransformOperationKind,
    from: f32,
    to: f32,
) -> VisualAnimation {
    animation_of(
        FfiVisualAnimationTargetKind::Transform,
        node.0,
        vec![
            transform_animation_keyframe(0.0, kind, from),
            transform_animation_keyframe(1.0, kind, to),
        ],
    )
}

pub fn spinning(node: SpatialNodeIndex) -> VisualAnimation {
    transform_animation_of(node, FfiVisualAnimationTransformOperationKind::Rotate, 0.0, 360.0)
}

pub fn opacity_animation_of(effect: EffectNodeIndex, from: f32, to: f32) -> VisualAnimation {
    let keyframe = |offset: f64, opacity: f32| VisualAnimationKeyframe {
        offset,
        easing: crate::easing::Easing::default(),
        value: VisualAnimationValue::Opacity(opacity),
    };
    animation_of(
        FfiVisualAnimationTargetKind::Opacity,
        effect.0,
        vec![keyframe(0.0, from), keyframe(1.0, to)],
    )
}
