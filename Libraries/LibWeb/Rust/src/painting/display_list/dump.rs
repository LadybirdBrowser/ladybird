/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::builder::{HEADER_SIZE, RecordedDisplayList, for_each_command, inline_transform_of, read_command};
use super::commands::*;
use super::dump_format::{
    DumpNames, DumpValue, dumped_record_field, path_description, push_clip_radii, push_fields, push_value,
};
use super::effect_clip_plan::EffectClipPlan;
use super::layers::{LayerPlan, LayerPlanInputs, build_layer_plan};
use super::nested_records::{
    NestedRecordsRole, for_each_command_including_nested, for_each_nested_record_span, span_bytes,
};
use super::record_fields::visit_command_fields;
use super::recorder::{RecordNotes, RecordOrigin};
use super::storage::DisplayListStorage;
use super::summary::{LiveContent, summarize};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::dump::{push_affine_transform, push_float_rect};
use crate::painting::paint_read::PaintRead;
use crate::painting::vector_image_records::display_list_holding_vector_image_records;
use crate::painting::visual_context::VisualContextTree;
use crate::painting::visual_context::VisualContextTreeDump;
use crate::painting::visual_context::dump::SlotKind;
use crate::stage::MainThread;
use libcompositing_rust::host::RecordSpan;
use libcompositing_rust::visual_context::CompositorScrollTable;
use libgfx_rust::path::Path;
use libgfx_rust::{FloatPoint, FloatRect, IntRect};
use std::collections::HashMap;
use std::ffi::c_void;
use std::fmt::Write;

crate::stage::main_thread_ffi_entries!();

/// What the display list dump prints besides what it always does.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiDisplayListDumpOptions {
    /// Names, on each record, the box and the paint phase that recorded it.
    pub owners: bool,
    /// Numbers every record, nested ones too, in tape order, and names the records of each layer by those numbers.
    pub record_ids: bool,
    /// Names visual context nodes by their indices in the tree rather than by the order the dump first prints them in.
    pub raw_node_indices: bool,
}

/// What a display list dump asks the document. The fields are private: the callbacks are reached
/// only through the methods below, which take the main thread token.
///
/// NB: The callbacks are typed as safe functions because the table only comes from the host through an export
///     whose safety contract keeps them callable, and the private fields let nothing else call them.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPaintingDumpCallbacks {
    context: *mut c_void,
    /// Describes the row in the slot, as its layout node describes itself.
    debug_description: extern "C" fn(context: *mut c_void, slot: NodeSlotId, description_sink: *mut c_void),
    append_text: extern "C" fn(context: *mut c_void, bytes: *const u8, byte_count: usize),
}

impl FfiPaintingDumpCallbacks {
    fn debug_description(&self, _: &MainThread, slot: NodeSlotId) -> String {
        let mut description = Vec::new();
        // The C++ host fills the description sink synchronously through the exported push
        // function.
        (self.debug_description)(self.context, slot, (&raw mut description).cast());
        String::from_utf8_lossy(&description).into_owned()
    }

    fn append_text(&self, _: &MainThread, text: &str) {
        // The C++ sink copies the completed dump synchronously.
        (self.append_text)(self.context, text.as_ptr(), text.len());
    }
}

struct VisualContextNodeOwners {
    spatial: HashMap<u32, NodeSlotId>,
    clip: HashMap<u32, NodeSlotId>,
    effect: HashMap<u32, NodeSlotId>,
}

impl VisualContextNodeOwners {
    fn collect(arena: &LayoutNodeArena, viewport: NodeSlotId) -> Self {
        let mut owners = Self {
            spatial: HashMap::new(),
            clip: HashMap::new(),
            effect: HashMap::new(),
        };
        owners.spatial.insert(VISUAL_VIEWPORT_NODE_INDEX.0, viewport);
        owners
            .spatial
            .insert(arena.live_paintable_data(viewport).own_scroll_node_index.0, viewport);
        let mut pending = vec![viewport];
        while let Some(slot) = pending.pop() {
            arena.with_paintable_visual_context_node_handles(slot, |handles| {
                for spatial in &handles.spatial {
                    owners.spatial.insert(spatial.0, slot);
                }
                for clip in handles.clip_handles() {
                    owners.clip.insert(clip.0, slot);
                }
                for effect in &handles.effects {
                    owners.effect.insert(effect.0, slot);
                }
            });
            pending.extend(arena.children(slot));
        }
        owners
    }

