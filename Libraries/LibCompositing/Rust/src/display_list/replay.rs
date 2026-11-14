/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::display_list::builder::{
    for_each_command, has_inline_clip_in_its_own_space, inline_clips_of, inline_transform_of, read_command,
};
use crate::display_list::commands::{
    ClipMode, ClipNodeIndex, DeclareMaskContent, DisplayListCommandHeader, DisplayListCommandRun,
    DisplayListCommandType, DisplayListPaintStyleType, DrawCompositedContext, DrawIsolatedGroup, DrawRepeatedTile,
    EffectNodeIndex, FillPath, FillRect, FillRectWithRoundedCorners, InlineClipKind, OptionalFloatRect, PaintCaret,
    PaintNestedDisplayList, PaintScrollBar, PathPaintKind, ReplayClip, ReplayLayer, ReplayMask, SpatialNodeIndex,
    StrokePath,
};
use crate::display_list::depth_sorted_plan::{DepthSortedReplayStepKind, build_depth_sorted_replay_plan};
use crate::display_list::effect_clip_plan::EffectClipPlan;
use crate::display_list::effect_geometry::{EffectGeometry, Extent, compute_effect_geometry};
use crate::display_list::nested_records::span_bytes;
use crate::display_list::storage::DisplayListStorage;
use crate::host::{
    CompositedContextDraw, IsolatedGroup, NestedDisplayListPlacement, NestedDisplayListReplay, PresentedScrollbar,
    RecordSpan, ResolvedFillRect, ResolvedFillRectWithRoundedCorners, TileRaster,
};
use crate::visual_context::queries::TreeCullingScratch;
use crate::visual_context::{
    ClipNodeData, ContextRef, EffectNodeData, SpatialData, VisualContextTree, WorldTransforms, device_offset_for_index,
    resolve_leaf_to_context_matrices,
};
use libgfx_rust::filter::Filter;
use libgfx_rust::path::Path;
use libgfx_rust::{
    AffineTransform, Color, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, FloatVector3, IntRect, IntSize,
    MaskKind, WindingRule, enclosing_int_rect, multiply_affine,
};
use std::cell::RefCell;
use std::collections::HashMap;
use std::hash::Hasher;

/// The raster primitives a replay draws through. Everything a painter is handed is resolved for the frame: it never
/// sees the visual context tree, the scroll offsets or the scrollbars of the frame.
pub trait ReplayPainter {
    fn canvas_matrix(&mut self) -> FloatMatrix4x4;
    fn set_matrix(&mut self, matrix: &FloatMatrix4x4);
    fn would_be_fully_clipped_by_painter(&mut self, rect: IntRect) -> bool;
    fn push_clip(&mut self, clip: &ReplayClip);
    fn push_clip_path(&mut self, path: &Path, winding_rule: WindingRule);
    fn push_layer(&mut self, layer: &ReplayLayer);
    // `backdrop`, when given, carries the backdrop filter the mask's layer starts with.
    fn push_mask(&mut self, mask: &ReplayMask, backdrop: Option<&ReplayLayer>);
    // Starts the content that masks the layer `push_mask` opened. `pop_mask` closes both.
    fn begin_mask_content(&mut self, mask: &ReplayMask);
    fn pop_mask(&mut self);
    fn pop(&mut self);
    fn push_device_space_plane_clip(&mut self, vertices: &[FloatVector3]);
    fn push_transform(&mut self, transform: &AffineTransform);
    fn push_clip_path_bytes(&mut self, path_bytes: &[u8], winding_rule: WindingRule, mode: ClipMode);
    // Plays one command as it was recorded. `command` holds the command struct, which can differ from the one at the
    // start of `payload`; the command's spans point into `payload`.
    fn play_command(&mut self, command_type: DisplayListCommandType, command: &[u8], payload: &[u8]);
    fn fill_rect(&mut self, fill: &ResolvedFillRect);
    fn fill_rect_with_rounded_corners(&mut self, fill: &ResolvedFillRectWithRoundedCorners);
    fn paint_scrollbar(&mut self, scrollbar: &PresentedScrollbar);
    fn draw_composited_context(&mut self, draw: &CompositedContextDraw);
    fn begin_isolated_group(&mut self, group: &IsolatedGroup);
    fn begin_isolated_group_mask(&mut self, kind: MaskKind);
    fn end_isolated_group_mask(&mut self);
    fn end_isolated_group(&mut self);
    // Answers whether the tile records are to be replayed into the raster, which `end_tile_raster` then finishes.
    fn begin_tile_raster(&mut self, tile: &TileRaster) -> bool;
    fn end_tile_raster(&mut self);
    // Unless the painter skips the list, the replay of the list is followed by `end_nested_display_list`.
    fn begin_nested_display_list(
        &mut self,
        placement: &NestedDisplayListPlacement,
        requires_direct_replay: bool,
    ) -> NestedDisplayListReplay;
    fn end_nested_display_list(&mut self);

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
    // How far the scrollbar has grown from its thin geometry into its expanded one.
    pub expansion: f32,
    pub opacity: f32,
}

/// A display list nested in another: its tape, the tree it was recorded against, the color of its root group, and
/// whether a raster of it can differ from replaying it.
#[derive(Clone, Copy)]
pub struct NestedDisplayList<'a> {
    pub storage: &'a DisplayListStorage,
    pub tree: &'a VisualContextTree,
    pub root_group_color: Color,
    pub requires_direct_replay: bool,
}

/// Where a replay finds the display lists that the lists it plays nest, by their resource ids.
pub trait NestedDisplayLists {
    fn nested_display_list(&self, display_list_id: u64) -> Option<NestedDisplayList<'_>>;
}

/// The surface that shows the frame of a context drawn into another, and the part of it the frame is in.
#[derive(Clone, Copy, Debug)]
pub struct ResolvedCompositedContext {
    pub surface: *mut std::ffi::c_void,
    pub content_rect: FloatRect,
}

/// Where a replay finds the frames of the contexts the list draws.
pub trait CompositedContexts {
    // The frame of the child context as it is drawn into `destination_rect` under `canvas_matrix`, none where the
    // context shows nothing there.
    fn resolve(
        &self,
        child_context_id: u64,
        destination_rect: FloatRect,
        canvas_matrix: &FloatMatrix4x4,
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
    ) -> Option<ResolvedCompositedContext> {
        None
    }
}

/// The resources of a replay that nests no display list.
pub struct NoNestedDisplayLists;

