/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::ffi_support::{FfiUtf16View, OwnedBox};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{DomPaintFact, NodeSlotId};
use crate::layout::query::inline_cursor::{CaretPosition, InlineCursor, InlineLine, LineEdge};
use crate::layout::script_entries::FfiCaretPosition;
use crate::layout::svg_formatting_context;
use crate::layout::used_values::FfiCssPixelPoint;
use crate::layout::used_values::FfiCssPixelRect;
use crate::layout::used_values::FfiCssPixelSize;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::host::visual_context::FfiSvgFilterPrimitive;
use crate::painting::paint_changes::{PaintChange, queue};
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::paintable_data::*;
use crate::painting::paintable_rows::{PaintableRowsRead, with_inline_pieces};
use crate::painting::svg_filter::SvgFilterPrimitive;
use crate::painting::visual_animation_builder::CompositorAnimationEffectState;
use crate::render_state::{DocumentHost, RenderWait};
use std::ffi::c_void;

mod main_thread_entries;

pub(crate) use main_thread_entries::MainThreadFfiEntry;

crate::render_state::held_node_entries!();

/// Answers `answer` from the render state of `host`'s document and `args`, as of every change the host queued,
/// spending `wait`: the host's painting, input, editing and devtools code reads what the render state laid out and
/// painted.
pub(crate) fn read_arena<A, R>(
    host: &DocumentHost,
    wait: impl RenderWait,
    args: A,
    answer: fn(&mut LayoutNodeArena, A) -> R,
) -> R {
    host.ask(wait, |state| answer(state.arena_mut(), args))
}

