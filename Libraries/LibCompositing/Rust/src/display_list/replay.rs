/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::display_list::builder::{
    bounding_rect_computed_from_payload, for_each_command, has_inline_clip_in_its_own_space, inline_clips_of,
    inline_transform_of, read_command,
};
use crate::display_list::commands::{
    ClipMode, ClipNodeIndex, DisplayListCommandHeader, DisplayListCommandRun, DisplayListCommandType,
    DisplayListPaintStyleType, DrawCompositedContext, DrawIsolatedGroup, DrawRepeatedTile, EffectNodeIndex, FillPath,
    FillRect, FillRectWithRoundedCorners, InlineClipKind, OptionalFloatRect, PaintCaret, PaintScrollBar,
    PaintVectorImage, PathPaintKind, ReplayClip, ReplayLayer, ReplayMask, SpatialNodeIndex, StrokePath,
};
use crate::display_list::depth_sorted_plan::{DepthSortedReplayStepKind, build_depth_sorted_replay_plan};
use crate::display_list::effect_clip_plan::EffectClipPlan;
use crate::display_list::effect_geometry::{
    EffectGeometry, Extent, compute_effect_geometry, nearest_effects_at_or_above_where,
};
use crate::display_list::nested_records::{holds_nested_records, span_bytes};
use crate::display_list::storage::{ChunkIndexInList, DisplayListStorage, ListSpan};
use crate::host::{
    CompositedContextDraw, IsolatedGroup, PresentedScrollbar, ResolvedFillRect, ResolvedFillRectWithRoundedCorners,
    SurfaceDraw, TileRaster, VectorImagePlacement, VectorImageReplay,
};
use crate::visual_context::queries::TreeCullingScratch;
use crate::visual_context::{
    ClipNodeData, ContextRef, EffectNodeData, RasterRoot, VisualContextTree, WorldTransforms, device_offset_for_index,
    resolve_leaf_to_context_matrices,
};
use libgfx_rust::filter::Filter;
use libgfx_rust::path::Path;
use libgfx_rust::{
    AffineTransform, Color, CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect,
    FloatVector3, IntRect, IntSize, MaskKind, WindingRule, enclosing_int_rect, multiply_affine,
};
use std::cell::RefCell;
use std::hash::Hasher;

/// The raster primitives a replay draws through. Everything a painter is handed is resolved for the frame: it never
/// sees the visual context tree, the scroll offsets or the scrollbars of the frame.
pub trait ReplayPainter {
    fn canvas_matrix(&mut self) -> FloatMatrix4x4;
    fn set_matrix(&mut self, matrix: &FloatMatrix4x4);
    fn would_be_fully_clipped_by_painter(&mut self, rect: IntRect) -> bool;
    /// The bounds of the area the clips leave the next draw, in the pixels of the canvas.
    fn device_clip_bounds(&mut self) -> IntRect;
    fn push_clip(&mut self, clip: &ReplayClip);
    fn push_clip_path(&mut self, path: &Path, winding_rule: WindingRule);
    fn push_layer(&mut self, layer: &ReplayLayer);
    /// `backdrop`, when given, carries the backdrop filter the mask's layer starts with.
    fn push_mask(&mut self, mask: &ReplayMask, backdrop: Option<&ReplayLayer>);
    /// Starts the content that masks the layer `push_mask` opened. `pop_mask` closes both.
    fn begin_mask_content(&mut self, mask: &ReplayMask);
    fn pop_mask(&mut self);
    fn pop(&mut self);
    fn push_device_space_plane_clip(&mut self, vertices: &[FloatVector3]);
    fn push_transform(&mut self, transform: &AffineTransform);
    fn push_clip_path_bytes(&mut self, path_bytes: &[u8], winding_rule: WindingRule, mode: ClipMode);
    /// Plays one command as it was recorded. `command` holds the command struct, which can differ from the one at the
    /// start of `payload`; the command's spans point into `payload`.
    fn play_command(&mut self, command_type: DisplayListCommandType, command: &[u8], payload: &[u8]);
    fn fill_rect(&mut self, fill: &ResolvedFillRect);
    fn fill_rect_with_rounded_corners(&mut self, fill: &ResolvedFillRectWithRoundedCorners);
    fn paint_scrollbar(&mut self, scrollbar: &PresentedScrollbar);
    fn draw_composited_context(&mut self, draw: &CompositedContextDraw);
    fn begin_isolated_group(&mut self, group: &IsolatedGroup);
    fn begin_isolated_group_mask(&mut self, kind: MaskKind);
    fn end_isolated_group_mask(&mut self);
    fn end_isolated_group(&mut self);
    /// Answers whether the tile records are to be replayed into the raster, which `end_tile_raster` then finishes.
    fn begin_tile_raster(&mut self, tile: &TileRaster) -> bool;
    fn end_tile_raster(&mut self);
    /// Unless the painter skips the image, the replay of its records is followed by `end_vector_image`.
    fn begin_vector_image(
        &mut self,
        placement: &VectorImagePlacement,
        requires_direct_replay: bool,
    ) -> VectorImageReplay;
    fn end_vector_image(&mut self);
    /// Draws pixels of a surface of the compositor under the canvas's matrix and clip.
    fn draw_surface(&mut self, draw: &SurfaceDraw);

    fn execute_run<'t>(&mut self, _run_index: usize, records: &'t [u8], context: &RecordsContext<'_, 't>) {
        replay_records(records, context, self);
    }
}

/// A scrollbar of the page as the frame presents it, which replaces the geometry its display list recorded.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ScrollbarShownByFrame {
    pub scroll_node_index: SpatialNodeIndex,
    pub vertical: bool,
    pub gutter_rect: FloatRect,
    pub thumb_rect: FloatRect,
    /// How far the scrollbar has grown from its thin geometry into its expanded one.
    pub expansion: f32,
    pub opacity: f32,
}

/// The scrollbar of the frame that a scrollbar command of the display list paints, if the frame shows it.
pub fn scrollbar_shown_by_frame<'a>(
    scrollbars: &'a [ScrollbarShownByFrame],
    command: &PaintScrollBar,
) -> Option<&'a ScrollbarShownByFrame> {
    scrollbars.iter().find(|scrollbar| {
        scrollbar.scroll_node_index == command.scroll_node_index && scrollbar.vertical == command.vertical
    })
}

/// The surface that shows the frame of a context drawn into another, and the part of it the frame is in.
#[derive(Clone, Copy, Debug)]
pub struct ResolvedCompositedContext {
    pub surface: *mut std::ffi::c_void,
    pub content_rect: FloatRect,
    /// The part of `content_rect` the surface holds, which is less than all of it where the frame was too large to
    /// raster whole.
    pub held_content_rect: FloatRect,
}

/// Where a replay finds the frames of the contexts the list draws.
pub trait CompositedContexts {
    /// The frame of the child context as it is drawn into `destination_rect` under `canvas_matrix`, where the clips
    /// leave it `visible_device_rect` of the canvas; none where the context shows nothing there.
    fn resolve(
        &self,
        child_context_id: u64,
        destination_rect: FloatRect,
        canvas_matrix: &FloatMatrix4x4,
        visible_device_rect: IntRect,
    ) -> Option<ResolvedCompositedContext>;
}

/// The contexts of a replay that draws no other context.
pub struct NoCompositedContexts;

impl CompositedContexts for NoCompositedContexts {
    fn resolve(
        &self,
        _child_context_id: u64,
        _destination_rect: FloatRect,
        _canvas_matrix: &FloatMatrix4x4,
        _visible_device_rect: IntRect,
    ) -> Option<ResolvedCompositedContext> {
        None
    }
}

/// The frame state a replay paints a display list with: where the frame presents the scroll containers and the
/// scrollbars, the time it shows a blinking caret at, and the frames of the contexts it draws.
#[derive(Clone, Copy)]
pub struct ReplayFrame<'a> {
    pub scroll_offsets: &'a [FloatPoint],
    pub scrollbars: &'a [ScrollbarShownByFrame],
    pub time_ns: i64,
    pub composited_contexts: &'a dyn CompositedContexts,
}

impl ReplayFrame<'_> {
    /// A frame that scrolls nothing, presents no scrollbar of its own, and draws no other context.
    pub fn unscrolled(time_ns: i64) -> Self {
        Self {
            scroll_offsets: &[],
            scrollbars: &[],
            time_ns,
            composited_contexts: &NoCompositedContexts,
        }
    }

    fn without_scroll_offsets(&self) -> Self {
        Self {
            scroll_offsets: &[],
            ..*self
        }
    }

    fn without_scroll_offsets_or_scrollbars(&self) -> Self {
        Self {
            scroll_offsets: &[],
            scrollbars: &[],
            ..*self
        }
    }
}

/// What playing a stream of records needs besides the records.
pub struct RecordsContext<'c, 't> {
    /// The tree the records were recorded against.
    pub tree: &'t VisualContextTree,
    pub frame: &'c ReplayFrame<'c>,
    pub to_root_matrix_of_inline_clip_space: &'c dyn Fn(SpatialNodeIndex) -> Option<FloatMatrix4x4>,
}

/// How far the inline clips of a command that apply in spaces of their own let it draw.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum OwnSpaceClipExtent {
    /// One of the clips cannot be mapped into the space of the command, or cuts a hole out of it.
    Unbounded,
    /// Within this rect, in the space of the command.
    Within(FloatRect),
}

/// The part of `command_bounds`, in the space of a command whose to-root matrix is `command_to_root`, that its inline
/// clips in spaces of their own leave the command to draw in, or None if it has no such clip.
pub(crate) fn own_space_clip_extent(
    header: &DisplayListCommandHeader,
    payload: &[u8],
    command_bounds: FloatRect,
    command_to_root: FloatMatrix4x4,
    mut to_root_matrix_of_clip_space: impl FnMut(SpatialNodeIndex) -> Option<FloatMatrix4x4>,
) -> Option<OwnSpaceClipExtent> {
    let mut extent = None;
    for clip in inline_clips_of(header, payload).filter(|clip| clip.has_own_space) {
        let clip_rect_in_command_space = (clip.mode == ClipMode::Intersect && command_to_root.is_2d_affine())
            .then(|| to_root_matrix_of_clip_space(clip.space))
            .flatten()
            .filter(|clip_to_root| clip_to_root.is_2d_affine())
            .and_then(|clip_to_root| {
                let root_to_command = command_to_root.extract_2d_affine().inverse()?;
                let clip_to_command = multiply_affine(root_to_command, clip_to_root.extract_2d_affine());
                Some(clip_to_command.map_rect(clip.clip_rect_or_path_device_bounds))
            });
        let Some(clip_rect_in_command_space) = clip_rect_in_command_space else {
            return Some(OwnSpaceClipExtent::Unbounded);
        };
        let extent_so_far = match extent {
            Some(OwnSpaceClipExtent::Within(rect)) => rect,
            _ => command_bounds,
        };
        extent = Some(OwnSpaceClipExtent::Within(
            extent_so_far.intersected(clip_rect_in_command_space),
        ));
    }
    extent
}

const CARET_BLINK_INTERVAL_NS: i64 = 500_000_000;

pub(crate) fn caret_is_visible_at_time(caret: &PaintCaret, time_ns: i64) -> bool {
    if !caret.should_blink {
        return true;
    }
    let elapsed_ns = time_ns.saturating_sub(caret.blink_cycle_start_time_ns).max(0);
    (elapsed_ns / CARET_BLINK_INTERVAL_NS) % 2 == 0
}

/// A scroll offset outside the scroll range moves the thumb past an end of its track, where it is cut short instead.
pub fn thumb_rect_kept_inside_track(mut thumb_rect: IntRect, track_rect: IntRect, vertical: bool) -> IntRect {
    if vertical {
        let top = thumb_rect.y.max(track_rect.y);
        let bottom = thumb_rect.bottom().min(track_rect.bottom());
        thumb_rect.y = top;
        thumb_rect.height = 0.max(bottom - top);
    } else {
        let left = thumb_rect.x.max(track_rect.x);
        let right = thumb_rect.right().min(track_rect.right());
        thumb_rect.x = left;
        thumb_rect.width = 0.max(right - left);
    }
    thumb_rect
}

/// A scrollbar at the geometry the frame presents it with, in the colors given.
fn presented_scrollbar_with_colors(
    gutter_rect: FloatRect,
    thumb_rect: FloatRect,
    expansion: f32,
    opacity: f32,
    thumb_color: Color,
    thumb_outline_color: Color,
    track_color: Color,
) -> PresentedScrollbar {
    let track_alpha_faded_in_by_expansion = (f32::from(track_color.alpha()) * expansion).round();
    PresentedScrollbar {
        gutter_rect,
        thumb_rect,
        thumb_color,
        thumb_outline_color,
        track_color: track_color.with_alpha(track_alpha_faded_in_by_expansion as u8),
        opacity,
    }
}

fn tile_raster_cache_key(tile_records: &[u8], tile_size: IntSize) -> u64 {
    let mut hasher = crate::fast_hash::fast_hasher();
    hasher.write(tile_records);
    hasher.write_i32(tile_size.width);
    hasher.write_i32(tile_size.height);
    hasher.finish()
}

fn rasterize_tile_if_pattern<'t, Painter: ReplayPainter + ?Sized>(
    paint_kind: PathPaintKind,
    paint_style: &crate::display_list::commands::DisplayListPaintStyle,
    payload: &'t [u8],
    context: &RecordsContext<'_, 't>,
    painter: &mut Painter,
) {
    if paint_kind != PathPaintKind::PaintStyle || paint_style.paint_style_type != DisplayListPaintStyleType::Pattern {
        return;
    }
    let tile_rect = paint_style.pattern_tile_rect;
    let content_scale = paint_style.pattern_content_scale;
    let tile_size = IntSize {
        width: (tile_rect.width * content_scale.width).ceil() as i32,
        height: (tile_rect.height * content_scale.height).ceil() as i32,
    };
    if tile_size.is_empty() {
        return;
    }
    let tile = TileRaster {
        key: 0,
        size: tile_size,
        cacheable: false,
    };
    if painter.begin_tile_raster(&tile) {
        let tile_frame = context.frame.without_scroll_offsets();
        replay_records(
            span_bytes(payload, paint_style.pattern_tile),
            &RecordsContext {
                tree: context.tree,
                frame: &tile_frame,
                to_root_matrix_of_inline_clip_space: &|_| None,
            },
            painter,
        );
        painter.end_tile_raster();
    }
}