    fn owner(&self, kind: SlotKind, index: u32) -> Option<NodeSlotId> {
        match kind {
            SlotKind::Spatial => self.spatial.get(&index).copied(),
            SlotKind::Clip => self.clip.get(&index).copied(),
            SlotKind::Effect => self.effect.get(&index).copied(),
        }
    }
}

/// Hands `append_line` each animation the compositor runs for the document of `host`, in the order the tree holds
/// them: the property it animates (0 for opacity, 1 for background color, 2 for filter, 3 for transform) and the element
/// of the box owning the node it drives, or zero. Answers, in bit 0, whether the animations share one timing anchor and,
/// in bit 1, whether every one drives a live node.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `append_line` must return before the next call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_dump_compositor_animations(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    viewport: NodeSlotId,
    context: *mut c_void,
    append_line: unsafe extern "C" fn(*mut c_void, u8, u32),
) -> u8 {
    use crate::painting::host::FfiVisualAnimationTargetKind;
    let (lines, flags) = crate::painting::ffi::read_arena(host, read, viewport, |arena, viewport| {
        let Some(tree) = arena.paint_state().visual_context.tree.clone() else {
            return (Vec::new(), 3);
        };
        let owners = VisualContextNodeOwners::collect(arena, viewport);
        let lines = tree
            .visual_animations()
            .iter()
            .map(|animation| {
                let kind = match animation.target_kind {
                    FfiVisualAnimationTargetKind::Opacity => 0,
                    FfiVisualAnimationTargetKind::BackgroundColor => 1,
                    FfiVisualAnimationTargetKind::Filter => 2,
                    FfiVisualAnimationTargetKind::Transform => 3,
                };
                let slot_kind = match animation.target_kind {
                    FfiVisualAnimationTargetKind::Transform => SlotKind::Spatial,
                    _ => SlotKind::Effect,
                };
                let element = animation
                    .node_indices
                    .first()
                    .and_then(|&index| owners.owner(slot_kind, index))
                    .and_then(|slot| arena.node_style_node(slot))
                    .map_or(0, |element| element.raw());
                (kind, element)
            })
            .collect::<Vec<_>>();
        let summary = tree.visual_animation_summary();
        (
            lines,
            u8::from(summary.share_timing_anchor) | (u8::from(summary.targets_are_valid) << 1),
        )
    });
    for (kind, element) in lines {
        // SAFETY: Guaranteed by the caller.
        unsafe { append_line(context, kind, element) };
    }
    flags
}

