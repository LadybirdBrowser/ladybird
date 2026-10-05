/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::ffi_support::FfiUtf16View;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::svg_formatting_context;
use crate::layout::used_values::FfiCssPixelPoint;
use crate::layout::used_values::FfiCssPixelRect;
use crate::layout::used_values::FfiCssPixelSize;
use crate::painting::filter_bytes::{FfiFilterFunction, filter_functions_graph};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::host::visual_context::FfiSvgFilterPrimitive;
use crate::painting::paint_changes::{PaintChange, queue};
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::paintable_data::*;
use crate::painting::paintable_rows::{PaintableRowsRead, with_inline_pieces};
use crate::painting::rect_to_viewport_transform::RectToViewportTransform;
use crate::painting::scroll_chain::ViewportWheelOverflow;
use crate::painting::svg_filter::SvgFilterPrimitive;
use crate::render_state::{DocumentHost, RenderWait};
use libcompositing_rust::ffi::{ffi_slice, tree_from_handle};
use libgfx_rust::filter::Filter;
use std::ffi::c_void;

mod main_thread_entries;

pub(crate) use main_thread_entries::MainThreadFfiEntry;

crate::render_state::held_node_entries!();

/// Answers `answer` from the render state of `host`'s document and `args`, as of every change the host queued,
/// spending `wait`: the host's painting, input, editing and devtools code reads what the render state laid out and
/// painted.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
pub(crate) unsafe fn read_arena<A, R>(
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

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiScrollbarData {
    pub gutter_rect: FfiCssPixelRect,
    pub thumb_rect: FfiCssPixelRect,
    pub track_rect: FfiCssPixelRect,
    pub thumb_travel_to_scroll_ratio: f64,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiOptionalScrollbarData {
    pub has_value: bool,
    pub value: FfiScrollbarData,
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
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_row(
    host: &DocumentHost,
    slot: NodeSlotId,
    row: &mut PaintableData,
) -> bool {
    let known = host.read_known_rows(|rows, _| {
        rows.paintable
            .paintable_row_is_populated(slot)
            .then(|| *rows.paintable.paintable_data(slot))
    });
    // SAFETY: Guaranteed by the caller.
    let answer = known.unwrap_or_else(|| unsafe {
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
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_has_paintable_row(host: &DocumentHost, slot: NodeSlotId) -> bool {
    if let Some(populated) = host.known_paintable_row_is_populated(slot) {
        return populated;
    }
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), slot, |arena, slot| {
            arena.paintable_rows().paintable_row_is_populated(slot)
        })
    }
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

/// # Safety
///
/// `arena` must be a live arena used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_scrollable_overflow(
    host: &DocumentHost,
    slot: NodeSlotId,
) -> FfiOptionalOverflowData {
    // The host reads the overflow from the rows it holds, where they were measured and still read as the arena.
    let known = host.read_known_rows(|rows, source| {
        rows.overflow_is_measured()
            .then(|| FfiOptionalOverflowData::of(source, slot))
    });
    if let Some(Some(overflow)) = known {
        return overflow;
    }
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), slot, |arena, slot| {
            arena.measure_scrollable_overflow();
            FfiOptionalOverflowData::of(&arena.paintable_rows(), slot)
        })
    }
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
        let mut value = rows.committed_side_data(slot).overflow_relative_to_padding_box;
        value.rect = rect.into();
        Self { has_value: true, value }
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

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_svg_viewport_size(
    host: &DocumentHost,
    slot: NodeSlotId,
) -> FfiCssPixelSize {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), slot, |arena, slot| {
            let paintable_rows = arena.paintable_rows();
            if !paintable_rows.paintable_row_is_populated(slot) {
                return FfiCssPixelSize::default();
            }
            crate::painting::paintable_geometry::committed_svg_viewport_size(&paintable_rows, slot)
        })
    }
}