impl NestedDisplayLists for NoNestedDisplayLists {
    fn nested_display_list(&self, _display_list_id: u64) -> Option<NestedDisplayList<'_>> {
        None
    }
}

/// What a replay paints a display list with besides the list: where the frame presents the scroll containers and the
/// scrollbars, the time it shows a blinking caret at, the display lists the list nests, and the frames of the contexts
/// it draws.
#[derive(Clone, Copy)]
pub struct ReplayFrame<'a> {
    pub scroll_offsets: &'a [FloatPoint],
    pub scrollbars: &'a [ScrollbarShownByFrame],
    pub time_ns: i64,
    pub nested_display_lists: &'a dyn NestedDisplayLists,
    pub composited_contexts: &'a dyn CompositedContexts,
}

impl<'a> ReplayFrame<'a> {
    /// A frame that scrolls nothing, presents no scrollbar of its own, and draws no other context.
    pub fn unscrolled(time_ns: i64, nested_display_lists: &'a dyn NestedDisplayLists) -> Self {
        Self {
            scroll_offsets: &[],
            scrollbars: &[],
            time_ns,
            nested_display_lists,
            composited_contexts: &NoCompositedContexts,
        }
    }

    // Tile records paint into a raster of their own, where nothing scrolls.
    fn for_tile_records(&self) -> Self {
        Self {
            scroll_offsets: &[],
            ..*self
        }
    }

    // A nested display list scrolls nothing, and the scrollbars of the frame do not belong to its scroll nodes.
    fn for_nested_display_list(&self) -> Self {
        Self {
            scroll_offsets: &[],
            scrollbars: &[],
            ..*self
        }
    }
}

/// What one display list holds for the streams of records it plays: the tree it was recorded against, and the
/// content that each of its masks declared.
pub struct ListReplay<'t> {
    pub tree: &'t VisualContextTree,
    declared_mask_contents: RefCell<HashMap<EffectNodeIndex, &'t [u8]>>,
}

impl<'t> ListReplay<'t> {
    pub fn new(tree: &'t VisualContextTree) -> Self {
        Self {
            tree,
            declared_mask_contents: RefCell::new(HashMap::new()),
        }
    }

    fn declare_mask_content(&self, effect: EffectNodeIndex, content: &'t [u8]) {
        self.declared_mask_contents.borrow_mut().insert(effect, content);
    }

    fn declared_mask_content(&self, effect: EffectNodeIndex) -> Option<&'t [u8]> {
        self.declared_mask_contents.borrow().get(&effect).copied()
    }
}

/// What playing a stream of records needs besides the records: the list they belong to, the frame, and the to-root
/// matrix of the space an inline clip of its own space is applied in.
pub struct RecordsContext<'c, 't> {
    pub list: &'c ListReplay<'t>,
    pub frame: &'c ReplayFrame<'c>,
    pub clip_space_matrix: &'c dyn Fn(SpatialNodeIndex) -> Option<FloatMatrix4x4>,
}

/// How far the inline clips of a command that apply in spaces of their own let it draw.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum OwnSpaceClipExtent {
    /// One of the clips cannot be mapped into the space of the command, or cuts a hole out of it.
    Unbounded,
    /// Within this rect, in the space of the command.
    Within(FloatRect),
}

/// The part of `bounds`, in the space of a command whose to-root matrix is `command_to_root`, that its inline clips
/// in spaces of their own leave the command to draw in, or None if it has no such clip. `clip_space_matrix` gives
/// the to-root matrix of a clip's space.
pub(crate) fn own_space_clip_extent(
    header: &DisplayListCommandHeader,
    payload: &[u8],
    bounds: FloatRect,
    command_to_root: FloatMatrix4x4,
    mut clip_space_matrix: impl FnMut(SpatialNodeIndex) -> Option<FloatMatrix4x4>,
) -> Option<OwnSpaceClipExtent> {
    let mut extent = None;
    for clip in inline_clips_of(header, payload).filter(|clip| clip.has_own_space) {
        let mapped_clip_rect = (clip.mode == ClipMode::Intersect && command_to_root.is_2d_affine())
            .then(|| clip_space_matrix(clip.space))
            .flatten()
            .filter(|clip_to_root| clip_to_root.is_2d_affine())
            .and_then(|clip_to_root| {
                let root_to_command = command_to_root.extract_2d_affine().inverse()?;
                let clip_to_command = multiply_affine(root_to_command, clip_to_root.extract_2d_affine());
                Some(clip_to_command.map_rect(clip.clip_rect_or_path_device_bounds))
            });
        let Some(mapped_clip_rect) = mapped_clip_rect else {
            return Some(OwnSpaceClipExtent::Unbounded);
        };
        let previous = match extent {
            Some(OwnSpaceClipExtent::Within(rect)) => rect,
            _ => bounds,
        };
        extent = Some(OwnSpaceClipExtent::Within(previous.intersected(mapped_clip_rect)));
    }
    extent
}

const CARET_BLINK_INTERVAL_NS: i64 = 500_000_000;

fn caret_is_visible_at_time(caret: &PaintCaret, time_ns: i64) -> bool {
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
pub fn presented_scrollbar_with_colors(
    gutter_rect: FloatRect,
    thumb_rect: FloatRect,
    expansion: f32,
    opacity: f32,
    thumb_color: Color,
    thumb_outline_color: Color,
    track_color: Color,
) -> PresentedScrollbar {
    // The gutter of a thin scrollbar is not painted, so the gutter grows out of the thin track as it fades in.
    let track_alpha = (f32::from(track_color.alpha()) * expansion).round();
    PresentedScrollbar {
        gutter_rect,
        thumb_rect,
        thumb_color,
        thumb_outline_color,
        track_color: track_color.with_alpha(track_alpha as u8),
        opacity,
    }
}

fn tile_raster_key(tile_records: &[u8], tile_size: IntSize) -> u64 {
    let mut hasher = crate::fast_hash::fast_hasher();
    hasher.write(tile_records);
    hasher.write_i32(tile_size.width);
    hasher.write_i32(tile_size.height);
    hasher.finish()
}

// The records a pattern paint style tiles are replayed into its raster before the path command that paints with it.
fn rasterize_pattern_tile_of<'t, Painter: ReplayPainter + ?Sized>(
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
        let tile_frame = context.frame.for_tile_records();
        replay_records(
            span_bytes(payload, paint_style.pattern_tile),
            &RecordsContext {
                list: context.list,
                frame: &tile_frame,
                clip_space_matrix: &|_| None,
            },
            painter,
        );
        painter.end_tile_raster();
    }
}

