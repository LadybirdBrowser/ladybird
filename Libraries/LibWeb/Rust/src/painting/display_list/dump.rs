/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::builder::{
    HEADER_SIZE, RecordedDisplayList, for_each_command, inline_clips_of, inline_transform_of, read_command,
};
use super::chunks::ChunkId;
use super::commands::*;
use super::dump_format::{
    DumpNames, DumpValue, dumped_record_field, json_corner_radii, json_fields, json_float, json_float_point,
    json_float_rect, json_int_rect, json_name, path_description, push_clip_radii, push_fields, push_value,
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
use crate::painting::visual_context::dump::{DumpedNode, SlotKind, tree_section_text};
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
    /// Prints, on each glyph run, the text it draws.
    pub text: bool,
    /// Prints, on each glyph run, the family, size and weight of its font.
    pub fonts: bool,
    /// Numbers every record, nested ones too, in tape order, and names the records of each layer by those numbers.
    pub record_ids: bool,
    /// Marks where each chunk of the tape starts, with its id, its record count and its size.
    pub chunks: bool,
    /// Names visual context nodes by their indices in the tree rather than by the order the dump first prints them in.
    pub raw_node_indices: bool,
    /// Writes one JSON object per line instead of text, with everything the other options add.
    pub jsonl: bool,
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
    /// Describes the font, a `Gfx::Font`, by its family, pixel size and weight.
    describe_font: extern "C" fn(context: *mut c_void, font: *const c_void, description_sink: *mut c_void),
}

impl FfiPaintingDumpCallbacks {
    fn debug_description(&self, _: &MainThread, slot: NodeSlotId) -> String {
        let mut description = Vec::new();
        // The C++ host fills the description sink synchronously through the exported push
        // function.
        (self.debug_description)(self.context, slot, (&raw mut description).cast());
        String::from_utf8_lossy(&description).into_owned()
    }