#[repr(C)]
pub struct FfiOptionalAffineTransform {
    pub has_value: bool,
    pub transform: svg_formatting_context::FfiAffineTransform,
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_svg_viewport_transform(
    host: &DocumentHost,
    slot: NodeSlotId,
) -> FfiOptionalAffineTransform {
    // SAFETY: Guaranteed by the caller.
    unsafe {
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
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_transform_reference_box(
    host: &DocumentHost,
    slot: NodeSlotId,
) -> FfiCssPixelRect {
    fn reference_box(rows: &impl PaintRead, slot: NodeSlotId) -> FfiCssPixelRect {
        committed_transform_reference_box(rows, slot).map_or_else(FfiCssPixelRect::default, Into::into)
    }
    if let Some(rect) = host.read_rows_between_jobs(node_read(), |rows| reference_box(rows, slot)) {
        return rect;
    }
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), slot, |arena, slot| {
            reference_box(&arena.paintable_rows(), slot)
        })
    }
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

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_physical_overflow_directions(
    host: &DocumentHost,
    paintable: NodeSlotId,
) -> FfiPhysicalOverflowDirections {
    // SAFETY: Guaranteed by the caller.
    unsafe {
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
}

/// # Safety
///
/// `arena` must be a live layout arena handle used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_background_color_can_be_compositor_animated(
    host: &DocumentHost,
    slot: NodeSlotId,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), slot, |arena, slot| {
            crate::painting::record::paint::background_resolution::background_color_can_be_compositor_animated(
                &arena.paintable_rows(),
                slot,
                crate::layout::viewport_propagation::root_background_source(arena),
            )
        })
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_visual_context_node_count(
    host: &DocumentHost,
    slot: NodeSlotId,
    list: crate::painting::host::FfiVisualContextBoxNodeList,
) -> usize {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), (slot, list), |arena, (slot, list)| {
            use crate::painting::host::FfiVisualContextBoxNodeList;
            arena.with_paintable_visual_context_node_handles(slot, |handles| match list {
                FfiVisualContextBoxNodeList::SpatialNodes => handles.spatial.len(),
                FfiVisualContextBoxNodeList::ClipNodes => handles.clip_handles().count(),
                FfiVisualContextBoxNodeList::EffectNodes => handles.effects.len(),
            })
        })
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread; `out` must have room for `capacity` indices.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_visual_context_copy_node_indices(
    host: &DocumentHost,
    slot: NodeSlotId,
    list: crate::painting::host::FfiVisualContextBoxNodeList,
    out: *mut u32,
    capacity: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let indices = unsafe {
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
    assert!(indices.len() <= capacity);
    // SAFETY: the caller warrants `capacity` writable indices behind `out`.
    unsafe { std::ptr::copy_nonoverlapping(indices.as_ptr(), out, indices.len()) };
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

/// The index of the sticky node the accumulated visual context tree holds for `paintable`, which
/// is where the scroll state snapshot keeps its resolved sticky offset, or `u32::MAX` when the tree
/// holds none.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_sticky_spatial_node_index(host: &DocumentHost, paintable: NodeSlotId) -> u32 {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), paintable, |arena, paintable| {
            let paint_state = arena.paint_state().borrow();
            paint_state
                .visual_context
                .scroll_state
                .states()
                .iter()
                .find(|state| state.is_sticky && state.paintable == paintable)
                .map_or(u32::MAX, |state| state.node_index.0)
        })
    }
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
    /// The recording flies beside the event loop.
    InFlight,
}

/// Why a rendering update's frame (its style transaction, or its recording) may not fly beside the
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
    /// The rendering update's style transaction flew, and the update ends between two tasks: its recording is made in
    /// step, so that the frame flies once.
    StyleFlew,
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
}

impl FfiPresentedRecording {
    /// What the host reads of a recording published with `output`, whose display list `output` holds for as long as
    /// the host reads it.
    pub(crate) fn of_output(output: &crate::painting::record::RecordingOutput) -> Self {
        Self {
            is_identical_to_published_recording: output.is_identical_to_published_recording,
            has_blocking_wheel_event_listeners: output.has_blocking_wheel_event_listeners,
            display_list: std::sync::Arc::as_ptr(&output.display_list).cast(),
        }
    }