/// Clears the committed box of `layout_node` and tells the document's chrome state, at once or
/// once the change that clears it is over.
pub(crate) fn paintable_cleared_from_node(
    host_calls: crate::layout::tree_mutation::HostCalls<'_>,
    arena: &mut LayoutNodeArena,
    layout_node: NodeSlotId,
) {
    // NB: A box can stay in the tree after it gives up its committed box, until its parent's rebuild frees it, and a
    //     layout before that keeps the fragments of its ancestors that hold it.
    if arena.slot_is_live(layout_node) {
        arena.invalidate_committed_geometry_of_ancestors(layout_node);
    }
    arena.clear_committed_fragment_link(layout_node);
    if let Some(reset) = arena.prepare_paintable_row_cleared_reset(layout_node) {
        host_calls.paintable_row_reset(reset);
        arena.paintable_row_cleared(reset);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum ScrollDirection {
    #[default]
    Horizontal,
    Vertical,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiChromeMetrics {
    pub scroll_thumb_min_length: CssPixels,
    pub scroll_thumb_padding_thin: CssPixels,
    pub scroll_thumb_thickness_thin: CssPixels,
    pub scroll_thumb_thickness: CssPixels,
    pub scroll_gutter_thickness: CssPixels,
    pub resize_gripper_size: CssPixels,
    pub resize_gripper_padding: CssPixels,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiPhysicalResizeAxes {
    pub horizontal: bool,
    pub vertical: bool,
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_set_chrome_state_callback(
    host: &DocumentHost,
    context: *mut c_void,
    callback: unsafe extern "C" fn(*mut c_void, NodeSlotId, PaintableRowResetKind),
) {
    host.host_tables().chrome_state_callback.set(Some((context, callback)));
}

/// Copies the row in `slot` to `row`, where it is populated. The host reads it from the rows it holds where they still
/// read as the arena, and asks otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_paintable_row(host: &DocumentHost, slot: NodeSlotId, row: &mut PaintableData) -> bool {
    let known = host.read_known_rows(|rows, _| {
        rows.paintable
            .paintable_row_is_populated(slot)
            .then(|| *rows.paintable.paintable_data(slot))
    });
    let answer = known.unwrap_or_else(|| {
        read_arena(host, node_read(), slot, |arena, slot| {
            let paintable_rows = arena.paintable_rows();
            paintable_rows
                .paintable_row_is_populated(slot)
                .then(|| *paintable_rows.paintable_data(slot))
        })
    });
    let Some(answer) = answer else {
        return false;
    };
    *row = answer;
    true
}

/// Whether the row in `slot` is populated, which the host answers without asking where it knows.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_has_paintable_row(host: &DocumentHost, slot: NodeSlotId) -> bool {
    if let Some(populated) = host.known_paintable_row_is_populated(slot) {
        return populated;
    }
    read_arena(host, node_read(), slot, |arena, slot| {
        arena.paintable_rows().paintable_row_is_populated(slot)
    })
}

#[repr(C)]
pub struct FfiPhysicalOverflowDirections {
    pub horizontal_axis_is_positive: bool,
    pub vertical_axis_is_positive: bool,
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread. The callbacks must remain valid until it is
/// destroyed and must not mutate layout geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_set_geometry_host(
    host: &DocumentHost,
    callbacks: crate::painting::host::FfiGeometryHostCallbacks,
) {
    host.host_tables().geometry_host.set(Some(callbacks));
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_paintable_scrollable_overflow(
    host: &DocumentHost,
    slot: NodeSlotId,
    out: &mut FfiOptionalOverflowData,
) {
    // The host reads the overflow from the rows it holds, where they were measured and still read as the arena.
    let known = host.read_known_rows(|rows, source| {
        rows.overflow_is_settled()
            .then(|| FfiOptionalOverflowData::of(source, slot))
    });
    if let Some(Some(overflow)) = known {
        *out = overflow;
        return;
    }
    *out = {
        read_arena(host, node_read(), slot, |arena, slot| {
            arena.settle_scrollable_overflow();
            FfiOptionalOverflowData::of(&arena.paintable_rows(), slot)
        })
    };
}

#[derive(Default)]
#[repr(C)]
pub struct FfiOptionalOverflowData {
    pub has_value: bool,
    pub value: crate::painting::paintable_data::FfiOverflowData,
}

impl FfiOptionalOverflowData {
    /// The scrollable overflow of the row in `slot` of `rows`, whose overflow is measured.
    fn of(rows: &impl GeometryRead, slot: NodeSlotId) -> Self {
        let Some(rect) = crate::painting::paintable_geometry::scrollable_overflow_rect(rows, slot) else {
            return Self::default();
        };
        Self {
            has_value: true,
            value: crate::painting::paintable_data::FfiOverflowData {
                rect: rect.into(),
                has_scrollable_overflow: crate::painting::paintable_geometry::has_scrollable_overflow(rows, slot),
            },
        }
    }
}

#[repr(C)]
#[derive(Default)]
pub struct FfiBoxModelMetrics {
    pub margin: crate::painting::paintable_data::FfiPixelBox,
    pub padding: crate::painting::paintable_data::FfiPixelBox,
    pub border: crate::painting::paintable_data::FfiPixelBox,
    pub inset: crate::painting::paintable_data::FfiPixelBox,
}

/// The geometry the box of an SVG element committed, in the user units of the SVG viewport it is laid out in. Returns
/// false, writing nothing, for a box without one.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_svg_geometry(
    host: &DocumentHost,
    slot: NodeSlotId,
    out: &mut svg_formatting_context::SvgGeometry,
) -> bool {
    let geometry = read_arena(host, node_read(), slot, |arena, slot| {
        if !arena.paintable_row_is_populated(slot) {
            return None;
        }
        crate::painting::paintable_geometry::committed_svg_geometry(arena, slot)
    });
    let Some(geometry) = geometry else {
        return false;
    };
    *out = geometry;
    true
}

#[repr(C)]
pub struct FfiOptionalAffineTransform {
    pub has_value: bool,
    pub transform: svg_formatting_context::FfiAffineTransform,
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_paintable_svg_viewport_transform(
    host: &DocumentHost,
    slot: NodeSlotId,
) -> FfiOptionalAffineTransform {
    read_arena(host, node_read(), slot, |arena, slot| {
        let transform = if arena.paintable_row_is_populated(slot) {
            crate::painting::paintable_geometry::committed_svg_viewport_transform(arena, slot)
        } else {
            None
        };
        FfiOptionalAffineTransform {
            has_value: transform.is_some(),
            transform: transform.unwrap_or_default(),
        }
    })
}

/// The transform an SVG element's box was laid out with: its own CSS transform about its transform origin, reduced to
/// 2D, with a <use> element's x/y translation multiplied in.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_paintable_svg_element_transform(
    host: &DocumentHost,
    slot: NodeSlotId,
) -> svg_formatting_context::FfiAffineTransform {
    let mut geometry = svg_formatting_context::SvgGeometry::default();
    render_state_svg_geometry(host, slot, &mut geometry);
    geometry.local_transform
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_paintable_transform_reference_box(
    host: &DocumentHost,
    slot: NodeSlotId,
    out: &mut FfiCssPixelRect,
) {
    fn reference_box(rows: &impl PaintRead, slot: NodeSlotId) -> FfiCssPixelRect {
        committed_transform_reference_box(rows, slot).map_or_else(FfiCssPixelRect::default, Into::into)
    }
    if let Some(rect) = host.read_rows_between_jobs(node_read(), |rows| reference_box(rows, slot)) {
        *out = rect;
        return;
    }
    *out = {
        read_arena(host, node_read(), slot, |arena, slot| {
            reference_box(&arena.paintable_rows(), slot)
        })
    };
}

/// The transform reference box of the box in `slot`, where the box was laid out.
pub(crate) fn committed_transform_reference_box(rows: &impl PaintRead, slot: NodeSlotId) -> Option<CssPixelRect> {
    if !rows.paintable_row_is_populated(slot) {
        return None;
    }
    let style = rows.node_style_if_live(slot)?;
    Some(crate::painting::visual_context::node_values::transform_reference_box(
        style, rows, slot,
    ))
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_physical_overflow_directions(
    host: &DocumentHost,
    paintable: NodeSlotId,
) -> FfiPhysicalOverflowDirections {
    read_arena(host, node_read(), paintable, |arena, paintable| {
        let directions = if arena.paintable_row_is_populated(paintable) {
            crate::painting::scrollable_overflow::physical_overflow_directions(arena, paintable)
        } else {
            crate::painting::scrollable_overflow::PhysicalOverflowDirections::default()
        };
        FfiPhysicalOverflowDirections {
            horizontal_axis_is_positive: directions.horizontal_axis_is_positive,
            vertical_axis_is_positive: directions.vertical_axis_is_positive,
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_background_color_can_be_compositor_animated(
    host: &DocumentHost,
    slot: NodeSlotId,
) -> bool {
    read_arena(host, node_read(), slot, |arena, slot| {
        crate::painting::record::paint::background_resolution::background_color_can_be_compositor_animated(
            &arena.paintable_rows(),
            slot,
            crate::layout::viewport_propagation::root_background_source(arena),
        )
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_paintable_visual_context_node_count(
    host: &DocumentHost,
    slot: NodeSlotId,
    list: crate::painting::host::FfiVisualContextBoxNodeList,
) -> usize {
    read_arena(host, node_read(), (slot, list), |arena, (slot, list)| {
        use crate::painting::host::FfiVisualContextBoxNodeList;
        arena.with_paintable_visual_context_node_handles(slot, |handles| match list {
            FfiVisualContextBoxNodeList::SpatialNodes => handles.spatial.len(),
            FfiVisualContextBoxNodeList::ClipNodes => handles.clip_handles().count(),
            FfiVisualContextBoxNodeList::EffectNodes => handles.effects.len(),
        })
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_paintable_visual_context_copy_node_indices(
    host: &DocumentHost,
    slot: NodeSlotId,
    list: crate::painting::host::FfiVisualContextBoxNodeList,
    out: ak::SpanMut<'_, u32>,
) {
    let indices = {
        read_arena(host, node_read(), (slot, list), |arena, (slot, list)| {
            use crate::painting::host::FfiVisualContextBoxNodeList;
            arena.with_paintable_visual_context_node_handles(slot, |handles| -> Vec<u32> {
                match list {
                    FfiVisualContextBoxNodeList::SpatialNodes => handles.spatial.iter().map(|index| index.0).collect(),
                    FfiVisualContextBoxNodeList::ClipNodes => handles.clip_handles().map(|index| index.0).collect(),
                    FfiVisualContextBoxNodeList::EffectNodes => handles.effects.iter().map(|index| index.0).collect(),
                }
            })
        })
    };
    out.into_slice()[..indices.len()].copy_from_slice(&indices);
}

pub(super) fn apply_walk_assignments(
    arena: &mut crate::layout::LayoutNodeArena,
    outcome: &mut crate::painting::visual_context::incremental::IncrementalUpdateOutcome,
) {
    let mut paintable_rows = arena.paintable_rows_mut();
    for assignment in std::mem::take(&mut outcome.assignments) {
        assignment.apply(&mut paintable_rows);
    }
}

/// How far sticky positioning moved `paintable` from its normal position, in device pixels, as the scroll state of
/// `host`'s document resolved it. Answers false when the accumulated visual context tree holds no sticky node for
/// `paintable`.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_sticky_device_offset(
    host: &DocumentHost,
    paintable: NodeSlotId,
    out: &mut libgfx_rust::FloatPoint,
) -> bool {
    let sticky_node_index = read_arena(host, node_read(), paintable, |arena, paintable| {
        let paint_state = arena.paint_state();
        paint_state
            .visual_context
            .scroll_state
            .states()
            .iter()
            .find(|state| state.is_sticky && state.paintable == paintable)
            .map(|state| state.node_index)
    });
    let Some(sticky_node_index) = sticky_node_index else {
        return false;
    };
    *out = host.scroll_state().device_offset_for_index(sticky_node_index);
    true
}

/// Marks the entry the event loop calls between two tasks to take a finished recording in.
pub(crate) struct TakesFinishedRecordingIn {
    _private: (),
}

const TAKES_FINISHED_RECORDING_IN: TakesFinishedRecordingIn = TakesFinishedRecordingIn { _private: () };

/// How a recording of a document started.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FfiRecordingStart {
    /// The document's viewport had no box to record.
    NothingToRecord,
    /// The recording is done, and pending for the host to publish.
    Recorded,
}

/// Why a rendering update's frame (its style update, or its recording) may not fly beside the
/// event loop: something reads what it computes before the next task.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[expect(dead_code, reason = "C++ constructs the variants")]
pub enum FfiFlightBlocker {
    /// Nothing blocks the frame, which may fly.
    None,
    /// The frame is not a rendering update's: the host records for a screenshot or a hit test, or
    /// renders inside a nested event loop, and reads the frame right after.
    NotInRenderingUpdate,
    /// A view transition of the document captures its rendering in step with the update.
    ViewTransition,
    /// Scroll-state container queries read the scroll state the update snapshots after layout.
    ScrollStateContainer,
    /// An element's `content-visibility: auto` is determined after layout, and styles it again.
    ContentVisibilityAuto,
    /// A scroll-driven timeline takes its time from the layout of the update it is stale in.
    ScrollTimeline,
}

/// How a recording in flight landed.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FfiRecordingLanding {
    /// No recording of the document is in flight.
    NoneInFlight,
    /// The recording has not finished yet.
    StillInFlight,
    /// The recording landed: what it recorded is pending for the host to publish and present, or the
    /// recording presented it itself.
    Landed,
    /// The recording landed after the host wrote the document's rows: what it recorded stands as the
    /// compositor's frame, whether it presented it or the host does, but not its hit-test list.
    LandedBehindRows,
    /// The recording found the document's viewport had no box to record.
    NothingRecorded,
    /// The committed frame recorded nothing, and presented the display list the compositor has.
    PresentedUnrecorded,
    /// The committed frame's recording renders SVG images, which the host renders for it with
    /// `render_state_render_vector_images`, and flies again.
    NeedsVectorImages,
}

/// A navigable's presenter and the seal of the frame it presents next (`Web::Compositor::NavigablePresenter` and
/// `Web::Compositor::SealedPresentation`), which pass between the host and what presents a frame beside it. Both or
/// neither are null.
#[repr(C)]
pub struct FfiPresentation {
    pub presenter: *mut c_void,
    pub sealed: *mut c_void,
}

impl Default for FfiPresentation {
    /// Names no presentation.
    fn default() -> Self {
        Self {
            presenter: std::ptr::null_mut(),
            sealed: std::ptr::null_mut(),
        }
    }
}

/// What the host reads of a published recording, to build its display list from.
#[repr(C)]
pub struct FfiPresentedRecording {
    pub is_identical_to_published_recording: bool,
    pub has_blocking_wheel_event_listeners: bool,
    /// The recorded display list, which the document's last recording holds until the next
    /// recording replaces it. The host takes a reference of its own to keep it longer.
    pub display_list: *const c_void,
    /// The color the canvas holds below the recording's layers.
    pub root_group_color: libgfx_rust::Color,
}

impl FfiPresentedRecording {
    /// What the host reads of a recording published with `output`, whose display list `output` holds for as long as
    /// the host reads it.
    pub(crate) fn of_output(output: &crate::painting::record::RecordingOutput) -> Self {
        Self {
            is_identical_to_published_recording: output.is_identical_to_published_recording,
            has_blocking_wheel_event_listeners: output.has_blocking_wheel_event_listeners,
            display_list: std::sync::Arc::as_ptr(&output.display_list).cast(),
            root_group_color: output.root_group_color,
        }
    }

    /// What the host reads of the last recording the document took in.
    pub(crate) fn of_last_recording(arena: &LayoutNodeArena) -> Self {
        let paint_state = arena.paint_state();
        let recording = paint_state.last_recording.as_deref();
        Self {
            is_identical_to_published_recording: recording
                .is_some_and(|recording| recording.is_identical_to_published_recording),
            has_blocking_wheel_event_listeners: recording
                .is_some_and(|recording| recording.has_blocking_wheel_event_listeners),
            display_list: recording.map_or(std::ptr::null(), |recording| {
                std::sync::Arc::as_ptr(&recording.display_list).cast()
            }),
            root_group_color: recording.map_or(libgfx_rust::Color::TRANSPARENT, |recording| recording.root_group_color),
        }
    }
}

/// One `<area>` of an image map, as the document hands it over: the style-tree identity to name as
/// the hit target, the state of its `shape` attribute, and where its parsed `coords` sit in the
/// flat array published beside it.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiImageMapArea {
    pub style_node: u32,
    pub shape: u8,
    pub coords_offset: u32,
    pub coords_count: u32,
}

/// Adds a color stop to `sink`, the paint server handed to the callback.
#[unsafe(no_mangle)]
pub extern "C" fn layout_arena_paint_push_color_stop(
    sink: &mut crate::painting::svg_paint_resources::PublishedSvgPaintServer,
    color: libgfx_rust::Color,
    position: f32,
) {
    if let crate::painting::svg_paint_resources::PublishedSvgPaintServer::Gradient(gradient) = sink {
        gradient
            .stops
            .push(crate::painting::svg_paint_resources::PublishedSvgGradientStop { color, position });
    }
}

/// Makes `sink`, the paint server handed to the callback, the gradient `description` describes.
#[unsafe(no_mangle)]
pub extern "C" fn layout_arena_svg_paint_resources_push_gradient(
    sink: &mut crate::painting::svg_paint_resources::PublishedSvgPaintServer,
    description: &crate::painting::host::FfiSvgGradientDescription,
) {
    *sink = crate::painting::svg_paint_resources::PublishedSvgPaintServer::Gradient(
        crate::painting::svg_paint_resources::PublishedSvgGradient {
            description: *description,
            stops: Vec::new(),
        },
    );
}

/// # Safety
///
/// `css_transform_entries` must point at `css_transform_count` readable `ComputedResolvedTransform` values of a live
/// computed style.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_svg_paint_resources_push_pattern(
    sink: &mut crate::painting::svg_paint_resources::PublishedSvgPaintServer,
    description: &crate::painting::host::FfiSvgPatternDescription,
    css_transform_entries: *const c_void,
    css_transform_count: usize,
) {
    *sink = crate::painting::svg_paint_resources::PublishedSvgPaintServer::Pattern(
        crate::painting::svg_paint_resources::PublishedSvgPattern {
            description: *description,
            css_transform: unsafe {
                ak::slice_from_raw_parts(
                    css_transform_entries.cast::<crate::css::computed_value_types::ComputedResolvedTransform>(),
                    css_transform_count,
                )
            }
            .to_vec(),
        },
    );
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[expect(dead_code, reason = "C++ constructs the variants")]
pub enum FfiImagePaintRecordKind {
    DecodedFrame,
    NestedDisplayList,
    Gradient,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiImagePaintRecordInputs {
    pub kind: FfiImagePaintRecordKind,
    pub device_pixels_per_css_pixel: f64,
    pub dest_rect: libgfx_rust::FloatRect,
    pub frame_id: u64,
    pub scaling_mode: libgfx_rust::ScalingMode,
    pub nested_display_list_id: u64,
    pub nested_display_list_size: libgfx_rust::IntSize,
    pub gradient_style_value: *const c_void,
    pub gradient_tile_size: FfiCssPixelSize,
    /// The `FfiColorResolutionStyle` the gradient's color stops resolve against.
    pub gradient_stop_color_resolution_style: *const c_void,
}

/// # Safety
///
/// `inputs`, and the gradient style value and color resolution style it points at, must be
/// live for the call. `consume` is called synchronously and takes ownership of the command
/// storage and visual context tree handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_web_record_image_paint_display_list(
    inputs: &FfiImagePaintRecordInputs,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const c_void, *const c_void),
) {
    use crate::css::color_resolution::ColorResolutionInput;
    use crate::painting::display_list::commands::{DisplayListResourceId, ImageFrameResourceId};
    use crate::painting::display_list::device_pixels::DevicePixelConverter;
    use crate::painting::display_list::recorder::DisplayListRecorder;
    use crate::painting::record::paint::gradient_resolution::{
        record_resolved_gradient_fill, resolve_gradient_paint_with_input,
    };
    use crate::painting::visual_context::{TransformData, TransformDataRole, VisualContextTree};
    use libgfx_rust::{CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint};
    let tree = VisualContextTree::create(TransformData {
        matrix: FloatMatrix4x4::identity(),
        origin: FloatPoint::default(),
        sorting_context_root_index: None,
        flattens_inherited_transform: false,
        role: TransformDataRole::CssTransform,
        synthetic_plane: false,
        establishes_sorting_context: false,
        bounds: libgfx_rust::FloatRect::default(),
    });
    // Force-dark never reaches here: an image is darkened by classifying the image itself — not by inverting the
    // fills that raster it.
    let mut recorder = DisplayListRecorder::new(None);
    let dest_rect = inputs.dest_rect;
    match inputs.kind {
        FfiImagePaintRecordKind::DecodedFrame => recorder.draw_scaled_decoded_image_frame(
            dest_rect,
            None,
            ImageFrameResourceId(inputs.frame_id),
            inputs.scaling_mode,
            CompositingAndBlendingOperator::Normal,
            None,
            ForceDarkRole::None,
        ),
        FfiImagePaintRecordKind::NestedDisplayList => recorder.paint_nested_display_list(
            DisplayListResourceId(inputs.nested_display_list_id),
            dest_rect,
            inputs.nested_display_list_size,
        ),
        FfiImagePaintRecordKind::Gradient => {
            // SAFETY: the host keeps the gradient style value and the style its color stops
            // resolve against live for the duration of the call.
            let (gradient_style_value, color_input) = unsafe {
                (
                    &*inputs
                        .gradient_style_value
                        .cast::<crate::css::style_value::StyleValueData>(),
                    ColorResolutionInput::for_ffi_style(inputs.gradient_stop_color_resolution_style.cast()),
                )
            };
            let resolved =
                resolve_gradient_paint_with_input(gradient_style_value, inputs.gradient_tile_size.into(), &color_input);
            record_resolved_gradient_fill(
                &mut recorder,
                DevicePixelConverter::new(inputs.device_pixels_per_css_pixel),
                &resolved,
                dest_rect,
                CompositingAndBlendingOperator::Normal,
                ForceDarkRole::None,
            );
        }
    }
    let recorded = recorder.into_builder().finish();
    unsafe {
        consume(
            context,
            std::sync::Arc::into_raw(std::sync::Arc::new(recorded)).cast(),
            std::sync::Arc::into_raw(std::sync::Arc::new(tree)).cast(),
        );
    }
}

/// The glyph cells a text content element's box committed for its own characters, one per code
/// unit. The cells belong to the committed fragment, so they are read before the next commit.
#[repr(C)]
pub struct FfiSvgTextCharacterCells {
    pub cells: *const svg_formatting_context::FfiSvgTextCharacterCell,
    pub count: usize,
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_paintable_svg_text_character_cells(
    host: &DocumentHost,
    paintable: NodeSlotId,
) -> FfiSvgTextCharacterCells {
    read_arena(host, node_read(), paintable, |arena, paintable| {
        let none = FfiSvgTextCharacterCells {
            cells: std::ptr::null(),
            count: 0,
        };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(paintable) {
            return none;
        }
        match crate::painting::paintable_geometry::committed_svg_text_character_cells(&paintable_rows, paintable) {
            Some(cells) => FfiSvgTextCharacterCells {
                cells: cells.as_ptr(),
                count: cells.len(),
            },
            None => none,
        }
    })
}

/// The part of `rect`, in the viewport, that the content clips of the containing blocks of the box in `slot` leave.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_visible_rect_in_containing_blocks(
    host: &DocumentHost,
    slot: NodeSlotId,
    viewport_mapping: crate::painting::viewport_mapping::FfiViewportMapping<'_>,
    rect: FfiCssPixelRect,
    space: crate::painting::viewport_mapping::ViewportSpace,
    out: &mut FfiCssPixelRect,
) {
    let viewport_mapping = viewport_mapping.resolve();
    let viewport_mapping = viewport_mapping.mapping();
    *out = host.read_rows(node_read(), false, |rows| {
        let visible =
            viewport_mapping.visible_rect_in_ancestor(rows, slot, rect.into(), NodeSlotId::INVALID, space, |clip| clip);
        visible.into()
    });
}

/// Whether a row has ever been given a style with `content-visibility: auto`, which is when a
/// layout commit collects the boxes with it. The host knows without asking right after the round that committed it.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_may_have_auto_content_visibility(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> bool {
    if let Some(facts) = host.known_arena_facts() {
        return facts.may_have_auto_content_visibility;
    }
    read_arena(host, read, (), |arena, ()| arena.may_have_auto_content_visibility())
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_collect_boxes_with_auto_content_visibility(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    root: NodeSlotId,
    context: *mut c_void,
    push_box: unsafe extern "C" fn(*mut c_void, NodeSlotId),
) {
    let boxes = {
        read_arena(host, read, root, |arena, root| {
            let mut boxes = Vec::new();
            crate::painting::content_visibility::for_each_box_with_auto_content_visibility(
                &arena.paintable_rows(),
                root,
                |slot| boxes.push(slot),
            );
            boxes
        })
    };
    for slot in boxes {
        // SAFETY: The C++ callback appends the slot id to a caller-owned collection.
        unsafe { push_box(context, slot) };
    }
}

#[repr(C)]
pub struct FfiOptionalCssPixelPoint {
    pub has_value: bool,
    pub x: CssPixels,
    pub y: CssPixels,
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_inline_paintable_first_piece_position(
    host: &DocumentHost,
    inline_paintable: NodeSlotId,
    out: &mut FfiOptionalCssPixelPoint,
) {
    *out = {
        read_arena(host, node_read(), inline_paintable, |arena, inline_paintable| {
            let mut result = FfiOptionalCssPixelPoint {
                has_value: false,
                x: CssPixels::from_raw(0),
                y: CssPixels::from_raw(0),
            };
            let paintable_rows = arena.paintable_rows();
            let Some(root) = paintable_rows.inline_pieces_root(inline_paintable) else {
                return result;
            };
            let border_widths = crate::painting::paintable_geometry::committed_border(arena, inline_paintable);
            let padding_widths = crate::painting::paintable_geometry::committed_padding(arena, inline_paintable);
            with_inline_pieces(&paintable_rows, inline_paintable, |piece| {
                let border_rect = CssPixelRect::from(piece.border_box_rect);
                let rect = if piece.is_geometry_only_placeholder {
                    border_rect
                } else {
                    let padding_rect = piece.shrunken_by_present_edges(border_rect, border_widths);
                    piece.shrunken_by_present_edges(padding_rect, padding_widths)
                };
                let root_position = crate::painting::paintable_geometry::absolute_line_root_position(
                    &paintable_rows,
                    root,
                    piece.line_index as usize,
                );
                result.has_value = true;
                result.x = rect.x + root_position.x;
                result.y = rect.y + root_position.y;
                false
            });
            result
        })
    };
}

/// The DOM offsets a line renders of a DOM text.
#[repr(C)]
pub struct FfiVisualLine {
    pub start_offset: usize,
    pub end_offset: usize,
}

/// Pushes the lines that hold rendered text of the DOM text whose layout node is `primary`, in DOM order.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_text_visual_lines(
    host: &DocumentHost,
    primary: NodeSlotId,
    context: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, FfiVisualLine),
) {
    let lines = {
        read_arena(host, node_read(), primary, |arena, primary| {
            InlineCursor::new(&arena.paintable_rows()).lines_of_text(primary)
        })
    };
    for line in lines.into_iter().filter(|line| line.has_fragments) {
        // SAFETY: The consumer copies the POD line synchronously.
        unsafe {
            push(
                context,
                FfiVisualLine {
                    start_offset: line.start_offset,
                    end_offset: line.end_offset,
                },
            );
        }
    }
}

/// Whether a line holds rendered text of the DOM text whose layout node is `primary`.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_text_has_rendered_text(host: &DocumentHost, primary: NodeSlotId) -> bool {
    read_arena(host, node_read(), primary, |arena, primary| {
        InlineCursor::new(&arena.paintable_rows())
            .lines_of_text(primary)
            .iter()
            .any(|line| line.has_fragments)
    })
}

/// Whether the DOM text whose layout node is `primary` is on a line: one that holds rendered text of it, or one that a
/// preserved newline in it ends with nothing on it.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_text_has_lines(host: &DocumentHost, primary: NodeSlotId) -> bool {
    read_arena(host, node_read(), primary, |arena, primary| {
        !InlineCursor::new(&arena.paintable_rows())
            .lines_of_text(primary)
            .is_empty()
    })
}

/// The caret position at the start of the first line of the DOM text whose layout node is `primary`, or at the end of
/// its last line.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_text_edge(
    host: &DocumentHost,
    primary: NodeSlotId,
    end: bool,
) -> FfiInlineCaretPosition {
    read_arena(host, node_read(), (primary, end), |arena, (primary, end)| {
        FfiInlineCaretPosition::of(InlineCursor::new(&arena.paintable_rows()).text_edge(primary, end))
    })
}

/// The first line of the DOM text whose layout node is `primary`, or its last, where it holds rendered text of it.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_text_rendered_edge_line(
    host: &DocumentHost,
    primary: NodeSlotId,
    end: bool,
) -> FfiInlineLine {
    read_arena(host, node_read(), (primary, end), |arena, (primary, end)| {
        FfiInlineLine::of(InlineCursor::new(&arena.paintable_rows()).rendered_text_edge_line(primary, end))
    })
}

/// The caret position one grapheme right of (`forward`) or left of `offset` in the DOM text whose layout node is
/// `primary`, where the grapheme boundary of the DOM text that way is `grapheme_boundary`, if `has_grapheme_boundary`.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_text_character_step(
    host: &DocumentHost,
    primary: NodeSlotId,
    offset: usize,
    downstream: bool,
    forward: bool,
    has_grapheme_boundary: bool,
    grapheme_boundary: usize,
) -> FfiInlineCaretPosition {
    let args = (
        primary,
        offset,
        downstream,
        forward,
        has_grapheme_boundary.then_some(grapheme_boundary),
    );
    read_arena(host, node_read(), args, |arena, args| {
        let (text, offset, downstream, forward, grapheme_boundary) = args;
        FfiInlineCaretPosition::of(
            InlineCursor::new(&arena.paintable_rows()).position_after_character_step(
                text,
                offset,
                downstream,
                forward,
                grapheme_boundary,
            ),
        )
    })
}

fn has_rendered_text_matching(
    arena: &impl PaintableRowsRead,
    node_slots: &[NodeSlotId],
    matches: impl Fn(&FragmentRecord) -> bool,
) -> bool {
    let mut found = false;
    crate::painting::text_fragment::for_each_fragment_of_nodes(arena, node_slots, |_, _, fragment| {
        if fragment.length_in_code_units > 0 && matches(fragment) {
            found = true;
            return false;
        }
        true
    });
    found
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_text_has_rendered_text_before(
    host: &DocumentHost,
    primary: NodeSlotId,
    offset: usize,
) -> bool {
    read_arena(host, node_read(), (primary, offset), |arena, (primary, offset)| {
        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        has_rendered_text_matching(&arena.paintable_rows(), node_slots, |fragment| {
            fragment.dom_start_offset_in_node < offset
        })
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_text_has_rendered_text_after(
    host: &DocumentHost,
    primary: NodeSlotId,
    offset: usize,
) -> bool {
    read_arena(host, node_read(), (primary, offset), |arena, (primary, offset)| {
        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        has_rendered_text_matching(&arena.paintable_rows(), node_slots, |fragment| {
            fragment.dom_end_offset_in_node > offset
        })
    })
}

#[repr(C)]
#[derive(Default)]
pub struct FfiOptionalCssPixels {
    pub has_value: bool,
    pub value: CssPixels,
}

impl FfiOptionalCssPixels {
    fn of(value: Option<CssPixels>) -> Self {
        Self {
            has_value: value.is_some(),
            value: value.unwrap_or_default(),
        }
    }
}

/// A line of the line root `root`, or none where `root` is invalid.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiInlineLine {
    pub root: NodeSlotId,
    pub index: u32,
}

impl FfiInlineLine {
    fn of(line: Option<InlineLine>) -> Self {
        line.map_or(
            Self {
                root: NodeSlotId::INVALID,
                index: 0,
            },
            |line| Self {
                root: line.root,
                index: line.index,
            },
        )
    }

    fn line(self, arena: &impl PaintRead) -> Option<InlineLine> {
        let line = InlineLine {
            root: self.root,
            index: self.index,
        };
        (!self.root.is_invalid()
            && arena.paintable_row_is_populated(self.root)
            && (line.index as usize) < arena.committed_side_data(self.root).lines().len())
        .then_some(line)
    }
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiInlineCaretPositionKind {
    None,
    Text,
    BesideAtomicInline,
    BeforeForcedBreak,
    StartOfBox,
}

/// A caret position as InlineCursor names it: `offset` in the DOM text whose layout node is `node` (leaning to the next
/// line where `downstream`), beside the atomic inline `node` (`after` it or before it), before the <br> `node`, or at the
/// start of the box `node`.
#[repr(C)]
pub struct FfiInlineCaretPosition {
    pub kind: FfiInlineCaretPositionKind,
    pub node: NodeSlotId,
    pub offset: usize,
    pub downstream: bool,
    pub after: bool,
}

impl FfiInlineCaretPosition {
    fn of(position: Option<CaretPosition>) -> Self {
        let (kind, node, offset, downstream, after) = match position {
            None => (FfiInlineCaretPositionKind::None, NodeSlotId::INVALID, 0, true, false),
            Some(CaretPosition::Text {
                text,
                offset,
                downstream,
            }) => (FfiInlineCaretPositionKind::Text, text, offset, downstream, false),
            Some(CaretPosition::BesideAtomicInline { node, after }) => {
                (FfiInlineCaretPositionKind::BesideAtomicInline, node, 0, true, after)
            }
            Some(CaretPosition::BeforeForcedBreak { node }) => {
                (FfiInlineCaretPositionKind::BeforeForcedBreak, node, 0, true, false)
            }
            Some(CaretPosition::StartOfBox { node }) => (FfiInlineCaretPositionKind::StartOfBox, node, 0, true, false),
        };
        Self {
            kind,
            node,
            offset,
            downstream,
            after,
        }
    }
}

/// The caret positions that produce a caret: those in DOM text and beside DOM elements, not in generated content.
fn produces_caret(arena: &impl PaintRead, fragment: &FragmentRecord) -> bool {
    arena.node_is_dom_backed(fragment.layout_node)
}

/// The caret positions a move between lines can start from at `position`: those it can take, and in DOM text that no line
/// holds there, such as collapsed whitespace, the nearest position that one does.
fn navigation_candidates<'a, A: PaintRead>(
    cursor: &'a InlineCursor<'a, A>,
    rows: &'a A,
    position: &'a FfiCaretPosition,
) -> impl Iterator<Item = CaretPosition> + 'a {
    position.candidates(rows).chain(
        std::iter::once_with(move || {
            rows.node_kind_if_live(position.node)
                .is_some_and(crate::layout::node_facts::kind_is_text)
                .then(|| cursor.nearest_text_position_on_a_line(position.node, position.offset))
                .flatten()
        })
        .flatten(),
    )
}

/// The line the caret at `position` is on, if any.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_caret_line(host: &DocumentHost, position: FfiCaretPosition) -> FfiInlineLine {
    read_arena(host, node_read(), position, |arena, position| {
        let rows = arena.paintable_rows();
        let cursor = InlineCursor::new(&rows);
        FfiInlineLine::of(
            navigation_candidates(&cursor, &rows, &position).find_map(|candidate| cursor.line_of_position(candidate)),
        )
    })
}

/// The line that holds `line` outside the atomic inline boxes its line root is in.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_caret_line_outside_atomic_inlines(
    host: &DocumentHost,
    line: FfiInlineLine,
) -> FfiInlineLine {
    read_arena(host, node_read(), line, |arena, line| {
        let rows = arena.paintable_rows();
        FfiInlineLine::of(
            line.line(&rows)
                .map(|line| InlineCursor::new(&rows).line_outside_atomic_inlines(line)),
        )
    })
}

/// The next line after `line` (or the previous one) in its line root that holds a caret position, if any.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_adjacent_caret_line(
    host: &DocumentHost,
    line: FfiInlineLine,
    forward: bool,
) -> FfiInlineLine {
    read_arena(host, node_read(), (line, forward), |arena, (line, forward)| {
        let rows = arena.paintable_rows();
        FfiInlineLine::of(
            line.line(&rows)
                .and_then(|line| InlineCursor::new(&rows).adjacent_line(line, forward)),
        )
    })
}

/// The caret position on `line` nearest the inline coordinate `x`, which a move between lines kept.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_caret_position_at_inline_coordinate(
    host: &DocumentHost,
    line: FfiInlineLine,
    x: CssPixels,
) -> FfiInlineCaretPosition {
    read_arena(host, node_read(), (line, x), |arena, (line, x)| {
        let rows = arena.paintable_rows();
        FfiInlineCaretPosition::of(line.line(&rows).and_then(|line| {
            InlineCursor::new(&rows).position_at_inline_coordinate(line, x, |fragment| produces_caret(&rows, fragment))
        }))
    })
}

/// The caret position at `edge` of `line`, among its editable line content where `editable_only`.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_caret_position_at_line_edge(
    host: &DocumentHost,
    line: FfiInlineLine,
    edge: LineEdge,
    editable_only: bool,
) -> FfiInlineCaretPosition {
    read_arena(
        host,
        node_read(),
        (line, edge, editable_only),
        |arena, (line, edge, editable_only)| {
            let rows = arena.paintable_rows();
            let is_editable = |node: NodeSlotId| {
                [Some(node), rows.node_parent_if_live(node)]
                    .into_iter()
                    .flatten()
                    .any(|node| rows.node_has_dom_paint_fact(node, DomPaintFact::EditableOrEditingHost))
            };
            FfiInlineCaretPosition::of(line.line(&rows).and_then(|line| {
                InlineCursor::new(&rows).line_edge(line, edge, |fragment| {
                    produces_caret(&rows, fragment) && (!editable_only || is_editable(fragment.layout_node))
                })
            }))
        },
    )
}

/// The middle of `line` along the block axis.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_caret_line_block_coordinate(
    host: &DocumentHost,
    line: FfiInlineLine,
    out: &mut CssPixels,
) {
    *out = read_arena(host, node_read(), line, |arena, line| {
        let rows = arena.paintable_rows();
        line.line(&rows)
            .map(|line| InlineCursor::new(&rows).line_block_coordinate(line))
            .unwrap_or_default()
    });
}