// Plays one record, inside its inline clips and transform, resolving what the frame decides about it.
fn play_record<'t, Painter: ReplayPainter + ?Sized>(
    header: &DisplayListCommandHeader,
    payload: &'t [u8],
    context: &RecordsContext<'_, 't>,
    painter: &mut Painter,
) {
    let tree = context.list.tree;
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
            // A scrollbar command of the display list with no scrollbar of the frame is painted as the command gives
            // it. The colors are the ones the display list resolved for the page, which it may have darkened.
            let shown_by_frame = context.frame.scrollbars.iter().find(|scrollbar| {
                scrollbar.scroll_node_index == command.scroll_node_index && scrollbar.vertical == command.vertical
            });
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
                    let device_offset =
                        device_offset_for_index(context.frame.scroll_offsets, command.scroll_node_index);
                    if command.vertical {
                        let shift = (-f64::from(device_offset.y) * command.scroll_size) as i32;
                        command.thumb_rect.y = command.thumb_rect.y.saturating_add(shift);
                    } else {
                        let shift = (-f64::from(device_offset.x) * command.scroll_size) as i32;
                        command.thumb_rect.x = command.thumb_rect.x.saturating_add(shift);
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
            // Snap iframe origins to device pixels for browser compatibility when transforms are absent or identity.
            // Preserve fractional placement under other transforms to retain the raster phase of surrounding content.
            if command.snap_origin_if_untransformed
                && tree.translations_snap_to_device_pixels(command.spatial_node_index)
            {
                dst_rect.x = dst_rect.x.round();
                dst_rect.y = dst_rect.y.round();
            }
            let canvas_matrix = painter.canvas_matrix();
            let Some(resolved) =
                context
                    .frame
                    .composited_contexts
                    .resolve(command.child_context_id.0, dst_rect, &canvas_matrix)
            else {
                return;
            };
            if resolved.surface.is_null() || resolved.content_rect.is_empty() {
                return;
            }
            painter.draw_composited_context(&CompositedContextDraw {
                surface: resolved.surface,
                src_rect: resolved.content_rect,
                dst_rect,
                scaling_mode: command.scaling_mode,
            });
        }
        DisplayListCommandType::DeclareMaskContent => {
            let command = read_command::<DeclareMaskContent>(payload);
            context
                .list
                .declare_mask_content(command.effect, span_bytes(payload, command.content));
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
                list: context.list,
                frame: context.frame,
                clip_space_matrix: &|_| None,
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
                key: tile_raster_key(tile_records, command.tile_size),
                size: command.tile_size,
                cacheable: true,
            };
            if painter.begin_tile_raster(&tile) {
                let tile_frame = context.frame.for_tile_records();
                replay_records(
                    tile_records,
                    &RecordsContext {
                        list: context.list,
                        frame: &tile_frame,
                        clip_space_matrix: &|_| None,
                    },
                    painter,
                );
                painter.end_tile_raster();
            }
            painter.play_command(header.command_type, payload, payload);
        }
        DisplayListCommandType::FillPath => {
            let command = read_command::<FillPath>(payload);
            rasterize_pattern_tile_of(command.paint_kind, &command.paint_style, payload, context, painter);
            painter.play_command(header.command_type, payload, payload);
        }
        DisplayListCommandType::StrokePath => {
            let command = read_command::<StrokePath>(payload);
            rasterize_pattern_tile_of(command.paint_kind, &command.paint_style, payload, context, painter);
            painter.play_command(header.command_type, payload, payload);
        }
        DisplayListCommandType::PaintNestedDisplayList => {
            let command = read_command::<PaintNestedDisplayList>(payload);
            let Some(nested) = context
                .frame
                .nested_display_lists
                .nested_display_list(command.display_list_id.0)
            else {
                return;
            };
            let placement = NestedDisplayListPlacement {
                display_list_id: command.display_list_id.0,
                rect: command.rect,
                list_size: command.list_size,
            };
            if painter.begin_nested_display_list(&placement, nested.requires_direct_replay)
                == NestedDisplayListReplay::Skipped
            {
                return;
            }
            let nested_frame = context.frame.for_nested_display_list();
            replay_display_list(
                nested.tree,
                nested.storage.bytes(),
                nested.storage.command_runs(),
                nested.storage.effect_clip_plan(nested.tree),
                nested.root_group_color,
                &nested_frame,
                ReplaySelection::default(),
                painter,
            );
            painter.end_nested_display_list();
        }
        _ => painter.play_command(header.command_type, payload, payload),
    }
}

/// Plays one stream of records: the records of a run, or the records nested in a command. Compositor
/// metadata is skipped, a command whose bounds are empty or clipped away is culled, and each command plays
/// inside its inline clips and transform. A clip whose space the context does not know applies in the space of its
/// command.
pub fn replay_records<'t, Painter: ReplayPainter + ?Sized>(
    records: &'t [u8],
    context: &RecordsContext<'_, 't>,
    painter: &mut Painter,
) {
    let clip_space_matrix = context.clip_space_matrix;
    for_each_command(records, |header, _, payload| {
        if header.command_type.is_compositor_metadata() {
            return;
        }
        if header.bounding_rect.is_empty() || painter.would_be_fully_clipped_by_painter(header.bounding_rect) {
            return;
        }
        // The matrix of the command's space, while an inline clip in a space of its own has the canvas in another.
        let mut command_space_matrix = None;
        if header.inline_clip_count != 0 && has_inline_clip_in_its_own_space(header, payload) {
            let command_to_root = painter.canvas_matrix();
            // NB: The bounds of a command do not include its clips in spaces of their own, which move apart from it
            //     as they scroll, so the command is culled against where they are now.
            if let Some(OwnSpaceClipExtent::Within(rect)) = own_space_clip_extent(
                header,
                payload,
                header.bounding_rect.to_float(),
                command_to_root,
                clip_space_matrix,
            ) && (rect.is_empty() || painter.would_be_fully_clipped_by_painter(enclosing_int_rect(rect)))
            {
                return;
            }
        }
        for clip in inline_clips_of(header, payload) {
            let own_space_matrix = clip.has_own_space.then(|| clip_space_matrix(clip.space)).flatten();
            if let Some(matrix) = &own_space_matrix {
                command_space_matrix.get_or_insert_with(|| painter.canvas_matrix());
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
                && let Some(matrix) = &command_space_matrix
            {
                painter.set_matrix(matrix);
            }
        }
        let inline_transform = inline_transform_of(header, payload);
        if let Some(transform) = &inline_transform {
            painter.push_transform(transform);
        }
        play_record(header, payload, context, painter);
        if inline_transform.is_some() {
            painter.pop();
        }
        for _ in 0..header.inline_clip_count {
            painter.pop();
        }
        // Popping a clip pushed in another space restores the matrix it was pushed with.
        if let Some(matrix) = &command_space_matrix {
            painter.set_matrix(matrix);
        }
    });
}

// Recorded bounds are whole device pixels around geometry that antialiasing and glyph rounding
// can push past them by up to this much, and a layer's bounds clip what is drawn into it.
const LAYER_CONTENT_MARGIN_IN_DEVICE_PIXELS: f32 = 2.0;

// One entry of the canvas save stack: a clip or an effect the replay has entered.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Applied {
    Clip(ClipNodeIndex),
    Effect(EffectNodeIndex),
}