/// # Safety
///
/// Dumps `display_list` with the nodes it names of the visual context tree of `host`'s document, prepared up to date.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread; `display_list` is the storage handle of the display
/// list, and it and every storage handle `callbacks` returns must remain live for this call. `debug_description` is
/// called synchronously with a `Vec<u8>` sink the host fills through `layout_arena_paint_push_bytes`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn painting_dump(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    viewport: NodeSlotId,
    device_viewport_rect: IntRect,
    options: FfiDisplayListDumpOptions,
    callbacks: FfiPaintingDumpCallbacks,
) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: As above.
    let visual_context_tree = unsafe { crate::painting::ffi::main_visual_context_tree(host, read) }
        .expect("a document with a committed viewport box has a visual context tree");
    let visual_context_tree = visual_context_tree.as_ref();
    let recording =
        crate::painting::ffi::read_arena(host, read, (), |arena, ()| arena.paint_state().last_recording.clone())
            .expect("the dump records the document first");
    // The dump shows the tape as the compositor replays it, with the records of the SVG images it draws.
    let display_list = DisplayListStorage::recorded(
        std::sync::Arc::new(display_list_holding_vector_image_records(
            &recording.display_list,
            &recording.resources.vector_images,
        )),
        recording
            .visual_context_tree
            .clone()
            .expect("a recording was recorded against a tree"),
    );
    let (tape, command_runs) = (display_list.bytes(), display_list.command_runs());
    let owners = {
        crate::painting::ffi::read_arena(host, read, viewport, |arena, viewport| {
            let mut owners = VisualContextNodeOwners::collect(arena, viewport);
            for owners in [&mut owners.spatial, &mut owners.clip, &mut owners.effect] {
                owners.retain(|_, &mut owner| arena.slot_is_live(owner));
            }
            owners
        })
    };
    let record_origins = options.owners.then(|| {
        let record_notes = recording.record_notes.clone().unwrap_or_default();
        record_origins_in_tape_order(&recording.display_list, &record_notes)
    });
    let owner_labels = record_origins
        .as_ref()
        .map(|origins| owner_labels(host, read, &main_thread, &callbacks, origins))
        .unwrap_or_default();
    let mut names = if options.raw_node_indices {
        DumpNames::default()
    } else {
        DumpNames::numbering_nodes_in_print_order()
    };
    let mut output = visual_context_tree.dump_nodes_reachable_from_runs(command_runs, &mut names, |kind, index| {
        let owner = owners.owner(kind, index)?;
        Some(callbacks.debug_description(&main_thread, owner))
    });
    dump_compositor_scroll_table(&mut output, &mut names, visual_context_tree.compositor_scroll_table());
    let record_ids = options
        .record_ids
        .then(|| RecordIdsOfTopLevelRecords::of(tape, command_runs));
    let layer_plan = dump_layers_if_every_canvas_and_video_kept_updating(
        &mut output,
        tape,
        command_runs,
        visual_context_tree,
        host.scroll_state().device_offsets(),
        device_viewport_rect,
        record_ids.as_ref(),
    );
    output.push_str("\nDisplayList:\n");
    let mut records = RecordDump {
        output: &mut output,
        names: &mut names,
        next_record_id: options.record_ids.then_some(1),
        record_origins: record_origins.map(|origins| RecordOriginsInDumpOrder {
            origins,
            next_index: 0,
            owner_labels,
        }),
    };
    match layer_plan {
        Some(layer_plan) => records.dump_top_level_commands_with_their_layers(&display_list, &layer_plan),
        None => records.dump_commands(&display_list),
    }
    callbacks.append_text(&main_thread, &output);
}

fn push_indent(output: &mut String, indent: usize) {
    output.extend(std::iter::repeat_n(' ', indent * 2));
}

fn css_pixel_point(point: crate::layout::used_values::FfiCssPixelPoint) -> FloatPoint {
    FloatPoint {
        x: point.x.to_float(),
        y: point.y.to_float(),
    }
}

fn css_pixel_rect(rect: crate::layout::used_values::FfiCssPixelRect) -> FloatRect {
    FloatRect::new(
        rect.x.to_float(),
        rect.y.to_float(),
        rect.width.to_float(),
        rect.height.to_float(),
    )
}

fn push_field(output: &mut String, names: &mut DumpNames, name: &str, value: DumpValue) {
    write!(output, " {name}=").unwrap();
    push_value(output, &value, names);
}

