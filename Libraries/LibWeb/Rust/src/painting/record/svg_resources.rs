/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::display_list::builder::PendingInlineClip;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::display_list::recorder::{IsolatedGroupEffects, OpenRecorderGroup};
use crate::painting::node_painting;
use crate::painting::record::cache::{CaptureKind, CaptureSite};
use crate::painting::record::trace::{Action, Observer, Operation};
use crate::painting::record::{PaintPhase, PaintRecorder};
use crate::painting::visual_context::build::{
    BoxFacts, compute_svg_viewport_transform_data, svg_viewport_transform_of,
};
use crate::painting::visual_context::node_values::{MaskLayerPresenceEntry, mask_layer_presence};
use crate::painting::visual_context::{ClipData, EffectNodeData, EffectNodeIndex, MaskData, MaskLayerOrigin};
use libgfx_rust::path::PathBuilder;
use libgfx_rust::{AffineTransform, CompositingAndBlendingOperator, FloatPoint, FloatRect, MaskKind, multiply_affine};

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum MaskLayerSet {
    CssAndSvg,
    SvgOnly,
}

#[derive(Clone, Copy)]
pub(crate) struct SvgResourceWalk {
    pub enclosing_context: ContextRef,
    pub draws_clip_path_geometry: bool,
}

fn affine_of_matrix(matrix: libgfx_rust::FloatMatrix4x4) -> AffineTransform {
    matrix.extract_2d_affine()
}

fn transformed_rect_clip(transform: AffineTransform, rect: FloatRect, clip: &ClipData) -> PendingInlineClip {
    if transform.b() == 0.0 && transform.c() == 0.0 {
        let mapped = transform.map_rect(rect);
        let mut corner_radii = clip.corner_radii;
        let scale_x = transform.a().abs();
        let scale_y = transform.d().abs();
        let mut has_corner_radii = false;
        for corner in [
            &mut corner_radii.top_left,
            &mut corner_radii.top_right,
            &mut corner_radii.bottom_right,
            &mut corner_radii.bottom_left,
        ] {
            corner.horizontal_radius = (corner.horizontal_radius as f32 * scale_x).round() as i32;
            corner.vertical_radius = (corner.vertical_radius as f32 * scale_y).round() as i32;
            has_corner_radii |= corner.horizontal_radius != 0 || corner.vertical_radius != 0;
        }
        if !has_corner_radii {
            return PendingInlineClip::intersecting_float_rect(mapped);
        }
        return PendingInlineClip::intersecting_rounded_rect(mapped, corner_radii);
    }
    PendingInlineClip::intersecting_path(
        &transformed_rect_path(transform, rect),
        libgfx_rust::WindingRule::Nonzero,
    )
}

fn transformed_rect_path(transform: AffineTransform, rect: FloatRect) -> libgfx_rust::path::OwnedPath {
    let corners = [
        FloatPoint { x: rect.x, y: rect.y },
        FloatPoint {
            x: rect.x + rect.width,
            y: rect.y,
        },
        FloatPoint {
            x: rect.x + rect.width,
            y: rect.y + rect.height,
        },
        FloatPoint {
            x: rect.x,
            y: rect.y + rect.height,
        },
    ]
    .map(|corner| transform.map_point(corner));
    let mut path = PathBuilder::new();
    path.move_to(corners[0].x, corners[0].y);
    for corner in &corners[1..] {
        path.line_to(corner.x, corner.y);
    }
    path.close();
    path.build()
}

fn transformed_int_rect_clip(transform: AffineTransform, rect: libgfx_rust::IntRect) -> PendingInlineClip {
    let rect = rect.to_float();
    if transform.b() == 0.0 && transform.c() == 0.0 {
        return PendingInlineClip::intersecting_float_rect(transform.map_rect(rect));
    }
    PendingInlineClip::intersecting_path(
        &transformed_rect_path(transform, rect),
        libgfx_rust::WindingRule::Nonzero,
    )
}