// Steady-state replays reuse the previous frame's capacity. Taking the storage out of the slot
// for the duration of a replay keeps re-entrant nested replays from clobbering the outer
// driver's buffers; the slot keeps whichever returned storage has the larger table.
#[derive(Default)]
struct ReplayScratchStorage {
    world: WorldTransforms,
    culling: TreeCullingScratch,
    applied: Vec<Applied>,
    target: Vec<Applied>,
    effect_geometry: Vec<EffectGeometry>,
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
    storage.applied.clear();
    storage.target.clear();
    storage.effect_geometry.clear();
    WARM_REPLAY_SCRATCH_STORAGE.with(|slot| {
        let mut slot = slot.borrow_mut();
        let slot_has_larger_table = slot
            .as_ref()
            .is_some_and(|kept| kept.world.to_root.capacity() >= storage.world.to_root.capacity());
        if !slot_has_larger_table {
            *slot = Some(storage);
        }
    });
}

#[derive(Clone, Copy, Default)]
pub struct ReplaySelection<'a> {
    pub record_spans: Option<&'a [RecordSpan]>,
    pub skip_root_isolation_layer: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum SwitchResult {
    Switched,
    CulledByEffect,
}

struct ReplayDriver<'a, Painter: ReplayPainter + ?Sized> {
    tree: &'a VisualContextTree,
    tape: &'a [u8],
    command_runs: &'a [DisplayListCommandRun],
    list: ListReplay<'a>,
    frame: &'a ReplayFrame<'a>,
    effect_clips: &'a EffectClipPlan,
    root_group_color: Color,
    selection: ReplaySelection<'a>,
    painter: &'a mut Painter,
    // Spatial nodes never touch the canvas save stack: any node's space is entered absolutely with a single
    // set_matrix(), so only clips and effects do.
    world: WorldTransforms,
    culling: TreeCullingScratch,
    spatial_dependency_order: Vec<u32>,
    tree_has_sorting_contexts: bool,
    replay_base_matrix: FloatMatrix4x4,
    // The draw space the canvas matrix currently equals, if known; popping a clip or a layer
    // resets the matrix to its save point, so unwinding invalidates it. Recorded streams contain
    // no matrix-mutating commands, so playing commands never invalidates the cache.
    current_ctm_space: Option<SpatialNodeIndex>,
    // The canvas save stack as clips and effects, the stack a target context needs, and the
    // pair the applied stack currently serves.
    applied: Vec<Applied>,
    target: Vec<Applied>,
    applied_context: Option<(ClipNodeIndex, EffectNodeIndex)>,
    // Filled when the first layer is entered, since only replays that enter layers need it.
    effect_geometry: Vec<EffectGeometry>,
    effect_geometry_is_known: bool,
}

fn replay_clip_of(clip: &crate::visual_context::ClipData) -> ReplayClip {
    ReplayClip {
        rect: clip.rect,
        corner_radii: clip.corner_radii,
        mode: clip.mode,
    }
}

// An effect keeps its filters for as long as the tree keeps its structure, so a painter can key what it builds from
// them on the two.
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

// Whether the backdrop of a backdrop-filter effect is drawn by its box's masks rather than by the effect's own layer.
fn backdrop_is_drawn_by_masks(tree: &VisualContextTree, effect: EffectNodeIndex) -> bool {
    let node = &tree.effect_nodes[effect.0 as usize];
    matches!(&node.data, EffectNodeData::Effects(effects) if effects.backdrop_filter.is_some())
        && !node.parent.is_none()
        && matches!(&tree.effect_nodes[node.parent.0 as usize].data, EffectNodeData::Mask(mask) if mask.masks_backdrop)
}

fn replay_mask_of(mask: &crate::visual_context::MaskData) -> ReplayMask {
    ReplayMask {
        rect: mask.rect,
        kind: mask.kind,
    }
}

// The whole device pixels covering `rect`, unless they reach past what an IntRect can hold, where
// converting would clamp the edges apart from each other.
fn int_rect_enclosing(rect: FloatRect) -> Option<IntRect> {
    let (left, top, right, bottom) = (
        rect.x.floor(),
        rect.y.floor(),
        rect.right().ceil(),
        rect.bottom().ceil(),
    );
    let fits = |value: f32| value.is_finite() && value >= i32::MIN as f32 && value < i32::MAX as f32;
    [left, top, right, bottom, right - left, bottom - top]
        .into_iter()
        .all(fits)
        .then(|| enclosing_int_rect(rect))
}