// Lists every row, unlike the tree section above it, which shows only the nodes that command runs reach.
fn dump_compositor_scroll_table(output: &mut String, names: &mut DumpNames, table: &CompositorScrollTable) {
    if table.nodes.is_empty() {
        return;
    }
    output.push_str("\nScrollNodes:\n");
    for node in table.nodes.iter() {
        output.push_str("ScrollNode");
        push_field(
            output,
            names,
            "scroll_node_index",
            DumpValue::SpatialNode(node.scroll_node_index),
        );
        push_field(
            output,
            names,
            "parent_scroll_node_index",
            DumpValue::SpatialNode(node.parent_scroll_node_index),
        );
        push_field(
            output,
            names,
            "scrollport_rect",
            DumpValue::IntRect(node.scrollport_rect),
        );
        push_field(
            output,
            names,
            "min_scroll_offset",
            DumpValue::FloatPoint(node.min_scroll_offset),
        );
        push_field(
            output,
            names,
            "max_scroll_offset",
            DumpValue::FloatPoint(node.max_scroll_offset),
        );
        writeln!(output, " is_viewport={}", node.is_viewport).unwrap();
        let Ok(container_index) = table
            .snap_containers
            .binary_search_by_key(&node.scroll_node_index, |container| container.scroll_node_index)
        else {
            continue;
        };
        let container = &table.snap_containers[container_index];
        push_indent(output, 1);
        output.push_str("SnapContainer");
        push_field(
            output,
            names,
            "snapport",
            DumpValue::FloatRect(css_pixel_rect(container.snapport)),
        );
        push_field(
            output,
            names,
            "min_scroll_offset",
            DumpValue::FloatPoint(css_pixel_point(container.min_scroll_offset)),
        );
        push_field(
            output,
            names,
            "max_scroll_offset",
            DumpValue::FloatPoint(css_pixel_point(container.max_scroll_offset)),
        );
        writeln!(
            output,
            " strictness={} snaps_x={} snaps_y={} horizontal_writing_mode={}",
            container.strictness, container.snaps_x, container.snaps_y, container.horizontal_writing_mode
        )
        .unwrap();
        let first_area_index = container.first_area_index as usize;
        for area in &table.snap_areas[first_area_index..first_area_index + container.area_count as usize] {
            push_indent(output, 2);
            output.push_str("SnapArea");
            write!(output, " pseudo_element_type={}", area.pseudo_element_type).unwrap();
            push_field(output, names, "rect", DumpValue::FloatRect(css_pixel_rect(area.rect)));
            writeln!(
                output,
                " align_x={} align_y={} always_stop={}",
                area.align_x, area.align_y, area.always_stop
            )
            .unwrap();
        }
    }
}

// A list that draws no canvas, video or nested context gets no section.
fn dump_layers_if_every_canvas_and_video_kept_updating(
    output: &mut String,
    tape: &[u8],
    command_runs: &[DisplayListCommandRun],
    tree: &VisualContextTree,
    scroll_offsets: &[FloatPoint],
    device_viewport_rect: IntRect,
    record_ids: Option<&RecordIdsOfTopLevelRecords>,
) -> Option<LayerPlan> {
    let summary = summarize(tape, command_runs);
    let mut active_media = Vec::new();
    for live_content_record in &summary.live_content_records {
        let content = live_content_record.content;
        if !matches!(content, LiveContent::ImageFrame(_)) && !active_media.contains(&content) {
            active_media.push(content);
        }
    }
    if active_media.is_empty() {
        return None;
    }
    let effect_clips =
        EffectClipPlan::new(tree, command_runs).expect("a display list's runs name live nodes of its tree");
    let layer_plan = build_layer_plan(&LayerPlanInputs {
        tape,
        command_runs,
        summary: &summary,
        effect_clips: &effect_clips,
        tree,
        scroll_offsets,
        viewport_rect: device_viewport_rect,
        active_media: &active_media,
    });
    match layer_plan {
        Ok(layer_plan) => {
            output.push_str("\nLayers:\n");
            layer_plan
                .write_layer_lines(output, "  ", |spans| {
                    record_ids.map(|record_ids| record_ids.describe_spans(spans))
                })
                .unwrap();
            Some(layer_plan)
        }
        Err(unlayered_reason) => {
            writeln!(output, "\nLayers: none ({unlayered_reason})").unwrap();
            None
        }
    }
}

/// The number of each top-level record of a tape, which counts the records nested in the ones before it too.
struct RecordIdsOfTopLevelRecords {
    ids_by_offset: HashMap<u32, u32>,
    record_count: u32,
    tape_size: u32,
}