fn play_record_resolving_frame_state<'t, Painter: ReplayPainter + ?Sized>(
    header: &DisplayListCommandHeader,
    payload: &'t [u8],
    context: &RecordsContext<'_, 't>,
    painter: &mut Painter,
) {
    let tree = context.tree;
    match header.command_type {
        DisplayListCommandType::FillRect => {
            let command = read_command::<FillRect>(payload);
            let color = if command.background_color_animation_effect == EffectNodeIndex::NONE {
                command.color
            } else {
                tree.sampled_background_color(command.background_color_animation_effect)
                    .unwrap_or(command.color)
            };
            painter.fill_rect(&ResolvedFillRect {
                rect: command.rect,
                color,
                compositing_and_blending_operator: command.compositing_and_blending_operator,
            });
        }
        DisplayListCommandType::FillRectWithRoundedCorners => {
            let command = read_command::<FillRectWithRoundedCorners>(payload);
            let color = if command.background_color_animation_effect == EffectNodeIndex::NONE {
                command.color
            } else {
                tree.sampled_background_color(command.background_color_animation_effect)
                    .unwrap_or(command.color)
            };
            painter.fill_rect_with_rounded_corners(&ResolvedFillRectWithRoundedCorners {
                rect: command.rect,
                color,
                corner_radii: command.corner_radii,
            });
        }
        DisplayListCommandType::PaintCaret => {
            let command = read_command::<PaintCaret>(payload);
            if caret_is_visible_at_time(&command, context.frame.time_ns) {
                painter.play_command(header.command_type, payload, payload);
            }
        }
        DisplayListCommandType::PaintScrollBar => {
            let mut command = read_command::<PaintScrollBar>(payload);
            let shown_by_frame = scrollbar_shown_by_frame(context.frame.scrollbars, &command);
            let scrollbar = match shown_by_frame {
                Some(scrollbar) => presented_scrollbar_with_colors(
                    scrollbar.gutter_rect,
                    scrollbar.thumb_rect,
                    scrollbar.expansion,
                    scrollbar.opacity,
                    command.thumb_color,
                    command.thumb_outline_color,
                    command.track_color,
                ),
                None => {
                    let scroll_offset_in_device_pixels =
                        device_offset_for_index(context.frame.scroll_offsets, command.scroll_node_index);
                    if command.vertical {
                        let thumb_shift = (-f64::from(scroll_offset_in_device_pixels.y) * command.scroll_size) as i32;
                        command.thumb_rect.y = command.thumb_rect.y.saturating_add(thumb_shift);
                    } else {
                        let thumb_shift = (-f64::from(scroll_offset_in_device_pixels.x) * command.scroll_size) as i32;
                        command.thumb_rect.x = command.thumb_rect.x.saturating_add(thumb_shift);
                    }
                    PresentedScrollbar {
                        gutter_rect: command.gutter_rect.to_float(),
                        thumb_rect: thumb_rect_kept_inside_track(
                            command.thumb_rect,
                            command.track_rect,
                            command.vertical,
                        )
                        .to_float(),
                        thumb_color: command.thumb_color,
                        thumb_outline_color: command.thumb_outline_color,
                        track_color: command.track_color,
                        opacity: 1.0,
                    }
                }
            };
            painter.paint_scrollbar(&scrollbar);
        }
        DisplayListCommandType::DrawCompositedContext => {
            let command = read_command::<DrawCompositedContext>(payload);
            let mut dst_rect = command.dst_rect;
            // Where it does not snap, the fractional origin keeps the raster phase of surrounding content.
            let origin_snaps_to_device_pixels = command.snap_origin_if_untransformed
                && tree.translations_snap_to_device_pixels(command.spatial_node_index);
            if origin_snaps_to_device_pixels {
                dst_rect.x = dst_rect.x.round();
                dst_rect.y = dst_rect.y.round();
            }
            let canvas_matrix = painter.canvas_matrix();
            let visible_device_rect = painter.device_clip_bounds();
            let Some(resolved_context) = context.frame.composited_contexts.resolve(
                command.child_context_id.0,
                dst_rect,
                &canvas_matrix,
                visible_device_rect,
            ) else {
                return;
            };
            let (content_rect, held_rect) = (resolved_context.content_rect, resolved_context.held_content_rect);
            if resolved_context.surface.is_null() || content_rect.is_empty() || held_rect.is_empty() {
                return;
            }
            // Only the part of the destination that shows the held part of the frame is drawn.
            let horizontal_scale = dst_rect.width / content_rect.width;
            let vertical_scale = dst_rect.height / content_rect.height;
            let held_dst_rect = FloatRect::new(
                dst_rect.x + (held_rect.x - content_rect.x) * horizontal_scale,
                dst_rect.y + (held_rect.y - content_rect.y) * vertical_scale,
                held_rect.width * horizontal_scale,
                held_rect.height * vertical_scale,
            );
            painter.draw_composited_context(&CompositedContextDraw {
                surface: resolved_context.surface,
                src_rect: held_rect,
                dst_rect: held_dst_rect,
                scaling_mode: command.scaling_mode,
            });
        }
        DisplayListCommandType::DrawIsolatedGroup => {
            let command = read_command::<DrawIsolatedGroup>(payload);
            let filter = span_bytes(payload, command.filter);
            painter.begin_isolated_group(&IsolatedGroup {
                clip_rect: command.clip_rect,
                opacity: command.opacity,
                compositing_and_blending_operator: command.compositing_and_blending_operator,
                filter_bytes: filter.as_ptr(),
                filter_bytes_size: filter.len(),
            });
            let nested_records_context = RecordsContext {
                tree: context.tree,
                frame: context.frame,
                to_root_matrix_of_inline_clip_space: &|_| None,
            };
            replay_records(span_bytes(payload, command.content), &nested_records_context, painter);
            if command.mask.size != 0 {
                painter.begin_isolated_group_mask(command.mask_kind);
                replay_records(span_bytes(payload, command.mask), &nested_records_context, painter);
                painter.end_isolated_group_mask();
            }
            painter.end_isolated_group();
        }
        DisplayListCommandType::DrawRepeatedTile => {
            let command = read_command::<DrawRepeatedTile>(payload);
            if command.tile_size.is_empty() || command.dst_rect.is_empty() || command.tile_step.is_empty() {
                return;
            }
            let tile_records = span_bytes(payload, command.tile);
            let tile = TileRaster {
                key: tile_raster_cache_key(tile_records, command.tile_size),
                size: command.tile_size,
                cacheable: true,
            };
            if painter.begin_tile_raster(&tile) {
                let tile_frame = context.frame.without_scroll_offsets();
                replay_records(
                    tile_records,
                    &RecordsContext {
                        tree: context.tree,
                        frame: &tile_frame,
                        to_root_matrix_of_inline_clip_space: &|_| None,
                    },
                    painter,
                );
                painter.end_tile_raster();
            }
            painter.play_command(header.command_type, payload, payload);
        }
        DisplayListCommandType::FillPath => {
            let command = read_command::<FillPath>(payload);
            rasterize_tile_if_pattern(command.paint_kind, &command.paint_style, payload, context, painter);
            painter.play_command(header.command_type, payload, payload);
        }
        DisplayListCommandType::StrokePath => {
            let command = read_command::<StrokePath>(payload);
            rasterize_tile_if_pattern(command.paint_kind, &command.paint_style, payload, context, painter);
            painter.play_command(header.command_type, payload, payload);
        }
        DisplayListCommandType::PaintVectorImage => {
            let command = read_command::<PaintVectorImage>(payload);
            let placement = VectorImagePlacement {
                vector_image_id: command.vector_image_id.0,
                rect: command.rect,
                recorded_size: command.recorded_size,
            };
            if painter.begin_vector_image(&placement, command.requires_direct_replay) == VectorImageReplay::Skipped {
                return;
            }
            let vector_image_frame = context.frame.without_scroll_offsets_or_scrollbars();
            replay_records(
                span_bytes(payload, command.records),
                &RecordsContext {
                    tree: context.tree,
                    frame: &vector_image_frame,
                    to_root_matrix_of_inline_clip_space: &|_| None,
                },
                painter,
            );
            painter.end_vector_image();
        }
        _ => painter.play_command(header.command_type, payload, payload),
    }
}

/// Plays one stream of records: the records of a run, or the records nested in a command. Compositor
/// metadata is skipped, a command that replays nested records is culled when its bounds are empty or clipped away, a
/// command that its inline clips in spaces of their own have moved off is culled, and each command plays inside its
/// inline clips and transform. A clip whose space the context does not know applies in the space of its command.
pub fn replay_records<'t, Painter: ReplayPainter + ?Sized>(
    records: &'t [u8],
    context: &RecordsContext<'_, 't>,
    painter: &mut Painter,
) {
    let to_root_matrix_of_inline_clip_space = context.to_root_matrix_of_inline_clip_space;
    for_each_command(records, |header, _, payload| {
        // NB: A mask composites the content it declares where the mask is, whatever the declaration is recorded under.
        if header.command_type.is_compositor_metadata()
            || header.command_type == DisplayListCommandType::DeclareMaskContent
        {
            return;
        }
        if holds_nested_records(header.command_type, payload) {
            let bounding_rect = bounding_rect_computed_from_payload(header, payload).unwrap_or_default();
            if bounding_rect.is_empty() || painter.would_be_fully_clipped_by_painter(bounding_rect) {
                return;
            }
        }
        let mut command_space_matrix_to_restore = None;
        if header.inline_clip_count != 0 && has_inline_clip_in_its_own_space(header, payload) {
            let command_to_root = painter.canvas_matrix();
            // NB: Recorded bounds leave out clips in spaces of their own, which scroll apart from the command.
            if let Some(OwnSpaceClipExtent::Within(part_left_by_own_space_clips)) = own_space_clip_extent(
                header,
                payload,
                bounding_rect_computed_from_payload(header, payload)
                    .unwrap_or_default()
                    .to_float(),
                command_to_root,
                to_root_matrix_of_inline_clip_space,
            ) && (part_left_by_own_space_clips.is_empty()
                || painter.would_be_fully_clipped_by_painter(enclosing_int_rect(part_left_by_own_space_clips)))
            {
                return;
            }
        }
        for clip in inline_clips_of(header, payload) {
            let own_space_matrix = clip
                .has_own_space
                .then(|| to_root_matrix_of_inline_clip_space(clip.space))
                .flatten();
            if let Some(matrix) = &own_space_matrix {
                command_space_matrix_to_restore.get_or_insert_with(|| painter.canvas_matrix());
                painter.set_matrix(matrix);
            }
            match clip.kind {
                InlineClipKind::Path => {
                    painter.push_clip_path_bytes(
                        span_bytes(payload, clip.path_data),
                        clip.path_winding_rule,
                        clip.mode,
                    );
                }
                InlineClipKind::Rect | InlineClipKind::RoundedRect => painter.push_clip(&ReplayClip {
                    rect: clip.clip_rect_or_path_device_bounds,
                    corner_radii: clip.corner_radii,
                    mode: clip.mode,
                }),
            }
            if own_space_matrix.is_some()
                && let Some(matrix) = &command_space_matrix_to_restore
            {
                painter.set_matrix(matrix);
            }
        }
        let inline_transform = inline_transform_of(header, payload);
        if let Some(transform) = &inline_transform {
            painter.push_transform(transform);
        }
        play_record_resolving_frame_state(header, payload, context, painter);
        if inline_transform.is_some() {
            painter.pop();
        }
        for _ in 0..header.inline_clip_count {
            painter.pop();
        }
        // Popping a clip pushed in another space restores the matrix it was pushed with.
        if let Some(matrix) = &command_space_matrix_to_restore {
            painter.set_matrix(matrix);
        }
    });
}

// Recorded bounds are whole device pixels around geometry that antialiasing and glyph rounding
// can push past them by up to this much, and a layer's bounds clip what is drawn into it.
const LAYER_CONTENT_MARGIN_IN_DEVICE_PIXELS: f32 = 2.0;

// Spaces are entered with one absolute set_matrix, so only clips and effects go on the canvas stack.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CanvasStackEntry {
    Clip(ClipNodeIndex),
    Effect(EffectNodeIndex),
}

// Steady-state replays reuse the previous frame's capacity. A replay takes the storage out of the slot, so that
// re-entrant nested replays cannot clobber the outer driver's buffers.
#[derive(Default)]
struct ReplayScratchStorage {
    world: WorldTransforms,
    culling: TreeCullingScratch,
    canvas_stack: Vec<CanvasStackEntry>,
    target_canvas_stack: Vec<CanvasStackEntry>,
    effect_geometry_by_effect_index: Vec<EffectGeometry>,
}

thread_local! {
    static WARM_REPLAY_SCRATCH_STORAGE: RefCell<Option<ReplayScratchStorage>> = const { RefCell::new(None) };
}

fn take_replay_scratch_storage() -> ReplayScratchStorage {
    WARM_REPLAY_SCRATCH_STORAGE.with(|slot| slot.borrow_mut().take().unwrap_or_default())
}

fn return_replay_scratch_storage(mut storage: ReplayScratchStorage) {
    storage.world.clear();
    storage.culling.clear();
    storage.canvas_stack.clear();
    storage.target_canvas_stack.clear();
    storage.effect_geometry_by_effect_index.clear();
    WARM_REPLAY_SCRATCH_STORAGE.with(|slot| {
        let mut slot = slot.borrow_mut();
        let slot_has_larger_table = slot
            .as_ref()
            .is_some_and(|cached| cached.world.to_root.capacity() >= storage.world.to_root.capacity());
        if !slot_has_larger_table {
            *slot = Some(storage);
        }
    });
}

/// Which records a replay plays, and what it leaves to the compositor.
#[derive(Clone, Copy, Default)]
pub struct ReplaySelection<'a> {
    /// The records to replay, in paint order, or none for the whole list.
    pub record_spans: Option<&'a [ListSpan]>,
    pub skip_root_isolation_layer: bool,
    pub allows_lcd_text: bool,
    /// Per clip node, whether the compositor applies it as it composites what the replay draws.
    pub clips_left_to_composite: Option<&'a [bool]>,
    /// The effect that the compositor composites the replayed records inside. The replay leaves it out, with the
    /// effects around it and the clips it is entered within, which the compositor applies as it composites.
    pub composited_effect: Option<EffectNodeIndex>,
    /// Where the frame is, in the coordinates of the list before the target's own transform, where the target is not
    /// the frame: a blur of a backdrop mirrors at the edge of the frame there, as it does at the edge of the frame's
    /// surface in a raster of the frame.
    pub frame_edge_backdrop_blurs_mirror_at: Option<FloatRect>,
    /// The node whose subtree the replay draws in its own coordinates, for rasters of a space whose anchor the
    /// compositor animates.
    pub raster_root: Option<RasterRoot>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum SwitchResult {
    Switched,
    CulledByEffect,
}

struct ReplayDriver<'a, Painter: ReplayPainter + ?Sized> {
    tree: &'a VisualContextTree,
    display_list: &'a DisplayListStorage,
    frame: &'a ReplayFrame<'a>,
    effect_clips: &'a EffectClipPlan,
    root_group_color: Color,
    selection: ReplaySelection<'a>,
    painter: &'a mut Painter,
    world: WorldTransforms,
    culling: TreeCullingScratch,
    spatial_dependency_order: Vec<u32>,
    tree_sorts_planes_by_depth: bool,
    replay_base_matrix: FloatMatrix4x4,
    // None once popping a clip or a layer has reset the canvas matrix to its save point. Played records never change
    // the matrix.
    draw_space_of_canvas_matrix: Option<SpatialNodeIndex>,
    canvas_stack: Vec<CanvasStackEntry>,
    target_canvas_stack: Vec<CanvasStackEntry>,
    clip_and_effect_of_canvas_stack: Option<(ClipNodeIndex, EffectNodeIndex)>,
    effect_geometry_by_effect_index: Vec<EffectGeometry>,
    effect_geometry_is_computed: bool,
    effects_left_to_composite_around_composited_effect: Vec<bool>,
    clips_left_to_composite_around_composited_effect: Vec<bool>,
    content_reads_layer_by_effect_index: Option<Vec<bool>>,
    first_backdrop_drawn_by_masks_by_reading_mask: std::cell::OnceCell<Vec<EffectNodeIndex>>,
}

fn effects_and_clips_left_to_composite_around(
    tree: &VisualContextTree,
    effect_clips: &EffectClipPlan,
    composited_effect: Option<EffectNodeIndex>,
) -> (Vec<bool>, Vec<bool>) {
    let Some(composited_effect) = composited_effect else {
        return (Vec::new(), Vec::new());
    };
    let mut effects_left_to_composite = vec![false; tree.effect_nodes.len()];
    let mut effect = composited_effect;
    while !effect.is_none() {
        effects_left_to_composite[effect.0 as usize] = true;
        effect = tree.effect_nodes[effect.0 as usize].parent;
    }
    let mut clips_left_to_composite = vec![false; tree.clip_nodes.len()];
    let mut clip = effect_clips.output_clip(composited_effect);
    while !clip.is_none() {
        clips_left_to_composite[clip.0 as usize] = true;
        clip = tree.clip_nodes[clip.0 as usize].parent;
    }
    (effects_left_to_composite, clips_left_to_composite)
}

fn replay_clip_of(clip: &crate::visual_context::ClipData) -> ReplayClip {
    ReplayClip {
        rect: clip.rect,
        corner_radii: clip.corner_radii,
        mode: clip.mode,
    }
}