impl<Painter: ReplayPainter + ?Sized> ReplayDriver<'_, Painter> {
    fn effect_layer_is_pushed(&self, effect: EffectNodeIndex) -> bool {
        self.tree.effect_pushes_layer_at_replay(effect)
            && !(self.selection.skip_root_isolation_layer && self.tree.root_isolation_effect == Some(effect))
    }

    fn ensure_ctm_space(&mut self, spatial: SpatialNodeIndex) {
        let space = self.world.draw_space[spatial.0 as usize];
        if self.current_ctm_space == Some(space) {
            return;
        }
        self.painter.set_matrix(&self.world.to_root[space.0 as usize]);
        self.current_ctm_space = Some(space);
    }

    fn effect_geometry(&mut self, effect: EffectNodeIndex) -> EffectGeometry {
        if !self.effect_geometry_is_known {
            compute_effect_geometry(self.tree, self.command_runs, &self.world, &mut self.effect_geometry);
            self.effect_geometry_is_known = true;
        }
        self.effect_geometry[effect.0 as usize]
    }

    // The bounds hint for an effect's layer, in the effect's space.
    fn layer_content_bounds(&mut self, effect: EffectNodeIndex, space: SpatialNodeIndex) -> Option<FloatRect> {
        let Extent::Within(content) = self.effect_geometry(effect).content else {
            return None;
        };
        let to_root_matrix = self.world.draw_matrix(space);
        if !to_root_matrix.is_2d_affine() {
            return None;
        }
        let root_to_layer = to_root_matrix.extract_2d_affine().inverse()?;
        let margin_x = LAYER_CONTENT_MARGIN_IN_DEVICE_PIXELS * (root_to_layer.a().abs() + root_to_layer.c().abs());
        let margin_y = LAYER_CONTENT_MARGIN_IN_DEVICE_PIXELS * (root_to_layer.b().abs() + root_to_layer.d().abs());
        let bounds = FloatRect::new(
            content.x - margin_x,
            content.y - margin_y,
            content.width + 2.0 * margin_x,
            content.height + 2.0 * margin_y,
        );
        // Bounds that overflowed would clip away everything drawn into the layer.
        [bounds.x, bounds.y, bounds.right(), bounds.bottom()]
            .into_iter()
            .all(f32::is_finite)
            .then_some(bounds)
    }

    // https://drafts.fxtf.org/filter-effects-2/#backdrop-filter-operation
    // The masks of a box with a backdrop filter apply to the filtered backdrop too. Skia reads a layer's
    // backdrop from the layer's parent, so the outermost of the box's mask layers starts with the
    // filtered backdrop, and the box's effects layer below the masks draws none. Opacity and blending
    // come after masking, so the outermost mask layer applies them too.
    fn backdrop_layer_of_outermost_mask(
        &self,
        target_index: usize,
        mask: &crate::visual_context::MaskData,
    ) -> Option<ReplayLayer> {
        if !mask.masks_backdrop {
            return None;
        }
        let tree = self.tree;
        let Applied::Effect(mask_effect) = self.target[target_index] else {
            unreachable!("a mask is an effect");
        };
        let parent = tree.effect_nodes[mask_effect.0 as usize].parent;
        if !parent.is_none()
            && matches!(&tree.effect_nodes[parent.0 as usize].data, EffectNodeData::Mask(parent) if parent.masks_backdrop)
        {
            return None;
        }
        self.target[target_index + 1..].iter().find_map(|entry| {
            let Applied::Effect(effect) = *entry else {
                return None;
            };
            match &tree.effect_nodes[effect.0 as usize].data {
                EffectNodeData::Effects(effects) if backdrop_is_drawn_by_masks(tree, effect) => {
                    let mut layer = replay_layer_of(effects, layer_filter_cache_key(tree, effect), None);
                    layer.backdrop_base_color = self.backdrop_base_color(mask_effect);
                    layer.backdrop_blur_edge = self.backdrop_blur_edge(effect);
                    Some(layer)
                }
                _ => None,
            }
        })
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
        let mut edge = backdrop.region.to_float();
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
                edge = edge.intersected(clip_rect);
            }
            clip = clip_node.parent;
        }
        OptionalFloatRect::from(Some(edge))
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

    // Merge the clip and effect chains from leaf to root. Each effect goes immediately
    // inside its output clip; reversing once produces the canvas stack in linear time.
    fn build_target(&mut self, context: ContextRef) {
        let tree = self.tree;
        self.target.clear();
        let mut clip = context.clip;
        let mut effect = context.effect;
        while !effect.is_none() {
            let node = &tree.effect_nodes[effect.0 as usize];
            while clip != self.effect_clips.output_clip(effect) {
                debug_assert!(
                    !clip.is_none(),
                    "an effect's output clip lies on the context's clip chain"
                );
                self.target.push(Applied::Clip(clip));
                clip = tree.clip_nodes[clip.0 as usize].parent;
            }
            self.target.push(Applied::Effect(effect));
            effect = node.parent;
        }
        while !clip.is_none() {
            self.target.push(Applied::Clip(clip));
            clip = tree.clip_nodes[clip.0 as usize].parent;
        }
        self.target.reverse();
    }

    // Contexts in the same effect share every layer. Their clip chains meet at or below
    // that effect's output clip, so only the trailing clips can need to be replaced.
    fn build_target_clip_suffix(&mut self, mut current: ClipNodeIndex, mut target: ClipNodeIndex) -> usize {
        self.target.clear();
        let mut current_depth = self.culling.clip_depth(current);
        let mut target_depth = self.culling.clip_depth(target);
        let mut clips_to_pop = 0;
        while current != target {
            if current_depth >= target_depth {
                current = self.tree.clip_nodes[current.0 as usize].parent;
                current_depth -= 1;
                clips_to_pop += 1;
            } else {
                self.target.push(Applied::Clip(target));
                target = self.tree.clip_nodes[target.0 as usize].parent;
                target_depth -= 1;
            }
        }
        self.target.reverse();
        let common_prefix_length = self.applied.len() - clips_to_pop;
        debug_assert!(
            self.applied[common_prefix_length..]
                .iter()
                .all(|entry| matches!(entry, Applied::Clip(_)))
        );
        common_prefix_length
    }

    fn restore_to_length(&mut self, length: usize) {
        let tree = self.tree;
        self.applied_context = None;
        while self.applied.len() > length {
            match self.applied.pop().expect("applied entries are not empty") {
                Applied::Clip(_) => {
                    self.painter.pop();
                    self.current_ctm_space = None;
                }
                Applied::Effect(effect) => {
                    let node = &tree.effect_nodes[effect.0 as usize];
                    match &node.data {
                        EffectNodeData::Effects(_) => {
                            if self.effect_layer_is_pushed(effect) {
                                self.painter.pop();
                                self.current_ctm_space = None;
                            }
                        }
                        EffectNodeData::Mask(mask) => {
                            self.ensure_ctm_space(node.spatial);
                            self.painter.begin_mask_content(&replay_mask_of(mask));
                            if let Some(content) = self.list.declared_mask_content(effect) {
                                let records_context = RecordsContext {
                                    list: &self.list,
                                    frame: self.frame,
                                    clip_space_matrix: &|_| None,
                                };
                                replay_records(content, &records_context, &mut *self.painter);
                            }
                            self.painter.pop_mask();
                            self.current_ctm_space = None;
                        }
                        // A marker touches no canvas state.
                        EffectNodeData::BackgroundColorAnimation => {}
                        EffectNodeData::Dead => unreachable!("a run never records under a tombstoned effect"),
                    }
                }
            }
        }
    }

    // OPTIMIZATION: When walking down to layer-pushing effects (opacity/blend/filter layers and masks), check
    //               culling before pushing each one. The clips entered so far are those outside the layer, so
    //               testing the layer's whole output against them is conservative and valid. This avoids
    //               expensive saveLayer/restore cycles for off-screen elements.
    fn switch_to_context(&mut self, context: ContextRef, may_cull_layers: bool) -> SwitchResult {
        if self.applied_context == Some((context.clip, context.effect)) {
            return SwitchResult::Switched;
        }

        let tree = self.tree;
        let (common_prefix_length, target_begin) = if let Some((clip, effect)) = self.applied_context
            && effect == context.effect
        {
            (self.build_target_clip_suffix(clip, context.clip), 0)
        } else {
            self.build_target(context);
            let common_prefix_length = self
                .applied
                .iter()
                .zip(&self.target)
                .take_while(|(applied, target)| applied == target)
                .count();
            (common_prefix_length, common_prefix_length)
        };

        self.restore_to_length(common_prefix_length);

        for i in target_begin..self.target.len() {
            let entry = self.target[i];
            match entry {
                Applied::Clip(clip) => {
                    let node = &tree.clip_nodes[clip.0 as usize];
                    self.ensure_ctm_space(node.spatial);
                    match &node.data {
                        ClipNodeData::Rect(clip) => self.painter.push_clip(&replay_clip_of(clip)),
                        ClipNodeData::Path(clip_path) => {
                            self.painter.push_clip_path(&clip_path.path, clip_path.fill_rule);
                        }
                        ClipNodeData::Dead => unreachable!("a run never records under a tombstoned clip"),
                    }
                }
                Applied::Effect(effect) => {
                    let node = &tree.effect_nodes[effect.0 as usize];
                    if node.data.pushes_layer() && may_cull_layers {
                        let culled_by_layer = match self.effect_geometry(effect).output {
                            Extent::Nothing => true,
                            Extent::Within(output) => match int_rect_enclosing(output) {
                                Some(output) => {
                                    self.ensure_ctm_space(node.spatial);
                                    self.painter.would_be_fully_clipped_by_painter(output)
                                }
                                None => false,
                            },
                            Extent::Anywhere => false,
                        };
                        if culled_by_layer {
                            self.restore_to_length(common_prefix_length);
                            // The canvas is unwound to the shared prefix; clearing the applied
                            // context keeps the fast path from reusing the pre-cull context while
                            // the applied stack still enables prefix reuse on the next switch.
                            return SwitchResult::CulledByEffect;
                        }
                    }
                    match &node.data {
                        EffectNodeData::Effects(effects) => {
                            if self.effect_layer_is_pushed(effect) {
                                self.ensure_ctm_space(node.spatial);
                                let content_bounds = self.layer_content_bounds(effect, node.spatial);
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
                                self.painter.push_layer(&layer);
                            }
                        }
                        EffectNodeData::Mask(mask) => {
                            self.ensure_ctm_space(node.spatial);
                            let backdrop = self.backdrop_layer_of_outermost_mask(i, mask);
                            self.painter.push_mask(&replay_mask_of(mask), backdrop.as_ref());
                        }
                        EffectNodeData::BackgroundColorAnimation => {}
                        EffectNodeData::Dead => unreachable!("a run never records under a tombstoned effect"),
                    }
                }
            }
            self.applied.push(entry);
        }

        self.applied_context = Some((context.clip, context.effect));
        SwitchResult::Switched
    }

    // A run enters its context once. Skipping a run with nothing to draw before entering its
    // context spares the pushes.
    fn execute_run(&mut self, run_index: usize) {
        let run = self.command_runs[run_index];
        self.execute_run_records(run_index, run.offset, run.offset + run.size);
    }

    fn execute_record_span(&mut self, span: RecordSpan) {
        let first_run = self
            .command_runs
            .partition_point(|run| run.offset + run.size <= span.begin_offset);
        for run_index in first_run..self.command_runs.len() {
            let run = self.command_runs[run_index];
            if run.offset >= span.end_offset {
                break;
            }
            self.execute_run_records(
                run_index,
                run.offset.max(span.begin_offset),
                (run.offset + run.size).min(span.end_offset),
            );
        }
    }

    fn execute_run_records(&mut self, run_index: usize, begin_offset: u32, end_offset: u32) {
        let run = self.command_runs[run_index];
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
        self.ensure_ctm_space(run.context.spatial);
        if self.painter.would_be_fully_clipped_by_painter(run.ink_bounds) {
            return;
        }
        let records = &self.tape[begin_offset as usize..end_offset as usize];
        let world = &self.world;
        let clip_space_matrix = |space: SpatialNodeIndex| world.draw_matrix_if_known(space);
        let records_context = RecordsContext {
            list: &self.list,
            frame: self.frame,
            clip_space_matrix: &clip_space_matrix,
        };
        self.painter.execute_run(run_index, records, &records_context);
    }

    fn execute(&mut self) {
        let tree = self.tree;
        if let Some(record_spans) = self.selection.record_spans {
            debug_assert!(!self.tree_has_sorting_contexts);
            for span in record_spans {
                self.execute_record_span(*span);
            }
        } else if !self.tree_has_sorting_contexts {
            for run_index in 0..self.command_runs.len() {
                self.execute_run(run_index);
            }
        } else {
            let root_isolation_effect = tree.root_isolation_effect;
            let plane_clip_base_length = usize::from(root_isolation_effect.is_some());
            let contexts = tree.resolve_sorting_contexts_in_order(&self.spatial_dependency_order);
            let parent_by_node: Vec<SpatialNodeIndex> = tree.spatial_nodes.iter().map(|node| node.parent).collect();
            let leaf_to_context_palette = resolve_leaf_to_context_matrices(
                &contexts,
                &self.spatial_dependency_order,
                &parent_by_node,
                &self.world.local,
                &self.world.flattens_inherited_transform,
            );
            let plan = build_depth_sorted_replay_plan(
                self.command_runs,
                &contexts,
                &self.world.to_root,
                &leaf_to_context_palette,
                &self.world.draw_space,
                &self.world.backface_culled,
                &self.culling,
            );
            for step in &plan.steps {
                match step.kind {
                    DepthSortedReplayStepKind::RunSpan => {
                        let first_run = step.first_run as usize;
                        for run_index in first_run..first_run + step.run_count as usize {
                            self.execute_run(run_index);
                        }
                    }
                    DepthSortedReplayStepKind::PushPlaneClip => {
                        // The root effect is a layer at the root of the effect tree under no
                        // clip, so the plane clip sits right above it.
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
                        self.restore_to_length(plane_clip_base_length);
                        let vertex_offset = step.vertex_offset as usize;
                        self.painter.push_device_space_plane_clip(
                            &plan.vertices[vertex_offset..vertex_offset + step.vertex_count as usize],
                        );
                        self.current_ctm_space = None;
                    }
                    DepthSortedReplayStepKind::PopPlaneClip => {
                        self.restore_to_length(plane_clip_base_length);
                        self.painter.pop();
                        self.current_ctm_space = None;
                    }
                }
            }
        }

        self.restore_to_length(0);
        // Node spaces were entered by setting the canvas matrix absolutely, outside any save, so the
        // matrix the replay entered with must be handed back explicitly.
        let replay_base_matrix = self.replay_base_matrix;
        self.painter.set_matrix(&replay_base_matrix);
    }
}

