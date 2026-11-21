/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::chunks::{Chunk, ChunkId, DisplayListChunk, chunk_id, chunks_of};
use super::commands::*;
use crate::display_list::ffi_bytes::FfiBytes;
use crate::display_list::placement::enclosing_int_rect_within_extent_limit;
use libgfx_rust::path::Path;
use libgfx_rust::{AffineTransform, CornerRadii, FloatRect, IntRect, WindingRule, enclosing_int_rect};
use std::rc::Rc;

pub const COMMAND_ALIGNMENT: usize = 8;
pub const HEADER_SIZE: usize = std::mem::size_of::<DisplayListCommandHeader>();
const PATH_DATA_ALIGNMENT: usize = std::mem::align_of::<u32>();

#[derive(Clone)]
pub struct PendingInlineClip {
    kind: InlineClipKind,
    mode: ClipMode,
    clip_rect_or_path_device_bounds: FloatRect,
    corner_radii: CornerRadii,
    path_winding_rule: WindingRule,
    serialized_path_bytes: Option<Rc<Vec<u8>>>,
    narrows_clip_rect_to_the_command_bounding_rect: bool,
    space: Option<SpatialNodeIndex>,
}

impl PendingInlineClip {
    pub fn intersecting_device_rect(rect: IntRect) -> Self {
        Self {
            narrows_clip_rect_to_the_command_bounding_rect: true,
            ..Self::intersecting_float_rect(rect.to_float())
        }
    }

    pub fn intersecting_float_rect(rect: FloatRect) -> Self {
        Self {
            kind: InlineClipKind::Rect,
            mode: ClipMode::Intersect,
            clip_rect_or_path_device_bounds: rect,
            corner_radii: CornerRadii::default(),
            path_winding_rule: WindingRule::Nonzero,
            serialized_path_bytes: None,
            narrows_clip_rect_to_the_command_bounding_rect: false,
            space: None,
        }
    }

    pub fn intersecting_rounded_rect(rect: FloatRect, corner_radii: CornerRadii) -> Self {
        Self {
            kind: InlineClipKind::RoundedRect,
            corner_radii,
            ..Self::intersecting_float_rect(rect)
        }
    }

    pub fn subtracting_rect(rect: FloatRect) -> Self {
        Self {
            mode: ClipMode::Difference,
            ..Self::intersecting_float_rect(rect)
        }
    }

    pub fn subtracting_rounded_rect(rect: FloatRect, corner_radii: CornerRadii) -> Self {
        Self {
            mode: ClipMode::Difference,
            ..Self::intersecting_rounded_rect(rect, corner_radii)
        }
    }

    pub fn intersecting_path(path: &Path, path_winding_rule: WindingRule) -> Self {
        Self {
            kind: InlineClipKind::Path,
            path_winding_rule,
            serialized_path_bytes: Some(Rc::new(path.serialize_to_bytes())),
            ..Self::intersecting_float_rect(enclosing_int_rect(FloatRect::from_array(path.bounding_box())).to_float())
        }
    }

    pub fn subtracting_path(path: &Path, path_winding_rule: WindingRule) -> Self {
        Self {
            mode: ClipMode::Difference,
            ..Self::intersecting_path(path, path_winding_rule)
        }
    }

    /// Whether this clip alone keeps what it clips within `rect`, in the space of the command it is recorded on.
    pub fn lies_within(&self, rect: FloatRect) -> bool {
        self.space.is_none()
            && self.mode == ClipMode::Intersect
            && rect.contains_rect(self.clip_rect_or_path_device_bounds)
    }

    /// The same clip, applied in `space` rather than in the space of the command it ends up on, unless it already has
    /// a space of its own. A clip in a space of its own does not bound the command, as the two spaces can move apart.
    pub fn in_space(&self, space: SpatialNodeIndex) -> Self {
        Self {
            narrows_clip_rect_to_the_command_bounding_rect: false,
            space: Some(self.space.unwrap_or(space)),
            ..self.clone()
        }
    }

    fn confines_the_command(&self) -> bool {
        self.space.is_none() && self.mode == ClipMode::Intersect
    }

    fn recorded_clip_rect(&self, clip_rect_narrowed_to_the_command_bounding_rect: FloatRect) -> FloatRect {
        if self.narrows_clip_rect_to_the_command_bounding_rect {
            clip_rect_narrowed_to_the_command_bounding_rect
        } else {
            self.clip_rect_or_path_device_bounds
        }
    }
}

struct RecordBounds {
    clip_rect_narrowed_to_the_command_bounding_rect: FloatRect,
    bounding_rect: Option<IntRect>,
}

fn record_bounds<C: DisplayListCommand>(
    command: &C,
    inline_clips: &[PendingInlineClip],
    inline_transform: Option<AffineTransform>,
) -> RecordBounds {
    let command_bounding_rect = command.bounding_rect();
    let confining_clips = || inline_clips.iter().filter(|clip| clip.confines_the_command());
    let clip_rect_narrowed_to_the_command_bounding_rect = record_bounding_rect(
        command_bounding_rect,
        inline_transform,
        confining_clips().map(|clip| clip.clip_rect_or_path_device_bounds),
    )
    .unwrap_or_default()
    .to_float();
    RecordBounds {
        clip_rect_narrowed_to_the_command_bounding_rect,
        bounding_rect: record_bounding_rect(
            command_bounding_rect,
            inline_transform,
            confining_clips().map(|clip| clip.recorded_clip_rect(clip_rect_narrowed_to_the_command_bounding_rect)),
        ),
    }
}