// An effect keeps its filters for as long as the tree keeps its structure.
fn layer_filter_cache_key(tree: &VisualContextTree, effect: EffectNodeIndex) -> u64 {
    let mut hasher = crate::fast_hash::fast_hasher();
    hasher.write_u64(tree.structural_epoch);
    hasher.write_u32(effect.0);
    hasher.finish()
}

fn replay_layer_of(
    effects: &crate::visual_context::EffectsData,
    filter_cache_key: u64,
    content_bounds: Option<FloatRect>,
) -> ReplayLayer {
    let (filter_bytes, filter_bytes_size) = match &effects.filter {
        Some(bytes) => (bytes.as_ptr(), bytes.len()),
        None => (std::ptr::null(), 0),
    };
    let (backdrop_filter_bytes, backdrop_filter_bytes_size, backdrop_region, backdrop_corner_radii) =
        match &effects.backdrop_filter {
            Some(backdrop) => (
                backdrop.filter.as_ptr(),
                backdrop.filter.len(),
                backdrop.region,
                backdrop.corner_radii,
            ),
            None => (std::ptr::null(), 0, IntRect::default(), CornerRadii::default()),
        };
    ReplayLayer {
        opacity: effects.opacity,
        blend_mode: effects.blend_mode,
        can_draw_directly_for_lcd_text: false,
        filter_bytes,
        filter_bytes_size,
        backdrop_filter_bytes,
        backdrop_filter_bytes_size,
        backdrop_region,
        backdrop_corner_radii,
        backdrop_base_color: Color::TRANSPARENT,
        backdrop_blur_edge: OptionalFloatRect::none(),
        filter_cache_key,
        content_bounds: OptionalFloatRect::from(content_bounds),
    }
}

fn plain_layer_of_bounds(content_bounds: Option<FloatRect>) -> ReplayLayer {
    ReplayLayer {
        opacity: 1.0,
        blend_mode: libgfx_rust::CompositingAndBlendingOperator::Normal,
        can_draw_directly_for_lcd_text: false,
        filter_bytes: std::ptr::null(),
        filter_bytes_size: 0,
        backdrop_filter_bytes: std::ptr::null(),
        backdrop_filter_bytes_size: 0,
        backdrop_region: IntRect::default(),
        backdrop_corner_radii: CornerRadii::default(),
        backdrop_base_color: Color::TRANSPARENT,
        backdrop_blur_edge: OptionalFloatRect::none(),
        filter_cache_key: 0,
        content_bounds: OptionalFloatRect::from(content_bounds),
    }
}

pub(crate) fn backdrop_is_drawn_by_masks(tree: &VisualContextTree, effect: EffectNodeIndex) -> bool {
    let node = &tree.effect_nodes[effect.0 as usize];
    matches!(&node.data, EffectNodeData::Effects(effects) if effects.backdrop_filter.is_some())
        && !node.parent.is_none()
        && matches!(&tree.effect_nodes[node.parent.0 as usize].data, EffectNodeData::Mask(mask) if mask.masks_backdrop)
}

// Per mask, the first effect whose backdrop the mask draws as the outermost of the masks of its box, if any. An effect
// that no run of the list is inside has no output clip, so the mask draws none of its backdrop.
fn first_backdrop_drawn_by_masks_by_reading_mask(
    tree: &VisualContextTree,
    effect_clips: &EffectClipPlan,
) -> Vec<EffectNodeIndex> {
    let mut first_backdrop_by_reading_mask = vec![EffectNodeIndex::NONE; tree.effect_nodes.len()];
    let node_reading_backdrop_by_effect_index = tree.nodes_reading_the_backdrops_of_effects();
    for index in 0..tree.effect_nodes.len() {
        let effect = EffectNodeIndex(index as u32);
        if !backdrop_is_drawn_by_masks(tree, effect) || effect_clips.output_clip_if_participating(effect).is_none() {
            continue;
        }
        let first_backdrop =
            &mut first_backdrop_by_reading_mask[node_reading_backdrop_by_effect_index[index].0 as usize];
        if first_backdrop.is_none() {
            *first_backdrop = effect;
        }
    }
    first_backdrop_by_reading_mask
}

fn replay_mask_of(mask: &crate::visual_context::MaskData) -> ReplayMask {
    ReplayMask {
        rect: mask.rect,
        kind: mask.kind,
    }
}

fn enclosing_int_rect_if_representable(rect: FloatRect) -> Option<IntRect> {
    let (left, top, right, bottom) = (
        rect.x.floor(),
        rect.y.floor(),
        rect.right().ceil(),
        rect.bottom().ceil(),
    );
    let fits_in_int_rect = |value: f32| value.is_finite() && value >= i32::MIN as f32 && value < i32::MAX as f32;
    [left, top, right, bottom, right - left, bottom - top]
        .into_iter()
        .all(fits_in_int_rect)
        .then(|| enclosing_int_rect(rect))
}