    /// What the host reads of the last recording the document took in.
    pub(crate) fn of_last_recording(arena: &LayoutNodeArena) -> Self {
        let paint_state = arena.paint_state().borrow();
        let recording = paint_state.last_recording.as_deref();
        Self {
            is_identical_to_published_recording: recording
                .is_some_and(|recording| recording.is_identical_to_published_recording),
            has_blocking_wheel_event_listeners: recording
                .is_some_and(|recording| recording.has_blocking_wheel_event_listeners),
            display_list: recording.map_or(std::ptr::null(), |recording| {
                std::sync::Arc::as_ptr(&recording.display_list).cast()
            }),
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

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_color_stop(
    sink: *mut c_void,
    color: libgfx_rust::Color,
    position: f32,
) {
    let sink = unsafe { &mut *sink.cast::<crate::painting::svg_paint_resources::PublishedSvgPaintServer>() };
    if let crate::painting::svg_paint_resources::PublishedSvgPaintServer::Gradient(gradient) = sink {
        gradient
            .stops
            .push(crate::painting::svg_paint_resources::PublishedSvgGradientStop { color, position });
    }
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously, and `description`
/// must be readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_svg_paint_resources_push_gradient(
    sink: *mut c_void,
    description: *const crate::painting::host::FfiSvgGradientDescription,
) {
    let sink = unsafe { &mut *sink.cast::<crate::painting::svg_paint_resources::PublishedSvgPaintServer>() };
    *sink = crate::painting::svg_paint_resources::PublishedSvgPaintServer::Gradient(
        crate::painting::svg_paint_resources::PublishedSvgGradient {
            description: unsafe { *description },
            stops: Vec::new(),
        },
    );
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously, `description` must be
/// readable, and `css_transform_entries` must point at `css_transform_count` readable
/// `ComputedResolvedTransform` values of a live computed style.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_svg_paint_resources_push_pattern(
    sink: *mut c_void,
    description: *const crate::painting::host::FfiSvgPatternDescription,
    css_transform_entries: *const c_void,
    css_transform_count: usize,
) {
    let sink = unsafe { &mut *sink.cast::<crate::painting::svg_paint_resources::PublishedSvgPaintServer>() };
    *sink = crate::painting::svg_paint_resources::PublishedSvgPaintServer::Pattern(
        crate::painting::svg_paint_resources::PublishedSvgPattern {
            description: unsafe { *description },
            css_transform: unsafe {
                ffi_slice(
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
    /// The `FfiColorResolutionInput` the gradient's color stops resolve against.
    pub gradient_stop_color_resolution_input: *const c_void,
}

/// # Safety
///
/// `inputs`, and the gradient style value and color resolution input it points at, must be
/// live for the call. `consume` is called synchronously and takes ownership of the command
/// storage and visual context tree handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_web_record_image_paint_display_list(
    inputs: *const FfiImagePaintRecordInputs,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const c_void, *const c_void),
) {
    use crate::css::color_resolution::{
        FfiColorResolutionInput, relative_color_context_from_ffi, resolution_input_from_ffi,
    };
    use crate::painting::display_list::commands::{DisplayListResourceId, ImageFrameResourceId};
    use crate::painting::display_list::device_pixels::DevicePixelConverter;
    use crate::painting::display_list::recorder::DisplayListRecorder;
    use crate::painting::record::paint::gradient_resolution::{
        record_resolved_gradient_fill, resolve_gradient_paint_with_input,
    };
    use crate::painting::visual_context::{TransformData, TransformDataRole, VisualContextTree};
    use libgfx_rust::{CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint};
    let inputs = unsafe { &*inputs };
    let tree = VisualContextTree::create(TransformData {
        matrix: FloatMatrix4x4::identity(),
        origin: FloatPoint::default(),
        sorting_context_root_index: None,
        flattens_inherited_transform: false,
        role: TransformDataRole::CssTransform,
        synthetic_plane: false,
        establishes_sorting_context: false,
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
            // SAFETY: the host keeps the gradient style value and its color resolution input
            // live for the duration of the call.
            let (gradient_style_value, color_resolution_input) = unsafe {
                (
                    &*inputs
                        .gradient_style_value
                        .cast::<crate::css::style_value::StyleValueData>(),
                    &*inputs
                        .gradient_stop_color_resolution_input
                        .cast::<FfiColorResolutionInput>(),
                )
            };
            let relative_color_channels = relative_color_context_from_ffi(color_resolution_input);
            // SAFETY: the borrowed input outlives the resolution below.
            let color_input = unsafe { resolution_input_from_ffi(color_resolution_input, &relative_color_channels) };
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

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_computed_svg_path(
    host: &DocumentHost,
    paintable: NodeSlotId,
) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), paintable, |arena, paintable| {
            let paintable_rows = arena.paintable_rows();
            if !paintable_rows.paintable_row_is_populated(paintable) {
                return std::ptr::null();
            }
            crate::painting::paintable_geometry::committed_svg_path(&paintable_rows, paintable)
                .map_or(std::ptr::null(), |path| path.as_raw())
        })
    }
}

#[repr(C)]
pub struct FfiCaretRectResult {
    pub found: bool,
    pub rect: FfiCssPixelRect,
    pub style_source: NodeSlotId,
    pub owner_paintable: NodeSlotId,
    pub nearest_self_painting_inline: NodeSlotId,
}

#[repr(C)]
pub struct FfiEmptyLineCaretRect {
    pub has_value: bool,
    pub rect: FfiCssPixelRect,
    pub style_source: NodeSlotId,
}

#[repr(C)]
pub struct FfiRectToViewportTransform {
    pub visual_context_tree: *const c_void,
    pub scroll_offsets: *const libgfx_rust::FloatPoint,
    pub scroll_offsets_len: usize,
    pub device_pixels_per_css_pixel: f32,
}

/// SAFETY: A non-null `visual_context_tree` must be a live retained tree handle and `scroll_offsets`
/// must address `scroll_offsets_len` points for as long as the returned borrow lives.
pub(crate) unsafe fn rect_to_viewport_transform_from_ffi(
    transform: &FfiRectToViewportTransform,
) -> Option<RectToViewportTransform<'_>> {
    if transform.visual_context_tree.is_null() {
        return None;
    }
    Some(RectToViewportTransform {
        visual_context_tree: unsafe { tree_from_handle(transform.visual_context_tree) },
        scroll_offsets: unsafe { ffi_slice(transform.scroll_offsets, transform.scroll_offsets_len) },
        device_pixels_per_css_pixel: transform.device_pixels_per_css_pixel,
    })
}

/// Whether a row has ever been given a style with `content-visibility: auto`, which is when a
/// layout commit collects the boxes with it.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_may_have_auto_content_visibility(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { read_arena(host, read, (), |arena, ()| arena.may_have_auto_content_visibility()) }
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
    // SAFETY: Guaranteed by the caller.
    let boxes = unsafe {
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

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_inline_paintable_has_content_pieces(
    host: &DocumentHost,
    inline_paintable: NodeSlotId,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), inline_paintable, |arena, inline_paintable| {
            let mut has_content = false;
            with_inline_pieces(&arena.paintable_rows(), inline_paintable, |piece, _| {
                if !piece.is_geometry_only_placeholder {
                    has_content = true;
                    return false;
                }
                true
            });
            has_content
        })
    }
}

#[repr(C)]
pub struct FfiOptionalCssPixelPoint {
    pub has_value: bool,
    pub x: CssPixels,
    pub y: CssPixels,
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_inline_paintable_first_piece_position(
    host: &DocumentHost,
    inline_paintable: NodeSlotId,
) -> FfiOptionalCssPixelPoint {
    // SAFETY: Guaranteed by the caller.
    unsafe {
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
            let root_position = crate::painting::paintable_geometry::absolute_position(&paintable_rows, root);
            let border_widths = crate::painting::paintable_geometry::committed_border(arena, inline_paintable);
            let padding_widths = crate::painting::paintable_geometry::committed_padding(arena, inline_paintable);
            with_inline_pieces(&paintable_rows, inline_paintable, |piece, _data| {
                let border_rect = CssPixelRect::from(piece.border_box_rect);
                let rect = if piece.is_geometry_only_placeholder {
                    border_rect
                } else {
                    let padding_rect = piece.shrunken_by_present_edges(border_rect, border_widths);
                    piece.shrunken_by_present_edges(padding_rect, padding_widths)
                };
                result.has_value = true;
                result.x = rect.x + root_position.x;
                result.y = rect.y + root_position.y;
                false
            });
            result
        })
    }
}