impl RecordIdsOfTopLevelRecords {
    fn of(tape: &[u8], command_runs: &[DisplayListCommandRun]) -> Self {
        let mut ids_by_offset = HashMap::new();
        let mut record_count = 0;
        for run in command_runs {
            let run_bytes = &tape[run.offset as usize..(run.offset + run.size) as usize];
            for_each_command(run_bytes, |_, offset_in_run, payload| {
                ids_by_offset.insert(run.offset + offset_in_run as u32, record_count + 1);
                let record_bytes = &run_bytes[offset_in_run..offset_in_run + HEADER_SIZE + payload.len()];
                for_each_command_including_nested(record_bytes, &mut |_, _, _| record_count += 1);
            });
        }
        Self {
            ids_by_offset,
            record_count,
            tape_size: tape.len() as u32,
        }
    }

    fn id_at(&self, offset: u32) -> u32 {
        if offset == self.tape_size {
            return self.record_count + 1;
        }
        self.ids_by_offset[&offset]
    }

    // Spans of top-level records as the ranges of the numbers of every record they hold.
    fn describe_spans(&self, spans: &[RecordSpan]) -> String {
        let mut text = String::new();
        for (index, span) in spans.iter().enumerate() {
            if index > 0 {
                text.push(',');
            }
            let (first, last) = (self.id_at(span.begin_offset), self.id_at(span.end_offset) - 1);
            if first == last {
                write!(text, "#{first}").unwrap();
            } else {
                write!(text, "#{first}-#{last}").unwrap();
            }
        }
        text
    }
}

/// The origin of each record of a tape in tape order, nested records after the one they are nested in. A nested record
/// whose recording noted no origin, such as a record of a pattern tile, which is recorded once and copied wherever it
/// is drawn, has the origin of the record it is nested in.
fn record_origins_in_tape_order(display_list: &RecordedDisplayList, notes: &RecordNotes) -> Vec<Option<RecordOrigin>> {
    fn collect(
        bytes: &[u8],
        base_offset: u32,
        enclosing_origin: Option<RecordOrigin>,
        notes: &RecordNotes,
        origins: &mut Vec<Option<RecordOrigin>>,
    ) {
        for_each_command(bytes, |header, record_offset, payload| {
            let record_offset = base_offset + record_offset as u32;
            let origin = notes.origin_at(record_offset).or(enclosing_origin);
            origins.push(origin);
            let payload_offset = record_offset + HEADER_SIZE as u32;
            for_each_nested_record_span(header.command_type, payload, |_, span| {
                collect(
                    span_bytes(payload, span),
                    payload_offset + span.offset,
                    origin,
                    notes,
                    origins,
                );
            });
        });
    }
    let mut origins = Vec::new();
    for run in &display_list.command_runs {
        let run_bytes = &display_list.bytes[run.offset as usize..(run.offset + run.size) as usize];
        collect(run_bytes, run.offset, None, notes, &mut origins);
    }
    origins
}

/// The labels of the live boxes that recorded the records.
fn owner_labels(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    main_thread: &MainThread,
    callbacks: &FfiPaintingDumpCallbacks,
    origins: &[Option<RecordOrigin>],
) -> HashMap<NodeSlotId, String> {
    let owners: Vec<NodeSlotId> = origins
        .iter()
        .flatten()
        .map(|origin| origin.owner)
        .collect::<std::collections::HashSet<_>>()
        .into_iter()
        .collect();
    let live_owners = crate::painting::ffi::read_arena(host, read, owners, |arena, owners| {
        owners
            .into_iter()
            .filter(|&owner| owner != NodeSlotId::INVALID && arena.slot_is_live(owner))
            .collect::<Vec<_>>()
    });
    live_owners
        .into_iter()
        .map(|owner| (owner, callbacks.debug_description(main_thread, owner)))
        .collect()
}

/// The origins of the records the DisplayList section prints, which it takes in turn. The records of an SVG image,
/// which the recording names by the image's id only, are not among them.
struct RecordOriginsInDumpOrder {
    origins: Vec<Option<RecordOrigin>>,
    next_index: usize,
    owner_labels: HashMap<NodeSlotId, String>,
}

/// Where the DisplayList section is written, with the names it gives what the records name.
struct RecordDump<'a> {
    output: &'a mut String,
    names: &'a mut DumpNames,
    // The number the next record is printed with, while records are numbered.
    next_record_id: Option<u32>,
    record_origins: Option<RecordOriginsInDumpOrder>,
}

