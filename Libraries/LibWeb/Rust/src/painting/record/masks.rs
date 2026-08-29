/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::display_list::commands::DisplayListResourceId;
use crate::painting::display_list::commands::{ContextRef, VISUAL_VIEWPORT_NODE_INDEX};
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::host::FfiMaskDisplayListRegistration;
use crate::painting::node_painting;
use crate::painting::paintable_geometry::absolute_border_box_rect;
use crate::painting::record::{NestedRecordingState, PaintPhase, PaintRecorder};
use crate::painting::visual_context::nested::{NestedAssignments, build_nested_svg_visual_context_tree};
use crate::painting::visual_context::{FrameData, FrameNodeIndex, MaskLayerOrigin, TransformData, TransformDataRole};
use libgfx_rust::{AffineTransform, FloatPoint};
use libgfx_rust::{affine_to_matrix, translated_then_multiplied};
use std::collections::HashMap;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum MaskLayerSet {
    CssAndSvg,
    SvgOnly,
}

#[derive(Clone)]
pub(crate) struct PrerecordedMaskLayer {
    pub origin: MaskLayerOrigin,
    pub mask_layer_area_is_empty: bool,
    pub display_list_id: Option<DisplayListResourceId>,
}

#[derive(Default)]
pub(crate) struct PrerecordedNestedDisplayLists {
    pub mask_entries: HashMap<u32, Vec<PrerecordedMaskLayer>>,
    pub pattern_paint_styles: HashMap<u32, crate::painting::display_list::recorder::PaintStyle>,
}

struct MaskLayerPresence {
    origin: MaskLayerOrigin,
    device_rect: libgfx_rust::IntRect,
    area: CssPixelRect,
}