fn rect_of_inline_clip_confining_its_command(clip: &DisplayListInlineClip) -> Option<FloatRect> {
    (!clip.has_own_space && clip.mode == ClipMode::Intersect).then_some(clip.clip_rect_or_path_device_bounds)
}

fn record_bounding_rect(
    command_bounding_rect: Option<IntRect>,
    inline_transform: Option<AffineTransform>,
    rects_of_inline_clips_confining_the_command: impl IntoIterator<Item = FloatRect>,
) -> Option<IntRect> {
    let mut bounding_rect = command_bounding_rect.map(int_rect_within_record_bounds_limit);
    if let (Some(transform), Some(rect)) = (inline_transform, bounding_rect) {
        bounding_rect = Some(enclosing_int_rect_within_extent_limit(
            transform.map_rect(rect.to_float()),
        ));
    }
    for clip_rect in rects_of_inline_clips_confining_the_command {
        let restriction = enclosing_int_rect_within_record_bounds_limit(clip_rect);
        bounding_rect = Some(match bounding_rect {
            Some(rect) => rect.intersected(restriction),
            None => restriction,
        });
    }
    bounding_rect
}

pub fn bounding_rect_computed_from_payload(header: &DisplayListCommandHeader, payload: &[u8]) -> Option<IntRect> {
    record_bounding_rect(
        command_bounding_rect_computed_from_payload(header.command_type, payload),
        inline_transform_of(header, payload),
        inline_clips_of(header, payload).filter_map(|clip| rect_of_inline_clip_confining_its_command(&clip)),
    )
}

// A finished tape together with the run table summarizing it and the chunks it is cut into.
#[derive(Clone, Default)]
pub struct RecordedDisplayList {
    pub bytes: Vec<u8>,
    pub command_runs: Vec<DisplayListCommandRun>,
    pub chunks: Vec<DisplayListChunk>,
}

impl RecordedDisplayList {
    /// The tape that holds `chunks` one after another, cut into them.
    pub fn of_chunks<'a>(chunks: impl IntoIterator<Item = &'a Chunk>) -> Self {
        let mut list = Self::default();
        for chunk in chunks {
            if chunk.records.is_empty() {
                continue;
            }
            let chunk_start = u32::try_from(list.bytes.len()).expect("display list exceeds u32");
            list.bytes.extend_from_slice(&chunk.records);
            list.command_runs
                .extend(chunk.runs.iter().map(|run| DisplayListCommandRun {
                    offset: run.offset + chunk_start,
                    ..*run
                }));
            list.chunks.push(DisplayListChunk {
                id: chunk.id,
                byte_end: u32::try_from(list.bytes.len()).expect("display list exceeds u32"),
                run_end: u32::try_from(list.command_runs.len()).expect("display list exceeds u32"),
            });
        }
        list
    }
}

fn name_unnamed_chunks(bytes: &[u8], runs: &[DisplayListCommandRun], chunks: &mut [DisplayListChunk]) {
    let mut ids = Vec::new();
    for (index, (chunk, chunk_bytes, chunk_runs)) in chunks_of(bytes, runs, chunks).enumerate() {
        if chunk.id == ChunkId::UNSET {
            let chunk_start = chunk.byte_end - chunk_bytes.len() as u32;
            ids.push((index, chunk_id(chunk_bytes, chunk_runs, chunk_start)));
        }
    }
    for (index, id) in ids {
        chunks[index].id = id;
    }
}

pub struct OpenGroup {
    record_start: usize,
    fixed_payload_size: usize,
    content_start: usize,
    mask_start: Option<usize>,
    trailing_inline_data_start: Option<usize>,
    suspended_inline_clips: Vec<PendingInlineClip>,
}

impl OpenGroup {
    fn payload_start(&self) -> usize {
        self.record_start + HEADER_SIZE
    }

    fn span_from(&self, start: usize, end: usize) -> DisplayListDataSpan {
        self.span_from_with_size(start, end - start)
    }

    fn span_from_with_size(&self, start: usize, size: usize) -> DisplayListDataSpan {
        DisplayListDataSpan {
            offset: u32::try_from(start - self.payload_start()).expect("display list payload exceeds u32"),
            size: u32::try_from(size).expect("display list payload exceeds u32"),
        }
    }
}

#[derive(Default)]
pub struct DisplayListBuilder {
    bytes: Vec<u8>,
    runs: Vec<DisplayListCommandRun>,
    // The chunks ended so far. What was appended since the last one ends is the chunk still open, whose runs are
    // the only ones a command can extend.
    chunks: Vec<DisplayListChunk>,
    open_group_depth: usize,
    open_group_context: Option<ContextRef>,
    last_record_is_alone_in_its_run: bool,
}