impl RecordDump<'_> {
    fn dump_commands(&mut self, display_list: &DisplayListStorage) {
        let (bytes, command_runs) = (display_list.bytes(), display_list.command_runs());
        for run in command_runs {
            let run_bytes = &bytes[run.offset as usize..(run.offset + run.size) as usize];
            self.dump_command_bytes(run_bytes, run.context, 0, None);
        }
    }

    // Each top-level command starts with the layer it draws into. The records nested in it draw into the same layer.
    fn dump_top_level_commands_with_their_layers(&mut self, display_list: &DisplayListStorage, layer_plan: &LayerPlan) {
        let (bytes, command_runs) = (display_list.bytes(), display_list.command_runs());
        for run in command_runs {
            let run_bytes = &bytes[run.offset as usize..(run.offset + run.size) as usize];
            for_each_command(run_bytes, |_, offset_in_run, payload| {
                let layer_label = format!("L{} ", layer_plan.layer_of_record(run.offset + offset_in_run as u32));
                let record_bytes = &run_bytes[offset_in_run..offset_in_run + HEADER_SIZE + payload.len()];
                let record_dump_start = self.output.len();
                self.dump_command_bytes(record_bytes, run.context, 0, None);
                let record_dump = self.output.split_off(record_dump_start);
                for (line_index, line) in record_dump.split_inclusive('\n').enumerate() {
                    if line_index == 0 {
                        self.output.push_str(&layer_label);
                    } else {
                        self.output.extend(std::iter::repeat_n(' ', layer_label.len()));
                    }
                    self.output.push_str(line);
                }
            });
        }
    }

    // The records of an SVG image take the origin of the record that draws the image.
    fn dump_command_bytes(
        &mut self,
        command_bytes: &[u8],
        context: ContextRef,
        base_indent: usize,
        origin_of_enclosing_vector_image: Option<Option<RecordOrigin>>,
    ) {
        for_each_command(command_bytes, |header, _, payload| {
            let origin = match origin_of_enclosing_vector_image {
                Some(origin) => origin,
                None => self
                    .record_origins
                    .as_mut()
                    .and_then(RecordOriginsInDumpOrder::take_next),
            };
            push_indent(self.output, base_indent);
            if let Some(record_id) = &mut self.next_record_id {
                write!(self.output, "#{record_id} ").unwrap();
                *record_id += 1;
            }
            write!(self.output, "{}@", header.command_type.name()).unwrap();
            write_context(self.output, self.names, context);
            let fields = command_fields(header.command_type, payload);
            push_fields(self.output, &fields, self.names);
            if header.inline_clip_count > 0 {
                dump_inline_clips(self.output, self.names, header, payload);
            }
            if let Some(transform) = inline_transform_of(header, payload) {
                self.output.push_str(" inline_transform=");
                push_affine_transform(self.output, transform);
            }
            if let Some(origin) = origin {
                self.push_origin(origin);
            }
            self.output.push('\n');

            self.dump_records_inside(
                header.command_type,
                payload,
                context,
                base_indent + 1,
                origin_of_enclosing_vector_image,
                origin,
            );
        });
    }

    fn dump_records_inside(
        &mut self,
        command_type: DisplayListCommandType,
        payload: &[u8],
        context: ContextRef,
        indent: usize,
        origin_of_enclosing_vector_image: Option<Option<RecordOrigin>>,
        origin: Option<RecordOrigin>,
    ) {
        for_each_nested_record_span(command_type, payload, |role, span| {
            let label = match role {
                NestedRecordsRole::IsolatedGroupMask => Some("Mask:\n"),
                NestedRecordsRole::PatternTile => Some("PatternTile:\n"),
                NestedRecordsRole::IsolatedGroupContent
                | NestedRecordsRole::RepeatedTile
                | NestedRecordsRole::MaskContent
                | NestedRecordsRole::VectorImage => None,
            };
            let mut nested_indent = indent;
            if let Some(label) = label {
                push_indent(self.output, indent);
                self.output.push_str(label);
                nested_indent += 1;
            }
            let origin_of_enclosing_vector_image = match role {
                NestedRecordsRole::VectorImage => origin_of_enclosing_vector_image.or(Some(origin)),
                _ => origin_of_enclosing_vector_image,
            };
            self.dump_command_bytes(
                span_bytes(payload, span),
                context,
                nested_indent,
                origin_of_enclosing_vector_image,
            );
        });
    }

    fn push_origin(&mut self, origin: RecordOrigin) {
        let owner_label = self
            .record_origins
            .as_ref()
            .and_then(|origins| origins.owner_labels.get(&origin.owner));
        if let Some(owner_label) = owner_label {
            write!(self.output, " owner={owner_label}").unwrap();
        }
        write!(self.output, " phase={}", origin.phase).unwrap();
    }
}