impl<Painter: ReplayPainter + ?Sized> ReplayDriver<'_, Painter> {
    fn clip_is_left_to_composite(&self, clip: ClipNodeIndex) -> bool {
        self.selection
            .clips_left_to_composite
            .is_some_and(|clips| clips.get(clip.0 as usize).copied().unwrap_or(false))
            || self
                .clips_left_to_composite_around_composited_effect
                .get(clip.0 as usize)
                .copied()
                .unwrap_or(false)
    }

    // Under dynamic MSAA, Skia rasters some content, such as antialiased rounded rects, differently into a layer than
    // into the surface below it, so the tiles get the effect's content inside a layer too.
    fn push_plain_layer_standing_in_for(&mut self, effect: EffectNodeIndex) {
        let spatial = self.tree.effect_nodes[effect.0 as usize].spatial;
        self.ensure_canvas_matrix_in_draw_space_of(spatial);
        let content_bounds = self.layer_content_bounds_in_effect_space(effect, spatial);
        self.painter.push_layer(&plain_layer_of_bounds(content_bounds));
    }

    // https://drafts.fxtf.org/filter-effects-2/#BackdropRoot
    // An element with a backdrop filter is a backdrop root, so what reads the backdrop inside it reads only the
    // element's own content, not the filtered backdrop its layer starts with.
    fn effect_isolates_content_from_filtered_backdrop(&mut self, effect: EffectNodeIndex) -> bool {
        let tree = self.tree;
        if !matches!(&tree.effect_nodes[effect.0 as usize].data, EffectNodeData::Effects(effects)
            if effects.backdrop_filter.is_some())
            || backdrop_is_drawn_by_masks(tree, effect)
        {
            return false;
        }
        let display_list = self.display_list;
        self.content_reads_layer_by_effect_index.get_or_insert_with(|| {
            let mut content_reads_layer_by_effect_index = vec![false; tree.effect_nodes.len()];
            let nearest_layer_by_effect_index =
                nearest_effects_at_or_above_where(tree, |effect, _| tree.effect_pushes_layer_at_replay(effect));
            let mut mark_layer_at_or_above_as_read = |effect: EffectNodeIndex| {
                let layer = nearest_layer_by_effect_index[effect.0 as usize];
                if !layer.is_none() {
                    content_reads_layer_by_effect_index[layer.0 as usize] = true;
                }
            };
            let mut node_reading_backdrop_by_effect_index = None;
            for (index, node) in tree.effect_nodes.iter().enumerate() {
                let EffectNodeData::Effects(effects) = &node.data else {
                    continue;
                };
                let reading_node = if effects.backdrop_filter.is_some() {
                    node_reading_backdrop_by_effect_index
                        .get_or_insert_with(|| tree.nodes_reading_the_backdrops_of_effects())[index]
                } else if effects.blend_mode != libgfx_rust::CompositingAndBlendingOperator::Normal {
                    EffectNodeIndex(index as u32)
                } else {
                    continue;
                };
                let effect_read_from = tree.effect_nodes[reading_node.0 as usize].parent;
                if !effect_read_from.is_none() {
                    mark_layer_at_or_above_as_read(effect_read_from);
                }
            }
            for record in &display_list.summary().destination_blending_records {
                let effect_read_from = display_list.run(record.run_index as usize).context.effect;
                if !effect_read_from.is_none() {
                    mark_layer_at_or_above_as_read(effect_read_from);
                }
            }
            content_reads_layer_by_effect_index
        })[effect.0 as usize]
    }

    fn effect_is_left_to_composite(&self, effect: EffectNodeIndex) -> bool {
        self.effects_left_to_composite_around_composited_effect
            .get(effect.0 as usize)
            .copied()
            .unwrap_or(false)
    }

    fn effect_layer_is_pushed(&self, effect: EffectNodeIndex) -> bool {
        self.tree.effect_pushes_layer_at_replay(effect)
            && !(self.selection.skip_root_isolation_layer && self.tree.root_isolation_effect == Some(effect))
    }

    fn ensure_canvas_matrix_in_draw_space_of(&mut self, spatial: SpatialNodeIndex) {
        let draw_space = self.world.draw_space[spatial.0 as usize];
        if self.draw_space_of_canvas_matrix == Some(draw_space) {
            return;
        }
        self.painter.set_matrix(&self.world.to_root[draw_space.0 as usize]);
        self.draw_space_of_canvas_matrix = Some(draw_space);
    }

    fn effect_geometry(&mut self, effect: EffectNodeIndex) -> EffectGeometry {
        if !self.effect_geometry_is_computed {
            compute_effect_geometry(
                self.tree,
                self.display_list.runs(),
                &self.world,
                &mut self.effect_geometry_by_effect_index,
            );
            self.effect_geometry_is_computed = true;
        }
        self.effect_geometry_by_effect_index[effect.0 as usize]
    }

    fn layer_content_bounds_in_effect_space(
        &mut self,
        effect: EffectNodeIndex,
        effect_space: SpatialNodeIndex,
    ) -> Option<FloatRect> {
        let Extent::Within(content) = self.effect_geometry(effect).content else {
            return None;
        };
        let effect_space_to_root = self.world.draw_matrix(effect_space);
        if !effect_space_to_root.is_2d_affine() {
            let content_in_root = self.world.bounds_in_root_of_rect_drawn_in(effect_space, content)?;
            let margin = 2.0 * LAYER_CONTENT_MARGIN_IN_DEVICE_PIXELS;
            return self
                .world
                .bounds_in_space_of_root_rect(effect_space, content_in_root.inflated(margin, margin));
        }
        let root_to_layer = effect_space_to_root.extract_2d_affine().inverse()?;
        let margin_x = LAYER_CONTENT_MARGIN_IN_DEVICE_PIXELS * (root_to_layer.a().abs() + root_to_layer.c().abs());
        let margin_y = LAYER_CONTENT_MARGIN_IN_DEVICE_PIXELS * (root_to_layer.b().abs() + root_to_layer.d().abs());
        let content_bounds_with_margin = FloatRect::new(
            content.x - margin_x,
            content.y - margin_y,
            content.width + 2.0 * margin_x,
            content.height + 2.0 * margin_y,
        );
        let bounds_are_finite = [
            content_bounds_with_margin.x,
            content_bounds_with_margin.y,
            content_bounds_with_margin.right(),
            content_bounds_with_margin.bottom(),
        ]
        .into_iter()
        .all(f32::is_finite);
        bounds_are_finite.then_some(content_bounds_with_margin)
    }

    // https://drafts.fxtf.org/filter-effects-2/#backdrop-filter-operation
    // The masks of a box with a backdrop filter apply to the filtered backdrop too. Skia reads a layer's
    // backdrop from the layer's parent, so the outermost of the box's mask layers starts with the
    // filtered backdrop, and the box's effects layer below the masks draws none. Opacity and blending
    // come after masking, so the outermost mask layer applies them too.
    fn backdrop_layer_of_outermost_mask(
        &self,
        mask_index_in_target_canvas_stack: usize,
        mask: &crate::visual_context::MaskData,
    ) -> Option<ReplayLayer> {
        if !mask.masks_backdrop {
            return None;
        }
        let tree = self.tree;
        let CanvasStackEntry::Effect(mask_effect) = self.target_canvas_stack[mask_index_in_target_canvas_stack] else {
            unreachable!("a mask is an effect");
        };
        let parent = tree.effect_nodes[mask_effect.0 as usize].parent;
        let is_inside_outer_mask_of_backdrop = !parent.is_none()
            && matches!(&tree.effect_nodes[parent.0 as usize].data, EffectNodeData::Mask(data) if data.masks_backdrop);
        if is_inside_outer_mask_of_backdrop {
            return None;
        }
        let backdrop_effect_in_target_canvas_stack = self.target_canvas_stack[mask_index_in_target_canvas_stack + 1..]
            .iter()
            .find_map(|entry| match *entry {
                CanvasStackEntry::Effect(effect) if backdrop_is_drawn_by_masks(tree, effect) => Some(effect),
                _ => None,
            });
        // Compositing a mask's content enters the mask without the effect whose backdrop it draws.
        let backdrop_effect = backdrop_effect_in_target_canvas_stack.or_else(|| {
            let backdrop_effect = self
                .first_backdrop_drawn_by_masks_by_reading_mask
                .get_or_init(|| first_backdrop_drawn_by_masks_by_reading_mask(tree, self.effect_clips))
                [mask_effect.0 as usize];
            (!backdrop_effect.is_none()).then_some(backdrop_effect)
        })?;
        let EffectNodeData::Effects(effects) = &tree.effect_nodes[backdrop_effect.0 as usize].data else {
            unreachable!("a backdrop that masks draw is of an effects node");
        };
        let mut layer = replay_layer_of(effects, layer_filter_cache_key(tree, backdrop_effect), None);
        layer.backdrop_base_color = self.backdrop_base_color(mask_effect);
        layer.backdrop_blur_edge = self.backdrop_blur_edge(backdrop_effect);
        Some(layer)
    }

    // https://drafts.csswg.org/filter-effects-2/#backdrop-filter-operation
    // If the filter functions list includes a blur() filter, the filter will be applied with edgeMode="mirror", with
    // the edge defined by the clipped, transformed border box of the element.
    // The clips come from the effect's clip chain and not from the canvas, whose clip also holds the rect a partial
    // repaint is limited to, and whose bounds lose the shape of a clip that turns along with the box.
    fn backdrop_blur_edge(&self, effect: EffectNodeIndex) -> OptionalFloatRect {
        let tree = self.tree;
        let node = &tree.effect_nodes[effect.0 as usize];
        let EffectNodeData::Effects(effects) = &node.data else {
            return OptionalFloatRect::none();
        };
        let Some(backdrop) = effects
            .backdrop_filter
            .as_ref()
            .filter(|backdrop| Filter::serialized_has_mirroring_blur(&backdrop.filter))
        else {
            return OptionalFloatRect::none();
        };
        let mut blur_edge = backdrop.region.to_float();
        if let Some(frame_rect) = self.selection.frame_edge_backdrop_blurs_mirror_at {
            let effect_to_target = self.world.draw_matrix(node.spatial);
            if effect_to_target.is_2d_affine()
                && self.replay_base_matrix.is_2d_affine()
                && let Some(target_to_effect) = effect_to_target.extract_2d_affine().inverse()
            {
                let frame_in_target = self.replay_base_matrix.extract_2d_affine().map_rect(frame_rect);
                blur_edge = blur_edge.intersected(target_to_effect.map_rect(frame_in_target));
            }
        }
        let mut clip = self.effect_clips.output_clip(effect);
        while !clip.is_none() {
            let clip_node = &tree.clip_nodes[clip.0 as usize];
            let clip_rect = match &clip_node.data {
                ClipNodeData::Rect(data) if data.mode == ClipMode::Intersect => Some(data.rect),
                ClipNodeData::Path(path) => Some(path.bounding_rect.to_float()),
                _ => None,
            };
            if let Some(clip_rect) = clip_rect.and_then(|rect| {
                self.world
                    .map_rect_between_spaces(rect, clip_node.spatial, node.spatial)
            }) {
                blur_edge = blur_edge.intersected(clip_rect);
            }
            clip = clip_node.parent;
        }
        OptionalFloatRect::from(Some(blur_edge))
    }

    // https://drafts.fxtf.org/filter-effects-2/#BackdropRoot
    // The root element group is a backdrop root, yet engines read the canvas surface below it into the backdrop, see
    // https://github.com/w3c/fxtf-drafts/issues/557. Skia reads a backdrop from the parent layer only, so a layer
    // whose nearest enclosing effect is the root isolation effect reads its backdrop over the color the canvas holds
    // outside every layer. Any other effect is a backdrop root, even one that pushes no layer, such as will-change.
    fn backdrop_base_color(&self, layer_effect: EffectNodeIndex) -> Color {
        let tree = self.tree;
        let mut enclosing_effect = tree.effect_nodes[layer_effect.0 as usize].parent;
        while !enclosing_effect.is_none()
            && matches!(
                tree.effect_nodes[enclosing_effect.0 as usize].data,
                EffectNodeData::BackgroundColorAnimation
            )
        {
            enclosing_effect = tree.effect_nodes[enclosing_effect.0 as usize].parent;
        }
        if tree.root_isolation_effect == Some(enclosing_effect) {
            self.root_group_color
        } else {
            Color::TRANSPARENT
        }
    }

    // Each effect goes right inside its output clip.
    fn build_target_canvas_stack(&mut self, context: ContextRef) {
        let tree = self.tree;
        self.target_canvas_stack.clear();
        let mut clip = context.clip;
        let mut effect = context.effect;
        while !effect.is_none() {
            let node = &tree.effect_nodes[effect.0 as usize];
            while clip != self.effect_clips.output_clip(effect) {
                debug_assert!(
                    !clip.is_none(),
                    "an effect's output clip lies on the context's clip chain"
                );
                self.target_canvas_stack.push(CanvasStackEntry::Clip(clip));
                clip = tree.clip_nodes[clip.0 as usize].parent;
            }
            self.target_canvas_stack.push(CanvasStackEntry::Effect(effect));
            effect = node.parent;
        }
        while !clip.is_none() {
            self.target_canvas_stack.push(CanvasStackEntry::Clip(clip));
            clip = tree.clip_nodes[clip.0 as usize].parent;
        }
        self.target_canvas_stack.reverse();
    }

    // Contexts in one effect share every layer, and their clip chains meet at or below the effect's output clip.
    fn build_target_clip_suffix(&mut self, mut entered_clip: ClipNodeIndex, mut target_clip: ClipNodeIndex) -> usize {
        self.target_canvas_stack.clear();
        let mut entered_clip_depth = self.culling.clip_depth(entered_clip);
        let mut target_clip_depth = self.culling.clip_depth(target_clip);
        let mut clips_to_pop = 0;
        while entered_clip != target_clip {
            if entered_clip_depth >= target_clip_depth {
                entered_clip = self.tree.clip_nodes[entered_clip.0 as usize].parent;
                entered_clip_depth -= 1;
                clips_to_pop += 1;
            } else {
                self.target_canvas_stack.push(CanvasStackEntry::Clip(target_clip));
                target_clip = self.tree.clip_nodes[target_clip.0 as usize].parent;
                target_clip_depth -= 1;
            }
        }
        self.target_canvas_stack.reverse();
        let common_prefix_length = self.canvas_stack.len() - clips_to_pop;
        debug_assert!(
            self.canvas_stack[common_prefix_length..]
                .iter()
                .all(|entry| matches!(entry, CanvasStackEntry::Clip(_)))
        );
        common_prefix_length
    }

    fn restore_canvas_stack_to_length(&mut self, length: usize) {
        let tree = self.tree;
        self.clip_and_effect_of_canvas_stack = None;
        while self.canvas_stack.len() > length {
            match self.canvas_stack.pop().expect("the canvas stack is not empty") {
                CanvasStackEntry::Clip(clip) if self.clip_is_left_to_composite(clip) => {}
                CanvasStackEntry::Clip(_) => {
                    self.painter.pop();
                    self.draw_space_of_canvas_matrix = None;
                }
                CanvasStackEntry::Effect(effect) if self.effect_is_left_to_composite(effect) => {
                    if self.effect_layer_is_pushed(effect) {
                        self.painter.pop();
                        self.draw_space_of_canvas_matrix = None;
                    }
                }
                CanvasStackEntry::Effect(effect) => {
                    let node = &tree.effect_nodes[effect.0 as usize];
                    match &node.data {
                        EffectNodeData::Effects(_) => {
                            if self.effect_layer_is_pushed(effect) {
                                if self.effect_isolates_content_from_filtered_backdrop(effect) {
                                    self.painter.pop();
                                }
                                self.painter.pop();
                                self.draw_space_of_canvas_matrix = None;
                            }
                        }
                        EffectNodeData::Mask(mask) => {
                            self.ensure_canvas_matrix_in_draw_space_of(node.spatial);
                            self.painter.begin_mask_content(&replay_mask_of(mask));
                            if let Some(content) = self.display_list.records_of_mask_content(effect) {
                                let records_context = RecordsContext {
                                    tree,
                                    frame: self.frame,
                                    to_root_matrix_of_inline_clip_space: &|_| None,
                                };
                                replay_records(content, &records_context, &mut *self.painter);
                            }
                            self.painter.pop_mask();
                            self.draw_space_of_canvas_matrix = None;
                        }
                        EffectNodeData::BackgroundColorAnimation => {}
                        EffectNodeData::Dead => unreachable!("a run never records under a tombstoned effect"),
                    }
                }
            }
        }
    }

    // OPTIMIZATION: A layer whose whole output the clips entered so far cull is not pushed, which spares off-screen
    //               elements a saveLayer/restore. Those clips are all outside the layer, so the test is conservative.
    fn switch_to_context(&mut self, context: ContextRef, may_cull_layers: bool) -> SwitchResult {
        if self.clip_and_effect_of_canvas_stack == Some((context.clip, context.effect)) {
            return SwitchResult::Switched;
        }

        let tree = self.tree;
        let (common_prefix_length, first_target_entry_to_enter) = if let Some((clip, effect)) =
            self.clip_and_effect_of_canvas_stack
            && effect == context.effect
        {
            (self.build_target_clip_suffix(clip, context.clip), 0)
        } else {
            self.build_target_canvas_stack(context);
            let common_prefix_length = self
                .canvas_stack
                .iter()
                .zip(&self.target_canvas_stack)
                .take_while(|(entered, target)| entered == target)
                .count();
            (common_prefix_length, common_prefix_length)
        };

        self.restore_canvas_stack_to_length(common_prefix_length);

        for target_entry_index in first_target_entry_to_enter..self.target_canvas_stack.len() {
            let entry = self.target_canvas_stack[target_entry_index];
            match entry {
                CanvasStackEntry::Clip(clip) if self.clip_is_left_to_composite(clip) => {}
                CanvasStackEntry::Clip(clip) => {
                    let node = &tree.clip_nodes[clip.0 as usize];
                    self.ensure_canvas_matrix_in_draw_space_of(node.spatial);
                    match &node.data {
                        ClipNodeData::Rect(clip) => self.painter.push_clip(&replay_clip_of(clip)),
                        ClipNodeData::Path(clip_path) => {
                            self.painter.push_clip_path(&clip_path.path, clip_path.fill_rule);
                        }
                        ClipNodeData::Dead => unreachable!("a run never records under a tombstoned clip"),
                    }
                }
                CanvasStackEntry::Effect(effect) if self.effect_is_left_to_composite(effect) => {
                    if self.effect_layer_is_pushed(effect) {
                        self.push_plain_layer_standing_in_for(effect);
                    }
                }
                CanvasStackEntry::Effect(effect) => {
                    let node = &tree.effect_nodes[effect.0 as usize];
                    if node.data.pushes_layer() && may_cull_layers {
                        let layer_output_is_culled = match self.effect_geometry(effect).output {
                            Extent::Nothing => true,
                            Extent::Within(output) => match enclosing_int_rect_if_representable(output) {
                                Some(output) => {
                                    self.ensure_canvas_matrix_in_draw_space_of(node.spatial);
                                    self.painter.would_be_fully_clipped_by_painter(output)
                                }
                                None => false,
                            },
                            Extent::Anywhere => false,
                        };
                        if layer_output_is_culled {
                            // Unwinding forgets the context of the canvas stack, so the next switch skips the fast
                            // path yet still reuses the shared prefix.
                            self.restore_canvas_stack_to_length(common_prefix_length);
                            return SwitchResult::CulledByEffect;
                        }
                    }
                    match &node.data {
                        EffectNodeData::Effects(effects) => {
                            if self.effect_layer_is_pushed(effect) {
                                self.ensure_canvas_matrix_in_draw_space_of(node.spatial);
                                let content_bounds = self.layer_content_bounds_in_effect_space(effect, node.spatial);
                                let mut layer =
                                    replay_layer_of(effects, layer_filter_cache_key(tree, effect), content_bounds);
                                if backdrop_is_drawn_by_masks(tree, effect) {
                                    // NB: The outermost mask layer draws the backdrop, and applies the opacity and
                                    //     blend mode to it and the masked content together.
                                    layer.backdrop_filter_bytes = std::ptr::null();
                                    layer.backdrop_filter_bytes_size = 0;
                                    layer.opacity = 1.0;
                                    layer.blend_mode = libgfx_rust::CompositingAndBlendingOperator::Normal;
                                } else if effects.backdrop_filter.is_some() {
                                    layer.backdrop_base_color = self.backdrop_base_color(effect);
                                    layer.backdrop_blur_edge = self.backdrop_blur_edge(effect);
                                }
                                // The canvas fill was recorded before this group. Where it painted opaque pixels,
                                // the painter can blend text directly into them.
                                layer.can_draw_directly_for_lcd_text = self.selection.allows_lcd_text
                                    && tree.root_isolation_effect == Some(effect)
                                    && !effects.needs_layer();
                                self.painter.push_layer(&layer);
                                if self.effect_isolates_content_from_filtered_backdrop(effect) {
                                    self.painter.push_layer(&plain_layer_of_bounds(content_bounds));
                                }
                            }
                        }
                        EffectNodeData::Mask(mask) => {
                            self.ensure_canvas_matrix_in_draw_space_of(node.spatial);
                            let backdrop_layer = self.backdrop_layer_of_outermost_mask(target_entry_index, mask);
                            self.painter.push_mask(&replay_mask_of(mask), backdrop_layer.as_ref());
                        }
                        EffectNodeData::BackgroundColorAnimation => {}
                        EffectNodeData::Dead => unreachable!("a run never records under a tombstoned effect"),
                    }
                }
            }
            self.canvas_stack.push(entry);
        }

        self.clip_and_effect_of_canvas_stack = Some((context.clip, context.effect));
        SwitchResult::Switched
    }

    fn execute_record_span(&mut self, span: ListSpan) {
        let display_list = self.display_list;
        for (chunk_index, begin_offset, end_offset) in display_list.parts_of_span(span) {
            let chunk = display_list.chunk(chunk_index);
            let first_run_index_in_chunk = chunk.index_of_first_run_ending_after(begin_offset);
            for (run_index_in_chunk, run) in chunk.runs().iter().enumerate().skip(first_run_index_in_chunk) {
                if run.offset >= end_offset {
                    break;
                }
                self.execute_run_records(
                    display_list.first_run_of_chunk(chunk_index) + run_index_in_chunk,
                    chunk_index,
                    *run,
                    run.offset.max(begin_offset),
                    (run.offset + run.size).min(end_offset),
                );
            }
        }
    }

    fn execute_run_records(
        &mut self,
        run_index: usize,
        chunk_index: ChunkIndexInList,
        run: DisplayListCommandRun,
        begin_offset: u32,
        end_offset: u32,
    ) {
        if self.world.backface_culled[run.context.spatial.0 as usize] {
            return;
        }
        if self.culling.context_culls_everything(run.context) {
            return;
        }
        if run.ink_bounds.is_empty() {
            return;
        }
        if self.switch_to_context(run.context, true) == SwitchResult::CulledByEffect {
            return;
        }
        self.ensure_canvas_matrix_in_draw_space_of(run.context.spatial);
        if self.painter.would_be_fully_clipped_by_painter(run.ink_bounds) {
            return;
        }
        let display_list = self.display_list;
        let records = &display_list.chunk(chunk_index).records()[begin_offset as usize..end_offset as usize];
        let world = &self.world;
        let to_root_matrix_of_inline_clip_space = |space: SpatialNodeIndex| world.draw_matrix_if_known(space);
        let records_context = RecordsContext {
            tree: self.tree,
            frame: self.frame,
            to_root_matrix_of_inline_clip_space: &to_root_matrix_of_inline_clip_space,
        };
        self.painter.execute_run(run_index, records, &records_context);
    }

    fn execute_selected_records_of_run(&mut self, run_index: usize) {
        let run_span = self.display_list.span_of_run(run_index);
        let Some(record_spans) = self.selection.record_spans else {
            self.execute_record_span(run_span);
            return;
        };
        let first_span_index = record_spans.partition_point(|span| span.end <= run_span.begin);
        for span in &record_spans[first_span_index..] {
            if span.begin >= run_span.end {
                break;
            }
            self.execute_record_span(ListSpan {
                begin: span.begin.max(run_span.begin),
                end: span.end.min(run_span.end),
            });
        }
    }

    fn spans_hold_records_sorted_by_depth(&self, record_spans: &[ListSpan]) -> bool {
        let display_list = self.display_list;
        let run_groups = display_list.run_groups(self.tree);
        record_spans.iter().any(|span| {
            display_list
                .parts_of_span(*span)
                .any(|(chunk_index, begin_offset, end_offset)| {
                    let chunk = display_list.chunk(chunk_index);
                    let first_run_index_in_chunk = chunk.index_of_first_run_ending_after(begin_offset);
                    chunk
                        .runs()
                        .iter()
                        .enumerate()
                        .skip(first_run_index_in_chunk)
                        .take_while(|(_, run)| run.offset < end_offset)
                        .any(|(run_index_in_chunk, _)| {
                            run_groups.run_is_sorted_by_depth(
                                display_list.first_run_of_chunk(chunk_index) + run_index_in_chunk,
                            )
                        })
                })
        })
    }

    fn execute(&mut self) {
        let tree = self.tree;
        if let Some(record_spans) = self.selection.record_spans
            && !(self.tree_sorts_planes_by_depth && self.spans_hold_records_sorted_by_depth(record_spans))
        {
            for span in record_spans {
                self.execute_record_span(*span);
            }
        } else if !self.tree_sorts_planes_by_depth {
            self.execute_record_span(self.display_list.whole_span());
        } else {
            let root_isolation_effect = tree.root_isolation_effect;
            let canvas_stack_length_below_plane_clips = usize::from(root_isolation_effect.is_some());
            let sorting_contexts = tree.resolve_sorting_contexts_in_order(&self.spatial_dependency_order);
            let parent_by_spatial_node: Vec<SpatialNodeIndex> =
                tree.spatial_nodes.iter().map(|node| node.parent).collect();
            let leaf_to_context_palette = resolve_leaf_to_context_matrices(
                &sorting_contexts,
                &self.spatial_dependency_order,
                &parent_by_spatial_node,
                &self.world.local,
                &self.world.flattens_inherited_transform,
            );
            let depth_sorted_plan = build_depth_sorted_replay_plan(
                self.display_list.runs(),
                &sorting_contexts,
                &self.world.to_root,
                &leaf_to_context_palette,
                &self.world.draw_space,
                &self.world.backface_culled,
                &self.culling,
            );
            for step in &depth_sorted_plan.steps {
                match step.kind {
                    DepthSortedReplayStepKind::RunSpan => {
                        let first_run_index = step.first_run as usize;
                        for run_index in first_run_index..first_run_index + step.run_count as usize {
                            self.execute_selected_records_of_run(run_index);
                        }
                    }
                    DepthSortedReplayStepKind::PushPlaneClip => {
                        // The root isolation effect is the root of the effect tree under no clip, so the plane clip
                        // sits right above it.
                        if let Some(root_isolation_effect) = root_isolation_effect {
                            self.switch_to_context(
                                ContextRef {
                                    spatial: tree.effect_nodes[root_isolation_effect.0 as usize].spatial,
                                    clip: ClipNodeIndex::NONE,
                                    effect: root_isolation_effect,
                                },
                                false,
                            );
                        }
                        self.restore_canvas_stack_to_length(canvas_stack_length_below_plane_clips);
                        let vertex_offset = step.vertex_offset as usize;
                        self.painter.push_device_space_plane_clip(
                            &depth_sorted_plan.vertices[vertex_offset..vertex_offset + step.vertex_count as usize],
                        );
                        self.draw_space_of_canvas_matrix = None;
                    }
                    DepthSortedReplayStepKind::PopPlaneClip => {
                        self.restore_canvas_stack_to_length(canvas_stack_length_below_plane_clips);
                        self.painter.pop();
                        self.draw_space_of_canvas_matrix = None;
                    }
                }
            }
        }

        self.restore_canvas_stack_to_length(0);
        // Spaces were entered by setting the canvas matrix outside any save, so no restore hands it back.
        let replay_base_matrix = self.replay_base_matrix;
        self.painter.set_matrix(&replay_base_matrix);
    }
}