#[repr(C)]
pub struct FfiVisualLine {
    pub start_offset: usize,
    pub end_offset: usize,
    pub end_offset_with_trailing_whitespace: usize,
    pub has_fragments: bool,
    pub owner_paintable: u32,
    pub line_index: u32,
}

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
    // SAFETY: Guaranteed by the caller.
    let lines = unsafe {
        read_arena(host, node_read(), primary, |arena, primary| {
            let fragments = arena.text_fragments(primary);
            crate::painting::visual_lines::collect_visual_lines(&arena.paintable_rows(), fragments.as_slice())
        })
    };
    for line in lines {
        // SAFETY: The consumer copies the POD line synchronously.
        unsafe {
            push(
                context,
                FfiVisualLine {
                    start_offset: line.start_offset,
                    end_offset: line.end_offset,
                    end_offset_with_trailing_whitespace: line.end_offset_with_trailing_whitespace,
                    has_fragments: line.has_fragments,
                    owner_paintable: line.owner.index,
                    line_index: line.line_index,
                },
            );
        }
    }
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

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_text_has_rendered_text_before(
    host: &DocumentHost,
    primary: NodeSlotId,
    offset: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), (primary, offset), |arena, (primary, offset)| {
            let fragments = arena.text_fragments(primary);
            let node_slots = fragments.as_slice();
            has_rendered_text_matching(&arena.paintable_rows(), node_slots, |fragment| {
                fragment.dom_start_offset_in_node < offset
            })
        })
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_text_has_rendered_text_after(
    host: &DocumentHost,
    primary: NodeSlotId,
    offset: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), (primary, offset), |arena, (primary, offset)| {
            let fragments = arena.text_fragments(primary);
            let node_slots = fragments.as_slice();
            has_rendered_text_matching(&arena.paintable_rows(), node_slots, |fragment| {
                fragment.dom_end_offset_in_node > offset
            })
        })
    }
}