impl RecordOriginsInDumpOrder {
    fn take_next(&mut self) -> Option<RecordOrigin> {
        let origin = self.origins.get(self.next_index).copied().flatten();
        self.next_index += 1;
        origin
    }
}

fn command_fields(command_type: DisplayListCommandType, payload: &[u8]) -> Vec<(&'static str, DumpValue)> {
    let mut fields = Vec::new();
    visit_command_fields(command_type, payload, &mut |name, value, holds_neutral_value| {
        if holds_neutral_value {
            return;
        }
        if let Some(name_and_value) = dumped_record_field(name, value, payload) {
            fields.push(name_and_value);
        }
    });
    fields
}

fn dump_inline_clips(output: &mut String, names: &mut DumpNames, header: &DisplayListCommandHeader, payload: &[u8]) {
    output.push_str(" inline_clips=[");
    let count = header.inline_clip_count as usize;
    let entries_size = count * INLINE_CLIP_ENTRY_SIZE;
    assert!(entries_size <= payload.len());
    let entries_offset = payload.len() - entries_size;
    for index in 0..count {
        if index > 0 {
            output.push_str(", ");
        }
        let clip = read_command::<DisplayListInlineClip>(&payload[entries_offset + index * INLINE_CLIP_ENTRY_SIZE..]);
        if clip.kind == InlineClipKind::Path {
            let start = clip.path_data.offset as usize;
            let size = clip.path_data.size as usize;
            assert!(start <= payload.len() && size <= payload.len() - start);
            let path = Path::from_serialized_bytes(&payload[start..start + size]);
            output.push_str("clip_path=[bounds: ");
            push_float_rect(output, clip.clip_rect_or_path_device_bounds);
            write!(output, ", {}]", path_description(&path)).unwrap();
            if clip.path_winding_rule != libgfx_rust::WindingRule::default() {
                write!(output, " winding_rule={:?}", clip.path_winding_rule).unwrap();
            }
        } else {
            output.push_str("clip=");
            push_float_rect(output, clip.clip_rect_or_path_device_bounds);
            if clip.corner_radii.has_any_radius() {
                output.push_str(" radii=");
                push_clip_radii(output, clip.corner_radii);
            }
        }
        if clip.mode == ClipMode::Difference {
            output.push_str(" mode=difference");
        }
        if clip.has_own_space {
            write!(output, " in {}", names.spatial(clip.space)).unwrap();
        }
    }
    output.push(']');
}