impl DisplayListBuilder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    pub fn command_runs(&self) -> &[DisplayListCommandRun] {
        &self.runs
    }

    pub fn chunks(&self) -> &[DisplayListChunk] {
        &self.chunks
    }

    /// Ends the tape with the chunk still open and names every chunk.
    pub fn finish(mut self) -> RecordedDisplayList {
        self.end_chunk();
        name_unnamed_chunks(&self.bytes, &self.runs, &mut self.chunks);
        RecordedDisplayList {
            bytes: self.bytes,
            command_runs: self.runs,
            chunks: self.chunks,
        }
    }

    /// Takes everything appended so far out as one chunk, named by what it holds, and starts again with nothing.
    /// What is appended next starts a run of its own.
    pub fn take_chunk(&mut self) -> Chunk {
        debug_assert_eq!(self.open_group_depth, 0, "a chunk holds whole records");
        debug_assert!(
            self.chunks.is_empty(),
            "a builder cuts its tape into chunks or takes them out, not both"
        );
        Chunk::named(std::mem::take(&mut self.bytes), std::mem::take(&mut self.runs))
    }

    /// Ends a chunk with what was appended since the last one ended, if anything was. What is appended next starts
    /// a run of its own.
    pub fn end_chunk(&mut self) {
        debug_assert_eq!(self.open_group_depth, 0, "a chunk holds whole records");
        let byte_end = u32::try_from(self.bytes.len()).expect("display list exceeds u32");
        if self.chunks.last().map_or(0, |chunk| chunk.byte_end) == byte_end {
            return;
        }
        self.chunks.push(DisplayListChunk {
            id: ChunkId::UNSET,
            byte_end,
            run_end: u32::try_from(self.runs.len()).expect("display list exceeds u32"),
        });
    }

    fn note_command(
        &mut self,
        command_type: DisplayListCommandType,
        bounding_rect: Option<IntRect>,
        context: ContextRef,
        offset: usize,
        size: usize,
    ) {
        let every_run_so_far_belongs_to_a_chunk_that_has_ended = self
            .chunks
            .last()
            .is_some_and(|chunk| chunk.run_end as usize == self.runs.len());
        let record_is_alone_in_its_run = command_type.is_alone_in_its_run();
        if every_run_so_far_belongs_to_a_chunk_that_has_ended
            || (!self.runs.is_empty() && (record_is_alone_in_its_run || self.last_record_is_alone_in_its_run))
        {
            self.runs.push(DisplayListCommandRun {
                offset: u32::try_from(offset).expect("display list exceeds u32"),
                context,
                ..Default::default()
            });
        }
        note_command(&mut self.runs, command_type, bounding_rect, context, offset, size);
        self.last_record_is_alone_in_its_run = record_is_alone_in_its_run;
    }

    pub fn byte_size(&self) -> usize {
        self.bytes.len()
    }

    pub fn append<C: DisplayListCommand>(&mut self, command: &C, inline_data: &[u8], context: ContextRef) {
        self.append_with_inline_clips(command, inline_data, context, &[]);
    }

    pub fn append_with_inline_clips<C: DisplayListCommand>(
        &mut self,
        command: &C,
        inline_data: &[u8],
        context: ContextRef,
        inline_clips: &[PendingInlineClip],
    ) {
        self.append_with_inline_state(command, inline_data, context, inline_clips, None);
    }

    pub fn append_with_inline_state<C: DisplayListCommand>(
        &mut self,
        command: &C,
        inline_data: &[u8],
        context: ContextRef,
        inline_clips: &[PendingInlineClip],
        inline_transform: Option<AffineTransform>,
    ) {
        debug_assert_eq!(self.bytes.len() % COMMAND_ALIGNMENT, 0);
        let (path_spans, unpadded_payload_size) =
            Self::place_inline_clip_paths(inline_clips, std::mem::size_of::<C>() + inline_data.len());
        let entries_size = Self::tail_entries_size(inline_clips, inline_transform);
        let padded_record_size =
            (HEADER_SIZE + unpadded_payload_size + entries_size).next_multiple_of(COMMAND_ALIGNMENT);
        let payload_size = padded_record_size - HEADER_SIZE;
        let entries_offset = payload_size - entries_size;
        debug_assert_eq!(entries_offset % COMMAND_ALIGNMENT, 0);
        let header = Self::header_for::<C>(inline_clips, inline_transform, payload_size);
        let bounds = record_bounds(command, inline_clips, inline_transform);
        let start = self.bytes.len();
        self.bytes.resize(start + padded_record_size, 0);
        header.write_ffi_bytes(&mut self.bytes[start..start + HEADER_SIZE]);
        let payload_start = start + HEADER_SIZE;
        command.write_ffi_bytes(&mut self.bytes[payload_start..payload_start + std::mem::size_of::<C>()]);
        let inline_start = payload_start + std::mem::size_of::<C>();
        self.bytes[inline_start..inline_start + inline_data.len()].copy_from_slice(inline_data);
        self.write_tail_entries(
            inline_clips,
            inline_transform,
            &path_spans,
            payload_start,
            entries_offset,
            bounds.clip_rect_narrowed_to_the_command_bounding_rect,
        );
        if self.open_group_depth == 0 {
            self.note_command(
                C::COMMAND_TYPE,
                bounds.bounding_rect,
                context,
                start,
                padded_record_size,
            );
        } else {
            self.note_group_context(context);
        }
    }

    pub fn open_group_depth(&self) -> usize {
        self.open_group_depth
    }

    pub fn begin_group<C: DisplayListCommand>(
        &mut self,
        inline_clips_applying_to_the_group_record: Vec<PendingInlineClip>,
    ) -> OpenGroup {
        const { assert!(std::mem::align_of::<C>() <= COMMAND_ALIGNMENT) };
        debug_assert_eq!(self.bytes.len() % COMMAND_ALIGNMENT, 0);
        let record_start = self.bytes.len();
        let fixed_payload_size = std::mem::size_of::<C>().next_multiple_of(COMMAND_ALIGNMENT);
        let content_start = record_start + HEADER_SIZE + fixed_payload_size;
        self.bytes.resize(content_start, 0);
        self.open_group_depth += 1;
        OpenGroup {
            record_start,
            fixed_payload_size,
            content_start,
            mask_start: None,
            trailing_inline_data_start: None,
            suspended_inline_clips: inline_clips_applying_to_the_group_record,
        }
    }

    pub fn begin_group_mask(&mut self, group: &mut OpenGroup) {
        debug_assert!(group.mask_start.is_none());
        debug_assert_eq!(self.bytes.len() % COMMAND_ALIGNMENT, 0);
        group.mask_start = Some(self.bytes.len());
    }

    fn group_nested_records_end(&self, group: &OpenGroup) -> usize {
        group.trailing_inline_data_start.unwrap_or(self.bytes.len())
    }

    pub fn group_content_span(&self, group: &OpenGroup) -> DisplayListDataSpan {
        group.span_from(
            group.content_start,
            group.mask_start.unwrap_or(self.group_nested_records_end(group)),
        )
    }

    pub fn group_mask_span(&self, group: &OpenGroup) -> DisplayListDataSpan {
        match group.mask_start {
            Some(mask_start) => group.span_from(mask_start, self.group_nested_records_end(group)),
            None => DisplayListDataSpan::default(),
        }
    }

    pub fn append_group_inline_data(&mut self, group: &mut OpenGroup, bytes: &[u8]) -> DisplayListDataSpan {
        debug_assert!(self.open_group_depth > 0);
        let start = self.bytes.len();
        group.trailing_inline_data_start.get_or_insert(start);
        let padded_end = (start + bytes.len()).next_multiple_of(COMMAND_ALIGNMENT);
        self.bytes.extend_from_slice(bytes);
        self.bytes.resize(padded_end, 0);
        group.span_from_with_size(start, bytes.len())
    }

    pub fn finish_group_clipped_to<C: DisplayListCommand>(
        &mut self,
        mut group: OpenGroup,
        command: &C,
        context: ContextRef,
        clip: IntRect,
    ) {
        group
            .suspended_inline_clips
            .push(PendingInlineClip::intersecting_device_rect(clip));
        self.finish_group(group, command, context);
    }

    pub fn finish_group<C: DisplayListCommand>(&mut self, group: OpenGroup, command: &C, context: ContextRef) {
        debug_assert!(self.open_group_depth > 0);
        debug_assert_eq!(self.bytes.len() % COMMAND_ALIGNMENT, 0);
        assert!(
            std::mem::size_of::<C>() <= group.fixed_payload_size,
            "a group must be finished with the command type it was begun with"
        );
        self.note_group_context(context);
        self.open_group_depth -= 1;
        let payload_end = self.bytes.len();
        let payload_start = group.payload_start();
        let inline_clips = group.suspended_inline_clips;
        let (path_spans, unpadded_payload_size) =
            Self::place_inline_clip_paths(&inline_clips, payload_end - payload_start);
        let entries_size = Self::tail_entries_size(&inline_clips, None);
        let padded_record_size =
            (HEADER_SIZE + unpadded_payload_size + entries_size).next_multiple_of(COMMAND_ALIGNMENT);
        let payload_size = padded_record_size - HEADER_SIZE;
        let entries_offset = payload_size - entries_size;
        let header = Self::header_for::<C>(&inline_clips, None, payload_size);
        let bounds = record_bounds(command, &inline_clips, None);
        self.bytes.resize(group.record_start + padded_record_size, 0);
        header.write_ffi_bytes(&mut self.bytes[group.record_start..group.record_start + HEADER_SIZE]);
        command.write_ffi_bytes(&mut self.bytes[payload_start..payload_start + std::mem::size_of::<C>()]);
        self.write_tail_entries(
            &inline_clips,
            None,
            &path_spans,
            payload_start,
            entries_offset,
            bounds.clip_rect_narrowed_to_the_command_bounding_rect,
        );
        if self.open_group_depth == 0 {
            self.note_command(
                C::COMMAND_TYPE,
                bounds.bounding_rect,
                context,
                group.record_start,
                padded_record_size,
            );
            self.open_group_context = None;
        }
    }

    fn note_group_context(&mut self, context: ContextRef) {
        if let Some(group_context) = self.open_group_context {
            debug_assert_eq!(group_context, context, "a group's nested records share its context");
        } else {
            self.open_group_context = Some(context);
        }
    }

    fn place_inline_clip_paths(
        inline_clips: &[PendingInlineClip],
        mut unpadded_payload_size: usize,
    ) -> (Vec<DisplayListDataSpan>, usize) {
        let mut path_spans = vec![DisplayListDataSpan::default(); inline_clips.len()];
        for (index, clip) in inline_clips.iter().enumerate() {
            let Some(path_bytes) = &clip.serialized_path_bytes else {
                continue;
            };
            let path_offset = unpadded_payload_size.next_multiple_of(PATH_DATA_ALIGNMENT);
            path_spans[index] = DisplayListDataSpan {
                offset: u32::try_from(path_offset).expect("display list payload exceeds u32"),
                size: u32::try_from(path_bytes.len()).expect("display list payload exceeds u32"),
            };
            unpadded_payload_size = path_offset + path_bytes.len();
        }
        (path_spans, unpadded_payload_size)
    }

    fn tail_entries_size(inline_clips: &[PendingInlineClip], inline_transform: Option<AffineTransform>) -> usize {
        inline_clips.len() * INLINE_CLIP_ENTRY_SIZE
            + if inline_transform.is_some() {
                INLINE_TRANSFORM_ENTRY_SIZE
            } else {
                0
            }
    }

    fn header_for<C: DisplayListCommand>(
        inline_clips: &[PendingInlineClip],
        inline_transform: Option<AffineTransform>,
        payload_size: usize,
    ) -> DisplayListCommandHeader {
        const { assert!(std::mem::align_of::<C>() <= COMMAND_ALIGNMENT) };
        DisplayListCommandHeader {
            command_type: C::COMMAND_TYPE,
            inline_clip_count: u8::try_from(inline_clips.len()).expect("too many inline clips on one command"),
            has_inline_transform: inline_transform.is_some(),
            payload_size: u32::try_from(payload_size).expect("display list payload exceeds u32"),
        }
    }

    fn write_tail_entries(
        &mut self,
        inline_clips: &[PendingInlineClip],
        inline_transform: Option<AffineTransform>,
        path_spans: &[DisplayListDataSpan],
        payload_start: usize,
        entries_offset: usize,
        clip_rect_narrowed_to_the_command_bounding_rect: FloatRect,
    ) {
        let mut entries_offset = entries_offset;
        if let Some(transform) = inline_transform {
            let entry_start = payload_start + entries_offset;
            DisplayListInlineTransform {
                transform,
                padding: [0; 2],
            }
            .write_ffi_bytes(&mut self.bytes[entry_start..entry_start + INLINE_TRANSFORM_ENTRY_SIZE]);
            entries_offset += INLINE_TRANSFORM_ENTRY_SIZE;
        }
        for (index, clip) in inline_clips.iter().enumerate() {
            if let Some(path_bytes) = &clip.serialized_path_bytes {
                let path_start = payload_start + path_spans[index].offset as usize;
                self.bytes[path_start..path_start + path_bytes.len()].copy_from_slice(path_bytes);
            }
            let entry = DisplayListInlineClip {
                clip_rect_or_path_device_bounds: clip
                    .recorded_clip_rect(clip_rect_narrowed_to_the_command_bounding_rect),
                corner_radii: clip.corner_radii,
                path_data: path_spans[index],
                path_winding_rule: clip.path_winding_rule,
                kind: clip.kind,
                mode: clip.mode,
                has_own_space: clip.space.is_some(),
                space: clip.space.unwrap_or_default(),
                padding: 0,
            };
            let entry_start = payload_start + entries_offset + index * INLINE_CLIP_ENTRY_SIZE;
            entry.write_ffi_bytes(&mut self.bytes[entry_start..entry_start + INLINE_CLIP_ENTRY_SIZE]);
        }
    }
}