impl PaintRecorder<'_> {
    fn mask_layer_presence(&mut self, paintable: NodeSlotId, set: MaskLayerSet) -> Vec<MaskLayerPresence> {
        crate::painting::visual_context::node_values::mask_layer_presence(
            self.layout_arena,
            self.visual_context_host,
            paintable,
            set == MaskLayerSet::CssAndSvg,
        )
        .into_iter()
        .map(|layer| MaskLayerPresence {
            origin: layer.origin,
            device_rect: self.converter.enclosing_device_rect(layer.area),
            area: layer.area,
        })
        .collect()
    }

    fn record_mask_entry(&mut self, paintable: NodeSlotId, set: MaskLayerSet) -> bool {
        let presence = self.mask_layer_presence(paintable, set);
        if presence.is_empty() {
            return false;
        }
        let mut any_svg_mask_layer_area_is_empty = false;
        let mut layers = Vec::new();
        for layer in presence {
            match layer.origin {
                MaskLayerOrigin::CssMaskLayers => {
                    let id = self.paint_css_mask_layers_to_display_list(paintable, layer.area);
                    layers.push(PrerecordedMaskLayer {
                        origin: layer.origin,
                        mask_layer_area_is_empty: layer.area.is_empty(),
                        display_list_id: Some(id),
                    });
                }
                MaskLayerOrigin::SvgMask | MaskLayerOrigin::SvgClip => {
                    let mask_layer_area_is_empty = layer.area.is_empty();
                    any_svg_mask_layer_area_is_empty |= mask_layer_area_is_empty;
                    let stacking_context_site_consumes_the_layer = set == MaskLayerSet::CssAndSvg
                        && crate::painting::style_queries::establishes_stacking_context(self.layout_arena, paintable);
                    if mask_layer_area_is_empty && !stacking_context_site_consumes_the_layer {
                        layers.push(PrerecordedMaskLayer {
                            origin: layer.origin,
                            mask_layer_area_is_empty: true,
                            display_list_id: None,
                        });
                        continue;
                    }
                    let id = if layer.origin == MaskLayerOrigin::SvgMask {
                        self.calculate_svg_mask_display_list(paintable, layer.device_rect)
                    } else {
                        self.calculate_svg_clip_display_list(paintable, layer.device_rect)
                    };
                    layers.push(PrerecordedMaskLayer {
                        origin: layer.origin,
                        mask_layer_area_is_empty,
                        display_list_id: id,
                    });
                }
            }
        }
        self.prerecorded.mask_entries.insert(paintable.index, layers);
        any_svg_mask_layer_area_is_empty
    }

    pub(crate) fn register_mask_display_lists(&mut self, paintable: NodeSlotId, set: MaskLayerSet) -> bool {
        let Some(layers) = self.prerecorded.mask_entries.get(&paintable.index).cloned() else {
            assert!(
                self.mask_layer_presence(paintable, set).is_empty(),
                "a masked box without a prerecorded mask entry"
            );
            return false;
        };
        let mut mask_frames: Vec<(FrameNodeIndex, MaskLayerOrigin)> = Vec::new();
        if let Some(nested) = &self.nested {
            if let Some(frames) = nested.assignments.mask_frames.get(&paintable.index) {
                for frame in frames {
                    if let Some(tree) = &self.nested_tree
                        && let FrameData::Mask(mask) = &tree.frame_nodes[frame.0 as usize].data
                    {
                        mask_frames.push((*frame, mask.origin));
                    }
                }
            }
        } else if let Some(tree) = self.paint_state.visual_context.tree.as_deref() {
            self.layout_arena
                .with_paintable_visual_context_node_handles(paintable, |handles| {
                    for index in handles.frame_handles() {
                        if let FrameData::Mask(mask) = &tree.frame_nodes[index.0 as usize].data {
                            mask_frames.push((index, mask.origin));
                        }
                    }
                });
        }
        let mut any_svg_mask_layer_area_is_empty = false;
        for layer in layers {
            if set == MaskLayerSet::SvgOnly && layer.origin == MaskLayerOrigin::CssMaskLayers {
                continue;
            }
            self.mark_open_captures_unsplicable();
            if set == MaskLayerSet::SvgOnly && layer.mask_layer_area_is_empty {
                any_svg_mask_layer_area_is_empty = true;
                continue;
            }
            let Some(display_list_id) = layer.display_list_id else {
                continue;
            };
            let frames: Vec<FrameNodeIndex> = mask_frames
                .iter()
                .filter(|(_, origin)| *origin == layer.origin)
                .map(|(frame, _)| *frame)
                .collect();
            assert!(!frames.is_empty(), "a mask display list without a mask node");
            self.recorder.register_mask_display_list(&frames, display_list_id);
        }
        any_svg_mask_layer_area_is_empty
    }

    pub(crate) fn prerecord_nested_display_lists(&mut self) {
        let masked_paintables = self.paint_state.visual_context.paintables_with_mask_nodes.clone();
        for paintable in masked_paintables {
            if !self.layout_arena.paintable_row_is_populated(paintable) {
                continue;
            }
            self.record_mask_entry(paintable, MaskLayerSet::CssAndSvg);
        }

        for node in self.layout_arena.svg_pattern_referencing_nodes() {
            if !self.layout_arena.paintable_row_is_populated(node) {
                continue;
            }
            if self.layout_node_is_inside_svg_resource_box(node) {
                continue;
            }
            if self.recorded_mask_entry_has_empty_svg_mask_layer_area(node) {
                continue;
            }
            crate::painting::record::paint::svg::record_pattern_paint_styles(self, node);
        }
    }

    fn prerecord_nested_display_lists_for_svg_subtree(&mut self, root: NodeSlotId) {
        let has_mask_nodes = self
            .nested
            .as_ref()
            .expect("the nested pre-pass runs with nested assignments")
            .assignments
            .mask_frames
            .contains_key(&root.index);
        if has_mask_nodes && self.record_mask_entry(root, MaskLayerSet::SvgOnly) {
            return;
        }
        crate::painting::record::paint::svg::record_pattern_paint_styles(self, root);
        let mut child = crate::painting::paint_order::first_paint_child(self.layout_arena, root);
        while let Some(paintable) = child {
            child = crate::painting::paint_order::next_paint_sibling(self.layout_arena, paintable);
            self.prerecord_nested_display_lists_for_svg_subtree(paintable);
        }
    }

    fn layout_node_is_inside_svg_resource_box(&self, node: NodeSlotId) -> bool {
        let mut ancestor = self.layout_arena.node_parent_if_live(node);
        while let Some(candidate) = ancestor {
            if matches!(
                self.layout_arena.node_kind_if_live(candidate),
                Some(NodeKind::SVGPatternBox | NodeKind::SVGMaskBox | NodeKind::SVGClipBox)
            ) {
                return true;
            }
            ancestor = self.layout_arena.node_parent_if_live(candidate);
        }
        false
    }

    fn recorded_mask_entry_has_empty_svg_mask_layer_area(&self, paintable: NodeSlotId) -> bool {
        let Some(layers) = self.prerecorded.mask_entries.get(&paintable.index) else {
            return false;
        };
        layers
            .iter()
            .any(|layer| layer.origin != MaskLayerOrigin::CssMaskLayers && layer.mask_layer_area_is_empty)
    }

    fn first_child_paintable_of_kind(&self, paintable: NodeSlotId, kind: NodeKind) -> Option<NodeSlotId> {
        let arena = self.layout_arena;
        let mut child = arena.node_first_child_if_live(paintable);
        while let Some(node) = child {
            if arena.node_kind_if_live(node) == Some(kind) {
                return self.layout_arena.paintable_row_is_populated(node).then_some(node);
            }
            child = arena.node_next_sibling_if_live(node);
        }
        None
    }

    fn target_user_space_object_bounding_box(&self, target: NodeSlotId) -> CssPixelRect {
        if self
            .layout_arena
            .node_kind_if_live(target)
            .is_some_and(node_painting::is_svg_path)
            && let Some(path) = crate::painting::paintable_geometry::committed_svg_path(self.layout_arena, target)
        {
            let [x, y, width, height] = path.bounding_box();
            return CssPixelRect::new(
                CssPixels::nearest_value_for_f32(x),
                CssPixels::nearest_value_for_f32(y),
                CssPixels::nearest_value_for_f32(width),
                CssPixels::nearest_value_for_f32(height),
            );
        }
        absolute_border_box_rect(self.layout_arena, target)
    }

    fn object_bounding_box_content_units_transform(&self, target: NodeSlotId) -> AffineTransform {
        let bounding_box = self.target_user_space_object_bounding_box(target);
        AffineTransform {
            values: [
                bounding_box.width.to_float(),
                0.0,
                0.0,
                bounding_box.height.to_float(),
                bounding_box.x.to_float(),
                bounding_box.y.to_float(),
            ],
        }
    }

    fn calculate_svg_mask_display_list(
        &mut self,
        target: NodeSlotId,
        mask_device_rect: libgfx_rust::IntRect,
    ) -> Option<DisplayListResourceId> {
        let mask_paintable = self.first_child_paintable_of_kind(target, NodeKind::SVGMaskBox)?;
        let content_units_transform = if self.hit_test_facts(target).svg_mask_content_units_object_bbox {
            self.object_bounding_box_content_units_transform(target)
        } else {
            AffineTransform::identity()
        };
        Some(self.paint_mask_or_clip_to_display_list(content_units_transform, mask_paintable, mask_device_rect, false))
    }

    fn calculate_svg_clip_display_list(
        &mut self,
        target: NodeSlotId,
        clip_device_rect: libgfx_rust::IntRect,
    ) -> Option<DisplayListResourceId> {
        let clip_paintable = self.first_child_paintable_of_kind(target, NodeKind::SVGClipBox)?;
        let content_units_transform = if self.hit_test_facts(target).svg_clip_path_units_object_bbox {
            self.object_bounding_box_content_units_transform(target)
        } else {
            AffineTransform::identity()
        };
        Some(self.paint_mask_or_clip_to_display_list(content_units_transform, clip_paintable, clip_device_rect, true))
    }

    fn paint_mask_or_clip_to_display_list(
        &mut self,
        content_units_transform: AffineTransform,
        paintable: NodeSlotId,
        mask_device_rect: libgfx_rust::IntRect,
        is_clip_path: bool,
    ) -> DisplayListResourceId {
        let device_scale = self.inputs.device_pixels_per_css_pixel as f32;
        // Content records in the resource's own units scaled by the device pixel ratio; the root maps
        // it through the content-units transform into the target's user space and rebases onto the
        // mask surface origin.
        let mut content_units_transform_in_recorded_space = content_units_transform;
        content_units_transform_in_recorded_space.values[4] = content_units_transform.values[4] * device_scale;
        content_units_transform_in_recorded_space.values[5] = content_units_transform.values[5] * device_scale;
        let surface_origin = FloatPoint {
            x: mask_device_rect.x as f32,
            y: mask_device_rect.y as f32,
        };
        let recorded_to_surface = translated_then_multiplied(
            FloatPoint {
                x: -surface_origin.x,
                y: -surface_origin.y,
            },
            content_units_transform_in_recorded_space,
        );
        let root_transform = TransformData {
            matrix: affine_to_matrix(recorded_to_surface),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        };
        self.record_nested_svg_display_list(paintable, root_transform, true, is_clip_path)
    }

    fn paint_css_mask_layers_to_display_list(
        &mut self,
        paintable: NodeSlotId,
        area: CssPixelRect,
    ) -> DisplayListResourceId {
        let mask_rect = CssPixelRect::new(CssPixels::default(), CssPixels::default(), area.width, area.height);
        let layout_arena = self.layout_arena;
        let node = paintable;
        let style = layout_arena
            .node_style_if_live(node)
            .expect("the mask recording target holds a live layout node");
        let is_root_element = crate::painting::style_queries::node_is_root_element(layout_arena, node);
        let resolved = crate::painting::record::paint::background_resolution::resolve_mask_layers(
            self, paintable, style, mask_rect,
        );
        let tree = crate::painting::visual_context::VisualContextTree::create(TransformData {
            matrix: libgfx_rust::FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        });
        let root_context = ContextRef::spatial_only(VISUAL_VIEWPORT_NODE_INDEX);
        let mut session = self.nested_recording_session(
            // A mask's output is coverage rather than color anyone sees, and a luminance mask's lightness is its alpha,
            // so force-dark stays out of it. Patterns are recorded elsewhere and keep it: they render as page content.
            DisplayListRecorder::new(None),
            Some(NestedRecordingState {
                assignments: NestedAssignments::default(),
            }),
            Some(tree),
            false,
        );
        session.recorder.set_accumulated_visual_context(root_context);
        // FIXME: Respect `image-rendering` here.
        let inputs = crate::painting::record::paint::background_resolution::BackgroundPaintInputs {
            resolved,
            border_radii: crate::painting::border_radii::BorderRadii::default(),
            image_rendering: crate::css::css_enums::image_rendering::AUTO,
            is_root_element,
        };
        crate::painting::record::paint::background::paint_resolved_background(&mut session, paintable, &inputs);
        let tree = session
            .nested_tree
            .take()
            .expect("CSS mask recording has a visual context tree");
        let recorded = session.recorder.into_builder().finish();
        self.paint_host.nested_display_list_from_tree(&recorded, tree, &[])
    }

    pub(crate) fn record_nested_svg_display_list(
        &mut self,
        root: NodeSlotId,
        root_transform: TransformData,
        include_root_element_transform: bool,
        is_clip_path: bool,
    ) -> DisplayListResourceId {
        let (tree, assignments) = build_nested_svg_visual_context_tree(
            self.layout_arena,
            self.visual_context_host,
            root,
            root_transform,
            include_root_element_transform,
            self.inputs.device_pixels_per_css_pixel,
        );
        let mut session = self.nested_recording_session(
            // A mask's output is coverage rather than color anyone sees, and a luminance mask's lightness is its alpha,
            // so force-dark stays out of it. Patterns are recorded elsewhere and keep it: they render as page content.
            DisplayListRecorder::new(None),
            Some(NestedRecordingState { assignments }),
            Some(tree),
            is_clip_path,
        );
        session.prerecord_nested_display_lists_for_svg_subtree(root);
        session.paint_svg(root, PaintPhase::Foreground);

        let tree = session.nested_tree.take().expect("nested tree");
        let nested_recorder = session.recorder;
        let mask_display_lists: Vec<FfiMaskDisplayListRegistration> = nested_recorder
            .mask_display_lists()
            .iter()
            .copied()
            .map(FfiMaskDisplayListRegistration::from)
            .collect();
        let recorded = nested_recorder.into_builder().finish();
        self.paint_host
            .nested_display_list_from_tree(&recorded, tree, &mask_display_lists)
    }
}