    fn describe_font(&self, _: &MainThread, font: &libgfx_rust::font::FontHandle) -> String {
        let mut description = Vec::new();
        // The C++ host fills the description sink synchronously through the exported push function, and the handle
        // keeps the font alive for the call.
        (self.describe_font)(self.context, font.as_raw(), (&raw mut description).cast());
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
    let options = if options.jsonl {
        FfiDisplayListDumpOptions {
            owners: true,
            text: true,
            fonts: true,
            record_ids: true,
            chunks: true,
            ..options
        }
    } else {
        options
    };
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
    let record_notes = (options.owners || options.text).then(|| {
        let record_notes = recording.record_notes.clone().unwrap_or_default();
        record_notes_in_tape_order(&recording.display_list, &record_notes)
    });
    let owner_labels = record_notes
        .as_ref()
        .filter(|_| options.owners)
        .map(|notes| owner_labels(host, read, &main_thread, &callbacks, notes))
        .unwrap_or_default();
    let font_descriptions = options.fonts.then(|| {
        recording
            .resources
            .with_vector_image_resources()
            .fonts
            .iter()
            .map(|(id, font)| (*id, callbacks.describe_font(&main_thread, font)))
            .collect::<HashMap<_, _>>()
    });
    let mut names = if options.raw_node_indices {
        DumpNames::default()
    } else {
        DumpNames::numbering_nodes_in_print_order()
    };
    let record_ids = options
        .record_ids
        .then(|| RecordIdsOfTopLevelRecords::of(tape, command_runs));
    let nodes = visual_context_tree.reachable_nodes_in_print_order(command_runs, &mut names, |kind, index| {
        let owner = owners.owner(kind, index)?;
        Some(callbacks.debug_description(&main_thread, owner))
    });
    let mut output = String::new();
    if options.jsonl {
        push_json_line(
            &mut output,
            serde_json::json!({
                "type": "header",
                "format": "ladybird-display-list-dump",
                "version": 1,
                "viewport": json_int_rect(device_viewport_rect),
                "node_names": if options.raw_node_indices { "raw-indices" } else { "print-order" },
                "record_count": record_ids.as_ref().map_or(0, |record_ids| record_ids.record_count),
            }),
        );
        for node in &nodes {
            push_json_line(&mut output, json_node(node));
        }
    } else {
        output = tree_section_text(&nodes);
    }
    dump_compositor_scroll_table(
        &mut output,
        &mut names,
        visual_context_tree.compositor_scroll_table(),
        options.jsonl,
    );
    let layer_plan = dump_layers_if_list_draws_canvas_video_or_nested_context(
        &mut output,
        &display_list,
        visual_context_tree,
        host.scroll_state().device_offsets(),
        device_viewport_rect,
        record_ids.as_ref(),
        options.jsonl,
    );
    if !options.jsonl {
        output.push_str("\nDisplayList:\n");
    }
    let mut records = RecordDump {
        output: &mut output,
        names: &mut names,
        jsonl: options.jsonl,
        prints_record_ids: options.record_ids,
        record_count: 0,
        record_notes: record_notes.map(|notes| RecordNotesInDumpOrder {
            notes,
            next_index: 0,
            owner_labels,
            prints_owners: options.owners,
            prints_glyph_run_texts: options.text,
        }),
        font_descriptions,
        chunk_starts: if options.chunks {
            ChunkStart::of_tape(&display_list)
        } else {
            HashMap::new()
        },
        current_chunk: None,
    };
    match layer_plan {
        Some(layer_plan) => records.dump_top_level_commands_with_their_layers(&display_list, &layer_plan),
        None => records.dump_commands(&display_list),
    }
    callbacks.append_text(&main_thread, &output);
}

/// Hands `append_text` what changed from the JSON Lines dump `previous`, if there is one, to the JSON Lines dump
/// `current` of the display list of the same document.
///
/// # Safety
///
/// `previous` must point to `previous_length` readable bytes where `has_previous` is set, and `current` to
/// `current_length`; `append_text` copies the report synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn painting_dump_changes(
    previous: *const u8,
    previous_length: usize,
    has_previous: bool,
    current: *const u8,
    current_length: usize,
    context: *mut c_void,
    append_text: extern "C" fn(context: *mut c_void, bytes: *const u8, byte_count: usize),
) {
    // SAFETY: Guaranteed by the caller.
    let previous = has_previous.then(|| unsafe { ak::slice_from_raw_parts(previous, previous_length) });
    // SAFETY: Guaranteed by the caller.
    let current = unsafe { ak::slice_from_raw_parts(current, current_length) };
    let report = super::dump_changes::display_list_dump_changes(
        previous.map(|previous| String::from_utf8_lossy(previous)).as_deref(),
        &String::from_utf8_lossy(current),
    );
    append_text(context, report.as_ptr(), report.len());
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

fn json_node(node: &DumpedNode) -> serde_json::Value {
    let mut object = serde_json::Map::new();
    object.insert("type".into(), serde_json::json!("node"));
    object.insert(
        "kind".into(),
        serde_json::json!(match node.kind {
            SlotKind::Spatial => "spatial",
            SlotKind::Clip => "clip",
            SlotKind::Effect => "effect",
        }),
    );
    object.insert("name".into(), serde_json::json!(node.name));
    object.insert("raw".into(), serde_json::json!(node.index));
    object.insert("depth".into(), serde_json::json!(node.depth));
    for (key, value) in [
        ("parent", &node.parent),
        ("space", &node.space),
        ("output_clip", &node.output_clip),
    ] {
        if let Some(value) = value {
            object.insert(key.into(), serde_json::json!(value));
        }
    }
    object.insert("description".into(), serde_json::json!(node.description));
    if let Some(owner) = &node.owner {
        object.insert("owner".into(), serde_json::json!(owner));
    }
    serde_json::Value::Object(object)
}

fn json_scroll_node(names: &mut DumpNames, table: &CompositorScrollTable, node_index: usize) -> serde_json::Value {
    use serde_json::json;
    let node = &table.nodes[node_index];
    let mut object = serde_json::Map::new();
    object.insert("type".into(), json!("scroll_node"));
    object.insert(
        "scroll_node".into(),
        json_name(names.spatial(node.scroll_node_index), node.scroll_node_index.0),
    );
    object.insert(
        "parent".into(),
        json_name(
            names.spatial(node.parent_scroll_node_index),
            node.parent_scroll_node_index.0,
        ),
    );
    object.insert("scrollport_rect".into(), json_int_rect(node.scrollport_rect));
    object.insert("min_scroll_offset".into(), json_float_point(node.min_scroll_offset));
    object.insert("max_scroll_offset".into(), json_float_point(node.max_scroll_offset));
    object.insert("is_viewport".into(), json!(node.is_viewport));
    if let Ok(container_index) = table
        .snap_containers
        .binary_search_by_key(&node.scroll_node_index, |container| container.scroll_node_index)
    {
        let container = &table.snap_containers[container_index];
        let first_area_index = container.first_area_index as usize;
        let areas: Vec<serde_json::Value> = table.snap_areas
            [first_area_index..first_area_index + container.area_count as usize]
            .iter()
            .map(|area| {
                json!({
                    "pseudo_element_type": area.pseudo_element_type,
                    "rect": json_float_rect(css_pixel_rect(area.rect)),
                    "align_x": area.align_x,
                    "align_y": area.align_y,
                    "always_stop": area.always_stop,
                })
            })
            .collect();
        object.insert(
            "snap_container".into(),
            json!({
                "snapport": json_float_rect(css_pixel_rect(container.snapport)),
                "min_scroll_offset": json_float_point(css_pixel_point(container.min_scroll_offset)),
                "max_scroll_offset": json_float_point(css_pixel_point(container.max_scroll_offset)),
                "strictness": container.strictness,
                "snaps_x": container.snaps_x,
                "snaps_y": container.snaps_y,
                "horizontal_writing_mode": container.horizontal_writing_mode,
                "areas": areas,
            }),
        );
    }
    serde_json::Value::Object(object)
}

// Lists every row, unlike the tree section above it, which shows only the nodes that command runs reach.
fn dump_compositor_scroll_table(
    output: &mut String,
    names: &mut DumpNames,
    table: &CompositorScrollTable,
    jsonl: bool,
) {
    if jsonl {
        for node_index in 0..table.nodes.len() {
            push_json_line(output, json_scroll_node(names, table, node_index));
        }
        return;
    }
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
fn dump_layers_if_list_draws_canvas_video_or_nested_context(
    output: &mut String,
    display_list: &DisplayListStorage,
    tree: &VisualContextTree,
    scroll_offsets: &[FloatPoint],
    device_viewport_rect: IntRect,
    record_ids: Option<&RecordIdsOfTopLevelRecords>,
    jsonl: bool,
) -> Option<LayerPlan> {
    let (tape, command_runs) = (display_list.bytes(), display_list.command_runs());
    let summary = summarize(tape, command_runs);
    let draws_canvas_video_or_nested_context = summary
        .live_content_records
        .iter()
        .any(|live_content_record| !matches!(live_content_record.content, LiveContent::ImageFrame(_)));
    if !draws_canvas_video_or_nested_context {
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
    });
    match layer_plan {
        Ok(layer_plan) if jsonl => {
            for (index, layer) in layer_plan.layers.iter().enumerate() {
                let mut object = serde_json::Map::new();
                object.insert("type".into(), serde_json::json!("layer"));
                object.insert("name".into(), serde_json::json!(format!("L{index}")));
                object.insert("kind".into(), serde_json::json!(layer.kind.to_string()));
                object.insert("rect".into(), json_int_rect(layer.rect));
                if let Some(record_ids) = record_ids {
                    object.insert("records".into(), record_ids.json_spans(&layer.record_spans));
                }
                match &layer.window_server_placement {
                    Some(Ok(placement)) => {
                        object.insert(
                            "shown_by_the_window_server".into(),
                            serde_json::json!(placement.to_string()),
                        );
                    }
                    Some(Err(rejection)) => {
                        object.insert("replayed".into(), serde_json::json!(rejection.to_string()));
                    }
                    None => {}
                }
                push_json_line(output, serde_json::Value::Object(object));
            }
            Some(layer_plan)
        }
        Err(unlayered_reason) if jsonl => {
            push_json_line(
                output,
                serde_json::json!({ "type": "unlayered", "reason": unlayered_reason.to_string() }),
            );
            None
        }
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

    fn json_spans(&self, spans: &[RecordSpan]) -> serde_json::Value {
        serde_json::Value::Array(
            spans
                .iter()
                .map(|span| serde_json::json!([self.id_at(span.begin_offset), self.id_at(span.end_offset) - 1]))
                .collect(),
        )
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

/// What the recording noted of a record, as the dump prints it.
#[derive(Default)]
struct RecordNote {
    origin: Option<RecordOrigin>,
    glyph_run_text: Option<String>,
}

/// The notes of each record of a tape in tape order, nested records after the one they are nested in. A nested record
/// whose recording noted no origin, such as a record of a pattern tile, which is recorded once and copied wherever it
/// is drawn, has the origin of the record it is nested in.
fn record_notes_in_tape_order(display_list: &RecordedDisplayList, notes: &RecordNotes) -> Vec<RecordNote> {
    fn collect(
        bytes: &[u8],
        base_offset: u32,
        enclosing_origin: Option<RecordOrigin>,
        notes: &RecordNotes,
        record_notes: &mut Vec<RecordNote>,
    ) {
        for_each_command(bytes, |header, record_offset, payload| {
            let record_offset = base_offset + record_offset as u32;
            let origin = notes.origin_at(record_offset).or(enclosing_origin);
            record_notes.push(RecordNote {
                origin,
                glyph_run_text: notes.glyph_run_text_at(record_offset).map(str::to_owned),
            });
            let payload_offset = record_offset + HEADER_SIZE as u32;
            for_each_nested_record_span(header.command_type, payload, |_, span| {
                collect(
                    span_bytes(payload, span),
                    payload_offset + span.offset,
                    origin,
                    notes,
                    record_notes,
                );
            });
        });
    }
    let mut record_notes = Vec::new();
    for run in &display_list.command_runs {
        let run_bytes = &display_list.bytes[run.offset as usize..(run.offset + run.size) as usize];
        collect(run_bytes, run.offset, None, notes, &mut record_notes);
    }
    record_notes
}

/// The labels of the live boxes that recorded the records.
fn owner_labels(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    main_thread: &MainThread,
    callbacks: &FfiPaintingDumpCallbacks,
    notes: &[RecordNote],
) -> HashMap<NodeSlotId, String> {
    let owners: Vec<NodeSlotId> = notes
        .iter()
        .filter_map(|note| note.origin)
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

/// The notes of the records the DisplayList section prints, which it takes in turn. The records of an SVG image, which
/// the recording names by the image's id only, are not among them.
struct RecordNotesInDumpOrder {
    notes: Vec<RecordNote>,
    next_index: usize,
    owner_labels: HashMap<NodeSlotId, String>,
    prints_owners: bool,
    prints_glyph_run_texts: bool,
}

/// Where the DisplayList section is written, with the names it gives what the records name.
struct RecordDump<'a> {
    output: &'a mut String,
    names: &'a mut DumpNames,
    jsonl: bool,
    prints_record_ids: bool,
    // How many records were dumped so far, which numbers the next one.
    record_count: u32,
    record_notes: Option<RecordNotesInDumpOrder>,
    // The descriptions of the fonts the glyph runs draw with, by their ids, where the dump prints them.
    font_descriptions: Option<HashMap<u64, String>>,
    // The chunks of the tape by the index of their first run, where the dump marks them.
    chunk_starts: HashMap<usize, ChunkStart>,
    current_chunk: Option<String>,
}

/// Where a chunk of the tape starts, with what it holds.
struct ChunkStart {
    name: String,
    id: ChunkId,
    record_count: usize,
    byte_count: u32,
}

impl ChunkStart {
    fn of_tape(display_list: &DisplayListStorage) -> HashMap<usize, Self> {
        let (bytes, command_runs, chunks) = (display_list.bytes(), display_list.command_runs(), display_list.chunks());
        (0..chunks.len())
            .map(|chunk_index| {
                let runs = &command_runs[chunks.runs_of(chunk_index)];
                let mut record_count = 0;
                for run in runs {
                    for_each_command_including_nested(
                        &bytes[run.offset as usize..(run.offset + run.size) as usize],
                        &mut |_, _, _| record_count += 1,
                    );
                }
                (
                    chunks.runs_of(chunk_index).start,
                    Self {
                        name: format!("k{}", chunk_index + 1),
                        id: chunks.id(chunk_index),
                        record_count,
                        byte_count: runs.iter().map(|run| run.size).sum(),
                    },
                )
            })
            .collect()
    }
}

/// Where a record sits: how deep in other records, in which of them and as what, and in which layer.
#[derive(Clone, Copy)]
struct RecordPlace {
    indent: usize,
    depth: usize,
    nested_in: Option<(u32, NestedRecordsRole)>,
    layer: Option<usize>,
    // The records of an SVG image take the origin of the record that draws the image.
    origin_of_enclosing_vector_image: Option<Option<RecordOrigin>>,
}

impl RecordPlace {
    const TOP_LEVEL: Self = Self {
        indent: 0,
        depth: 0,
        nested_in: None,
        layer: None,
        origin_of_enclosing_vector_image: None,
    };
}

fn nested_records_role_name(role: NestedRecordsRole) -> &'static str {
    match role {
        NestedRecordsRole::IsolatedGroupContent => "content",
        NestedRecordsRole::IsolatedGroupMask => "mask",
        NestedRecordsRole::RepeatedTile => "tile",
        NestedRecordsRole::MaskContent => "mask-content",
        NestedRecordsRole::PatternTile => "pattern-tile",
        NestedRecordsRole::VectorImage => "vector-image",
    }
}

impl RecordDump<'_> {
    fn mark_chunk_starting_at(&mut self, run_index: usize) {
        let Some(chunk) = self.chunk_starts.get(&run_index) else {
            return;
        };
        if self.jsonl {
            push_json_line(
                self.output,
                serde_json::json!({
                    "type": "chunk",
                    "name": chunk.name,
                    "id": format!("{:016x}{:016x}", chunk.id.0[0], chunk.id.0[1]),
                    "first_record": self.record_count + 1,
                    "records": chunk.record_count,
                    "bytes": chunk.byte_count,
                }),
            );
        } else {
            writeln!(
                self.output,
                "Chunk {} id={:012x} records={} bytes={}",
                chunk.name,
                chunk.id.0[0] >> 16,
                chunk.record_count,
                chunk.byte_count
            )
            .unwrap();
        }
        self.current_chunk = Some(chunk.name.clone());
    }

    fn dump_commands(&mut self, display_list: &DisplayListStorage) {
        let (bytes, command_runs) = (display_list.bytes(), display_list.command_runs());
        for (run_index, run) in command_runs.iter().enumerate() {
            self.mark_chunk_starting_at(run_index);
            let run_bytes = &bytes[run.offset as usize..(run.offset + run.size) as usize];
            self.dump_command_bytes(run_bytes, run.context, RecordPlace::TOP_LEVEL);
        }
    }

    // Each top-level command starts with the layer it draws into. The records nested in it draw into the same layer.
    fn dump_top_level_commands_with_their_layers(&mut self, display_list: &DisplayListStorage, layer_plan: &LayerPlan) {
        let (bytes, command_runs) = (display_list.bytes(), display_list.command_runs());
        for (run_index, run) in command_runs.iter().enumerate() {
            self.mark_chunk_starting_at(run_index);
            let run_bytes = &bytes[run.offset as usize..(run.offset + run.size) as usize];
            for_each_command(run_bytes, |_, offset_in_run, payload| {
                let layer = layer_plan.layer_of_record(run.offset + offset_in_run as u32);
                let record_bytes = &run_bytes[offset_in_run..offset_in_run + HEADER_SIZE + payload.len()];
                let place = RecordPlace {
                    layer: Some(layer),
                    ..RecordPlace::TOP_LEVEL
                };
                if self.jsonl {
                    self.dump_command_bytes(record_bytes, run.context, place);
                    return;
                }
                let layer_label = format!("L{layer} ");
                let record_dump_start = self.output.len();
                self.dump_command_bytes(record_bytes, run.context, place);
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

    fn dump_command_bytes(&mut self, command_bytes: &[u8], context: ContextRef, place: RecordPlace) {
        for_each_command(command_bytes, |header, _, payload| {
            let note = match place.origin_of_enclosing_vector_image {
                Some(origin) => RecordNote {
                    origin,
                    glyph_run_text: None,
                },
                None => self
                    .record_notes
                    .as_mut()
                    .map(RecordNotesInDumpOrder::take_next)
                    .unwrap_or_default(),
            };
            self.record_count += 1;
            let record_id = self.record_count;
            if self.jsonl {
                self.push_json_record(record_id, header, payload, context, place, &note);
            } else {
                push_indent(self.output, place.indent);
                if self.prints_record_ids {
                    write!(self.output, "#{record_id} ").unwrap();
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
                self.push_note(header.command_type, payload, &note);
                self.output.push('\n');
            }

            self.dump_records_inside(header.command_type, payload, context, place, record_id, note.origin);
        });
    }

    fn dump_records_inside(
        &mut self,
        command_type: DisplayListCommandType,
        payload: &[u8],
        context: ContextRef,
        place: RecordPlace,
        record_id: u32,
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
            let mut nested_indent = place.indent + 1;
            if let Some(label) = label
                && !self.jsonl
            {
                push_indent(self.output, place.indent + 1);
                self.output.push_str(label);
                nested_indent += 1;
            }
            let origin_of_enclosing_vector_image = match role {
                NestedRecordsRole::VectorImage => place.origin_of_enclosing_vector_image.or(Some(origin)),
                _ => place.origin_of_enclosing_vector_image,
            };
            self.dump_command_bytes(
                span_bytes(payload, span),
                context,
                RecordPlace {
                    indent: nested_indent,
                    depth: place.depth + 1,
                    nested_in: Some((record_id, role)),
                    layer: place.layer,
                    origin_of_enclosing_vector_image,
                },
            );
        });
    }

    fn font_description(&self, command_type: DisplayListCommandType, payload: &[u8]) -> Option<&String> {
        let font_descriptions = self.font_descriptions.as_ref()?;
        let font_id = match command_type {
            DisplayListCommandType::DrawGlyphRun => read_command::<DrawGlyphRun>(payload).font_id,
            DisplayListCommandType::PaintTextShadow => read_command::<PaintTextShadow>(payload).font_id,
            _ => return None,
        };
        font_descriptions.get(&font_id.0)
    }

    // What the recording noted of the record, and the font a glyph run draws with: the text, the font and then the
    // origin.
    fn push_note(&mut self, command_type: DisplayListCommandType, payload: &[u8], note: &RecordNote) {
        let (prints_owners, prints_glyph_run_texts) = self.record_notes.as_ref().map_or((false, false), |notes| {
            (notes.prints_owners, notes.prints_glyph_run_texts)
        });
        if prints_glyph_run_texts && let Some(text) = &note.glyph_run_text {
            push_glyph_run_text(self.output, text);
        }
        if let Some(description) = self.font_description(command_type, payload) {
            let font = format!(" font=\"{description}\"");
            self.output.push_str(&font);
        }
        let Some(origin) = note.origin.filter(|_| prints_owners) else {
            return;
        };
        let owner_label = self
            .record_notes
            .as_ref()
            .and_then(|notes| notes.owner_labels.get(&origin.owner));
        if let Some(owner_label) = owner_label {
            write!(self.output, " owner={owner_label}").unwrap();
        }
        write!(self.output, " phase={}", origin.phase).unwrap();
    }

    fn push_json_record(
        &mut self,
        record_id: u32,
        header: &DisplayListCommandHeader,
        payload: &[u8],
        context: ContextRef,
        place: RecordPlace,
        note: &RecordNote,
    ) {
        use serde_json::{Value, json};
        let mut record = serde_json::Map::new();
        record.insert("type".into(), json!("record"));
        record.insert("id".into(), json!(record_id));
        record.insert("op".into(), json!(header.command_type.name()));
        record.insert("depth".into(), json!(place.depth));
        if let Some((parent_id, role)) = place.nested_in {
            record.insert("parent".into(), json!(parent_id));
            record.insert("role".into(), json!(nested_records_role_name(role)));
        }
        if let Some(layer) = place.layer {
            record.insert("layer".into(), json!(format!("L{layer}")));
        }
        if let Some(chunk) = &self.current_chunk {
            record.insert("chunk".into(), json!(chunk));
        }
        record.insert("context".into(), json_context(self.names, context));
        let fields = command_fields(header.command_type, payload);
        record.insert("fields".into(), json_fields(&fields, self.names));
        if header.inline_clip_count > 0 {
            record.insert("inline_clips".into(), json_inline_clips(self.names, header, payload));
        }
        if let Some(transform) = inline_transform_of(header, payload) {
            record.insert(
                "inline_transform".into(),
                Value::Array(transform.values.iter().map(|value| json_float(*value)).collect()),
            );
        }
        if let Some(text) = &note.glyph_run_text {
            record.insert("text".into(), json!(text));
        }
        if let Some(description) = self.font_description(header.command_type, payload) {
            record.insert("font".into(), json!(description));
        }
        if let Some(origin) = note.origin {
            let owner_label = self
                .record_notes
                .as_ref()
                .and_then(|notes| notes.owner_labels.get(&origin.owner));
            if let Some(owner_label) = owner_label {
                record.insert("owner".into(), json!(owner_label));
            }
            record.insert("phase".into(), json!(origin.phase));
        }
        push_json_line(self.output, Value::Object(record));
    }
}

fn push_json_line(output: &mut String, value: serde_json::Value) {
    output.push_str(&value.to_string());
    output.push('\n');
}

fn json_context(names: &mut DumpNames, context: ContextRef) -> serde_json::Value {
    let mut object = serde_json::Map::new();
    object.insert(
        "spatial".into(),
        json_name(names.spatial(context.spatial), context.spatial.0),
    );
    if !context.clip.is_none() {
        object.insert("clip".into(), json_name(names.clip(context.clip), context.clip.0));
    }
    if !context.effect.is_none() {
        object.insert(
            "effect".into(),
            json_name(names.effect(context.effect), context.effect.0),
        );
    }
    serde_json::Value::Object(object)
}

fn json_inline_clips(names: &mut DumpNames, header: &DisplayListCommandHeader, payload: &[u8]) -> serde_json::Value {
    use serde_json::json;
    serde_json::Value::Array(
        inline_clips_of(header, payload)
            .map(|clip| {
                let mut object = serde_json::Map::new();
                object.insert("kind".into(), json!(format!("{:?}", clip.kind)));
                object.insert("rect".into(), json_float_rect(clip.clip_rect_or_path_device_bounds));
                if clip.kind == InlineClipKind::Path {
                    let path = Path::from_serialized_bytes(span_bytes(payload, clip.path_data));
                    object.insert("path".into(), json!(path_description(&path)));
                    object.insert("winding_rule".into(), json!(format!("{:?}", clip.path_winding_rule)));
                } else if clip.corner_radii.has_any_radius() {
                    object.insert("corner_radii".into(), json_corner_radii(clip.corner_radii));
                }
                if clip.mode == ClipMode::Difference {
                    object.insert("mode".into(), json!("difference"));
                }
                if clip.has_own_space {
                    object.insert("space".into(), json_name(names.spatial(clip.space), clip.space.0));
                }
                serde_json::Value::Object(object)
            })
            .collect(),
    )
}

impl RecordNotesInDumpOrder {
    fn take_next(&mut self) -> RecordNote {
        let note = self
            .notes
            .get_mut(self.next_index)
            .map(std::mem::take)
            .unwrap_or_default();
        self.next_index += 1;
        note
    }
}

const GLYPH_RUN_TEXT_PRINTED_LENGTH: usize = 40;

/// The text a glyph run draws, quoted and escaped, cut to its first characters with its full length after it where it
/// is longer.
fn push_glyph_run_text(output: &mut String, text: &str) {
    output.push_str(" text=\"");
    let length = text.chars().count();
    for character in text.chars().take(GLYPH_RUN_TEXT_PRINTED_LENGTH) {
        match character {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\n' => output.push_str("\\n"),
            '\t' => output.push_str("\\t"),
            character if character.is_control() => write!(output, "\\u{{{:x}}}", character as u32).unwrap(),
            character => output.push(character),
        }
    }
    if length > GLYPH_RUN_TEXT_PRINTED_LENGTH {
        write!(output, "…\" text_length={length}").unwrap();
    } else {
        output.push('"');
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

    #[test]
    fn glyph_run_text_is_escaped_and_cut_with_its_full_length_after_it() {
        let text = |text: &str| {
            let mut output = String::new();
            push_glyph_run_text(&mut output, text);
            output
        };
        assert_eq!(text("say \"hi\"\\\n\t\u{7}"), r#" text="say \"hi\"\\\n\t\u{7}""#);
        assert_eq!(
            text(&"é".repeat(41)),
            format!(" text=\"{}…\" text_length=41", "é".repeat(40))
        );
    }
}