/// The inline coordinate of the caret at `position`, which a move to another line keeps.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_caret_inline_coordinate(
    host: &DocumentHost,
    position: FfiCaretPosition,
    out: &mut FfiOptionalCssPixels,
) {
    *out = read_arena(host, node_read(), position, |arena, position| {
        let rows = arena.paintable_rows();
        let cursor = InlineCursor::new(&rows);
        FfiOptionalCssPixels::of(
            navigation_candidates(&cursor, &rows, &position)
                .find_map(|candidate| cursor.inline_coordinate_of(candidate)),
        )
    });
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
#[expect(dead_code, reason = "C++ constructs the variants")]
pub enum FfiTextCaretMovement {
    NextLine,
    PreviousLine,
    NextPage,
    PreviousPage,
    LineStart,
    LineEnd,
    LineLeft,
    LineRight,
}

#[repr(C)]
#[derive(Default)]
pub struct FfiTextCaretMove {
    pub found: bool,
    pub offset: usize,
    pub downstream: bool,
    /// For a move to another line, the inline coordinate it kept, for the next such move to keep.
    pub inline_coordinate: FfiOptionalCssPixels,
}

/// Moves the caret at `offset` in the DOM text (`text_length` long) whose layout node is `primary`: to an edge of its
/// line, or to the position nearest `inline_coordinate` (or else the caret's own) on the next or previous line, or on
/// the farthest line no more than `page_distance` away. A move past the last or first line goes to the end or start of
/// the text.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn render_state_text_caret_move(
    host: &DocumentHost,
    primary: NodeSlotId,
    offset: usize,
    downstream: bool,
    text_length: usize,
    movement: FfiTextCaretMovement,
    inline_coordinate: FfiOptionalCssPixels,
    page_distance: CssPixels,
    out: &mut FfiTextCaretMove,
) {
    let args = (
        primary,
        offset,
        downstream,
        text_length,
        movement,
        inline_coordinate,
        page_distance,
    );
    *out = read_arena(host, node_read(), args, |arena, args| {
        use FfiTextCaretMovement::*;
        let (text, offset, downstream, text_length, movement, inline_coordinate, page_distance) = args;
        let rows = arena.paintable_rows();
        let cursor = InlineCursor::new(&rows);
        let fragments = arena.text_fragments(text);
        let in_text = |node: &NodeSlotId| fragments.as_slice().contains(node);
        let moved = |position, inline_coordinate| match position {
            Some(CaretPosition::Text {
                text,
                offset,
                downstream,
            }) if in_text(&text) => FfiTextCaretMove {
                found: true,
                offset,
                downstream,
                inline_coordinate: FfiOptionalCssPixels::of(inline_coordinate),
            },
            _ => FfiTextCaretMove::default(),
        };
        let mut position = CaretPosition::Text {
            text,
            offset,
            downstream,
        };
        let Some(start) = cursor.line_of(text, offset, downstream) else {
            return FfiTextCaretMove::default();
        };
        let accepts = |fragment: &FragmentRecord| in_text(&fragment.layout_node);
        let edge = |edge| moved(cursor.line_edge(start.line, edge, accepts), None);
        let forward = match movement {
            LineStart => return edge(LineEdge::Start),
            LineEnd => return edge(LineEdge::End),
            LineLeft => return edge(LineEdge::Left),
            LineRight => return edge(LineEdge::Right),
            NextLine | NextPage => true,
            PreviousLine | PreviousPage => false,
        };
        let page_distance = matches!(movement, NextPage | PreviousPage).then_some(page_distance);
        let goal = if inline_coordinate.has_value {
            inline_coordinate.value
        } else if let Some(coordinate) = cursor.caret_inline_coordinate(start.line, position) {
            coordinate
        } else {
            return FfiTextCaretMove::default();
        };
        let start_coordinate = cursor.line_block_coordinate(start.line);
        let mut line = start.line;
        while let Some(next) = cursor.adjacent_line(line, forward) {
            let distance = cursor.line_block_coordinate(next) - start_coordinate;
            if page_distance.is_some_and(|page_distance| distance.max(-distance) > page_distance) {
                return moved(Some(position), Some(goal));
            }
            let Some(position_on_next) = cursor.position_at_inline_coordinate(next, goal, accepts) else {
                return moved(Some(position), Some(goal));
            };
            position = position_on_next;
            line = next;
            if page_distance.is_none() {
                return moved(Some(position), Some(goal));
            }
        }
        let offset = if forward { text_length } else { 0 };
        moved(
            Some(CaretPosition::Text {
                text,
                offset,
                downstream: true,
            }),
            Some(goal),
        )
    });
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_grid_layout_json(
    host: &DocumentHost,
    paintable: NodeSlotId,
    container_node_id: i64,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let data = {
        read_arena(host, node_read(), paintable, |arena, paintable| {
            if !arena.paintable_row_is_populated(paintable) {
                return None;
            }
            crate::painting::paintable_geometry::committed_grid_layout_data(arena, paintable)
        })
    };
    if let Some(data) = data {
        let json = crate::painting::devtools_layout::serialize_grid_layout(&data, container_node_id);
        unsafe { consume(context, json.as_ptr(), json.len()) };
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_flex_layout_json(
    host: &DocumentHost,
    paintable: NodeSlotId,
    container_node_id: i64,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    document_context: *const c_void,
    resolve_node_id: unsafe extern "C" fn(*const c_void, u32) -> i64,
) {
    let data = {
        read_arena(host, node_read(), paintable, |arena, paintable| {
            if !arena.paintable_row_is_populated(paintable) {
                return None;
            }
            crate::painting::paintable_geometry::committed_flex_layout_data(arena, paintable)
        })
    };
    if let Some(data) = data {
        // SAFETY: The host answers synchronously from the document the paintable belongs to.
        let json = crate::painting::devtools_layout::serialize_flex_layout(&data, container_node_id, |style_node| {
            let node_id = unsafe { resolve_node_id(document_context, style_node.raw()) };
            (node_id >= 0).then_some(node_id)
        });
        unsafe { consume(context, json.as_ptr(), json.len()) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_paintable_used_grid_tracks(
    host: &DocumentHost,
    paintable: NodeSlotId,
    columns: bool,
) -> *const c_void {
    read_arena(
        host,
        node_read(),
        (paintable, columns),
        |arena, (paintable, columns)| {
            if !arena.paintable_row_is_populated(paintable) {
                return std::ptr::null();
            }
            let Some(tracks) = crate::painting::paintable_geometry::committed_used_grid_tracks(arena, paintable) else {
                return std::ptr::null();
            };
            let list = if columns { &tracks.columns } else { &tracks.rows };
            crate::css::style_value_arc::StyleValueArc::into_raw(crate::css::style_value_arc::StyleValueArc::new(
                list.style_value(),
            ))
            .cast()
        },
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_main_visual_context_tree_retain(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> *const c_void {
    fn retain(tree: Option<&std::sync::Arc<crate::painting::visual_context::VisualContextTree>>) -> *const c_void {
        tree.map_or(std::ptr::null(), |tree| {
            std::sync::Arc::into_raw(std::sync::Arc::clone(tree)).cast()
        })
    }
    if let Some(tree) = host.read_known_rows(|rows, _| retain(rows.visual_context_tree.as_ref())) {
        return tree;
    }
    read_arena(host, read, (), |arena, ()| {
        retain(arena.paint_state().visual_context.tree.as_ref())
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_has_visual_context_tree(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> bool {
    read_arena(host, read, (), |arena, ()| {
        let paint_state = arena.paint_state();
        paint_state.visual_context.tree.is_some()
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_visual_context_tree_structural_epoch(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> u64 {
    let known = host.read_known_rows(|rows, _| {
        rows.visual_context_tree
            .as_ref()
            .map_or(0, |tree| tree.structural_epoch)
    });
    known.unwrap_or_else(|| {
        read_arena(host, read, (), |arena, ()| {
            let paint_state = arena.paint_state();
            paint_state.visual_context.structural_epoch()
        })
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn render_state_visual_context_tree_has_visual_animations(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> bool {
    read_arena(host, read, (), |arena, ()| {
        let paint_state = arena.paint_state();
        paint_state
            .visual_context
            .tree
            .as_deref()
            .is_some_and(|tree| tree.has_visual_animations())
    })
}

/// Hands `push_keyframe_color` the keyframe colors of each background color animation the visual context tree runs on
/// the effect node, in keyframe order.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_background_color_animation_keyframe_colors(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    effect_node_index: u32,
    context: *mut c_void,
    push_keyframe_color: unsafe extern "C" fn(*mut c_void, libgfx_rust::Color),
) {
    use crate::painting::host::FfiVisualAnimationTargetKind;
    use crate::painting::visual_animation::VisualAnimationValue;
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(
            host,
            read,
            (effect_node_index, context, push_keyframe_color),
            |arena, (effect_node_index, context, push_keyframe_color)| {
                let paint_state = arena.paint_state();
                let Some(tree) = paint_state.visual_context.tree.as_deref() else {
                    return;
                };
                let animations_of_node = tree.visual_animations().iter().filter(|animation| {
                    animation.target_kind == FfiVisualAnimationTargetKind::BackgroundColor
                        && animation.node_indices.contains(&effect_node_index)
                });
                for keyframe in animations_of_node.flat_map(|animation| &animation.keyframes) {
                    if let VisualAnimationValue::BackgroundColor(color) = keyframe.value {
                        push_keyframe_color(context, color);
                    }
                }
            },
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_animation_effect_state_create() -> *mut CompositorAnimationEffectState {
    Box::into_raw(Box::new(CompositorAnimationEffectState::default()))
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_animation_effect_state_destroy(state: OwnedBox<CompositorAnimationEffectState>) {
    drop(state);
}

/// Builds the animation of the request's target kind for the effect and keeps it pending with the
/// effect. The target's nodes come from the arena's main tree.
///
/// # Safety
///
/// `document_host` must be a live document host on its document's thread, and the request and host, with every range
/// they address, live for the call. The host's callbacks run synchronously and may not touch the arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_build(
    state: &mut CompositorAnimationEffectState,
    document_host: &DocumentHost,
    request: *const crate::painting::host::FfiCompositorAnimationRequest,
    host: &crate::painting::host::FfiCompositorAnimationHost,
) -> crate::painting::host::FfiCompositorAnimationBuildOutcome {
    use crate::painting::visual_animation_builder::{Host, Request};
    // SAFETY: Guaranteed by the caller.
    let mut request = unsafe { Request::new(&*request) };
    let host = Host::new(host);
    let layout_node = request.layout_node();
    if request.target_kind() == crate::painting::host::FfiVisualAnimationTargetKind::Transform {
        let additional_element_transform = read_arena(document_host, node_read(), layout_node, |arena, layout_node| {
            crate::painting::paintable_geometry::committed_svg_additional_element_transform(arena, layout_node)
        });
        if let Some(transform) = additional_element_transform {
            request.set_additional_translation(transform.e, transform.f);
        }
    }
    state.build(&request, &host, |kind| {
        read_arena(
            document_host,
            node_read(),
            (layout_node, kind),
            |arena, (layout_node, kind)| {
                let tree = arena.paint_state().visual_context.tree.clone();
                arena.paintable_visual_animation_target_indices(layout_node, tree.as_deref(), kind)
            },
        )
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_animation_effect_discard_pending(
    state: &mut CompositorAnimationEffectState,
    kind: crate::painting::host::FfiVisualAnimationTargetKind,
) {
    state.discard_pending(kind);
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_animation_effect_has_pending(state: &CompositorAnimationEffectState) -> bool {
    state.has_pending()
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_animation_effect_clear_pending(state: &mut CompositorAnimationEffectState) {
    state.clear_pending();
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_animation_effect_has_retained(state: &CompositorAnimationEffectState) -> bool {
    state.has_retained()
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_animation_effect_clear_retained(state: &mut CompositorAnimationEffectState) {
    state.clear_retained();
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_animation_effect_reset(state: &mut CompositorAnimationEffectState) {
    state.reset();
}

/// # Safety
///
/// The request and host, with every range they address, must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_only_translates_horizontally(
    request: *const crate::painting::host::FfiCompositorAnimationRequest,
    host: &crate::painting::host::FfiCompositorAnimationHost,
) -> bool {
    use crate::painting::visual_animation_builder::{Host, Request, effect_only_translates_horizontally};
    let request = unsafe { Request::new(&*request) };
    effect_only_translates_horizontally(&request, &Host::new(host))
}

/// # Safety
///
/// The request and host, with every range they address, must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_transform_preserves_axes(
    request: *const crate::painting::host::FfiCompositorAnimationRequest,
    host: &crate::painting::host::FfiCompositorAnimationHost,
) -> bool {
    use crate::painting::visual_animation_builder::{Host, Request, effect_transform_preserves_axes};
    let request = unsafe { Request::new(&*request) };
    effect_transform_preserves_axes(&request, &Host::new(host))
}

/// Starts an update pass: the effects publish into an empty document list.
///
/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously; `bytes` must point at
/// `length` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_bytes(sink: *mut c_void, bytes: *const u8, length: usize) {
    // SAFETY: `sink` is the Vec pointer handed out by the host callback wrapper; the caller
    // guarantees the byte range.
    let vec = unsafe { &mut *sink.cast::<Vec<u8>>() };
    if length > 0 {
        vec.extend_from_slice(unsafe { ak::slice_from_raw_parts(bytes, length) });
    }
}

/// # Safety
///
/// Every pointer in `primitive` must be readable for the length that accompanies it, each UTF-16
/// view must satisfy [`FfiUtf16View::units`], and each non-null component transfer table must
/// hold 256 bytes.
unsafe fn svg_filter_primitive_from_ffi(primitive: &FfiSvgFilterPrimitive) -> SvgFilterPrimitive {
    let name = |view: FfiUtf16View| view.to_utf16().unwrap_or_default();
    SvgFilterPrimitive {
        values: primitive.values,
        in1: name(primitive.in1),
        in2: name(primitive.in2),
        result: name(primitive.result),
        merge_inputs: unsafe { ak::slice_from_raw_parts(primitive.merge_inputs, primitive.merge_input_count) }
            .iter()
            .map(|view| name(*view))
            .collect(),
        color_matrix_values: unsafe {
            ak::slice_from_raw_parts(primitive.color_matrix_values, primitive.color_matrix_value_count)
        }
        .to_vec(),
        component_transfer_tables: primitive.component_transfer_tables.map(|table| {
            (!table.is_null()).then(|| {
                let table = unsafe { ak::slice_from_raw_parts(table, 256) };
                Box::new(<[u8; 256]>::try_from(table).expect("a component transfer table holds 256 entries"))
            })
        }),
        image_frame: (!primitive.image_frame.is_null())
            .then(|| unsafe { libgfx_rust::image_frame::ImageFrameHandle::retain(primitive.image_frame) }),
    }
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously; `primitive` must be
/// readable, with every pointer in it readable for the length that accompanies it, each UTF-16
/// view satisfying [`FfiUtf16View::units`], each non-null component transfer table holding
/// 256 bytes, and `image_frame` null or pointing to a live `Gfx::DecodedImageFrame`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_svg_filter_primitive(
    sink: *mut c_void,
    primitive: *const FfiSvgFilterPrimitive,
) {
    let primitives = unsafe { &mut *sink.cast::<Vec<SvgFilterPrimitive>>() };
    primitives.push(unsafe { svg_filter_primitive_from_ffi(&*primitive) });
}

/// Notes that the SVG paint resources the document's rows enrolled may have changed, and answers whether any row
/// enrolled one, which only then are synced again. A row a frame in flight enrolls is synced as it enrolls.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_note_svg_paint_resources_changed(host: &DocumentHost) -> bool {
    let enrolled = host.svg_paint_resources_may_be_enrolled();
    if enrolled {
        queue(host, PaintChange::SvgPaintResourcesChanged);
    }
    enrolled
}