/// Appends one record, with neither inline clips nor an inline transform, to a tape without a run table, which the
/// tape of a 2D canvas is.
pub fn append_record<C: DisplayListCommand>(bytes: &mut Vec<u8>, command: &C, inline_data: &[u8]) {
    debug_assert_eq!(bytes.len() % COMMAND_ALIGNMENT, 0);
    let padded_record_size =
        (HEADER_SIZE + std::mem::size_of::<C>() + inline_data.len()).next_multiple_of(COMMAND_ALIGNMENT);
    let header = DisplayListBuilder::header_for::<C>(&[], None, padded_record_size - HEADER_SIZE);
    let start = bytes.len();
    bytes.resize(start + padded_record_size, 0);
    header.write_ffi_bytes(&mut bytes[start..start + HEADER_SIZE]);
    let payload_start = start + HEADER_SIZE;
    command.write_ffi_bytes(&mut bytes[payload_start..payload_start + std::mem::size_of::<C>()]);
    let inline_start = payload_start + std::mem::size_of::<C>();
    bytes[inline_start..inline_start + inline_data.len()].copy_from_slice(inline_data);
}

pub(crate) fn note_command(
    runs: &mut Vec<DisplayListCommandRun>,
    command_type: DisplayListCommandType,
    bounding_rect: Option<IntRect>,
    context: ContextRef,
    offset: usize,
    record_size: usize,
) {
    let offset = u32::try_from(offset).expect("display list exceeds u32");
    let record_size = u32::try_from(record_size).expect("display list record exceeds u32");
    if runs.last().is_none_or(|run| run.context != context) {
        runs.push(DisplayListCommandRun {
            offset,
            context,
            ..Default::default()
        });
    }
    let run = runs.last_mut().expect("a run was pushed above");
    debug_assert_eq!(run.offset + run.size, offset);
    run.size += record_size;
    if command_type.is_compositor_metadata() {
        run.has_compositor_metadata = true;
    } else {
        debug_assert!(bounding_rect.is_some(), "a draw command must report its bounding rect");
        run.ink_bounds = run.ink_bounds.united(bounding_rect.unwrap_or_default());
    }
}