#[allow(clippy::too_many_arguments)]
pub fn replay_display_list<Painter: ReplayPainter + ?Sized>(
    tree: &VisualContextTree,
    tape: &[u8],
    command_runs: &[DisplayListCommandRun],
    effect_clips: &EffectClipPlan,
    root_group_color: Color,
    frame: &ReplayFrame<'_>,
    selection: ReplaySelection<'_>,
    painter: &mut Painter,
) {
    let replay_base_matrix = painter.canvas_matrix();
    let mut scratch = take_replay_scratch_storage();
    tree.fill_culling_scratch(&mut scratch.culling);
    let mut driver = ReplayDriver {
        tree,
        tape,
        command_runs,
        list: ListReplay::new(tree),
        frame,
        effect_clips,
        root_group_color,
        selection,
        painter,
        world: scratch.world,
        culling: scratch.culling,
        spatial_dependency_order: tree.spatial_dependency_order(),
        tree_has_sorting_contexts: tree.spatial_nodes.iter().any(|node| {
            matches!(&node.data, SpatialData::Transform(transform) if transform.sorting_context_root_index.is_some())
        }),
        replay_base_matrix,
        current_ctm_space: None,
        applied: scratch.applied,
        target: scratch.target,
        applied_context: None,
        effect_geometry: scratch.effect_geometry,
        effect_geometry_is_known: false,
    };
    driver.world.fill(
        tree,
        &driver.spatial_dependency_order,
        frame.scroll_offsets,
        Some(replay_base_matrix),
    );
    driver.execute();
    return_replay_scratch_storage(ReplayScratchStorage {
        world: std::mem::take(&mut driver.world),
        culling: std::mem::take(&mut driver.culling),
        applied: std::mem::take(&mut driver.applied),
        target: std::mem::take(&mut driver.target),
        effect_geometry: std::mem::take(&mut driver.effect_geometry),
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::commands::ContextRef;
    use crate::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX;
    use crate::node_slot_id::NodeSlotId;
    use crate::visual_context::{
        BackdropFilterData, BackfaceVisibilityData, ClipData, ClipMode, EffectsData, MaskData, MaskLayerOrigin,
        SpatialData, TransformData, TransformDataRole,
    };
    use ak::math;
    use libgfx_rust::{
        CompositingAndBlendingOperator, CornerRadii, FloatRect, MaskKind, scale_matrix, translation_matrix,
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
        backdrop_base_colors: Vec<Color>,
        run_record_sizes: Vec<usize>,
    }

    impl RecordingPainter {
        fn new() -> Self {
            Self {
                events: Vec::new(),
                base_matrix: translation_matrix(100.0, 200.0, 0.0),
                fully_clipped_x: i32::MIN,
                layer_content_bounds: Vec::new(),
                backdrop_base_colors: Vec::new(),
                run_record_sizes: Vec::new(),
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
        fn push_clip(&mut self, clip: &ReplayClip) {
            self.events.push(PainterEvent::PushClip(clip.rect));
        }
        fn push_clip_path(&mut self, _path: &Path, _winding_rule: WindingRule) {
            unreachable!("these tests build no clip paths");
        }
        fn push_layer(&mut self, layer: &ReplayLayer) {
            self.events.push(PainterEvent::PushLayer(layer.opacity));
            self.layer_content_bounds.push(layer.content_bounds.get());
            if layer.backdrop_filter_bytes_size != 0 {
                self.backdrop_base_colors.push(layer.backdrop_base_color);
            }
        }
        fn push_mask(&mut self, mask: &ReplayMask, _backdrop: Option<&ReplayLayer>) {
            self.events.push(PainterEvent::PushMask(mask.rect));
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
        fn fill_rect(&mut self, _fill: &ResolvedFillRect) {}
        fn fill_rect_with_rounded_corners(&mut self, _fill: &ResolvedFillRectWithRoundedCorners) {}
        fn paint_scrollbar(&mut self, _scrollbar: &PresentedScrollbar) {}
        fn draw_composited_context(&mut self, _draw: &CompositedContextDraw) {}
        fn begin_isolated_group(&mut self, _group: &IsolatedGroup) {}
        fn begin_isolated_group_mask(&mut self, _kind: MaskKind) {}
        fn end_isolated_group_mask(&mut self) {}
        fn end_isolated_group(&mut self) {}
        fn begin_tile_raster(&mut self, _tile: &TileRaster) -> bool {
            false
        }
        fn end_tile_raster(&mut self) {}
        fn begin_nested_display_list(
            &mut self,
            _placement: &NestedDisplayListPlacement,
            _requires_direct_replay: bool,
        ) -> NestedDisplayListReplay {
            NestedDisplayListReplay::Skipped
        }
        fn end_nested_display_list(&mut self) {}

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
        clip_space_matrix: &dyn Fn(SpatialNodeIndex) -> Option<FloatMatrix4x4>,
        painter: &mut RecordingPainter,
    ) {
        let tree = identity_tree();
        let list = ListReplay::new(&tree);
        let frame = ReplayFrame::unscrolled(0, &NoNestedDisplayLists);
        replay_records(
            records,
            &RecordsContext {
                list: &list,
                frame: &frame,
                clip_space_matrix,
            },
            painter,
        );
    }

    fn replay(tree: &VisualContextTree, runs: &[DisplayListCommandRun], painter: &mut RecordingPainter) {
        replay_display_list(
            tree,
            &[],
            runs,
            &EffectClipPlan::new(tree, runs).unwrap(),
            Color::TRANSPARENT,
            &ReplayFrame::unscrolled(0, &NoNestedDisplayLists),
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
        let tape = vec![0u8; runs.len() * run_size as usize];
        let mut painter = RecordingPainter::new();
        replay_display_list(
            tree,
            &tape,
            runs,
            &EffectClipPlan::new(tree, runs).unwrap(),
            Color::TRANSPARENT,
            &ReplayFrame::unscrolled(0, &NoNestedDisplayLists),
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
        let spans = [
            RecordSpan {
                begin_offset: 0,
                end_offset: 8,
            },
            RecordSpan {
                begin_offset: 16,
                end_offset: 24,
            },
        ];
        let painter = replay_selection(
            &tree,
            &mut runs,
            8,
            ReplaySelection {
                record_spans: Some(&spans),
                skip_root_isolation_layer: false,
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
    fn a_span_replay_plays_the_records_of_a_run_inside_the_span() {
        let mut runs = [ClipNodeIndex::NONE, ClipNodeIndex::NONE]
            .map(|_| run(VISUAL_VIEWPORT_NODE_INDEX, ContextRef::default(), visible_bounds()));
        runs[1].context.effect = EffectNodeIndex(0);
        let mut tree = identity_tree();
        effect_under(&mut tree, effects(1.0), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let spans = [RecordSpan {
            begin_offset: 8,
            end_offset: 24,
        }];
        let painter = replay_selection(
            &tree,
            &mut runs,
            16,
            ReplaySelection {
                record_spans: Some(&spans),
                skip_root_isolation_layer: false,
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
                record_spans: None,
                skip_root_isolation_layer: true,
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
        let clip_space_matrix = translation_matrix(102.0, 203.0, 0.0);
        let mut painter = RecordingPainter::new();
        let command_space_matrix = painter.base_matrix;
        replay_test_records(
            builder.bytes(),
            &|space| (space == clip_space).then_some(clip_space_matrix),
            &mut painter,
        );
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::PushClip(clip_rect),
                PainterEvent::SetMatrix(clip_space_matrix),
                PainterEvent::PushClip(clip_rect),
                PainterEvent::SetMatrix(command_space_matrix),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(command_space_matrix),
            ]
        );
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
        let clip_space_matrix = translation_matrix(0.0, -50.0, 0.0);
        let mut painter = RecordingPainter::new();
        replay_test_records(
            builder.bytes(),
            &|space| (space == clip_space).then_some(clip_space_matrix),
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

    // A layer spanning content far apart must not be culled through a clamped conversion of its
    // bounds, whose edges would no longer enclose the visible part.
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
        // Inside the layer the painter still culls the run by its own ink; a real canvas clip has
        // grown by the blur there.
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

    // A layer's bounds are its content plus the margin it keeps for antialiasing, in its own units.
    fn with_margin(rect: FloatRect, margin: f32) -> FloatRect {
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
                Some(with_margin(FloatRect::new(-3.0, -3.0, 33.0, 18.0), 2.0)),
                Some(with_margin(FloatRect::new(0.0, 0.0, 10.0, 10.0), 2.0)),
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
            [Some(with_margin(FloatRect::new(0.0, 0.0, 40.0, 40.0), 4.0))]
        );
    }

    #[test]
    fn a_layer_whose_content_can_land_anywhere_has_no_bounds() {
        let mut tree = identity_tree();
        let tilted = tree.append_spatial(transform(rotate_y(30.0), None), VISUAL_VIEWPORT_NODE_INDEX);
        let flat_layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let tilted_layer = tree.append_effect(effects(0.5), EffectNodeIndex::NONE, tilted, ClipNodeIndex::NONE);
        let runs = [
            run(tilted, effect_context(flat_layer), IntRect::new(0, 0, 10, 10)),
            run(tilted, effect_context(tilted_layer), IntRect::new(0, 0, 10, 10)),
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

    // An absolute child that skips its parent's overflow clip records under the parent's effect
    // and the effect's output clip: the layer is entered once and the overflow clip moves in and
    // out of it around the child's run.
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

    // A box appends an effect with the widest clip a chain could take; tightening moves it back
    // down to the clips its contexts share, so the layer is entered inside them.
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

    // Two chains that differ only in their clip copies swap clips inside the shared layer.
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

    // A mask nested in a layer is entered inside its own output clip, which may sit above a clip
    // pushed inside the layer; that clip is re-pushed inside the mask and the mask exits once.
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
        // will-change: opacity keeps an effects node that pushes no layer, which is a backdrop root nonetheless.
        let will_change = effect_under(&mut tree, effects(1.0), root, ClipNodeIndex::NONE);
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
        let in_will_change = effect_under(&mut tree, backdrop(), will_change, ClipNodeIndex::NONE);
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
            &[],
            &runs,
            &EffectClipPlan::new(&tree, &runs).unwrap(),
            white,
            &ReplayFrame::unscrolled(0, &NoNestedDisplayLists),
            ReplaySelection::default(),
            &mut painter,
        );
        assert_eq!(painter.backdrop_base_colors, vec![white, Color::TRANSPARENT]);
    }

    // A background-color animation marker is an effect without canvas state: entering or leaving
    // it neither pushes nor invalidates the matrix.
    #[test]
    fn a_background_color_animation_marker_pushes_nothing() {
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

    // Culling the second of two new layers unwinds to the shared prefix, so the next run rebuilds
    // from there.
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
            &[],
            &runs,
            &EffectClipPlan::new(&tree, &runs).unwrap(),
            Color::TRANSPARENT,
            &ReplayFrame {
                scroll_offsets: &scroll_offsets,
                ..ReplayFrame::unscrolled(0, &NoNestedDisplayLists)
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

    // Plane clips are pushed right above the root isolation layer: everything under it unwinds
    // around each plane boundary, and the layer itself is entered once.
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