pub fn replay_display_list<Painter: ReplayPainter + ?Sized>(
    tree: &VisualContextTree,
    display_list: &DisplayListStorage,
    root_group_color: Color,
    frame: &ReplayFrame<'_>,
    selection: ReplaySelection<'_>,
    painter: &mut Painter,
) {
    let mut driver = ReplayDriver::new(tree, display_list, root_group_color, frame, selection, painter);
    driver.execute();
    driver.return_scratch_storage();
}

impl<'a, Painter: ReplayPainter + ?Sized> ReplayDriver<'a, Painter> {
    fn new(
        tree: &'a VisualContextTree,
        display_list: &'a DisplayListStorage,
        root_group_color: Color,
        frame: &'a ReplayFrame<'a>,
        selection: ReplaySelection<'a>,
        painter: &'a mut Painter,
    ) -> Self {
        let replay_base_matrix = painter.canvas_matrix();
        let effect_clips = display_list.effect_clip_plan(tree);
        let mut scratch = take_replay_scratch_storage();
        tree.fill_culling_scratch(&mut scratch.culling);
        let (effects_left_to_composite, clips_left_to_composite) =
            effects_and_clips_left_to_composite_around(tree, effect_clips, selection.composited_effect);
        let mut driver = ReplayDriver {
            tree,
            display_list,
            frame,
            effect_clips,
            root_group_color,
            selection,
            painter,
            world: scratch.world,
            culling: scratch.culling,
            spatial_dependency_order: tree.spatial_dependency_order(),
            tree_sorts_planes_by_depth: tree.sorts_planes_by_depth(),
            replay_base_matrix,
            draw_space_of_canvas_matrix: None,
            canvas_stack: scratch.canvas_stack,
            target_canvas_stack: scratch.target_canvas_stack,
            clip_and_effect_of_canvas_stack: None,
            effect_geometry_by_effect_index: scratch.effect_geometry_by_effect_index,
            effect_geometry_is_computed: false,
            effects_left_to_composite_around_composited_effect: effects_left_to_composite,
            clips_left_to_composite_around_composited_effect: clips_left_to_composite,
            content_reads_layer_by_effect_index: None,
            first_backdrop_drawn_by_masks_by_reading_mask: std::cell::OnceCell::new(),
        };
        driver.world.fill(
            tree,
            &driver.spatial_dependency_order,
            frame.scroll_offsets,
            Some(replay_base_matrix),
            driver.selection.raster_root,
        );
        driver
    }

    fn return_scratch_storage(&mut self) {
        return_replay_scratch_storage(ReplayScratchStorage {
            world: std::mem::take(&mut self.world),
            culling: std::mem::take(&mut self.culling),
            canvas_stack: std::mem::take(&mut self.canvas_stack),
            target_canvas_stack: std::mem::take(&mut self.target_canvas_stack),
            effect_geometry_by_effect_index: std::mem::take(&mut self.effect_geometry_by_effect_index),
        });
    }
}

/// The compositing of rasters of a frame inside the effects the compositor applies to them as it composites them,
/// which it enters and leaves through the same context switches a replay of the list makes, so that every effect
/// composites as a replay of the list composites it.
pub struct CompositingInsideEffects<'d, 'a, Painter: ReplayPainter + ?Sized> {
    driver: &'d mut ReplayDriver<'a, Painter>,
    canvas_matrix_of_raster_pixels: FloatMatrix4x4,
    canvas_stack_lengths_inside_entered_effects: Vec<usize>,
}

impl<Painter: ReplayPainter + ?Sized> CompositingInsideEffects<'_, '_, Painter> {
    /// Enters `effect` from the innermost effect entered so far, which is one of the effects around it.
    pub fn enter_effect(&mut self, effect: EffectNodeIndex) {
        let node = &self.driver.tree.effect_nodes[effect.0 as usize];
        let context = ContextRef {
            spatial: node.spatial,
            clip: self.driver.effect_clips.output_clip(effect),
            effect,
        };
        self.driver.switch_to_context(context, false);
        self.canvas_stack_lengths_inside_entered_effects
            .push(self.driver.canvas_stack.len());
    }

    /// Leaves the innermost effect entered, which composites what was drawn inside it.
    pub fn leave_effect(&mut self) {
        self.canvas_stack_lengths_inside_entered_effects.pop();
        self.return_to_innermost_entered_effect();
    }

    fn return_to_innermost_entered_effect(&mut self) {
        let canvas_stack_length_inside_innermost_effect = self
            .canvas_stack_lengths_inside_entered_effects
            .last()
            .copied()
            .unwrap_or(0);
        self.driver
            .restore_canvas_stack_to_length(canvas_stack_length_inside_innermost_effect);
    }

    /// Draws the pixels of `source_rect` of a raster over `target_rect` of the surface, within `clips` of the surface,
    /// inside the innermost effect entered.
    pub fn draw_raster(
        &mut self,
        surface: *mut std::ffi::c_void,
        source_rect: IntRect,
        target_rect: IntRect,
        clips: CompositeClipsOfRaster<'_>,
        target_to_surface: Option<AffineTransform>,
    ) {
        self.draw_at_raster_pixels_inside_innermost_effect(clips, target_to_surface, |painter| {
            painter.draw_surface(&SurfaceDraw {
                surface,
                source_rect,
                target_rect,
                filtered: target_to_surface.is_some(),
            });
        });
    }

    /// Fills `rect` of the surface with `color` over what is below, within `clips` of the surface, inside the
    /// innermost effect entered, as a raster all of that color would draw.
    pub fn fill_rect(
        &mut self,
        rect: IntRect,
        color: Color,
        clips: CompositeClipsOfRaster<'_>,
        rect_to_surface: Option<AffineTransform>,
    ) {
        self.draw_at_raster_pixels_inside_innermost_effect(clips, rect_to_surface, |painter| {
            painter.fill_rect(&ResolvedFillRect {
                rect,
                color,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            });
        });
    }

    /// Draws through `drawn_to_raster_pixels`, which maps what `draw` draws to the surface's pixels, where it does not
    /// draw at them, within `clips`, which are at them.
    fn draw_at_raster_pixels_inside_innermost_effect(
        &mut self,
        clips: CompositeClipsOfRaster<'_>,
        drawn_to_raster_pixels: Option<AffineTransform>,
        draw: impl FnOnce(&mut Painter),
    ) {
        self.return_to_innermost_entered_effect();
        self.driver.painter.set_matrix(&self.canvas_matrix_of_raster_pixels);
        self.driver.draw_space_of_canvas_matrix = None;
        let clips_to_push = clips
            .rect
            .map(|rect| (rect, CornerRadii::default()))
            .into_iter()
            .chain(clips.rounded_rects.iter().copied());
        let mut pushed_clip_count = 0;
        for (rect, corner_radii) in clips_to_push {
            self.driver.painter.push_clip(&ReplayClip {
                rect: rect.to_float(),
                corner_radii,
                mode: ClipMode::Intersect,
            });
            pushed_clip_count += 1;
        }
        if let Some(drawn_to_raster_pixels) = drawn_to_raster_pixels {
            let matrix = self
                .canvas_matrix_of_raster_pixels
                .multiplied(libgfx_rust::affine_to_matrix(drawn_to_raster_pixels));
            self.driver.painter.set_matrix(&matrix);
        }
        draw(self.driver.painter);
        for _ in 0..pushed_clip_count {
            self.driver.painter.pop();
        }
    }

    /// Replays the records within `span` inside the innermost effect entered, which they are recorded within.
    pub fn replay_records(&mut self, span: ListSpan) {
        self.return_to_innermost_entered_effect();
        self.driver.execute_record_span(span);
    }
}

/// The clips a raster is composited within, in the pixels of the surface it is composited into: a rect, and rounded
/// rects within it.
#[derive(Clone, Copy, Default)]
pub struct CompositeClipsOfRaster<'a> {
    pub rect: Option<IntRect>,
    pub rounded_rects: &'a [(IntRect, CornerRadii)],
}