pub fn for_each_command<'a>(bytes: &'a [u8], mut f: impl FnMut(&DisplayListCommandHeader, usize, &'a [u8])) {
    let mut offset = 0;
    while offset < bytes.len() {
        let header = read_header(&bytes[offset..]);
        let payload_start = offset + HEADER_SIZE;
        let payload_end = payload_start + header.payload_size as usize;
        f(&header, offset, &bytes[payload_start..payload_end]);
        offset = payload_end;
    }
    assert_eq!(
        offset,
        bytes.len(),
        "display list byte range does not end on a record boundary"
    );
}

pub fn read_command<C: Copy>(payload: &[u8]) -> C {
    assert!(payload.len() >= std::mem::size_of::<C>());
    // SAFETY: Display-list records are native-layout copies of these `Copy` command structs. A
    // tape is either recorded by this crate or checked by `validate::validate_tape` when it
    // arrives from another process, so every enum and bool byte holds a valid value.
    // `read_unaligned` does not require the payload pointer to have `C`'s alignment.
    unsafe { std::ptr::read_unaligned(payload.as_ptr().cast::<C>()) }
}

pub fn inline_transform_entry_offset(header: &DisplayListCommandHeader, payload: &[u8]) -> Option<usize> {
    if !header.has_inline_transform {
        return None;
    }
    Some(payload.len() - header.inline_clip_count as usize * INLINE_CLIP_ENTRY_SIZE - INLINE_TRANSFORM_ENTRY_SIZE)
}