// A context prints its spatial, clip and effect nodes.
fn write_context(output: &mut String, names: &mut DumpNames, context: ContextRef) {
    output.push_str(&names.spatial(context.spatial));
    if !context.clip.is_none() {
        write!(output, "/{}", names.clip(context.clip)).unwrap();
    }
    if !context.effect.is_none() {
        write!(output, "/{}", names.effect(context.effect)).unwrap();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::painting::display_list::builder::DisplayListBuilder;
    use crate::painting::display_list::builder::{HEADER_SIZE, read_header};
    use libgfx_rust::{Color, CompositingAndBlendingOperator, IntRect};

    #[test]
    fn command_dump_matches_the_canonical_format() {
        use crate::painting::visual_context::{
            ClipNodeData, ClipNodeIndex, EffectNodeData, EffectNodeIndex, TransformData, TransformDataRole,
        };
        let mut tree = VisualContextTree::create(TransformData {
            matrix: libgfx_rust::FloatMatrix4x4::identity(),
            origin: libgfx_rust::FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        });
        let clip = tree.append_clip(
            ClipNodeData::rect_clip(libgfx_rust::FloatRect::new(0.0, 0.0, 9.0, 9.0)),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let effect = tree.append_effect(
            EffectNodeData::BackgroundColorAnimation,
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let effect_only = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect,
            ..ContextRef::default()
        };
        let clip_and_effect = ContextRef {
            clip,
            effect,
            ..ContextRef::default()
        };
        let fill = FillRect {
            rect: IntRect::new(1, 2, 30, 40),
            color: Color::from_rgba(101, 2, 0, 204),
            compositing_and_blending_operator: CompositingAndBlendingOperator::Multiply,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let dump = |context: ContextRef| {
            let mut builder = DisplayListBuilder::new();
            builder.append(
                &fill,
                &[],
                ContextRef {
                    spatial: SpatialNodeIndex(0),
                    ..context
                },
            );
            let bytes = builder.bytes();
            let header = read_header(bytes);
            let payload = &bytes[HEADER_SIZE..HEADER_SIZE + header.payload_size as usize];
            let mut output = format!("{}@", header.command_type.name());
            let mut names = DumpNames::default();
            write_context(&mut output, &mut names, builder.command_runs()[0].context);
            push_fields(&mut output, &command_fields(header.command_type, payload), &mut names);
            output
        };
        assert_eq!(
            dump(clip_and_effect),
            "FillRect@s0/c0/e0 rect=[1,2 30x40] color=rgba(101, 2, 0, 0.8) compositing_and_blending_operator=Multiply"
        );
        assert_eq!(
            dump(effect_only),
            "FillRect@s0/e0 rect=[1,2 30x40] color=rgba(101, 2, 0, 0.8) compositing_and_blending_operator=Multiply"
        );
        assert_eq!(
            dump(ContextRef::default()),
            "FillRect@s0 rect=[1,2 30x40] color=rgba(101, 2, 0, 0.8) compositing_and_blending_operator=Multiply"
        );
    }

    #[test]
    fn record_ids_count_nested_records_and_name_spans_of_top_level_records() {
        let fill = FillRect {
            rect: IntRect::new(0, 0, 10, 10),
            color: Color::from_rgba(0, 0, 0, 255),
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let mut builder = DisplayListBuilder::new();
        builder.append(&fill, &[], ContextRef::default());
        let group = builder.begin_group::<DrawIsolatedGroup>(Vec::new());
        builder.append(&fill, &[], ContextRef::default());
        builder.append(&fill, &[], ContextRef::default());
        let group_command = DrawIsolatedGroup {
            clip_rect: libgfx_rust::FloatRect::new(0.0, 0.0, 10.0, 10.0),
            content: builder.group_content_span(&group),
            mask: DisplayListDataSpan::default(),
            filter: DisplayListDataSpan::default(),
            opacity: 0.5,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            mask_kind: libgfx_rust::MaskKind::Alpha,
        };
        builder.finish_group(group, &group_command, ContextRef::default());
        builder.append(&fill, &[], ContextRef::default());
        let (tape, runs) = (builder.bytes(), builder.command_runs());
        let record_ids = RecordIdsOfTopLevelRecords::of(tape, runs);
        let mut top_level_offsets = Vec::new();
        for_each_command(tape, |_, offset, _| top_level_offsets.push(offset as u32));
        let span = |begin: usize, end: Option<usize>| RecordSpan {
            begin_offset: top_level_offsets[begin],
            end_offset: end.map_or(tape.len() as u32, |end| top_level_offsets[end]),
        };
        assert_eq!(record_ids.describe_spans(&[span(0, Some(1))]), "#1");
        assert_eq!(record_ids.describe_spans(&[span(1, Some(2))]), "#2-#4");
        assert_eq!(record_ids.describe_spans(&[span(0, Some(1)), span(2, None)]), "#1,#5");
    }
}