/// Composites rasters of the frame into the painter's surface through `composite`, which draws them at the painter's
/// matrix and enters effects as a replay of the list under that matrix would, outside the root isolation layer, which
/// the surface stands for.
pub fn composite_inside_effects<Painter: ReplayPainter + ?Sized>(
    tree: &VisualContextTree,
    display_list: &DisplayListStorage,
    root_group_color: Color,
    frame: &ReplayFrame<'_>,
    painter: &mut Painter,
    composite: impl FnOnce(&mut CompositingInsideEffects<'_, '_, Painter>),
) {
    let canvas_matrix_of_raster_pixels = painter.canvas_matrix();
    let mut driver = ReplayDriver::new(
        tree,
        display_list,
        root_group_color,
        frame,
        ReplaySelection {
            skip_root_isolation_layer: true,
            ..ReplaySelection::default()
        },
        painter,
    );
    let mut compositing = CompositingInsideEffects {
        driver: &mut driver,
        canvas_matrix_of_raster_pixels,
        canvas_stack_lengths_inside_entered_effects: Vec::new(),
    };
    composite(&mut compositing);
    driver.restore_canvas_stack_to_length(0);
    driver.painter.set_matrix(&canvas_matrix_of_raster_pixels);
    driver.return_scratch_storage();
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::commands::ContextRef;
    use crate::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX;
    use crate::display_list::storage::ListPosition;
    use crate::node_slot_id::NodeSlotId;
    use crate::visual_context::{
        BackdropFilterData, BackfaceVisibilityData, ClipData, ClipMode, EffectsData, MaskData, MaskLayerOrigin,
        SpatialData, TransformData, TransformDataRole,
    };
    use ak::math;
    use libgfx_rust::{
        CompositingAndBlendingOperator, CornerRadii, FloatRect, MaskKind, perspective_matrix, scale_matrix,
        translation_matrix,
    };

    #[test]
    fn a_child_stored_below_its_parent_gets_the_same_palette_entry() {
        let root = TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        };
        let translated = |x: f32, y: f32| {
            SpatialData::Transform(TransformData {
                matrix: translation_matrix(x, y, 0.0),
                ..root
            })
        };
        let flipped = SpatialData::Transform(TransformData {
            matrix: scale_matrix(-1.0, 1.0, -1.0),
            ..root
        });

        let mut in_order = VisualContextTree::create(root);
        let parent = in_order.append_spatial(translated(10.0, 0.0), VISUAL_VIEWPORT_NODE_INDEX);
        let child = in_order.append_spatial(translated(0.0, 5.0), parent);
        let plane = in_order.append_spatial(flipped.clone(), child);
        let marker = in_order.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: child,
                flattens_inherited_transform: false,
            }),
            plane,
        );

        let mut permuted = VisualContextTree::create(root);
        let permuted_marker = permuted.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: SpatialNodeIndex(3),
                flattens_inherited_transform: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let permuted_plane = permuted.append_spatial(flipped, VISUAL_VIEWPORT_NODE_INDEX);
        let permuted_child = permuted.append_spatial(translated(0.0, 5.0), VISUAL_VIEWPORT_NODE_INDEX);
        let permuted_parent = permuted.append_spatial(translated(10.0, 0.0), VISUAL_VIEWPORT_NODE_INDEX);
        permuted.spatial_nodes[permuted_marker.0 as usize].parent = permuted_plane;
        permuted.spatial_nodes[permuted_plane.0 as usize].parent = permuted_child;
        permuted.spatial_nodes[permuted_child.0 as usize].parent = permuted_parent;

        let base = scale_matrix(2.0, 2.0, 1.0);
        let in_order_palette = WorldTransforms::new(&in_order, &[], Some(base));
        let permuted_palette = WorldTransforms::new(&permuted, &[], Some(base));
        let pairs = [
            (VISUAL_VIEWPORT_NODE_INDEX, VISUAL_VIEWPORT_NODE_INDEX),
            (parent, permuted_parent),
            (child, permuted_child),
            (plane, permuted_plane),
            (marker, permuted_marker),
        ];
        let map = |index: SpatialNodeIndex| pairs.iter().find(|(original, _)| *original == index).unwrap().1;
        for (original, mapped) in pairs {
            assert_eq!(
                permuted_palette.to_root[mapped.0 as usize],
                in_order_palette.to_root[original.0 as usize]
            );
            assert_eq!(
                permuted_palette.local[mapped.0 as usize],
                in_order_palette.local[original.0 as usize]
            );
            assert_eq!(
                permuted_palette.draw_space[mapped.0 as usize],
                map(in_order_palette.draw_space[original.0 as usize])
            );
            assert_eq!(
                permuted_palette.backface_culled[mapped.0 as usize],
                in_order_palette.backface_culled[original.0 as usize]
            );
            assert_eq!(
                permuted_palette.flattens_inherited_transform[mapped.0 as usize],
                in_order_palette.flattens_inherited_transform[original.0 as usize]
            );
        }
        assert!(in_order_palette.backface_culled[marker.0 as usize]);
        assert_eq!(in_order_palette.draw_space[marker.0 as usize], plane);
    }

    #[derive(Debug, PartialEq)]
    enum PainterEvent {
        SetMatrix(FloatMatrix4x4),
        PushClip(FloatRect),
        PushLayer(f32),
        PushMask(IntRect),
        PopMask,
        Pop,
        PushPlaneClip(usize),
        Run(usize),
    }

    struct RecordingPainter {
        events: Vec<PainterEvent>,
        base_matrix: FloatMatrix4x4,
        fully_clipped_x: i32,
        layer_content_bounds: Vec<Option<FloatRect>>,
        layers_that_can_draw_directly_for_lcd_text: Vec<bool>,
        backdrop_base_colors: Vec<Color>,
        masks_drawing_backdrop: Vec<bool>,
        run_record_sizes: Vec<usize>,
        filled_rects: Vec<IntRect>,
        begun_isolated_groups: usize,
    }

    impl RecordingPainter {
        fn new() -> Self {
            Self {
                events: Vec::new(),
                base_matrix: translation_matrix(100.0, 200.0, 0.0),
                fully_clipped_x: i32::MIN,
                layer_content_bounds: Vec::new(),
                layers_that_can_draw_directly_for_lcd_text: Vec::new(),
                backdrop_base_colors: Vec::new(),
                masks_drawing_backdrop: Vec::new(),
                run_record_sizes: Vec::new(),
                filled_rects: Vec::new(),
                begun_isolated_groups: 0,
            }
        }
    }

    impl ReplayPainter for RecordingPainter {
        fn canvas_matrix(&mut self) -> FloatMatrix4x4 {
            self.base_matrix
        }
        fn set_matrix(&mut self, matrix: &FloatMatrix4x4) {
            self.events.push(PainterEvent::SetMatrix(*matrix));
        }
        fn would_be_fully_clipped_by_painter(&mut self, rect: IntRect) -> bool {
            rect.x == self.fully_clipped_x
        }
        fn device_clip_bounds(&mut self) -> IntRect {
            IntRect::new(i32::MIN / 2, i32::MIN / 2, i32::MAX, i32::MAX)
        }
        fn push_clip(&mut self, clip: &ReplayClip) {
            self.events.push(PainterEvent::PushClip(clip.rect));
        }
        fn push_clip_path(&mut self, _path: &Path, _winding_rule: WindingRule) {
            unreachable!("these tests build no clip paths");
        }
        fn push_layer(&mut self, layer: &ReplayLayer) {
            self.events.push(PainterEvent::PushLayer(layer.opacity));
            self.layer_content_bounds.push(layer.content_bounds.get());
            self.layers_that_can_draw_directly_for_lcd_text
                .push(layer.can_draw_directly_for_lcd_text);
            if layer.backdrop_filter_bytes_size != 0 {
                self.backdrop_base_colors.push(layer.backdrop_base_color);
            }
        }
        fn push_mask(&mut self, mask: &ReplayMask, backdrop: Option<&ReplayLayer>) {
            self.events.push(PainterEvent::PushMask(mask.rect));
            self.masks_drawing_backdrop.push(backdrop.is_some());
        }
        fn begin_mask_content(&mut self, _mask: &ReplayMask) {}
        fn pop_mask(&mut self) {
            self.events.push(PainterEvent::PopMask);
        }
        fn pop(&mut self) {
            self.events.push(PainterEvent::Pop);
        }
        fn push_device_space_plane_clip(&mut self, vertices: &[FloatVector3]) {
            self.events.push(PainterEvent::PushPlaneClip(vertices.len()));
        }
        fn push_transform(&mut self, _transform: &AffineTransform) {}

        fn push_clip_path_bytes(&mut self, _path_bytes: &[u8], _winding_rule: WindingRule, _mode: ClipMode) {}

        fn play_command(&mut self, _command_type: DisplayListCommandType, _command: &[u8], _payload: &[u8]) {}
        fn fill_rect(&mut self, fill: &ResolvedFillRect) {
            self.filled_rects.push(fill.rect);
        }
        fn fill_rect_with_rounded_corners(&mut self, _fill: &ResolvedFillRectWithRoundedCorners) {}
        fn paint_scrollbar(&mut self, _scrollbar: &PresentedScrollbar) {}
        fn draw_composited_context(&mut self, _draw: &CompositedContextDraw) {}
        fn begin_isolated_group(&mut self, _group: &IsolatedGroup) {
            self.begun_isolated_groups += 1;
        }
        fn begin_isolated_group_mask(&mut self, _kind: MaskKind) {}
        fn end_isolated_group_mask(&mut self) {}
        fn end_isolated_group(&mut self) {}
        fn begin_tile_raster(&mut self, _tile: &TileRaster) -> bool {
            false
        }
        fn end_tile_raster(&mut self) {}
        fn begin_vector_image(
            &mut self,
            _placement: &VectorImagePlacement,
            _requires_direct_replay: bool,
        ) -> VectorImageReplay {
            VectorImageReplay::Skipped
        }
        fn end_vector_image(&mut self) {}
        fn draw_surface(&mut self, _draw: &SurfaceDraw) {}

        fn execute_run<'t>(&mut self, run_index: usize, records: &'t [u8], _context: &RecordsContext<'_, 't>) {
            self.events.push(PainterEvent::Run(run_index));
            self.run_record_sizes.push(records.len());
        }
    }

    fn transform(matrix: FloatMatrix4x4, sorting_context_root_index: Option<SpatialNodeIndex>) -> SpatialData {
        SpatialData::Transform(TransformData {
            matrix,
            origin: FloatPoint::default(),
            sorting_context_root_index,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        })
    }

    fn identity_tree() -> VisualContextTree {
        let SpatialData::Transform(root) = transform(FloatMatrix4x4::identity(), None) else {
            unreachable!()
        };
        VisualContextTree::create(root)
    }

    fn clip(rect: FloatRect) -> ClipNodeData {
        ClipNodeData::Rect(ClipData {
            rect,
            corner_radii: CornerRadii::default(),
            mode: ClipMode::Intersect,
        })
    }

    fn effects(opacity: f32) -> EffectNodeData {
        EffectNodeData::Effects(EffectsData {
            isolation: false,
            opacity,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
            backdrop_filter: None,
        })
    }

    fn mask(rect: IntRect) -> EffectNodeData {
        EffectNodeData::Mask(MaskData {
            rect,
            kind: MaskKind::Alpha,
            origin: MaskLayerOrigin::CssMaskLayers,
            masks_backdrop: false,
        })
    }

    fn root_clip(tree: &mut VisualContextTree, rect: FloatRect) -> ClipNodeIndex {
        tree.append_clip(clip(rect), ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX)
    }

    fn effect_under(
        tree: &mut VisualContextTree,
        data: EffectNodeData,
        parent: EffectNodeIndex,
        output_clip: ClipNodeIndex,
    ) -> EffectNodeIndex {
        tree.append_effect(data, parent, VISUAL_VIEWPORT_NODE_INDEX, output_clip)
    }

    fn run(spatial: SpatialNodeIndex, context: ContextRef, ink_bounds: IntRect) -> DisplayListCommandRun {
        DisplayListCommandRun {
            offset: 0,
            size: 0,
            context: ContextRef { spatial, ..context },
            ink_bounds,
            has_compositor_metadata: false,
        }
    }

    fn rotate_y(degrees: f32) -> FloatMatrix4x4 {
        let (sin, cos) = math::sincos(degrees.to_radians());
        FloatMatrix4x4 {
            elements: [
                [cos, 0.0, sin, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [-sin, 0.0, cos, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
        }
    }

    fn visible_bounds() -> IntRect {
        IntRect::new(0, 0, 10, 10)
    }

    fn replay_test_records(
        records: &[u8],
        to_root_matrix_of_inline_clip_space: &dyn Fn(SpatialNodeIndex) -> Option<FloatMatrix4x4>,
        painter: &mut RecordingPainter,
    ) {
        let tree = identity_tree();
        let frame = ReplayFrame::unscrolled(0);
        replay_records(
            records,
            &RecordsContext {
                tree: &tree,
                frame: &frame,
                to_root_matrix_of_inline_clip_space,
            },
            painter,
        );
    }

    fn list_of_one_chunk(record_bytes: &[u8], runs: &[DisplayListCommandRun]) -> std::sync::Arc<DisplayListStorage> {
        DisplayListStorage::of_chunks(vec![std::sync::Arc::new(
            crate::display_list::storage::ListChunk::copied(record_bytes, runs, 0),
        )])
        .unwrap()
    }

    // Lays the runs over a record each, as a recorded list holds no run without a record.
    fn list_of_runs(runs: &[DisplayListCommandRun]) -> std::sync::Arc<DisplayListStorage> {
        let mut builder = crate::display_list::builder::DisplayListBuilder::new();
        builder.append(&fill_rect_command(visible_bounds()), &[], ContextRef::default());
        let record = builder.bytes();
        let runs: Vec<DisplayListCommandRun> = runs
            .iter()
            .enumerate()
            .map(|(index, run)| DisplayListCommandRun {
                offset: (index * record.len()) as u32,
                size: record.len() as u32,
                ..*run
            })
            .collect();
        list_of_one_chunk(&record.repeat(runs.len()), &runs)
    }

    fn replay(tree: &VisualContextTree, runs: &[DisplayListCommandRun], painter: &mut RecordingPainter) {
        replay_display_list(
            tree,
            &list_of_runs(runs),
            Color::TRANSPARENT,
            &ReplayFrame::unscrolled(0),
            ReplaySelection::default(),
            painter,
        );
    }

    fn replay_selection(
        tree: &VisualContextTree,
        runs: &mut [DisplayListCommandRun],
        run_size: u32,
        selection: ReplaySelection<'_>,
    ) -> RecordingPainter {
        for (index, run) in runs.iter_mut().enumerate() {
            run.offset = index as u32 * run_size;
            run.size = run_size;
        }
        let record_bytes = vec![0u8; runs.len() * run_size as usize];
        let mut painter = RecordingPainter::new();
        replay_display_list(
            tree,
            &list_of_one_chunk(&record_bytes, runs),
            Color::TRANSPARENT,
            &ReplayFrame::unscrolled(0),
            selection,
            &mut painter,
        );
        painter
            .events
            .retain(|event| !matches!(event, PainterEvent::SetMatrix(_)));
        painter
    }

    #[test]
    fn a_span_replay_plays_only_its_runs_and_enters_their_contexts_from_what_it_played() {
        let mut tree = identity_tree();
        let rect = FloatRect::new(0.0, 0.0, 100.0, 100.0);
        let clip = root_clip(&mut tree, rect);
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let clipped = ContextRef {
            clip,
            ..ContextRef::default()
        };
        let in_layer = ContextRef {
            effect: layer,
            ..ContextRef::default()
        };
        let mut runs =
            [clipped, in_layer, clipped].map(|context| run(VISUAL_VIEWPORT_NODE_INDEX, context, visible_bounds()));
        let span = |begin, end| ListSpan {
            begin: ListPosition::new(ChunkIndexInList(0), begin),
            end: ListPosition::new(ChunkIndexInList(0), end),
        };
        let spans = [span(0, 8), span(16, 24)];
        let painter = replay_selection(
            &tree,
            &mut runs,
            8,
            ReplaySelection {
                record_spans: Some(&spans),
                ..Default::default()
            },
        );
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::PushClip(rect),
                PainterEvent::Run(0),
                PainterEvent::Run(2),
                PainterEvent::Pop,
            ]
        );
    }

    #[test]
    fn a_replay_enters_and_leaves_contexts_across_chunks_as_it_does_across_runs() {
        let mut tree = identity_tree();
        let rect = FloatRect::new(0.0, 0.0, 100.0, 100.0);
        let clip = root_clip(&mut tree, rect);
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let clipped = ContextRef {
            clip,
            ..ContextRef::default()
        };
        let in_layer = ContextRef {
            effect: layer,
            ..ContextRef::default()
        };
        let mut runs = [clipped, in_layer, in_layer, clipped]
            .map(|context| run(VISUAL_VIEWPORT_NODE_INDEX, context, visible_bounds()));
        for (index, run) in runs.iter_mut().enumerate() {
            run.offset = index as u32 * 16;
            run.size = 16;
        }
        let record_bytes = vec![0u8; 64];
        let one_chunk = list_of_one_chunk(&record_bytes, &runs);
        let two_chunks = DisplayListStorage::of_chunks(vec![
            std::sync::Arc::new(crate::display_list::storage::ListChunk::copied(
                &record_bytes[..32],
                &runs[..2],
                0,
            )),
            std::sync::Arc::new(crate::display_list::storage::ListChunk::copied(
                &record_bytes[32..],
                &runs[2..],
                32,
            )),
        ])
        .unwrap();
        let events_of = |list: &DisplayListStorage, spans: Option<&[ListSpan]>| {
            let mut painter = RecordingPainter::new();
            replay_display_list(
                &tree,
                list,
                Color::TRANSPARENT,
                &ReplayFrame::unscrolled(0),
                ReplaySelection {
                    record_spans: spans,
                    ..Default::default()
                },
                &mut painter,
            );
            painter
                .events
                .retain(|event| !matches!(event, PainterEvent::SetMatrix(_)));
            (painter.events, painter.run_record_sizes)
        };
        let whole_list = events_of(&one_chunk, None);
        assert_eq!(
            whole_list.0,
            vec![
                PainterEvent::PushClip(rect),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::PushLayer(0.5),
                PainterEvent::Run(1),
                PainterEvent::Run(2),
                PainterEvent::Pop,
                PainterEvent::PushClip(rect),
                PainterEvent::Run(3),
                PainterEvent::Pop,
            ]
        );
        assert_eq!(events_of(&two_chunks, None), whole_list);
        // A span that starts and ends inside runs plays the same records whichever chunks hold them.
        let span = |begin: ListPosition, end: ListPosition| [ListSpan { begin, end }];
        assert_eq!(
            events_of(
                &two_chunks,
                Some(&span(
                    ListPosition::new(ChunkIndexInList(0), 24),
                    ListPosition::new(ChunkIndexInList(1), 8)
                ))
            ),
            events_of(
                &one_chunk,
                Some(&span(
                    ListPosition::new(ChunkIndexInList(0), 24),
                    ListPosition::new(ChunkIndexInList(0), 40)
                ))
            )
        );
    }

    #[test]
    fn a_span_replay_plays_the_records_of_a_run_inside_the_span() {
        let mut runs = [ClipNodeIndex::NONE, ClipNodeIndex::NONE]
            .map(|_| run(VISUAL_VIEWPORT_NODE_INDEX, ContextRef::default(), visible_bounds()));
        runs[1].context.effect = EffectNodeIndex(0);
        let mut tree = identity_tree();
        effect_under(&mut tree, effects(1.0), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let spans = [ListSpan {
            begin: ListPosition::new(ChunkIndexInList(0), 8),
            end: ListPosition::new(ChunkIndexInList(0), 24),
        }];
        let painter = replay_selection(
            &tree,
            &mut runs,
            16,
            ReplaySelection {
                record_spans: Some(&spans),
                ..Default::default()
            },
        );
        assert_eq!(painter.events, vec![PainterEvent::Run(0), PainterEvent::Run(1)]);
        assert_eq!(painter.run_record_sizes, vec![8, 8]);
    }

    #[test]
    fn a_replay_that_skips_the_root_isolation_layer_pushes_only_the_layers_inside_it() {
        let mut tree = identity_tree();
        let root = effect_under(&mut tree, effects(1.0), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        tree.root_isolation_effect = Some(root);
        let layer = effect_under(&mut tree, effects(0.5), root, ClipNodeIndex::NONE);
        let mut runs = [root, layer].map(|effect| {
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                ContextRef {
                    effect,
                    ..ContextRef::default()
                },
                visible_bounds(),
            )
        });
        let painter = replay_selection(
            &tree,
            &mut runs,
            8,
            ReplaySelection {
                skip_root_isolation_layer: true,
                ..Default::default()
            },
        );
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::Run(0),
                PainterEvent::PushLayer(0.5),
                PainterEvent::Run(1),
                PainterEvent::Pop,
            ]
        );
    }

    #[test]
    fn only_the_plain_root_layer_of_an_lcd_presentation_can_draw_directly() {
        let mut tree = identity_tree();
        let root = effect_under(&mut tree, effects(1.0), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        tree.root_isolation_effect = Some(root);
        let nested = effect_under(&mut tree, effects(0.5), root, ClipNodeIndex::NONE);
        let runs = [root, nested].map(|effect| {
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                ContextRef {
                    effect,
                    ..ContextRef::default()
                },
                visible_bounds(),
            )
        });

        let white = Color::from_rgb(255, 255, 255);
        for (color, allows_lcd_text, expected) in [
            (white, true, [true, false]),
            (white, false, [false, false]),
            (Color::TRANSPARENT, true, [true, false]),
        ] {
            let mut painter = RecordingPainter::new();
            replay_display_list(
                &tree,
                &list_of_runs(&runs),
                color,
                &ReplayFrame::unscrolled(0),
                ReplaySelection {
                    allows_lcd_text,
                    ..Default::default()
                },
                &mut painter,
            );
            assert_eq!(painter.layers_that_can_draw_directly_for_lcd_text, expected);
        }
    }

    #[test]
    fn an_inline_clip_in_a_space_of_its_own_is_pushed_under_the_matrix_of_that_space() {
        let clip_space = SpatialNodeIndex(7);
        let clip_rect = FloatRect::new(1.0, 2.0, 3.0, 4.0);
        let mut builder = crate::display_list::builder::DisplayListBuilder::new();
        builder.append_with_inline_clips(
            &crate::display_list::commands::FillRect {
                rect: IntRect::new(0, 0, 10, 10),
                color: Color::default(),
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            &[],
            ContextRef::default(),
            &[
                crate::display_list::builder::PendingInlineClip::intersecting_float_rect(clip_rect),
                crate::display_list::builder::PendingInlineClip::intersecting_float_rect(clip_rect)
                    .in_space(clip_space),
            ],
        );
        // The clip lands on the command, at (3, 5) in its space.
        let to_root_matrix_of_inline_clip_space = translation_matrix(102.0, 203.0, 0.0);
        let mut painter = RecordingPainter::new();
        let command_space_matrix = painter.base_matrix;
        replay_test_records(
            builder.bytes(),
            &|space| (space == clip_space).then_some(to_root_matrix_of_inline_clip_space),
            &mut painter,
        );
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::PushClip(clip_rect),
                PainterEvent::SetMatrix(to_root_matrix_of_inline_clip_space),
                PainterEvent::PushClip(clip_rect),
                PainterEvent::SetMatrix(command_space_matrix),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(command_space_matrix),
            ]
        );
    }

    fn fill_rect_command(rect: IntRect) -> crate::display_list::commands::FillRect {
        crate::display_list::commands::FillRect {
            rect,
            color: Color::default(),
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        }
    }

    fn append_group_holding_a_fill(builder: &mut crate::display_list::builder::DisplayListBuilder, rect: IntRect) {
        let group = builder.begin_group::<DrawIsolatedGroup>(Vec::new());
        builder.append(&fill_rect_command(rect), &[], ContextRef::default());
        let content = builder.group_content_span(&group);
        builder.finish_group(
            group,
            &DrawIsolatedGroup {
                clip_rect: rect.to_float(),
                content,
                mask: crate::display_list::commands::DisplayListDataSpan::default(),
                filter: crate::display_list::commands::DisplayListDataSpan::default(),
                opacity: 1.0,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                mask_kind: MaskKind::Alpha,
            },
            ContextRef::default(),
        );
    }

    #[test]
    fn a_group_outside_the_painter_clip_is_culled_with_its_nested_records() {
        let clipped_away = IntRect::new(999, 0, 10, 10);
        let visible = IntRect::new(0, 0, 10, 10);
        let mut builder = crate::display_list::builder::DisplayListBuilder::new();
        append_group_holding_a_fill(&mut builder, clipped_away);
        append_group_holding_a_fill(&mut builder, visible);
        let mut painter = RecordingPainter::new();
        painter.fully_clipped_x = clipped_away.x;
        replay_test_records(builder.bytes(), &|_| None, &mut painter);
        assert_eq!(painter.begun_isolated_groups, 1);
        assert_eq!(painter.filled_rects, vec![visible]);
    }

    #[test]
    fn a_plain_record_outside_the_painter_clip_still_plays() {
        let clipped_away = IntRect::new(999, 0, 10, 10);
        let mut builder = crate::display_list::builder::DisplayListBuilder::new();
        builder.append(&fill_rect_command(clipped_away), &[], ContextRef::default());
        let mut painter = RecordingPainter::new();
        painter.fully_clipped_x = clipped_away.x;
        replay_test_records(builder.bytes(), &|_| None, &mut painter);
        assert_eq!(painter.filled_rects, vec![clipped_away]);
    }

    #[test]
    fn a_command_whose_inline_clip_in_a_space_of_its_own_has_scrolled_off_it_is_culled() {
        let clip_space = SpatialNodeIndex(7);
        let mut builder = crate::display_list::builder::DisplayListBuilder::new();
        builder.append_with_inline_clips(
            &crate::display_list::commands::FillRect {
                rect: IntRect::new(0, 0, 10, 10),
                color: Color::default(),
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            &[],
            ContextRef::default(),
            &[
                crate::display_list::builder::PendingInlineClip::intersecting_float_rect(FloatRect::new(
                    1.0, 2.0, 3.0, 4.0,
                ))
                .in_space(clip_space),
            ],
        );
        let to_root_matrix_of_inline_clip_space = translation_matrix(0.0, -50.0, 0.0);
        let mut painter = RecordingPainter::new();
        replay_test_records(
            builder.bytes(),
            &|space| (space == clip_space).then_some(to_root_matrix_of_inline_clip_space),
            &mut painter,
        );
        assert_eq!(painter.events, vec![]);
    }

    #[test]
    fn an_inline_clip_in_an_unknown_space_is_pushed_in_the_space_of_its_command() {
        let mut builder = crate::display_list::builder::DisplayListBuilder::new();
        builder.append_with_inline_clips(
            &crate::display_list::commands::FillRect {
                rect: IntRect::new(0, 0, 10, 10),
                color: Color::default(),
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            &[],
            ContextRef::default(),
            &[
                crate::display_list::builder::PendingInlineClip::intersecting_float_rect(FloatRect::new(
                    1.0, 2.0, 3.0, 4.0,
                ))
                .in_space(SpatialNodeIndex(u32::MAX - 1)),
            ],
        );
        let mut painter = RecordingPainter::new();
        replay_test_records(builder.bytes(), &|_| None, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::PushClip(FloatRect::new(1.0, 2.0, 3.0, 4.0)),
                PainterEvent::Pop
            ]
        );
    }

    #[test]
    fn runs_sharing_a_stack_prefix_push_it_once_and_re_enter_their_space_after_a_pop() {
        let mut tree = identity_tree();
        let clip_node = root_clip(&mut tree, FloatRect::new(1.0, 2.0, 3.0, 4.0));
        let clip_context = ContextRef {
            clip: clip_node,
            effect: EffectNodeIndex::NONE,
            ..ContextRef::default()
        };
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, clip_node);
        let layer_context = ContextRef {
            clip: clip_node,
            effect: layer,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, clip_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, clip_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(1.0, 2.0, 3.0, 4.0)),
                PainterEvent::Run(0),
                PainterEvent::PushLayer(0.5),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::Run(2),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn a_run_under_an_empty_effective_clip_touches_nothing() {
        let mut tree = identity_tree();
        let empty_clip = root_clip(&mut tree, FloatRect::default());
        let empty_context = ContextRef {
            clip: empty_clip,
            effect: EffectNodeIndex::NONE,
            ..ContextRef::default()
        };
        let empty_mask = effect_under(
            &mut tree,
            mask(IntRect::default()),
            EffectNodeIndex::NONE,
            ClipNodeIndex::NONE,
        );
        let empty_mask_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: empty_mask,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, empty_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, empty_mask_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(painter.events, vec![PainterEvent::SetMatrix(base)]);
    }

    #[test]
    fn a_fully_clipped_layer_is_culled_and_the_next_run_pushes_the_stack_again() {
        let mut tree = identity_tree();
        let clip_node = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 50.0, 50.0));
        let off_screen_layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, clip_node);
        let layer = effect_under(&mut tree, effects(0.25), EffectNodeIndex::NONE, clip_node);
        let context_in = |effect| ContextRef {
            clip: clip_node,
            effect,
            ..ContextRef::default()
        };
        let off_screen = IntRect::new(999, 0, 10, 10);
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, context_in(off_screen_layer), off_screen),
            run(VISUAL_VIEWPORT_NODE_INDEX, context_in(layer), visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        painter.fully_clipped_x = 999;
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::PushLayer(0.25),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn a_layer_too_wide_for_device_pixel_bounds_is_not_culled() {
        let mut tree = identity_tree();
        let far_left = tree.append_spatial(
            transform(translation_matrix(-4.0e9, 0.0, 0.0), None),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let layer_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: layer,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, visible_bounds()),
            run(far_left, layer_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        painter.fully_clipped_x = i32::MIN;
        replay(&tree, &runs, &mut painter);
        assert!(painter.events.contains(&PainterEvent::Run(0)));
        assert!(painter.events.contains(&PainterEvent::Run(1)));
    }

    #[test]
    fn a_layer_whose_blur_reaches_the_clip_is_not_culled_by_its_unblurred_content() {
        let mut tree = identity_tree();
        let clip_node = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 50.0, 50.0));
        let blur = effect_under(
            &mut tree,
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: Some(std::sync::Arc::new(
                    libgfx_rust::filter::Filter::blur(2.0, 2.0, None).serialize(),
                )),
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            clip_node,
        );
        let blur_context = ContextRef {
            clip: clip_node,
            effect: blur,
            ..ContextRef::default()
        };
        let runs = [run(
            VISUAL_VIEWPORT_NODE_INDEX,
            blur_context,
            IntRect::new(999, 0, 10, 10),
        )];
        let mut painter = RecordingPainter::new();
        painter.fully_clipped_x = 999;
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        // Inside the layer this painter still culls the run by its ink, where a real canvas clip grows by the blur.
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::PushLayer(1.0),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    fn effect_context(effect: EffectNodeIndex) -> ContextRef {
        ContextRef {
            clip: ClipNodeIndex::NONE,
            effect,
            ..ContextRef::default()
        }
    }

    fn inflated_by_margin(rect: FloatRect, margin: f32) -> FloatRect {
        FloatRect::new(
            rect.x - margin,
            rect.y - margin,
            rect.width + 2.0 * margin,
            rect.height + 2.0 * margin,
        )
    }

    #[test]
    fn a_layer_is_bounded_by_everything_drawn_into_it() {
        let mut tree = identity_tree();
        let outer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let blur = effect_under(
            &mut tree,
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: Some(std::sync::Arc::new(
                    libgfx_rust::filter::Filter::blur(1.0, 1.0, None).serialize(),
                )),
                backdrop_filter: None,
            }),
            outer,
            ClipNodeIndex::NONE,
        );
        let runs = [
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                effect_context(blur),
                IntRect::new(0, 0, 10, 10),
            ),
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                effect_context(outer),
                IntRect::new(20, 5, 10, 10),
            ),
        ];
        let mut painter = RecordingPainter::new();
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.layer_content_bounds,
            [
                Some(inflated_by_margin(FloatRect::new(-3.0, -3.0, 33.0, 18.0), 2.0)),
                Some(inflated_by_margin(FloatRect::new(0.0, 0.0, 10.0, 10.0), 2.0)),
            ]
        );
    }

    #[test]
    fn a_scaled_layer_widens_its_margin_to_keep_two_device_pixels() {
        let mut tree = identity_tree();
        let half = tree.append_spatial(transform(scale_matrix(0.5, 0.5, 1.0), None), VISUAL_VIEWPORT_NODE_INDEX);
        let double = tree.append_spatial(transform(scale_matrix(4.0, 4.0, 1.0), None), half);
        let layer = tree.append_effect(effects(0.5), EffectNodeIndex::NONE, half, ClipNodeIndex::NONE);
        let runs = [run(double, effect_context(layer), IntRect::new(0, 0, 10, 10))];
        let mut painter = RecordingPainter::new();
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.layer_content_bounds,
            [Some(inflated_by_margin(FloatRect::new(0.0, 0.0, 40.0, 40.0), 4.0))]
        );
    }

    fn assert_layer_bounds_are_approximately(layer_content_bounds: &[Option<FloatRect>], expected: &[FloatRect]) {
        assert_eq!(layer_content_bounds.len(), expected.len(), "{layer_content_bounds:?}");
        let edges = |rect: FloatRect| [rect.x, rect.y, rect.right(), rect.bottom()];
        for (bounds, expected) in layer_content_bounds.iter().zip(expected) {
            let bounds = bounds.unwrap_or_else(|| panic!("a layer has no bounds, not {expected:?}"));
            for (edge, expected_edge) in edges(bounds).into_iter().zip(edges(*expected)) {
                assert!((edge - expected_edge).abs() < 0.001, "{bounds:?} is not {expected:?}");
            }
        }
    }

    #[test]
    fn a_layer_over_or_in_a_tilted_space_is_bounded_where_its_content_projects() {
        let mut tree = identity_tree();
        let tilted = tree.append_spatial(transform(rotate_y(60.0), None), VISUAL_VIEWPORT_NODE_INDEX);
        let flat_layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let tilted_layer = tree.append_effect(effects(0.5), EffectNodeIndex::NONE, tilted, ClipNodeIndex::NONE);
        let runs = [
            run(tilted, effect_context(flat_layer), IntRect::new(0, 0, 10, 10)),
            run(tilted, effect_context(tilted_layer), IntRect::new(0, 0, 10, 10)),
        ];
        let mut painter = RecordingPainter::new();
        replay(&tree, &runs, &mut painter);
        assert_layer_bounds_are_approximately(
            &painter.layer_content_bounds,
            &[
                inflated_by_margin(FloatRect::new(0.0, 0.0, 5.0, 10.0), 2.0),
                FloatRect::new(-4.0, -2.0, 18.0, 14.0),
            ],
        );
    }

    #[test]
    fn a_layer_whose_content_lies_partly_behind_the_eye_has_no_bounds() {
        let mut tree = identity_tree();
        let tilted = tree.append_spatial(
            transform(perspective_matrix(100.0).multiplied(rotate_y(60.0)), None),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let flat_layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let tilted_layer = tree.append_effect(effects(0.5), EffectNodeIndex::NONE, tilted, ClipNodeIndex::NONE);
        let runs = [
            run(tilted, effect_context(flat_layer), IntRect::new(-1000, 0, 2000, 10)),
            run(tilted, effect_context(tilted_layer), IntRect::new(-1000, 0, 2000, 10)),
        ];
        let mut painter = RecordingPainter::new();
        replay(&tree, &runs, &mut painter);
        assert_eq!(painter.layer_content_bounds, [None, None]);
    }

    #[test]
    fn mask_effects_pop_with_their_effect_index() {
        let mut tree = identity_tree();
        let mask_effect = effect_under(
            &mut tree,
            mask(IntRect::new(0, 0, 20, 20)),
            EffectNodeIndex::NONE,
            ClipNodeIndex::NONE,
        );
        let mask_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: mask_effect,
            ..ContextRef::default()
        };
        let runs = [run(VISUAL_VIEWPORT_NODE_INDEX, mask_context, visible_bounds())];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushMask(IntRect::new(0, 0, 20, 20)),
                PainterEvent::Run(0),
                PainterEvent::PopMask,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn an_escaping_absolute_child_under_an_opacity_layer_pushes_the_layer_once() {
        let mut tree = identity_tree();
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let overflow_clip = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 100.0, 100.0));
        let normal_context = ContextRef {
            clip: overflow_clip,
            effect: layer,
            ..ContextRef::default()
        };
        let escaping_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: layer,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, normal_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, escaping_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, normal_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 100.0, 100.0)),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::Run(1),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 100.0, 100.0)),
                PainterEvent::Run(2),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn a_layer_nothing_escapes_is_entered_inside_the_clips_its_contexts_share() {
        let mut tree = identity_tree();
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let outer_clip = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 100.0, 100.0));
        let inner_clip = tree.append_clip(
            clip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
            outer_clip,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let nested = effect_under(&mut tree, effects(0.25), layer, ClipNodeIndex::NONE);
        let outer_context = ContextRef {
            clip: outer_clip,
            effect: layer,
            ..ContextRef::default()
        };
        let nested_context = ContextRef {
            clip: inner_clip,
            effect: nested,
            ..ContextRef::default()
        };
        tree.effect_nodes[layer.0 as usize].local_clip = outer_clip;
        tree.effect_nodes[nested.0 as usize].local_clip = inner_clip;
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, outer_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, nested_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 100.0, 100.0)),
                PainterEvent::PushLayer(0.5),
                PainterEvent::Run(0),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::PushLayer(0.25),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn a_clip_inside_a_layer_is_re_pushed_when_switching_chains_without_popping_the_layer() {
        let mut tree = identity_tree();
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let own_clip = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 10.0, 10.0));
        let escaping_clip = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 20.0, 20.0));
        let own_context = ContextRef {
            clip: own_clip,
            effect: layer,
            ..ContextRef::default()
        };
        let escaping_context = ContextRef {
            clip: escaping_clip,
            effect: layer,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, own_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, escaping_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, own_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 10.0, 10.0)),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 20.0, 20.0)),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 10.0, 10.0)),
                PainterEvent::Run(2),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn switching_clip_depths_inside_one_effect_keeps_the_layer_and_its_output_clip() {
        let mut tree = identity_tree();
        let outer_rect = FloatRect::new(0.0, 0.0, 100.0, 100.0);
        let common_rect = FloatRect::new(0.0, 0.0, 90.0, 90.0);
        let deep_rect = FloatRect::new(0.0, 0.0, 80.0, 80.0);
        let other_rect = FloatRect::new(0.0, 0.0, 70.0, 70.0);
        let outer = root_clip(&mut tree, outer_rect);
        let common = tree.append_clip(clip(common_rect), outer, VISUAL_VIEWPORT_NODE_INDEX);
        let deep = tree.append_clip(clip(deep_rect), common, VISUAL_VIEWPORT_NODE_INDEX);
        let other = tree.append_clip(clip(other_rect), outer, VISUAL_VIEWPORT_NODE_INDEX);
        let effect = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, outer);
        let runs = [deep, other, outer, common].map(|clip| {
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                ContextRef {
                    clip,
                    effect,
                    ..ContextRef::default()
                },
                visible_bounds(),
            )
        });
        let mut painter = RecordingPainter::new();
        replay(&tree, &runs, &mut painter);
        let events: Vec<_> = painter
            .events
            .into_iter()
            .filter(|event| !matches!(event, PainterEvent::SetMatrix(_)))
            .collect();
        assert_eq!(
            events,
            vec![
                PainterEvent::PushClip(outer_rect),
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushClip(common_rect),
                PainterEvent::PushClip(deep_rect),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::PushClip(other_rect),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::Run(2),
                PainterEvent::PushClip(common_rect),
                PainterEvent::Run(3),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::Pop,
            ]
        );
    }

    #[test]
    fn a_later_clip_escape_keeps_the_opacity_group_open() {
        let mut tree = identity_tree();
        let rect = FloatRect::new(0.0, 0.0, 100.0, 100.0);
        let local_clip = root_clip(&mut tree, rect);
        let effect = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, local_clip);
        let runs = [local_clip, ClipNodeIndex::NONE].map(|clip| {
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                ContextRef {
                    clip,
                    effect,
                    ..ContextRef::default()
                },
                visible_bounds(),
            )
        });
        let mut painter = RecordingPainter::new();
        replay(&tree, &runs, &mut painter);
        let events: Vec<_> = painter
            .events
            .into_iter()
            .filter(|event| !matches!(event, PainterEvent::SetMatrix(_)))
            .collect();
        assert_eq!(
            events,
            vec![
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushClip(rect),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::Run(1),
                PainterEvent::Pop,
            ]
        );
    }

    #[test]
    fn a_mask_nested_in_a_layer_pops_once_with_its_effect_index() {
        let mut tree = identity_tree();
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let overflow_clip = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 100.0, 100.0));
        let mask_effect = effect_under(&mut tree, mask(IntRect::new(0, 0, 20, 20)), layer, ClipNodeIndex::NONE);
        let layer_context = ContextRef {
            clip: overflow_clip,
            effect: layer,
            ..ContextRef::default()
        };
        let masked_context = ContextRef {
            clip: overflow_clip,
            effect: mask_effect,
            ..ContextRef::default()
        };
        let escaping_masked_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: mask_effect,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, masked_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, escaping_masked_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        let overflow = FloatRect::new(0.0, 0.0, 100.0, 100.0);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushClip(overflow),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushMask(IntRect::new(0, 0, 20, 20)),
                PainterEvent::PushClip(overflow),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::Run(2),
                PainterEvent::PopMask,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(overflow),
                PainterEvent::Run(3),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn a_mask_draws_only_the_backdrop_of_an_effect_its_list_draws_into() {
        let mut tree = identity_tree();
        let root = effect_under(&mut tree, effects(1.0), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        tree.root_isolation_effect = Some(root);
        let mask_drawing_backdrop = effect_under(
            &mut tree,
            EffectNodeData::Mask(MaskData {
                rect: visible_bounds(),
                kind: MaskKind::Alpha,
                origin: MaskLayerOrigin::CssMaskLayers,
                masks_backdrop: true,
            }),
            root,
            ClipNodeIndex::NONE,
        );
        let box_with_blurred_backdrop = effect_under(
            &mut tree,
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: Some(BackdropFilterData {
                    filter: std::sync::Arc::new(libgfx_rust::filter::Filter::blur(2.0, 2.0, None).serialize()),
                    region: visible_bounds(),
                    corner_radii: CornerRadii::default(),
                }),
            }),
            mask_drawing_backdrop,
            ClipNodeIndex::NONE,
        );
        let run_in = |effect| {
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                ContextRef {
                    effect,
                    ..ContextRef::default()
                },
                visible_bounds(),
            )
        };

        // A list from another process can draw under the mask and nothing inside the box.
        let mut painter = RecordingPainter::new();
        replay(&tree, &[run_in(mask_drawing_backdrop)], &mut painter);
        assert!(painter.events.contains(&PainterEvent::Run(0)));
        assert_eq!(painter.masks_drawing_backdrop, vec![false]);

        let runs = [run_in(box_with_blurred_backdrop)];
        let mut painter = RecordingPainter::new();
        composite_inside_effects(
            &tree,
            &list_of_runs(&runs),
            Color::TRANSPARENT,
            &ReplayFrame::unscrolled(0),
            &mut painter,
            |compositing| {
                compositing.enter_effect(mask_drawing_backdrop);
                compositing.leave_effect();
            },
        );
        assert_eq!(painter.masks_drawing_backdrop, vec![true]);
    }

    #[test]
    fn a_layer_carries_its_backdrop_filter() {
        let filter = std::sync::Arc::new(vec![1, 2, 3]);
        let region = IntRect::new(1, 2, 30, 40);
        let radii = CornerRadii::uniform(5);
        let with_backdrop = EffectsData {
            isolation: false,
            opacity: 0.5,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
            backdrop_filter: Some(BackdropFilterData {
                filter: filter.clone(),
                region,
                corner_radii: radii,
            }),
        };
        let layer = replay_layer_of(&with_backdrop, 3, None);
        assert_eq!(layer.opacity, 0.5);
        assert_eq!(layer.backdrop_filter_bytes, filter.as_ptr());
        assert_eq!(layer.backdrop_filter_bytes_size, 3);
        assert_eq!(layer.backdrop_region, region);
        assert_eq!(layer.backdrop_corner_radii, radii);
        assert_eq!(layer.filter_cache_key, 3);

        let EffectNodeData::Effects(without_backdrop) = effects(0.5) else {
            unreachable!()
        };
        let layer = replay_layer_of(&without_backdrop, 4, None);
        assert!(layer.backdrop_filter_bytes.is_null());
        assert_eq!(layer.backdrop_filter_bytes_size, 0);
        assert_eq!(layer.backdrop_region, IntRect::default());
    }

    #[test]
    fn only_a_backdrop_whose_backdrop_root_is_the_root_isolation_effect_reads_the_root_group_color() {
        let mut tree = identity_tree();
        let root = effect_under(&mut tree, effects(1.0), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        tree.root_isolation_effect = Some(root);
        let will_change_without_layer = effect_under(&mut tree, effects(1.0), root, ClipNodeIndex::NONE);
        let backdrop = || {
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: Some(BackdropFilterData {
                    filter: std::sync::Arc::new(vec![1]),
                    region: visible_bounds(),
                    corner_radii: CornerRadii::default(),
                }),
            })
        };
        let in_root = effect_under(&mut tree, backdrop(), root, ClipNodeIndex::NONE);
        let in_will_change = effect_under(&mut tree, backdrop(), will_change_without_layer, ClipNodeIndex::NONE);
        let runs = [in_root, in_will_change].map(|effect| {
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                ContextRef {
                    effect,
                    ..ContextRef::default()
                },
                visible_bounds(),
            )
        });
        let white = Color::from_rgb(255, 255, 255);
        let mut painter = RecordingPainter::new();
        replay_display_list(
            &tree,
            &list_of_runs(&runs),
            white,
            &ReplayFrame::unscrolled(0),
            ReplaySelection::default(),
            &mut painter,
        );
        assert_eq!(painter.backdrop_base_colors, vec![white, Color::TRANSPARENT]);
    }

    #[test]
    fn a_replay_inside_a_composited_effect_leaves_out_the_effect_and_those_around_it() {
        let mut tree = identity_tree();
        let root = effect_under(&mut tree, effects(1.0), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        tree.root_isolation_effect = Some(root);
        let backdrop = || {
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: Some(BackdropFilterData {
                    filter: std::sync::Arc::new(vec![1]),
                    region: visible_bounds(),
                    corner_radii: CornerRadii::default(),
                }),
            })
        };
        let composited_box = effect_under(&mut tree, backdrop(), root, ClipNodeIndex::NONE);
        let other_box = effect_under(&mut tree, backdrop(), root, ClipNodeIndex::NONE);
        let runs = [composited_box, other_box].map(|effect| {
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                ContextRef {
                    effect,
                    ..ContextRef::default()
                },
                visible_bounds(),
            )
        });
        let white = Color::from_rgb(255, 255, 255);
        let mut painter = RecordingPainter::new();
        replay_display_list(
            &tree,
            &list_of_runs(&runs),
            white,
            &ReplayFrame::unscrolled(0),
            ReplaySelection {
                composited_effect: Some(composited_box),
                ..ReplaySelection::default()
            },
            &mut painter,
        );
        // The composited box and the root isolation layer get plain layers standing in for theirs, and only the other
        // box reads a backdrop.
        assert_eq!(
            painter
                .events
                .iter()
                .filter(|event| matches!(event, PainterEvent::PushLayer(_)))
                .count(),
            3
        );
        assert_eq!(painter.backdrop_base_colors, vec![white]);
    }

    #[test]
    fn a_background_color_animation_marker_pushes_nothing_and_keeps_the_matrix() {
        let mut tree = identity_tree();
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let marker = effect_under(
            &mut tree,
            EffectNodeData::BackgroundColorAnimation,
            layer,
            ClipNodeIndex::NONE,
        );
        let layer_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: layer,
            ..ContextRef::default()
        };
        let marker_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: marker,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, marker_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, marker_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushLayer(0.5),
                PainterEvent::Run(0),
                PainterEvent::Run(1),
                PainterEvent::Run(2),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn culling_the_second_of_two_new_layers_unwinds_to_the_shared_prefix() {
        let mut tree = identity_tree();
        let clip_node = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 50.0, 50.0));
        let outer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, clip_node);
        let inner = effect_under(&mut tree, effects(0.25), outer, clip_node);
        let context_in = |effect| ContextRef {
            clip: clip_node,
            effect,
            ..ContextRef::default()
        };
        let off_screen = IntRect::new(999, 0, 10, 10);
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, context_in(inner), off_screen),
            run(VISUAL_VIEWPORT_NODE_INDEX, context_in(outer), visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        painter.fully_clipped_x = 999;
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::PushLayer(0.5),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::PushLayer(0.5),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn runs_on_a_culled_back_face_are_skipped_and_scroll_offsets_enter_the_palette() {
        let mut tree = identity_tree();
        let flipped = tree.append_spatial(
            transform(scale_matrix(-1.0, 1.0, -1.0), None),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let marker = tree.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: VISUAL_VIEWPORT_NODE_INDEX,
                flattens_inherited_transform: false,
            }),
            flipped,
        );
        let scroll_node = tree.append_spatial(
            SpatialData::Scroll(crate::visual_context::ScrollData {
                state_slot: crate::visual_context::scroll_state::NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let runs = [
            run(marker, ContextRef::default(), visible_bounds()),
            run(scroll_node, ContextRef::default(), visible_bounds()),
        ];
        let mut scroll_offsets = vec![FloatPoint::default(); 4];
        scroll_offsets[scroll_node.0 as usize] = FloatPoint { x: 0.0, y: -30.7 };
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay_display_list(
            &tree,
            &list_of_runs(&runs),
            Color::TRANSPARENT,
            &ReplayFrame {
                scroll_offsets: &scroll_offsets,
                ..ReplayFrame::unscrolled(0)
            },
            ReplaySelection::default(),
            &mut painter,
        );
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base.multiplied(translation_matrix(0.0, -31.0, 0.0))),
                PainterEvent::Run(1),
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn intersecting_planes_of_a_sorting_context_replay_under_plane_clips() {
        let mut tree = identity_tree();
        let context_root = tree.append_spatial(transform(FloatMatrix4x4::identity(), None), VISUAL_VIEWPORT_NODE_INDEX);
        let first_plane = tree.append_spatial(transform(rotate_y(45.0), Some(context_root)), context_root);
        let second_plane = tree.append_spatial(transform(rotate_y(-45.0), Some(context_root)), context_root);
        let plane_bounds = IntRect::new(-50, -50, 100, 100);
        let runs = [
            run(first_plane, ContextRef::default(), plane_bounds),
            run(second_plane, ContextRef::default(), plane_bounds),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        let plane_clip_pushes = painter
            .events
            .iter()
            .filter(|event| matches!(event, PainterEvent::PushPlaneClip(_)))
            .count();
        let pops = painter
            .events
            .iter()
            .filter(|event| **event == PainterEvent::Pop)
            .count();
        let executed_runs = painter
            .events
            .iter()
            .filter(|event| matches!(event, PainterEvent::Run(_)))
            .count();
        assert!(plane_clip_pushes > 0);
        assert_eq!(pops, plane_clip_pushes);
        assert!(executed_runs >= runs.len());
        assert_eq!(painter.events.last(), Some(&PainterEvent::SetMatrix(base)));
    }

    #[test]
    fn plane_clips_unwind_to_the_root_isolation_layer() {
        let mut tree = identity_tree();
        let root_layer = effect_under(&mut tree, effects(1.0), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        tree.root_isolation_effect = Some(root_layer);
        let clip_node = root_clip(&mut tree, FloatRect::new(-100.0, -100.0, 300.0, 300.0));
        let clipped_context = ContextRef {
            clip: clip_node,
            effect: root_layer,
            ..ContextRef::default()
        };
        let context_root = tree.append_spatial(transform(FloatMatrix4x4::identity(), None), VISUAL_VIEWPORT_NODE_INDEX);
        let first_plane = tree.append_spatial(transform(rotate_y(45.0), Some(context_root)), context_root);
        let second_plane = tree.append_spatial(transform(rotate_y(-45.0), Some(context_root)), context_root);
        let plane_bounds = IntRect::new(-50, -50, 100, 100);
        let runs = [
            run(first_plane, clipped_context, plane_bounds),
            run(second_plane, clipped_context, plane_bounds),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        let layer_pushes = painter
            .events
            .iter()
            .filter(|event| matches!(event, PainterEvent::PushLayer(_)))
            .count();
        assert_eq!(layer_pushes, 1);
        let mut depth = 0i32;
        let mut plane_clip_pushes = 0;
        for event in &painter.events {
            match event {
                PainterEvent::PushPlaneClip(_) => {
                    assert_eq!(depth, 1, "a plane clip is pushed right above the root layer");
                    plane_clip_pushes += 1;
                    depth += 1;
                }
                PainterEvent::PushClip(_) | PainterEvent::PushLayer(_) | PainterEvent::PushMask(_) => depth += 1,
                PainterEvent::Pop | PainterEvent::PopMask => depth -= 1,
                _ => {}
            }
        }
        assert!(plane_clip_pushes > 0);
        assert_eq!(depth, 0);
        assert_eq!(painter.events.last(), Some(&PainterEvent::SetMatrix(base)));
    }
}