pub fn inline_transform_of(header: &DisplayListCommandHeader, payload: &[u8]) -> Option<AffineTransform> {
    inline_transform_entry_offset(header, payload)
        .map(|offset| read_command::<DisplayListInlineTransform>(&payload[offset..]).transform)
}

/// The inline clip entries at the end of a record's payload, in the order the player pushes them.
pub fn inline_clips_of<'a>(
    header: &DisplayListCommandHeader,
    payload: &'a [u8],
) -> impl Iterator<Item = DisplayListInlineClip> + use<'a> {
    let count = usize::from(header.inline_clip_count);
    let entries = &payload[payload.len() - count * INLINE_CLIP_ENTRY_SIZE..];
    entries
        .as_chunks::<INLINE_CLIP_ENTRY_SIZE>()
        .0
        .iter()
        .map(|entry| read_command::<DisplayListInlineClip>(entry))
}

/// Whether one of the inline clips of a record applies in a space of its own, read without decoding the clips.
pub fn has_inline_clip_in_its_own_space(header: &DisplayListCommandHeader, payload: &[u8]) -> bool {
    let count = usize::from(header.inline_clip_count);
    let entries = &payload[payload.len() - count * INLINE_CLIP_ENTRY_SIZE..];
    entries
        .as_chunks::<INLINE_CLIP_ENTRY_SIZE>()
        .0
        .iter()
        .any(|entry| entry[std::mem::offset_of!(DisplayListInlineClip, has_own_space)] != 0)
}

pub fn read_header(bytes: &[u8]) -> DisplayListCommandHeader {
    assert!(bytes.len() >= HEADER_SIZE);
    let cursor = HeaderReader { bytes };
    DisplayListCommandHeader {
        command_type: DisplayListCommandType::from_u8(
            cursor.u8_at(std::mem::offset_of!(DisplayListCommandHeader, command_type)),
        )
        .expect("invalid display list command type"),
        inline_clip_count: cursor.u8_at(std::mem::offset_of!(DisplayListCommandHeader, inline_clip_count)),
        has_inline_transform: cursor.bool_at(std::mem::offset_of!(DisplayListCommandHeader, has_inline_transform)),
        payload_size: cursor.u32_at(std::mem::offset_of!(DisplayListCommandHeader, payload_size)),
    }
}

struct HeaderReader<'a> {
    bytes: &'a [u8],
}