impl<O: Observer> PaintRecorder<'_, O> {
    fn mask_layer_presence(&self, paintable: NodeSlotId, set: MaskLayerSet) -> Vec<MaskLayerPresenceEntry> {
        mask_layer_presence(self.layout_arena, paintable, set == MaskLayerSet::CssAndSvg)
    }

    fn mask_effect_of_layer(
        &self,
        paintable: NodeSlotId,
        origin: MaskLayerOrigin,
    ) -> Option<(EffectNodeIndex, MaskData)> {
        let tree = self.paint_state.visual_context.tree.as_deref()?;
        self.layout_arena
            .with_paintable_visual_context_node_handles(paintable, |handles| {
                handles
                    .effects
                    .iter()
                    .find_map(|effect| match &tree.effect_nodes[effect.0 as usize].data {
                        EffectNodeData::Mask(mask) if mask.origin == origin => Some((*effect, *mask)),
                        _ => None,
                    })
            })
    }

    pub(crate) fn declare_mask_contents(&mut self, paintable: NodeSlotId, set: MaskLayerSet) -> bool {
        let presence = self.mask_layer_presence(paintable, set);
        if presence.is_empty() {
            return false;
        }
        self.mark_open_captures_unsplicable();
        if set == MaskLayerSet::SvgOnly && presence.iter().any(|layer| layer.area.is_empty()) {
            return true;
        }
        let stacking_context_site_consumes_the_layer = set == MaskLayerSet::CssAndSvg
            && crate::painting::style_queries::establishes_stacking_context(self.layout_arena, paintable);
        for layer in &presence {
            if set == MaskLayerSet::SvgOnly && layer.origin == MaskLayerOrigin::CssMaskLayers {
                continue;
            }
            let Some((effect, mask)) = self.mask_effect_of_layer(paintable, layer.origin) else {
                continue;
            };
            let group = self.recorder.begin_mask_content();
            match layer.origin {
                MaskLayerOrigin::CssMaskLayers => {
                    self.trace_paint(Operation::Producer(Some(paintable), "css-mask"), |this| {
                        this.paint_css_mask_layers(paintable, layer.area);
                    });
                }
                MaskLayerOrigin::SvgMask | MaskLayerOrigin::SvgClip => {
                    if !layer.area.is_empty() || stacking_context_site_consumes_the_layer {
                        self.record_referenced_svg_mask_or_clip_content(
                            paintable,
                            layer.origin,
                            AffineTransform::identity(),
                        );
                    }
                }
            }
            self.recorder.finish_mask_content(group, effect, mask.rect);
        }
        false
    }

    fn first_child_paintable_of_kind(&self, paintable: NodeSlotId, kind: NodeKind) -> Option<NodeSlotId> {
        crate::painting::svg_masking::first_child_paintable_of_kind(self.layout_arena, paintable, kind)
    }

    fn object_bounding_box_content_units_transform(&self, target: NodeSlotId) -> AffineTransform {
        crate::painting::svg_masking::object_bounding_box_content_units_transform(self.layout_arena, target)
    }

    fn record_referenced_svg_mask_or_clip_content(
        &mut self,
        target: NodeSlotId,
        origin: MaskLayerOrigin,
        target_to_enclosing_space: AffineTransform,
    ) {
        let (resource_kind, draws_clip_path_geometry, producer) = match origin {
            MaskLayerOrigin::SvgMask => (NodeKind::SVGMaskBox, false, "svg-mask"),
            MaskLayerOrigin::SvgClip => (NodeKind::SVGClipBox, true, "svg-clip"),
            MaskLayerOrigin::CssMaskLayers => unreachable!("CSS mask layers are painted, not walked"),
        };
        let Some(resource_box) = self.first_child_paintable_of_kind(target, resource_kind) else {
            return;
        };
        let content_units_object_bbox =
            crate::painting::paintable_geometry::committed_svg_resource_content_units_are_object_bounding_box(
                self.layout_arena,
                resource_box,
            );
        let mut content_units_transform_in_recorded_space = if content_units_object_bbox {
            self.object_bounding_box_content_units_transform(target)
        } else {
            AffineTransform::identity()
        };
        let device_scale = self.inputs.device_pixels_per_css_pixel as f32;
        content_units_transform_in_recorded_space.values[4] *= device_scale;
        content_units_transform_in_recorded_space.values[5] *= device_scale;
        let root_transform = multiply_affine(target_to_enclosing_space, content_units_transform_in_recorded_space);
        // A mask's output is coverage rather than color anyone sees, and a luminance mask's lightness is its alpha,
        // so force-dark stays out of it. Patterns keep it: they render as page content.
        let suspended_force_dark = self.recorder.suspend_force_dark();
        self.trace_paint(Operation::Producer(Some(target), producer), |this| {
            this.walk_svg_resource(resource_box, root_transform, true, draws_clip_path_geometry);
        });
        self.recorder.restore_force_dark(suspended_force_dark);
    }

    fn paint_css_mask_layers(&mut self, paintable: NodeSlotId, area: CssPixelRect) {
        let layout_arena = self.layout_arena;
        let style = layout_arena
            .node_style_if_live(paintable)
            .expect("the mask recording target holds a live layout node");
        let is_root_element = crate::painting::style_queries::node_is_root_element(layout_arena, paintable);
        let resolved =
            crate::painting::record::paint::background_resolution::resolve_mask_layers(self, paintable, style, area);
        // A mask's output is coverage rather than color anyone sees, and a luminance mask's lightness is its alpha,
        // so force-dark stays out of it.
        let suspended_force_dark = self.recorder.suspend_force_dark();
        // FIXME: Respect `image-rendering` here.
        let inputs = crate::painting::record::paint::background_resolution::BackgroundPaintInputs {
            resolved,
            border_radii: crate::painting::border_radii::BorderRadii::default(),
            image_rendering: crate::css::css_enums::image_rendering::AUTO,
            is_root_element,
        };
        crate::painting::record::paint::background::paint_resolved_background(self, paintable, &inputs);
        self.recorder.restore_force_dark(suspended_force_dark);
    }

    pub(crate) fn walk_svg_resource(
        &mut self,
        root: NodeSlotId,
        root_transform: AffineTransform,
        include_root_element_transform: bool,
        draws_clip_path_geometry: bool,
    ) {
        let walk = SvgResourceWalk {
            enclosing_context: self.recorder.accumulated_visual_context(),
            draws_clip_path_geometry,
        };
        let enclosing_walk = self.svg_resource_walk.replace(walk);
        let enclosing_transform = self.recorder.set_ambient_inline_transform(Some(root_transform));
        self.paint_node(root, PaintPhase::Background);
        self.paint_node(root, PaintPhase::Border);
        self.paint_svg_box_inside_resource(root, root_transform, include_root_element_transform);
        self.recorder.set_ambient_inline_transform(enclosing_transform);
        self.svg_resource_walk = enclosing_walk;
        self.recorder.set_accumulated_visual_context(walk.enclosing_context);
    }

    pub(crate) fn paint_svg_box_inside_resource(
        &mut self,
        svg_box: NodeSlotId,
        parent_to_enclosing_space: AffineTransform,
        include_element_transform: bool,
    ) {
        self.trace_scope(Operation::Producer(Some(svg_box), "svg"), Action::Walk, |this| {
            this.paint_svg_box_inside_resource_impl(svg_box, parent_to_enclosing_space, include_element_transform);
        });
    }

    fn paint_svg_box_inside_resource_impl(
        &mut self,
        svg_box: NodeSlotId,
        parent_to_enclosing_space: AffineTransform,
        include_element_transform: bool,
    ) {
        let walk = self
            .svg_resource_walk
            .expect("resource content paints inside a resource walk");
        if self
            .mask_layer_presence(svg_box, MaskLayerSet::SvgOnly)
            .iter()
            .any(|layer| layer.area.is_empty())
        {
            return;
        }
        let facts = BoxFacts::gather(
            self.layout_arena,
            self.visual_context_host,
            svg_box,
            self.inputs.device_pixels_per_css_pixel,
            false,
        );
        self.recorder.set_accumulated_visual_context(walk.enclosing_context);

        let effects_group = facts
            .effects_data()
            .map(|effects| (self.recorder.begin_isolated_group(), effects));

        let mut to_enclosing_space = parent_to_enclosing_space;
        if include_element_transform && let Some(transform) = facts.transform {
            to_enclosing_space = multiply_affine(
                to_enclosing_space,
                affine_of_matrix(transform.matrix_including_origin()),
            );
        }

        let mut mask_groups: Vec<(OpenRecorderGroup, MaskData, usize)> = Vec::new();
        for mask in facts
            .mask_layers
            .iter()
            .filter(|layer| layer.origin != MaskLayerOrigin::CssMaskLayers)
        {
            let clip_depth = self.recorder.ambient_inline_clip_depth();
            self.recorder
                .push_ambient_inline_clips(&[transformed_int_rect_clip(to_enclosing_space, mask.rect)]);
            mask_groups.push((self.recorder.begin_isolated_group(), *mask, clip_depth));
        }

        let enclosing_transform = self.recorder.set_ambient_inline_transform(Some(to_enclosing_space));
        self.paint_svg_box_own_content_inside_resource(svg_box);

        let clip_depth = self.recorder.ambient_inline_clip_depth();
        if facts.may_have_clip
            && let Some(clip) = facts.overflow_clip
        {
            self.recorder
                .push_ambient_inline_clips(&[transformed_rect_clip(to_enclosing_space, clip.rect, &clip)]);
        }
        let mut descendants_to_enclosing_space = to_enclosing_space;
        if let Some(svg_viewport_transform) = svg_viewport_transform_of(self.layout_arena, svg_box) {
            let viewport_transform_data = compute_svg_viewport_transform_data(
                self.layout_arena,
                svg_box,
                svg_viewport_transform,
                self.inputs.device_pixels_per_css_pixel,
            );
            descendants_to_enclosing_space = multiply_affine(
                descendants_to_enclosing_space,
                affine_of_matrix(viewport_transform_data.matrix),
            );
        }
        self.recorder
            .set_ambient_inline_transform(Some(descendants_to_enclosing_space));
        let mut child = crate::painting::paint_order::first_paint_child(self.layout_arena, svg_box);
        while let Some(current) = child {
            child = crate::painting::paint_order::next_paint_sibling(self.layout_arena, current);
            self.paint_svg_box_inside_resource(current, descendants_to_enclosing_space, true);
        }
        self.recorder.truncate_ambient_inline_clips(clip_depth);
        self.recorder.set_ambient_inline_transform(enclosing_transform);

        for (mut group, mask, clip_depth) in mask_groups.into_iter().rev() {
            self.recorder.begin_group_mask(&mut group);
            self.record_referenced_svg_mask_or_clip_content(svg_box, mask.origin, to_enclosing_space);
            self.recorder.finish_group_with_effects(
                group,
                IsolatedGroupEffects {
                    clip_rect: None,
                    opacity: 1.0,
                    filter: None,
                    compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                    mask_kind: mask.kind,
                },
            );
            self.recorder.truncate_ambient_inline_clips(clip_depth);
        }
        if let Some((group, effects)) = effects_group {
            self.recorder.finish_group_with_effects(
                group,
                IsolatedGroupEffects {
                    clip_rect: None,
                    opacity: effects.opacity,
                    filter: effects.filter,
                    compositing_and_blending_operator: effects.blend_mode,
                    mask_kind: MaskKind::Alpha,
                },
            );
        }
    }

    fn paint_svg_box_own_content_inside_resource(&mut self, svg_box: NodeSlotId) {
        // For elements with SVG filters, emit a transparent FillRect to trigger filter application.
        // This ensures content-generating filters (feFlood, feImage) work even with empty source.
        if let Some(svg_filter_bounds) = self.layout_arena.paintable_side_data(svg_box).svg_filter_bounds.get() {
            let device_rect = self
                .converter
                .enclosing_device_rect(crate::css::css_pixels::CssPixelRect::from(svg_filter_bounds));
            self.recorder.fill_rect_transparent(device_rect);
        }
        let kind = self.layout_arena.node_kind_if_live(svg_box);
        if kind != Some(NodeKind::SVGSVGBox)
            && !kind.is_some_and(node_painting::is_svg)
            && kind.is_some_and(crate::layout::node_facts::kind_is_replaced_box)
        {
            self.paint_svg_box_phase_inside_resource(svg_box, PaintPhase::Background);
        }
        self.paint_svg_box_phase_inside_resource(svg_box, PaintPhase::Foreground);
    }

    fn paint_svg_box_phase_inside_resource(&mut self, svg_box: NodeSlotId, phase: PaintPhase) {
        self.trace_paint(
            Operation::Capture(CaptureSite {
                paintable: svg_box,
                kind: CaptureKind::BoxPhase(phase),
            }),
            |this| crate::painting::record::paint::paint(this, svg_box, phase),
        );
    }
}