#[repr(C)]
pub struct FfiOptionalCssPixels {
    pub has_value: bool,
    pub value: CssPixels,
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_visual_line_caret_inline_coordinate(
    host: &DocumentHost,
    owner_paintable: u32,
    line_index: u32,
    primary: NodeSlotId,
    offset: usize,
) -> FfiOptionalCssPixels {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(
            host,
            node_read(),
            (owner_paintable, line_index, primary, offset),
            |arena, (owner_paintable, line_index, primary, offset)| {
                let paintable_rows = arena.paintable_rows();
                let fragments = arena.text_fragments(primary);
                let node_slots = fragments.as_slice();
                let coordinate = crate::painting::visual_lines::caret_inline_coordinate(
                    &paintable_rows,
                    owner_paintable,
                    line_index,
                    node_slots,
                    offset,
                );
                match coordinate {
                    Some(value) => FfiOptionalCssPixels { has_value: true, value },
                    None => FfiOptionalCssPixels {
                        has_value: false,
                        value: CssPixels::from_raw(0),
                    },
                }
            },
        )
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_visual_line_offset_closest_to_inline_coordinate(
    host: &DocumentHost,
    owner_paintable: u32,
    line_index: u32,
    primary: NodeSlotId,
    inline_coordinate: CssPixels,
    fallback_offset: usize,
) -> usize {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(
            host,
            node_read(),
            (owner_paintable, line_index, primary, inline_coordinate, fallback_offset),
            |arena, (owner_paintable, line_index, primary, inline_coordinate, fallback_offset)| {
                let paintable_rows = arena.paintable_rows();
                let fragments = arena.text_fragments(primary);
                let node_slots = fragments.as_slice();
                crate::painting::visual_lines::offset_closest_to_inline_coordinate(
                    &paintable_rows,
                    owner_paintable,
                    line_index,
                    node_slots,
                    inline_coordinate,
                )
                .unwrap_or(fallback_offset)
            },
        )
    }
}

#[repr(C)]
pub struct FfiOptionalCssPixelRect {
    pub has_value: bool,
    pub rect: FfiCssPixelRect,
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
    // SAFETY: Guaranteed by the caller.
    let data = unsafe {
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
    // SAFETY: Guaranteed by the caller.
    let data = unsafe {
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

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_paintable_used_grid_tracks(
    host: &DocumentHost,
    paintable: NodeSlotId,
    columns: bool,
) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(
            host,
            node_read(),
            (paintable, columns),
            |arena, (paintable, columns)| {
                if !arena.paintable_row_is_populated(paintable) {
                    return std::ptr::null();
                }
                let Some(tracks) = crate::painting::paintable_geometry::committed_used_grid_tracks(arena, paintable)
                else {
                    return std::ptr::null();
                };
                let list = if columns { &tracks.columns } else { &tracks.rows };
                std::sync::Arc::into_raw(std::sync::Arc::new(list.style_value())).cast()
            },
        )
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread. The
/// returned tree is retained; the caller owns one reference and releases it with
/// `visual_context_tree_release`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_main_visual_context_tree_retain(
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
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, read, (), |arena, ()| {
            retain(arena.paint_state().borrow().visual_context.tree.as_ref())
        })
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_has_visual_context_tree(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, read, (), |arena, ()| {
            let paint_state = arena.paint_state().borrow();
            paint_state.visual_context.tree.is_some()
        })
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_visual_context_tree_structural_epoch(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> u64 {
    let known = host.read_known_rows(|rows, _| {
        rows.visual_context_tree
            .as_ref()
            .map_or(0, |tree| tree.structural_epoch)
    });
    // SAFETY: Guaranteed by the caller.
    known.unwrap_or_else(|| unsafe {
        read_arena(host, read, (), |arena, ()| {
            let paint_state = arena.paint_state().borrow();
            paint_state.visual_context.structural_epoch()
        })
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_visual_context_tree_has_visual_animations(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, read, (), |arena, ()| {
            let paint_state = arena.paint_state().borrow();
            paint_state
                .visual_context
                .tree
                .as_deref()
                .is_some_and(|tree| tree.has_visual_animations())
        })
    }
}

/// # Safety
///
/// The returned handle is owned by the caller until `compositor_animation_effect_state_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_state_create() -> *mut c_void {
    Box::into_raw(Box::new(
        crate::painting::visual_animation_builder::CompositorAnimationEffectState::default(),
    ))
    .cast()
}

/// # Safety
///
/// `state` must be a handle from `compositor_animation_effect_state_create` that is given up here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_state_destroy(state: *mut c_void) {
    if state.is_null() {
        return;
    }
    // SAFETY: The caller gives up the handle it created.
    drop(unsafe {
        Box::from_raw(state.cast::<crate::painting::visual_animation_builder::CompositorAnimationEffectState>())
    });
}

/// Builds the animation of the request's target kind for the effect and keeps it pending with the
/// effect. The target's nodes come from the arena's main tree.
///
/// # Safety
///
/// `state` must be a live effect state handle, `document_host` a live document host on its document's thread, and the request and host, with every range they address, live
/// for the call. The host's callbacks run synchronously and may not touch the arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_build(
    state: *mut c_void,
    document_host: &DocumentHost,
    request: *const crate::painting::host::FfiCompositorAnimationRequest,
    host: *const crate::painting::host::FfiCompositorAnimationHost,
) -> crate::painting::host::FfiCompositorAnimationBuildOutcome {
    use crate::painting::visual_animation_builder::{Host, Request, effect_state_from_handle};
    let state = unsafe { effect_state_from_handle(state) };
    let request = unsafe { Request::new(&*request) };
    let host = Host::new(unsafe { &*host });
    let layout_node = request.layout_node();
    state.build(&request, &host, |kind| {
        // SAFETY: Guaranteed by the caller.
        unsafe {
            read_arena(
                document_host,
                node_read(),
                (layout_node, kind),
                |arena, (layout_node, kind)| {
                    let tree = arena.paint_state().borrow().visual_context.tree.clone();
                    arena.paintable_visual_animation_target_indices(layout_node, tree.as_deref(), kind)
                },
            )
        }
    })
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_discard_pending(
    state: *mut c_void,
    kind: crate::painting::host::FfiVisualAnimationTargetKind,
) {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.discard_pending(kind);
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_has_pending(state: *mut c_void) -> bool {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.has_pending()
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_clear_pending(state: *mut c_void) {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.clear_pending();
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_has_retained(state: *mut c_void) -> bool {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.has_retained()
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_clear_retained(state: *mut c_void) {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.clear_retained();
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_reset(state: *mut c_void) {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.reset();
}

/// # Safety
///
/// The request and host, with every range they address, must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_only_translates_horizontally(
    request: *const crate::painting::host::FfiCompositorAnimationRequest,
    host: *const crate::painting::host::FfiCompositorAnimationHost,
) -> bool {
    use crate::painting::visual_animation_builder::{Host, Request, effect_only_translates_horizontally};
    let request = unsafe { Request::new(&*request) };
    effect_only_translates_horizontally(&request, &Host::new(unsafe { &*host }))
}

/// # Safety
///
/// The request and host, with every range they address, must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_transform_preserves_axes(
    request: *const crate::painting::host::FfiCompositorAnimationRequest,
    host: *const crate::painting::host::FfiCompositorAnimationHost,
) -> bool {
    use crate::painting::visual_animation_builder::{Host, Request, effect_transform_preserves_axes};
    let request = unsafe { Request::new(&*request) };
    effect_transform_preserves_axes(&request, &Host::new(unsafe { &*host }))
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
        vec.extend_from_slice(unsafe { std::slice::from_raw_parts(bytes, length) });
    }
}

/// # Safety
///
/// Every pointer in `primitive` must be readable for the length that accompanies it, each UTF-16
/// view must satisfy [`FfiUtf16View::units`], and each non-null component transfer table must
/// hold 256 bytes.
unsafe fn svg_filter_primitive_from_ffi(primitive: &FfiSvgFilterPrimitive) -> SvgFilterPrimitive {
    let name = |view: FfiUtf16View| unsafe { view.to_utf16() }.unwrap_or_default();
    SvgFilterPrimitive {
        values: primitive.values,
        in1: name(primitive.in1),
        in2: name(primitive.in2),
        result: name(primitive.result),
        merge_inputs: unsafe { ffi_slice(primitive.merge_inputs, primitive.merge_input_count) }
            .iter()
            .map(|view| name(*view))
            .collect(),
        color_matrix_values: unsafe { ffi_slice(primitive.color_matrix_values, primitive.color_matrix_value_count) }
            .to_vec(),
        component_transfer_tables: primitive.component_transfer_tables.map(|table| {
            (!table.is_null()).then(|| {
                let table = unsafe { ffi_slice(table, 256) };
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
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_note_svg_paint_resources_changed(host: &DocumentHost) -> bool {
    let enrolled = host.svg_paint_resources_may_be_enrolled();
    if enrolled {
        // SAFETY: As above.
        unsafe { queue(host, PaintChange::SvgPaintResourcesChanged) };
    }
    enrolled
}

/// Serializes the graph a list of filter functions describes and hands the bytes to `append`.
/// Returns false for an empty list, which appends nothing.
///
/// # Safety
///
/// `functions` must point to `count` readable functions, and `append` must accept `context` and
/// the byte range it is handed, synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_filter_functions_serialize(
    functions: *const FfiFilterFunction,
    count: usize,
    append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    context: *mut c_void,
) -> bool {
    let functions = unsafe { ffi_slice(functions, count) };
    let Some(graph) = filter_functions_graph(functions.iter().copied().map(Filter::from)) else {
        return false;
    };
    let bytes = graph.serialize();
    unsafe { append(context, bytes.as_ptr(), bytes.len()) };
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_pixels::CssPixelRect;
    use crate::layout::LayoutNodeArena;
    use crate::layout::node_data::NodeKind;
    use crate::painting::host::RootBackgroundSource;
    use crate::painting::paintable_data::FfiOverflowData;
    use crate::painting::record::damage::PaintDamage;
    use crate::painting::visual_context::dirty::VisualContextBoxDirtyKind;
    use crate::painting::visual_context::{TransformData, TransformDataRole, VisualContextTree};

    #[test]
    fn publishing_measured_rows_can_remeasure_viewport_overflow_and_invalidate_painting() {
        {
            let mut arena = LayoutNodeArena::new();
            let viewport = arena.allocate_for_test().slot;
            arena.write_shape(viewport).set_kind(NodeKind::Viewport);
            arena.populate_paintable_row(viewport);
            arena.scrollable_overflow.viewport.set(Some(viewport));
            let root = arena.allocate_for_test().slot;
            arena.populate_paintable_row(root);
            {
                let mut state = arena.paint_state().borrow_mut();
                state.root_background_source = Some(RootBackgroundSource {
                    root_layout_node: root,
                    ..Default::default()
                });
                state.visual_context.tree = Some(std::sync::Arc::new(VisualContextTree::create(TransformData {
                    matrix: libgfx_rust::FloatMatrix4x4::identity(),
                    origin: Default::default(),
                    sorting_context_root_index: None,
                    flattens_inherited_transform: false,
                    role: TransformDataRole::CssTransform,
                    synthetic_plane: false,
                    establishes_sorting_context: false,
                })));
                state.visual_context.dirty_boxes.clear();
            }
            // Leave stale overflow for the hit-test geometry query to remeasure. Losing
            // scrollability must invalidate both the root background and visual context.
            arena.committed_side_data_mut(viewport).overflow_relative_to_padding_box = FfiOverflowData {
                rect: CssPixelRect::new(
                    CssPixels::from_integer(0),
                    CssPixels::from_integer(0),
                    CssPixels::from_integer(100),
                    CssPixels::from_integer(2000),
                )
                .into(),
                has_scrollable_overflow: true,
            };
            arena
                .paintable_side_data(viewport)
                .overflow_measured_this_commit
                .set(true);
            arena.note_publishing_paint_recording_started();
            arena.clear_paint_damage_consumed_by_published_recording();

            // Hit testing reads rows published with every row's overflow measured.
            let rows = arena.publish_row_snapshot(true);
            let absolute_rects = std::cell::RefCell::default();
            let source = crate::painting::paint_read::PaintSource::over_rows(&rows.paintable, &absolute_rects);
            let rect = crate::painting::paintable_geometry::scrollable_overflow_rect(&source, viewport);
            assert_eq!(rect, Some(CssPixelRect::default()));
            assert!(arena.scrollable_overflow.geometry_changed.get());
            assert!(arena.scrollable_overflow.scrollability_changed.get());
            assert!(arena.paint_damage_of_row(root).contains(PaintDamage::DRAW_BACKGROUND));
            assert!(
                arena
                    .paint_damage_of_row(viewport)
                    .contains(PaintDamage::SCROLL_METADATA)
            );
            assert!(
                arena.paint_state().borrow().visual_context.dirty_boxes.boxes[&viewport]
                    .contains(VisualContextBoxDirtyKind::ScrollableOverflowFlipped)
            );
        }
    }

    #[test]
    fn preparing_for_rendering_measures_root_overflow_before_recording_reads_it() {
        let mut arena = LayoutNodeArena::new();
        let viewport = arena.allocate_for_test().slot;
        arena.write_shape(viewport).set_kind(NodeKind::Viewport);
        arena.populate_paintable_row(viewport);
        arena.scrollable_overflow.viewport.set(Some(viewport));
        let root = arena.allocate_for_test().slot;
        arena.write_shape(root).set_kind(NodeKind::BlockContainer);
        arena.populate_paintable_row(root);
        // A structural change invalidated the root's overflow, measured earlier in this commit,
        // without queueing a recalculation, so nothing but a query measures it again. Measuring
        // it drops its scrollable overflow.
        arena.committed_side_data_mut(root).overflow_relative_to_padding_box = FfiOverflowData {
            rect: CssPixelRect::new(
                CssPixels::from_integer(0),
                CssPixels::from_integer(0),
                CssPixels::from_integer(100),
                CssPixels::from_integer(2000),
            )
            .into(),
            has_scrollable_overflow: true,
        };
        arena.paintable_side_data(root).overflow_measured_this_commit.set(true);
        arena.paint_state().borrow_mut().visual_context.dirty_boxes.clear();

        let outcome = crate::painting::paint_passes::prepare_for_rendering(
            &arena,
            RootBackgroundSource {
                root_layout_node: root,
                ..Default::default()
            },
            false,
            &crate::painting::host::FfiVisualContextTreeInputs {
                device_pixels_per_css_pixel: 1.0,
                visual_viewport_offset_x: 0.0,
                visual_viewport_offset_y: 0.0,
                visual_viewport_scale: 1.0,
                viewport_wheel_overflow_x: 0,
                viewport_wheel_overflow_y: 0,
            },
        )
        .outcome;
        assert!(outcome.requires_visual_context_update);
        assert!(
            arena.paint_state().borrow().visual_context.dirty_boxes.boxes[&root]
                .contains(VisualContextBoxDirtyKind::ScrollableOverflowFlipped)
        );

        // Recording reads the root's overflow while it holds the paint state.
        let _paint_state = arena.paint_state().borrow();
        let canvas_rect = crate::painting::record::paint::background_resolution::root_background_canvas_rect(
            &arena.paintable_rows(),
            root,
            CssPixelRect::default(),
        );
        assert_eq!(canvas_rect, CssPixelRect::default());
    }
}