impl HeaderReader<'_> {
    fn u8_at(&self, offset: usize) -> u8 {
        self.bytes[offset]
    }
    fn bool_at(&self, offset: usize) -> bool {
        self.bytes[offset] != 0
    }
    fn u32_at(&self, offset: usize) -> u32 {
        u32::from_ne_bytes(self.bytes[offset..offset + 4].try_into().unwrap())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libgfx_rust::{Color, CompositingAndBlendingOperator, FloatRect, IntRect};

    fn context(spatial: u32, effect: Option<u32>) -> ContextRef {
        ContextRef {
            spatial: SpatialNodeIndex(spatial),
            clip: ClipNodeIndex::NONE,
            effect: effect.map_or(EffectNodeIndex::NONE, EffectNodeIndex),
        }
    }

    fn fill_rect(x: i32, y: i32, width: i32, height: i32) -> FillRect {
        FillRect {
            rect: IntRect::new(x, y, width, height),
            color: Color::default(),
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        }
    }

    fn assert_runs_cover_tape(builder: &DisplayListBuilder) {
        let mut expected_offset = 0;
        for run in builder.command_runs() {
            assert_eq!(run.offset, expected_offset);
            assert_ne!(run.size, 0);
            assert_eq!(run.size as usize % COMMAND_ALIGNMENT, 0);
            expected_offset += run.size;
        }
        assert_eq!(expected_offset as usize, builder.byte_size());
    }

    #[test]
    fn consecutive_commands_in_one_context_share_a_run() {
        let mut builder = DisplayListBuilder::new();
        let root = context(0, None);
        builder.append(&fill_rect(0, 0, 10, 10), &[], root);
        builder.append(&fill_rect(20, 20, 10, 10), &[], root);
        assert_runs_cover_tape(&builder);
        let runs = builder.command_runs();
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].context, root);
        assert_eq!(runs[0].ink_bounds, IntRect::new(0, 0, 30, 30));
        assert!(!runs[0].has_compositor_metadata);
    }

    #[test]
    fn command_bytes_are_independent_of_the_run_context() {
        let mut a = DisplayListBuilder::new();
        let mut b = DisplayListBuilder::new();
        let command = fill_rect(10, 20, 30, 40);
        a.append(&command, &[], context(1, None));
        b.append(
            &command,
            &[],
            ContextRef {
                spatial: SpatialNodeIndex(2),
                clip: ClipNodeIndex(3),
                effect: EffectNodeIndex(4),
            },
        );
        assert_eq!(a.bytes(), b.bytes());
        assert_ne!(a.command_runs()[0].context, b.command_runs()[0].context);
    }

    #[test]
    fn context_changes_start_new_runs() {
        let mut builder = DisplayListBuilder::new();
        let a = context(1, None);
        let b = context(1, Some(0));
        builder.append(&fill_rect(0, 0, 10, 10), &[], a);
        builder.append(&fill_rect(0, 0, 10, 10), &[], b);
        builder.append(&fill_rect(0, 0, 10, 10), &[], b);
        builder.append(&fill_rect(0, 0, 10, 10), &[], a);
        assert_runs_cover_tape(&builder);
        let contexts: Vec<_> = builder.command_runs().iter().map(|run| run.context).collect();
        assert_eq!(contexts, vec![a, b, a]);
    }

    #[test]
    fn ink_bounds_skip_metadata() {
        let mut builder = DisplayListBuilder::new();
        let root = context(0, None);
        builder.append(&fill_rect(5, 5, 10, 10), &[], root);
        let metadata = CompositorBlockingWheelEventRegion {
            rect: FloatRect::new(0.0, 0.0, 900.0, 900.0),
        };
        builder.append(&metadata, &[], root);
        let run = builder.command_runs()[0];
        assert_eq!(run.ink_bounds, IntRect::new(5, 5, 10, 10));
        assert!(run.has_compositor_metadata);
    }

    fn inline_clip_entries(builder: &DisplayListBuilder, record_offset: usize) -> Vec<DisplayListInlineClip> {
        let header = read_header(&builder.bytes()[record_offset..]);
        let payload =
            &builder.bytes()[record_offset + HEADER_SIZE..record_offset + HEADER_SIZE + header.payload_size as usize];
        let count = header.inline_clip_count as usize;
        let entries_offset = payload.len() - count * INLINE_CLIP_ENTRY_SIZE;
        (0..count)
            .map(|index| {
                let entry_bytes = &payload[entries_offset + index * INLINE_CLIP_ENTRY_SIZE..];
                let reader = HeaderReader { bytes: entry_bytes };
                DisplayListInlineClip {
                    clip_rect_or_path_device_bounds: FloatRect {
                        x: f32::from_ne_bytes(entry_bytes[0..4].try_into().unwrap()),
                        y: f32::from_ne_bytes(entry_bytes[4..8].try_into().unwrap()),
                        width: f32::from_ne_bytes(entry_bytes[8..12].try_into().unwrap()),
                        height: f32::from_ne_bytes(entry_bytes[12..16].try_into().unwrap()),
                    },
                    corner_radii: CornerRadii::default(),
                    path_data: DisplayListDataSpan {
                        offset: reader.u32_at(std::mem::offset_of!(DisplayListInlineClip, path_data)),
                        size: reader.u32_at(std::mem::offset_of!(DisplayListInlineClip, path_data) + 4),
                    },
                    path_winding_rule: WindingRule::Nonzero,
                    kind: match reader.u8_at(std::mem::offset_of!(DisplayListInlineClip, kind)) {
                        0 => InlineClipKind::Rect,
                        1 => InlineClipKind::RoundedRect,
                        _ => InlineClipKind::Path,
                    },
                    mode: if reader.u8_at(std::mem::offset_of!(DisplayListInlineClip, mode)) == 0 {
                        ClipMode::Intersect
                    } else {
                        ClipMode::Difference
                    },
                    has_own_space: reader.u8_at(std::mem::offset_of!(DisplayListInlineClip, has_own_space)) != 0,
                    space: SpatialNodeIndex(reader.u32_at(std::mem::offset_of!(DisplayListInlineClip, space))),
                    padding: 0,
                }
            })
            .collect()
    }

    fn bounding_rect_of_record_at(builder: &DisplayListBuilder, record_offset: usize) -> Option<IntRect> {
        let header = read_header(&builder.bytes()[record_offset..]);
        let payload_start = record_offset + HEADER_SIZE;
        bounding_rect_computed_from_payload(
            &header,
            &builder.bytes()[payload_start..payload_start + header.payload_size as usize],
        )
    }

    // A list from another process can record a command through an inline transform that maps it past what an
    // `IntRect` holds.
    #[test]
    fn a_bounding_rect_mapped_through_an_inline_transform_is_kept_within_the_extent_limit() {
        let mut builder = DisplayListBuilder::new();
        let scale = 1e9;
        let maps_fill_far_past_every_edge = AffineTransform::new(scale, 0.0, 0.0, scale, -50.0 * scale, -50.0 * scale);
        builder.append_with_inline_state(
            &fill_rect(0, 0, 100, 100),
            &[],
            context(0, None),
            &[],
            Some(maps_fill_far_past_every_edge),
        );
        let bounding_rect = bounding_rect_of_record_at(&builder, 0).expect("the fill has a bounding rect");
        assert!(bounding_rect.contains_rect(IntRect::new(0, 0, 800, 600)));
        assert!(crate::display_list::placement::extent_limit_rect().contains_rect(bounding_rect.to_float()));
    }

    #[test]
    fn a_confining_clip_narrows_the_bounding_rect_and_appends_a_tail_entry() {
        let mut builder = DisplayListBuilder::new();
        let root = context(0, None);
        builder.append_with_inline_clips(
            &fill_rect(0, 0, 100, 100),
            &[],
            root,
            &[PendingInlineClip::intersecting_device_rect(IntRect::new(
                10, 10, 20, 20,
            ))],
        );
        builder.append(&fill_rect(50, 50, 10, 10), &[], root);
        let header = read_header(builder.bytes());
        assert_eq!(header.inline_clip_count, 1);
        assert_eq!(
            bounding_rect_of_record_at(&builder, 0),
            Some(IntRect::new(10, 10, 20, 20))
        );
        let entries = inline_clip_entries(&builder, 0);
        assert_eq!(entries[0].kind, InlineClipKind::Rect);
        assert_eq!(entries[0].mode, ClipMode::Intersect);
        assert_eq!(
            entries[0].clip_rect_or_path_device_bounds,
            FloatRect::new(10.0, 10.0, 20.0, 20.0)
        );
        let runs = builder.command_runs();
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].ink_bounds, IntRect::new(10, 10, 50, 50));
    }

    #[test]
    fn a_difference_entry_keeps_the_command_bounds_and_serializes_its_own_rect() {
        let mut builder = DisplayListBuilder::new();
        let root = context(0, None);
        builder.append_with_inline_clips(
            &fill_rect(0, 0, 100, 100),
            &[],
            root,
            &[
                PendingInlineClip::intersecting_float_rect(FloatRect::new(5.0, 5.0, 200.0, 200.0)),
                PendingInlineClip::subtracting_rect(FloatRect::new(20.0, 20.0, 10.0, 10.0)),
            ],
        );
        let header = read_header(builder.bytes());
        assert_eq!(header.inline_clip_count, 2);
        assert_eq!(
            bounding_rect_of_record_at(&builder, 0),
            Some(IntRect::new(5, 5, 95, 95))
        );
        let entries = inline_clip_entries(&builder, 0);
        assert_eq!(
            entries[0].clip_rect_or_path_device_bounds,
            FloatRect::new(5.0, 5.0, 200.0, 200.0)
        );
        assert_eq!(entries[1].mode, ClipMode::Difference);
        assert_eq!(
            entries[1].clip_rect_or_path_device_bounds,
            FloatRect::new(20.0, 20.0, 10.0, 10.0)
        );
    }

    #[test]
    fn a_chunk_keeps_its_records_byte_identical_wherever_a_tape_holds_it() {
        let mut builder = DisplayListBuilder::new();
        let recorded = context(4, None);
        builder.append_with_inline_clips(
            &fill_rect(0, 0, 100, 100),
            &[1, 2, 3],
            recorded,
            &[PendingInlineClip::intersecting_device_rect(IntRect::new(
                10, 10, 20, 20,
            ))],
        );
        let chunk = builder.take_chunk();
        let mut before = DisplayListBuilder::new();
        before.append(&fill_rect(0, 0, 1, 1), &[], context(1, None));
        let before = before.take_chunk();

        let tape = RecordedDisplayList::of_chunks([&before, &chunk]);
        let chunk_start = tape.chunks[0].byte_end as usize;
        assert_eq!(&tape.bytes[chunk_start..], &chunk.records[..]);
        assert_eq!(tape.command_runs[1].offset as usize, chunk_start);
        assert_eq!(tape.command_runs[1].context, recorded);
        assert_eq!(read_header(&tape.bytes[chunk_start..]).inline_clip_count, 1);
        assert_eq!(tape.chunks[1].id, chunk.id);
    }

    #[test]
    fn a_chunk_boundary_separates_runs_in_one_context() {
        let mut builder = DisplayListBuilder::new();
        let a = context(2, None);
        builder.append(&fill_rect(0, 0, 10, 10), &[], a);
        builder.append(&fill_rect(20, 0, 10, 10), &[], a);
        let first = builder.take_chunk();
        // Taking a chunk that holds nothing gives an empty one, which a tape leaves out.
        let empty = builder.take_chunk();
        builder.append(&fill_rect(40, 0, 10, 10), &[], a);
        builder.append(&fill_rect(60, 0, 10, 10), &[], a);
        assert_runs_cover_tape(&builder);
        let second = builder.take_chunk();
        assert!(empty.records.is_empty() && empty.runs.is_empty());
        assert_eq!(first.runs.len(), 1);
        assert_eq!(second.runs[0].offset, 0);
        let tape = RecordedDisplayList::of_chunks([&first, &empty, &second]);
        assert_eq!(tape.command_runs.len(), 2);
        assert_eq!(tape.chunks.len(), 2);
        assert_eq!(tape.chunks[0].run_end, 1);
        assert_eq!(tape.chunks[0].byte_end, tape.command_runs[1].offset);
        assert_eq!(tape.chunks[1].run_end, 2);
        assert_eq!(tape.chunks[1].byte_end as usize, tape.bytes.len());
        assert_eq!(tape.command_runs[0].ink_bounds, IntRect::new(0, 0, 30, 10));
        assert_eq!(tape.command_runs[1].ink_bounds, IntRect::new(40, 0, 30, 10));
    }

    #[test]
    fn a_tape_of_taken_chunks_is_the_tape_that_cutting_one_recording_gives() {
        // One chunk for each row: rects in two contexts, where the first context of a row is the last of the row
        // before it, and a metadata command in the third row.
        let record_row = |builder: &mut DisplayListBuilder, row: usize| {
            let y = row as i32 * 10;
            let (first_context, second_context) = if row.is_multiple_of(2) { (1, 2) } else { (2, 1) };
            builder.append(&fill_rect(0, y, 10, 10), &[], context(first_context, None));
            builder.append(&fill_rect(20, y, 10, 10), &[], context(second_context, Some(0)));
            if row == 2 {
                builder.append(
                    &CompositorBlockingWheelEventRegion {
                        rect: FloatRect::new(0.0, 0.0, 500.0, 500.0),
                    },
                    &[],
                    context(second_context, Some(0)),
                );
            }
        };
        let row_count = 5;
        let mut taken = DisplayListBuilder::new();
        let chunks: Vec<Chunk> = (0..row_count)
            .map(|row| {
                record_row(&mut taken, row);
                taken.take_chunk()
            })
            .collect();
        let mut cut = DisplayListBuilder::new();
        for row in 0..row_count {
            record_row(&mut cut, row);
            cut.end_chunk();
        }
        let cut = cut.finish();

        let of_chunks = RecordedDisplayList::of_chunks(&chunks);
        assert_eq!(of_chunks.bytes, cut.bytes);
        assert_eq!(of_chunks.command_runs, cut.command_runs);
        assert_eq!(of_chunks.chunks, cut.chunks);
        // A chunk is named by what it holds, wherever a tape holds it.
        let reversed = RecordedDisplayList::of_chunks(chunks.iter().rev());
        assert_eq!(reversed.chunks.last().unwrap().id, of_chunks.chunks[0].id);
    }
}
