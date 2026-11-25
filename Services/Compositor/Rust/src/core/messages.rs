/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The stubs of the messages of WebContent and of a presentation that reach the scroll and input logic: what their
//! sender may ask for, and the answer a synchronous one gets.

use super::{CompositorCore, ContextOwnerCheckResult};
use crate::async_scroll_update_outbox::FIRST_OPERATION_ID_THE_MAIN_THREAD_REQUESTS;
use crate::canvas_2d_contexts::ContextCreation;
use crate::display_list_chunk_stores::chunks_of_list_sent_whole;
use crate::host::{PresentationId, WebContentClientId};
use crate::raster_host::SharedMemory;
use crate::webgl::host::{ContextOptions, MappedMemory};
use crate::webgl::replay::ReplayError;
use libcompositing_rust::display_list::commands::CanvasId;
use libcompositing_rust::display_list::replay::{ReplayFrame, ReplaySelection, replay_display_list};
use libcompositing_rust::display_list::storage::DisplayListStorage;
use libcompositing_rust::display_list::window::RecordedWindow;
use libcompositing_rust::ipc_types::compositor_control_server::messages::{
    AwaitSynchronizedResizeFrame, BeginSynchronizedResize, BindPresentation, BindPresentationResponse,
    CommitSynchronizedResize, CreateRemoteLayer, CreateRemoteLayerResponse, DestroyRemoteLayer,
    SetContextPresentsFromCpuMemory, SetContextScalesFramesToFit, SetDisplayMetadata, SetRemoteLayerFit, SetUiOverlays,
    SetWindowServerMayShowHighDynamicRange, ShowContextInRemoteLayer,
};
use libcompositing_rust::ipc_types::compositor_presentation_client::messages::DidNotDispatchInputEvent;
use libcompositing_rust::ipc_types::compositor_presentation_server::messages::{
    HandleAndDispatchKeyEvent, HandleAndDispatchMouseEvent, HandleAndDispatchPinchEvent, PresentedBitmapReadyToPaint,
    SetClientGpuPresentationCapability, SetScrollbarTrackPressJumps, SetScrollbarsAlwaysShown,
};
use libcompositing_rust::ipc_types::compositor_web_content_server::messages::{
    ApplyScrollRequests, BeginInputTraceForTesting, BeginInputTraceForTestingResponse,
    ClearPerTestInputStateForTesting, ClearPerTestInputStateForTestingResponse, CreateCanvas2dContext,
    CreateWebglContext, CreateWebglContextResponse, DestroyCanvas2dContext, DestroyWebglContext,
    HurryRenderingOpportunity, InvalidateKeyboardScrollState, InvalidateKeyboardScrollStateResponse,
    InvalidateWheelEventListenerState, PresentFrame, RasterizeDisplayList, RasterizeDisplayListResponse,
    ReadBackCanvas2dPixels, ReadBackCanvas2dPixelsResponse, RequestRenderingOpportunity, ReserveCanvasIds,
    ReserveCanvasIdsResponse, RouteFirstWheelStepForTesting, RouteFirstWheelStepForTestingResponse,
    ScrollStateForTesting, ScrollStateForTestingResponse, SetParentContext, StopPresentingToClient,
    TakeInputTraceForTesting, TakeInputTraceForTestingResponse, TakePendingAsyncScrollUpdates,
    TakePendingAsyncScrollUpdatesResponse, TakeWheelEventFromMainThread, TakeWheelEventFromMainThreadResponse,
    UpdateCanvas2dStream, UpdateDisplayList, UpdateScrollState, UpdateVisualContextTree, WebglClearDrawingBuffer,
    WebglCommands, WebglCommandsFromSharedBuffer, WebglDrainCommandBuffer, WebglDrainCommandBufferResponse,
    WebglPresentCanvas, WebglReadBackDrawingBuffer, WebglReadBackDrawingBufferResponse, WebglReadBufferSubData,
    WebglReadBufferSubDataResponse, WebglReadPixels, WebglReadPixelsResponse, WebglSetCommandBuffer, WebglSyncCall,
    WebglSyncCallResponse, WheelEventCanceled, WheelEventLetThrough, WheelEventLetThroughResponse,
};
use libcompositing_rust::ipc_types::render_clock_server::messages::RequestClockTick;
use libcompositing_rust::ipc_types::{
    Compositing_RemoteLayerHostHandle, Compositing_RemoteLayerHostKind, Compositing_WebGL_WebGLVersion,
    Compositing_WheelEventAnswer as WheelEventAnswer, Compositing_WheelStepLetThrough as WheelStepLetThrough,
    Compositing_WindowResizingInProgress as WindowResizingInProgress, Core_AnonymousBuffer, LayerPlanOnWire,
    SharedDisplayList, compositor_control_server, compositor_presentation_server, compositor_web_content_server,
    render_clock_server,
};
use libcompositing_rust::scrolling::{AccumulatedVisualContextTree, DisplayList};
use libcompositing_rust::scrolling::{
    CompositorContextId, KeyEvent, KeyModifier, MouseButton, MouseEvent, MouseEventType, PendingAsyncScrollUpdates,
    PinchEvent, ScrollRequest,
};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{FloatPoint, IntRect, IntSize};
use libipc_rust::Misbehavior;
use std::io::Write;
use std::rc::Rc;

// The time a display list takes to be taken in, which WebContent's time to publish it is logged next to.
fn display_list_timing_enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| {
        std::env::var_os("LADYBIRD_DISPLAY_LIST_TIMING").is_some_and(|value| !value.is_empty() && value != "0")
    })
}

const DISPLAY_LIST_WITH_UNUSABLE_SCROLLING_STATE: &str =
    "WebContent published a display list with scrolling state the compositor cannot use";

fn misbehavior_of_layer_plan(why_plan_fails_checks: &'static str) -> Misbehavior {
    let _ = writeln!(
        std::io::stderr(),
        "Compositor: Rejecting plan of layers from WebContent: {why_plan_fails_checks}"
    );
    Misbehavior("WebContent sent a plan of layers that does not fit its display list and visual context tree")
}

fn is_valid_device_pixel_ratio(device_pixel_ratio: f64) -> bool {
    device_pixel_ratio.is_finite() && device_pixel_ratio > 0.0
}

// Wheel deltas are scrolled by as floats.
fn is_valid_wheel_delta(delta: f64) -> bool {
    delta.is_finite() && delta.abs() <= f32::MAX as f64
}

fn is_valid_wheel_event(event: &MouseEvent) -> bool {
    event.r#type == MouseEventType::MouseWheel
        && is_valid_wheel_delta(event.wheel_delta_x)
        && is_valid_wheel_delta(event.wheel_delta_y)
}

fn is_valid_wheel_step_let_through(step: &WheelStepLetThrough) -> bool {
    step.position.x.is_finite()
        && step.position.y.is_finite()
        && step.delta_in_css_pixels.x.is_finite()
        && step.delta_in_css_pixels.y.is_finite()
}

fn is_valid_scroll_request(scroll_request: &ScrollRequest) -> bool {
    let is_finite = |point: FloatPoint| point.x.is_finite() && point.y.is_finite();
    if scroll_request
        .jump_offset
        .is_some_and(|jump_offset| !is_finite(jump_offset))
    {
        return false;
    }
    if let Some(relative_scroll) = &scroll_request.relative_scroll {
        if !is_finite(relative_scroll.delta)
            || !is_finite(relative_scroll.lower_bound)
            || !is_finite(relative_scroll.upper_bound)
        {
            return false;
        }
        if relative_scroll.lower_bound.x > relative_scroll.upper_bound.x
            || relative_scroll.lower_bound.y > relative_scroll.upper_bound.y
        {
            return false;
        }
    }
    if let Some(scale) = scroll_request.visual_viewport_scale
        && (!scale.is_finite() || scale <= 0.0)
    {
        return false;
    }
    if let Some(smooth_scroll) = &scroll_request.smooth_scroll {
        if !is_finite(smooth_scroll.destination_offset) {
            return false;
        }
        if !(smooth_scroll.relative_lower_bound.x <= smooth_scroll.relative_upper_bound.x
            && smooth_scroll.relative_lower_bound.y <= smooth_scroll.relative_upper_bound.y)
        {
            return false;
        }
        // WebContent names the compositor's own scrolls only to head them somewhere else.
        if smooth_scroll.operation_id < FIRST_OPERATION_ID_THE_MAIN_THREAD_REQUESTS
            && !smooth_scroll.retargets_operation
        {
            return false;
        }
    }
    true
}

// NB: The IPC decoder already rejects key codes, event types, wheel delta precisions and scroll gesture phases that do
//     not exist, and pinch scales that are not finite. These check the rest of what the UI sends.

fn is_valid_modifiers(modifiers: KeyModifier) -> bool {
    (modifiers & !KeyModifier::Mod_Mask).is_empty()
}

fn is_valid_key_event(event: &KeyEvent) -> bool {
    is_valid_modifiers(event.modifiers)
        && event.code_point <= 0x10ffff
        // Only the compositor sets this, on its way to WebContent.
        && !event.async_scroll_performed_default_action
}

fn is_valid_mouse_event(event: &MouseEvent) -> bool {
    let all_buttons = MouseButton::Primary
        | MouseButton::Secondary
        | MouseButton::Middle
        | MouseButton::Backward
        | MouseButton::Forward;
    [
        MouseButton::None,
        MouseButton::Primary,
        MouseButton::Secondary,
        MouseButton::Middle,
        MouseButton::Backward,
        MouseButton::Forward,
    ]
    .contains(&event.button)
        && (event.buttons & !all_buttons).is_empty()
        && is_valid_modifiers(event.modifiers)
        && is_valid_wheel_delta(event.wheel_delta_x)
        && is_valid_wheel_delta(event.wheel_delta_y)
        && is_valid_wheel_delta(event.wheel_tick_120ths_x)
        && is_valid_wheel_delta(event.wheel_tick_120ths_y)
        && event.click_count >= 0
        // Only the compositor sets these, on its way to WebContent.
        && !event.async_scroll_performed_default_action
        && event.scrollbar_dragged_by_compositor.is_none()
        && event.wheel_gesture_mark.is_none()
}

fn is_valid_pinch_event(event: &PinchEvent) -> bool {
    is_valid_modifiers(event.modifiers)
        // Only the compositor sets this, on its way to WebContent.
        && !event.async_zoom_performed_default_action
}

impl CompositorCore {
    fn context_is_owned_by(
        &self,
        context_id: CompositorContextId,
        client_id: WebContentClientId,
    ) -> Result<bool, Misbehavior> {
        match self.check_context_owner(context_id, client_id) {
            ContextOwnerCheckResult::OwnedByClient => Ok(true),
            ContextOwnerCheckResult::ContextUnavailable => Ok(false),
            ContextOwnerCheckResult::ConflictingOwner => Err(Misbehavior(
                "WebContent tried to use a compositor context owned by another connection",
            )),
        }
    }

    fn is_bound_to(&self, context_id: CompositorContextId, presentation_id: PresentationId) -> bool {
        self.context_if_present(context_id)
            .is_some_and(|context| context.borrow().bound_presentation() == Some(presentation_id))
    }
}

pub struct MessagesFromWebContent<'a> {
    pub core: &'a CompositorCore,
    pub client_id: WebContentClientId,
    pub now: MonotonicTime,
}

impl MessagesFromWebContent<'_> {
    fn context_is_owned_by_sender(&self, context_id: CompositorContextId) -> Result<bool, Misbehavior> {
        self.core.context_is_owned_by(context_id, self.client_id)
    }
}

impl MessagesFromWebContent<'_> {
    /// Rasterizes the display list `message` hands over into the bitmap staged for it, and answers whether it did.
    fn rasterize_staged_display_list(&mut self, message: RasterizeDisplayList) -> Result<bool, Misbehavior> {
        let raster = self.core.raster();
        let (client, token) = (self.client_id.0, message.token);
        let not_rasterized = |reason: &str| {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Not rasterizing display list from WebContent: {reason}"
            );
            raster.discard_staged_rasterization(client, token);
            Ok(false)
        };
        let SharedDisplayList {
            chunks_in_paint_order,
            packed_chunks,
            packed_chunk_sizes,
            properties,
            ..
        } = message.display_list;
        let storage = match raster
            .receive_display_list_chunks(packed_chunks, &packed_chunk_sizes)
            .and_then(|sent| chunks_of_list_sent_whole(sent, &chunks_in_paint_order))
            .and_then(DisplayListStorage::of_chunks)
        {
            Ok(storage) => storage,
            Err(error) => {
                let _ = writeln!(
                    std::io::stderr(),
                    "Compositor: Rejecting display list to rasterize from WebContent: {error}"
                );
                raster.discard_staged_rasterization(client, token);
                return Err(Misbehavior(
                    "WebContent asked to rasterize a display list its shared buffer does not hold",
                ));
            }
        };
        let tree = message.visual_context_tree;
        if properties.compatible_visual_context_tree_structural_epoch != tree.structural_epoch
            || !tree.display_list_references_only_live_nodes(storage.runs())
        {
            return not_rasterized("Display list does not match its visual context tree");
        }
        let summary = storage.summary();
        let mut image_frame_ids = summary.image_frame_ids.clone();
        tree.for_each_effects_filter_image_frame_id(&mut |id| image_frame_ids.push(id));
        if !raster.staged_rasterization_has_resources(client, token, &summary.font_ids, &image_frame_ids) {
            return not_rasterized("Display list draws resources that were not staged for it");
        }
        let time_ns = self.now.nanoseconds();
        let success = raster.rasterize_staged(client, token, |painter| {
            let frame = ReplayFrame::unscrolled(time_ns);
            replay_display_list(
                &tree,
                &storage,
                properties.root_group_color,
                &frame,
                ReplaySelection::default(),
                painter,
            );
        });
        Ok(success)
    }
}

fn misbehavior(error: ReplayError) -> Misbehavior {
    Misbehavior(error.0)
}

impl MessagesFromWebContent<'_> {
    fn map_shared_memory(&self, buffer: Core_AnonymousBuffer) -> Result<Option<SharedMemory>, Misbehavior> {
        self.core.raster().map_shared_memory(buffer).ok_or(Misbehavior(
            "WebContent sent shared memory that does not hold as much as it claims",
        ))
    }
}

impl compositor_web_content_server::Stub for MessagesFromWebContent<'_> {
    fn reserve_canvas_ids(&mut self, _: ReserveCanvasIds) -> Result<ReserveCanvasIdsResponse, Misbehavior> {
        let reserved_ids = self
            .core
            .canvas_2d_contexts()
            .reserve_ids(self.client_id, self.core.raster())
            .map_err(Misbehavior)?;
        Ok(ReserveCanvasIdsResponse {
            first_canvas_id: CanvasId(reserved_ids.start),
            count: reserved_ids.end - reserved_ids.start,
        })
    }

    fn create_canvas_2d_context(&mut self, message: CreateCanvas2dContext) -> Result<(), Misbehavior> {
        let creation = ContextCreation {
            canvas_id: message.canvas_id,
            size: message.size,
            alpha: message.alpha,
            will_read_frequently: message.will_read_frequently,
            color_space: message.color_space,
        };
        self.core
            .canvas_2d_contexts()
            .create(self.client_id, self.core.raster(), creation)
            .map_err(Misbehavior)
    }

    fn update_canvas_2d_stream(&mut self, message: UpdateCanvas2dStream) -> Result<(), Misbehavior> {
        self.core
            .canvas_2d_contexts()
            .play_stream(self.client_id, self.core.raster(), &message.segments)
            .map_err(Misbehavior)
    }

    fn destroy_canvas_2d_context(&mut self, message: DestroyCanvas2dContext) -> Result<(), Misbehavior> {
        self.core
            .canvas_2d_contexts()
            .destroy(self.client_id, self.core.raster(), message.canvas_id);
        Ok(())
    }

    fn create_webgl_context(&mut self, message: CreateWebglContext) -> Result<CreateWebglContextResponse, Misbehavior> {
        let options = ContextOptions {
            webgl2: message.webgl_version == Compositing_WebGL_WebGLVersion::WebGL2,
            depth: message.depth,
            stencil: message.stencil,
            antialias: message.antialias,
            size: message.size,
        };
        let created = self
            .core
            .webgl_contexts()
            .create(self.core.raster(), self.client_id, options);
        Ok(match created {
            Some(created) => CreateWebglContextResponse {
                success: true,
                canvas_id: created.canvas_id,
                supported_extensions: created.supported_extensions.into_iter().map(Into::into).collect(),
            },
            None => CreateWebglContextResponse {
                success: false,
                canvas_id: CanvasId(0),
                supported_extensions: Vec::new(),
            },
        })
    }

    fn destroy_webgl_context(&mut self, message: DestroyWebglContext) -> Result<(), Misbehavior> {
        self.core
            .webgl_contexts()
            .destroy(self.core.raster(), self.client_id, message.canvas_id);
        Ok(())
    }

    fn webgl_set_command_buffer(&mut self, message: WebglSetCommandBuffer) -> Result<(), Misbehavior> {
        let command_buffer = self.map_shared_memory(message.command_buffer)?;
        self.core
            .webgl_contexts()
            .set_command_buffer(
                self.client_id,
                message.canvas_id,
                command_buffer.map(|memory| Box::new(memory) as Box<dyn MappedMemory>),
            )
            .map_err(misbehavior)
    }

    fn webgl_commands_from_shared_buffer(&mut self, message: WebglCommandsFromSharedBuffer) -> Result<(), Misbehavior> {
        self.core
            .webgl_contexts()
            .execute_commands_from_shared_buffer(
                self.core.raster(),
                self.client_id,
                message.canvas_id,
                message.offset,
                message.size_in_bytes,
                message.flush_sequence_number,
                &message.image_sources,
            )
            .map_err(misbehavior)
    }

    fn webgl_drain_command_buffer(
        &mut self,
        _: WebglDrainCommandBuffer,
    ) -> Result<WebglDrainCommandBufferResponse, Misbehavior> {
        Ok(WebglDrainCommandBufferResponse)
    }

    fn webgl_commands(&mut self, message: WebglCommands) -> Result<(), Misbehavior> {
        let commands = self.map_shared_memory(message.commands)?;
        self.core
            .webgl_contexts()
            .execute_commands(
                self.core.raster(),
                self.client_id,
                message.canvas_id,
                commands.as_ref().map(|memory| memory as &dyn MappedMemory),
                message.size_in_bytes,
                &message.image_sources,
            )
            .map_err(misbehavior)
    }

    fn webgl_present_canvas(&mut self, message: WebglPresentCanvas) -> Result<(), Misbehavior> {
        self.core
            .webgl_contexts()
            .present(
                self.core.raster(),
                self.client_id,
                message.canvas_id,
                message.preserve_drawing_buffer,
            )
            .map_err(misbehavior)
    }

    fn webgl_clear_drawing_buffer(&mut self, message: WebglClearDrawingBuffer) -> Result<(), Misbehavior> {
        self.core
            .webgl_contexts()
            .clear_drawing_buffer(self.core.raster(), self.client_id, message.canvas_id);
        Ok(())
    }

    fn webgl_sync_call(&mut self, message: WebglSyncCall) -> Result<WebglSyncCallResponse, Misbehavior> {
        let reply = self
            .core
            .webgl_contexts()
            .execute_sync_call(self.core.raster(), self.client_id, message.canvas_id, &message.request)
            .map_err(misbehavior)?;
        Ok(WebglSyncCallResponse { reply })
    }

    fn webgl_read_pixels(&mut self, message: WebglReadPixels) -> Result<WebglReadPixelsResponse, Misbehavior> {
        let pixels = self.map_shared_memory(message.pixels)?;
        let rect = IntRect {
            x: message.x,
            y: message.y,
            width: message.width,
            height: message.height,
        };
        let result = self
            .core
            .webgl_contexts()
            .read_pixels(
                self.core.raster(),
                self.client_id,
                message.canvas_id,
                rect,
                message.format,
                message.r#type,
                message.buf_size,
                pixels.as_ref().map(|memory| memory as &dyn MappedMemory),
            )
            .map_err(misbehavior)?;
        Ok(WebglReadPixelsResponse {
            length: result.length,
            columns: result.columns,
            rows: result.rows,
        })
    }

    fn webgl_read_buffer_sub_data(
        &mut self,
        message: WebglReadBufferSubData,
    ) -> Result<WebglReadBufferSubDataResponse, Misbehavior> {
        let data = self.map_shared_memory(message.data)?;
        let success = self
            .core
            .webgl_contexts()
            .read_buffer_sub_data(
                self.core.raster(),
                self.client_id,
                message.canvas_id,
                message.target,
                message.offset,
                message.size,
                data.as_ref().map(|memory| memory as &dyn MappedMemory),
            )
            .map_err(misbehavior)?;
        Ok(WebglReadBufferSubDataResponse { success })
    }

    fn read_back_canvas_2d_pixels(
        &mut self,
        message: ReadBackCanvas2dPixels,
    ) -> Result<ReadBackCanvas2dPixelsResponse, Misbehavior> {
        let pixels = self.map_shared_memory(message.pixels)?;
        let size = pixels.and_then(|pixels| {
            self.core.raster().read_back_canvas_2d_pixels(
                self.client_id.0,
                message.canvas_id,
                message.rect,
                message.alpha_type,
                message.color_space,
                &pixels,
            )
        });
        Ok(ReadBackCanvas2dPixelsResponse {
            size: size.unwrap_or_default(),
        })
    }

    fn webgl_read_back_drawing_buffer(
        &mut self,
        message: WebglReadBackDrawingBuffer,
    ) -> Result<WebglReadBackDrawingBufferResponse, Misbehavior> {
        let pixels = self.map_shared_memory(message.pixels)?;
        let size = self
            .core
            .webgl_contexts()
            .read_back_drawing_buffer(
                self.core.raster(),
                self.client_id,
                message.canvas_id,
                message.rect,
                pixels.as_ref().map(|memory| memory as &dyn MappedMemory),
            )
            .map_err(misbehavior)?;
        Ok(WebglReadBackDrawingBufferResponse { size })
    }

    fn update_display_list(&mut self, message: UpdateDisplayList) -> Result<(), Misbehavior> {
        let started_at = std::time::Instant::now();
        let SharedDisplayList {
            chunks_in_paint_order,
            packed_chunks,
            packed_chunk_sizes,
            properties,
            async_scrolling_metadata,
        } = message.display_list;
        let sent = match self
            .core
            .raster()
            .receive_display_list_chunks(packed_chunks, &packed_chunk_sizes)
        {
            Ok(sent) => sent,
            Err(error) => {
                let _ = writeln!(
                    std::io::stderr(),
                    "Compositor: Rejecting display list from WebContent: {error}"
                );
                return Err(Misbehavior(
                    "WebContent published a display list its shared buffer does not hold",
                ));
            }
        };
        let sent_chunk_count = sent.len();
        // The chunks are taken in before anything else is checked: the list counts as one of the latest of the context
        // on both ends whether it is installed or not.
        let chunks = match self.core.take_in_display_list_chunks(
            self.client_id,
            message.context_id,
            sent,
            &chunks_in_paint_order,
        ) {
            Ok(Some(chunks)) => chunks,
            Ok(None) => return Ok(()),
            Err(error) => return Err(Misbehavior(error)),
        };
        let chunk_count = chunks.len();
        let storage = match DisplayListStorage::of_chunks(chunks) {
            Ok(storage) => storage,
            Err(error) => {
                let _ = writeln!(
                    std::io::stderr(),
                    "Compositor: Dropping display list update from WebContent: {error}"
                );
                return Ok(());
            }
        };
        let adopt_time = started_at.elapsed();
        let tree = message.visual_context_tree;
        if !self.context_is_owned_by_sender(message.context_id)? {
            return Ok(());
        }
        self.core.forget_layer_plan(message.context_id);
        if properties.compatible_visual_context_tree_structural_epoch != tree.structural_epoch {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Dropping inconsistent display list update (display list epoch {}, tree epoch {})",
                properties.compatible_visual_context_tree_structural_epoch,
                tree.structural_epoch
            );
            return Ok(());
        }
        if !tree.display_list_references_only_live_nodes(storage.runs()) {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Dropping display list update that names nodes its visual context tree does not hold"
            );
            return Ok(());
        }
        if !self
            .core
            .raster()
            .context_holds_fonts(message.context_id, &storage.summary().font_ids)
        {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Dropping display list update that draws fonts its context does not hold"
            );
            return Ok(());
        }
        let mut image_frame_ids = storage.summary().image_frame_ids.clone();
        tree.for_each_effects_filter_image_frame_id(&mut |id| image_frame_ids.push(id));
        if !self
            .core
            .raster()
            .take_registered_image_frames(message.context_id, &image_frame_ids)
        {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Dropping display list update that draws image frames that are not registered"
            );
            return Ok(());
        }
        let window = match properties.window.as_ref().map(RecordedWindow::from_ipc) {
            Some(None) => {
                let _ = writeln!(
                    std::io::stderr(),
                    "Compositor: Dropping display list update whose window no recording could have placed"
                );
                return Ok(());
            }
            window => window.flatten(),
        };
        let scroll_state_snapshot = message.scroll_state;
        let storage_size = storage.byte_size();
        let display_list = DisplayList::new(
            storage,
            properties.id,
            properties.compatible_visual_context_tree_structural_epoch,
            async_scrolling_metadata,
            properties.surface_clear_color,
            properties.root_group_color,
        )
        .with_window(window);
        if !self.core.install_display_list_update(
            message.context_id,
            Rc::new(display_list),
            AccumulatedVisualContextTree::from_arc(tree),
            scroll_state_snapshot,
            message.keyboard_scroll_state,
            self.now,
        ) {
            return Err(Misbehavior(DISPLAY_LIST_WITH_UNUSABLE_SCROLLING_STATE));
        }
        self.core
            .take_layer_plan_of_installed_update(
                message.context_id,
                message.layer_plan.map(LayerPlanOnWire::into_received),
            )
            .map_err(misbehavior_of_layer_plan)?;
        self.core.keep_only_drawn_resources(message.context_id);
        if display_list_timing_enabled() {
            let _ = writeln!(
                std::io::stderr(),
                "DISPLAY_LIST_RECEIVE bytes={} chunks={} sent={sent_chunk_count} adopt={} µs install={} µs",
                storage_size,
                chunk_count,
                adopt_time.as_micros(),
                (started_at.elapsed() - adopt_time).as_micros()
            );
        }
        Ok(())
    }

    fn update_visual_context_tree(&mut self, message: UpdateVisualContextTree) -> Result<(), Misbehavior> {
        let scroll_state_snapshot = message.scroll_state;
        if !scroll_state_snapshot.has_only_finite_offsets() {
            return Err(Misbehavior(
                "WebContent published a visual context tree with a scroll state the compositor cannot use",
            ));
        }
        let tree = message.visual_context_tree;
        if !self.context_is_owned_by_sender(message.context_id)? {
            return Ok(());
        }
        self.core.forget_layer_plan(message.context_id);
        let mut image_frame_ids = Vec::new();
        tree.for_each_effects_filter_image_frame_id(&mut |id| image_frame_ids.push(id));
        if !self
            .core
            .raster()
            .take_registered_image_frames(message.context_id, &image_frame_ids)
        {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Dropping visual context tree update that draws image frames that are not registered"
            );
            return Ok(());
        }
        let tree_was_replaced = self.core.update_visual_context_tree(
            message.context_id,
            AccumulatedVisualContextTree::from_arc(tree),
            scroll_state_snapshot,
            message.keyboard_scroll_state,
            self.now,
        );
        if tree_was_replaced {
            self.core
                .take_layer_plan_of_installed_update(
                    message.context_id,
                    message.layer_plan.map(LayerPlanOnWire::into_received),
                )
                .map_err(misbehavior_of_layer_plan)?;
            self.core.keep_only_drawn_resources(message.context_id);
        }
        Ok(())
    }

    fn rasterize_display_list(
        &mut self,
        message: RasterizeDisplayList,
    ) -> Result<RasterizeDisplayListResponse, Misbehavior> {
        Ok(RasterizeDisplayListResponse {
            success: self.rasterize_staged_display_list(message)?,
        })
    }

    fn update_scroll_state(&mut self, message: UpdateScrollState) -> Result<(), Misbehavior> {
        let scroll_state_snapshot = message.scroll_state;
        if !scroll_state_snapshot.has_only_finite_offsets() {
            return Err(Misbehavior("Invalid scroll state"));
        }
        if self.context_is_owned_by_sender(message.context_id)? {
            if !self.core.update_scroll_state(
                message.context_id,
                message.visual_context_tree_structural_epoch,
                scroll_state_snapshot,
                message.keyboard_scroll_state,
            ) {
                let _ = writeln!(
                    std::io::stderr(),
                    "Compositor: Dropping scroll state update built for another visual context tree (epoch {})",
                    message.visual_context_tree_structural_epoch
                );
                return Ok(());
            }
            self.core
                .take_layer_plan_of_scroll_state(
                    message.context_id,
                    message.layer_plan.map(LayerPlanOnWire::into_received),
                )
                .map_err(misbehavior_of_layer_plan)?;
        }
        Ok(())
    }

    fn invalidate_wheel_event_listener_state(
        &mut self,
        message: InvalidateWheelEventListenerState,
    ) -> Result<(), Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core
                .invalidate_wheel_event_listener_state(message.context_id, message.generation);
        }
        Ok(())
    }

    fn invalidate_keyboard_scroll_state(
        &mut self,
        message: InvalidateKeyboardScrollState,
    ) -> Result<InvalidateKeyboardScrollStateResponse, Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core
                .invalidate_keyboard_scroll_state(message.context_id, message.generation);
        }
        Ok(InvalidateKeyboardScrollStateResponse)
    }

    fn take_wheel_event_from_main_thread(
        &mut self,
        message: TakeWheelEventFromMainThread,
    ) -> Result<TakeWheelEventFromMainThreadResponse, Misbehavior> {
        let answer = if self.context_is_owned_by_sender(message.context_id)? && is_valid_wheel_event(&message.event) {
            self.core.take_wheel_event_from_main_thread(
                message.context_id,
                &message.event,
                message.operation_tracking,
                message.routing,
                self.now,
            )
        } else {
            WheelEventAnswer::default()
        };
        Ok(TakeWheelEventFromMainThreadResponse { answer })
    }

    fn wheel_event_let_through(
        &mut self,
        message: WheelEventLetThrough,
    ) -> Result<WheelEventLetThroughResponse, Misbehavior> {
        let answer =
            if self.context_is_owned_by_sender(message.context_id)? && is_valid_wheel_step_let_through(&message.step) {
                self.core.scroll_for_wheel_event_the_page_let_through(
                    message.context_id,
                    &message.step,
                    message.operation_tracking,
                    self.client_id,
                    self.now,
                )
            } else {
                WheelEventAnswer::default()
            };
        Ok(WheelEventLetThroughResponse { answer })
    }

    fn wheel_event_canceled(&mut self, message: WheelEventCanceled) -> Result<(), Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core
                .forget_wheel_gesture_the_page_canceled(message.context_id, message.wheel_gesture_id);
        }
        Ok(())
    }

    fn apply_scroll_requests(&mut self, message: ApplyScrollRequests) -> Result<(), Misbehavior> {
        if !self.context_is_owned_by_sender(message.context_id)? {
            return Ok(());
        }
        if !message.scroll_requests.iter().all(is_valid_scroll_request) {
            return Err(Misbehavior("Invalid scroll request"));
        }
        self.core
            .apply_scroll_requests(message.context_id, &message.scroll_requests, self.now);
        Ok(())
    }

    fn scroll_state_for_testing(
        &mut self,
        message: ScrollStateForTesting,
    ) -> Result<ScrollStateForTestingResponse, Misbehavior> {
        let scroll_state = if self.context_is_owned_by_sender(message.context_id)? {
            self.core
                .scroll_state_for_testing(message.context_id, message.stretched_scroller)
        } else {
            String::new()
        };
        Ok(ScrollStateForTestingResponse {
            scroll_state: scroll_state.into(),
        })
    }

    fn route_first_wheel_step_for_testing(
        &mut self,
        message: RouteFirstWheelStepForTesting,
    ) -> Result<RouteFirstWheelStepForTestingResponse, Misbehavior> {
        let route = if self.context_is_owned_by_sender(message.context_id)? {
            self.core.route_first_wheel_step_for_testing(
                message.context_id,
                message.position,
                message.delta_in_css_pixels,
            )
        } else {
            None
        };
        Ok(match route {
            Some(route) => RouteFirstWheelStepForTestingResponse {
                decision: Some(route.route.decision),
                scroller: route.route.scroller,
                document_id: route.document_id,
            },
            None => RouteFirstWheelStepForTestingResponse {
                decision: None,
                scroller: None,
                document_id: None,
            },
        })
    }

    fn begin_input_trace_for_testing(
        &mut self,
        message: BeginInputTraceForTesting,
    ) -> Result<BeginInputTraceForTestingResponse, Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core.begin_input_trace_for_testing(message.context_id);
        }
        Ok(BeginInputTraceForTestingResponse)
    }

    fn clear_per_test_input_state_for_testing(
        &mut self,
        message: ClearPerTestInputStateForTesting,
    ) -> Result<ClearPerTestInputStateForTestingResponse, Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core.clear_per_test_input_state_for_testing(message.context_id);
        }
        Ok(ClearPerTestInputStateForTestingResponse)
    }

    fn take_input_trace_for_testing(
        &mut self,
        message: TakeInputTraceForTesting,
    ) -> Result<TakeInputTraceForTestingResponse, Misbehavior> {
        let records = if self.context_is_owned_by_sender(message.context_id)? {
            self.core.take_input_trace_for_testing(message.context_id)
        } else {
            Vec::new()
        };
        Ok(TakeInputTraceForTestingResponse { records })
    }

    fn take_pending_async_scroll_updates(
        &mut self,
        message: TakePendingAsyncScrollUpdates,
    ) -> Result<TakePendingAsyncScrollUpdatesResponse, Misbehavior> {
        let updates = if self.context_is_owned_by_sender(message.context_id)? {
            self.core.take_pending_async_scroll_updates(message.context_id)
        } else {
            PendingAsyncScrollUpdates::default()
        };
        Ok(TakePendingAsyncScrollUpdatesResponse { updates })
    }

    fn set_parent_context(&mut self, message: SetParentContext) -> Result<(), Misbehavior> {
        if !self.context_is_owned_by_sender(message.context_id)? {
            return Ok(());
        }
        if let Some(parent_context_id) = message.parent_context_id
            && !self
                .core
                .context_can_be_nested_in(message.context_id, parent_context_id)
        {
            return Err(Misbehavior(
                "WebContent nested a compositor context that cannot be nested",
            ));
        }
        self.core
            .set_parent_context(message.context_id, message.parent_context_id, self.now);
        Ok(())
    }

    fn stop_presenting_to_client(&mut self, message: StopPresentingToClient) -> Result<(), Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core.stop_presenting_to_client(message.context_id, self.now);
        }
        Ok(())
    }

    fn viewport_size_updated(
        &mut self,
        message: compositor_web_content_server::messages::ViewportSizeUpdated,
    ) -> Result<(), Misbehavior> {
        if !self.context_is_owned_by_sender(message.context_id)? {
            return Ok(());
        }
        if !is_valid_device_pixel_ratio(message.device_pixel_ratio) {
            return Err(Misbehavior("WebContent sent an invalid device pixel ratio"));
        }
        self.core.viewport_size_updated(
            message.context_id,
            message.viewport_size,
            message.device_pixel_ratio,
            message.window_resize_in_progress == WindowResizingInProgress::Yes,
            self.now,
        );
        Ok(())
    }

    fn request_rendering_opportunity(&mut self, message: RequestRenderingOpportunity) -> Result<(), Misbehavior> {
        if !self.context_is_owned_by_sender(message.context_id)? {
            return Ok(());
        }
        if !message.maximum_frames_per_second.is_finite() || message.maximum_frames_per_second <= 0.0 {
            return Err(Misbehavior("WebContent sent an invalid maximum frame rate"));
        }
        self.core
            .request_rendering_opportunity(message.context_id, message.maximum_frames_per_second, self.now);
        Ok(())
    }

    fn hurry_rendering_opportunity(&mut self, message: HurryRenderingOpportunity) -> Result<(), Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core.hurry_rendering_opportunity(message.context_id, self.now);
        }
        Ok(())
    }

    fn present_frame(&mut self, message: PresentFrame) -> Result<(), Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core
                .present_frame_of_page(message.context_id, message.viewport_rect, self.now);
        }
        Ok(())
    }
}

impl render_clock_server::Stub for MessagesFromWebContent<'_> {
    fn request_clock_tick(&mut self, message: RequestClockTick) -> Result<(), Misbehavior> {
        if !self.context_is_owned_by_sender(message.context_id)? {
            return Ok(());
        }
        if !message.maximum_frames_per_second.is_finite() || message.maximum_frames_per_second <= 0.0 {
            return Err(Misbehavior("WebContent sent an invalid maximum clock tick rate"));
        }
        self.core
            .request_clock_tick(message.context_id, message.maximum_frames_per_second);
        Ok(())
    }
}

pub struct MessagesFromPresentation<'a> {
    pub core: &'a CompositorCore,
    pub presentation_id: PresentationId,
    pub now: MonotonicTime,
}

impl MessagesFromPresentation<'_> {
    fn sender_gives_input_to(&self, context_id: CompositorContextId, event_id: u64) -> bool {
        if self.core.is_bound_to(context_id, self.presentation_id) {
            return true;
        }
        self.core
            .host
            .post_to_presentation(self.presentation_id, DidNotDispatchInputEvent { context_id, event_id });
        false
    }
}

impl compositor_presentation_server::Stub for MessagesFromPresentation<'_> {
    fn handle_and_dispatch_key_event(&mut self, message: HandleAndDispatchKeyEvent) -> Result<(), Misbehavior> {
        if !is_valid_key_event(&message.event) {
            return Err(Misbehavior("Invalid key event"));
        }
        if self.sender_gives_input_to(message.context_id, message.event.id) {
            self.core.handle_and_dispatch_key_event(
                message.context_id,
                message.event,
                message.allow_keyboard_scroll,
                self.now,
            );
        }
        Ok(())
    }

    fn handle_and_dispatch_pinch_event(&mut self, message: HandleAndDispatchPinchEvent) -> Result<(), Misbehavior> {
        if !is_valid_pinch_event(&message.event) {
            return Err(Misbehavior("Invalid pinch event"));
        }
        if self.sender_gives_input_to(message.context_id, message.event.id) {
            self.core
                .handle_and_dispatch_pinch_event(message.context_id, message.event);
        }
        Ok(())
    }

    fn handle_and_dispatch_mouse_event(&mut self, message: HandleAndDispatchMouseEvent) -> Result<(), Misbehavior> {
        if !is_valid_mouse_event(&message.event) {
            return Err(Misbehavior("Invalid mouse event"));
        }
        if self.sender_gives_input_to(message.context_id, message.event.id) {
            self.core
                .handle_and_dispatch_mouse_event(message.context_id, message.event, self.now);
        }
        Ok(())
    }

    fn presented_bitmap_ready_to_paint(&mut self, message: PresentedBitmapReadyToPaint) -> Result<(), Misbehavior> {
        if self.core.is_bound_to(message.context_id, self.presentation_id) {
            self.core
                .presented_store_released(message.context_id, message.bitmap_id, self.now);
        }
        Ok(())
    }

    fn set_client_gpu_presentation_capability(
        &mut self,
        message: SetClientGpuPresentationCapability,
    ) -> Result<(), Misbehavior> {
        self.core.set_presentation_gpu_capability(
            self.presentation_id,
            message.supported,
            message.adapter_luid,
            self.now,
        );
        Ok(())
    }

    fn set_scrollbar_track_press_jumps(&mut self, message: SetScrollbarTrackPressJumps) -> Result<(), Misbehavior> {
        self.core
            .set_scrollbar_track_press_jumps(self.presentation_id, message.jumps, self.now);
        Ok(())
    }

    fn set_scrollbars_always_shown(&mut self, message: SetScrollbarsAlwaysShown) -> Result<(), Misbehavior> {
        self.core
            .set_scrollbars_always_shown(self.presentation_id, message.always_shown, self.now);
        Ok(())
    }
}

pub struct MessagesFromControl<'a> {
    pub core: &'a CompositorCore,
    pub now: MonotonicTime,
}

impl compositor_control_server::Stub for MessagesFromControl<'_> {
    fn create_remote_layer(&mut self, message: CreateRemoteLayer) -> Result<CreateRemoteLayerResponse, Misbehavior> {
        let host_handle = self
            .core
            .create_remote_layer(PresentationId(message.presentation_id), message.remote_layer_id)
            .unwrap_or_else(|| {
                let _ = writeln!(
                    std::io::stderr(),
                    "Compositor: Ignoring remote layer {} for presentation {}, which is already gone",
                    message.remote_layer_id,
                    message.presentation_id
                );
                Compositing_RemoteLayerHostHandle {
                    kind: Compositing_RemoteLayerHostKind::None,
                    id: 0,
                }
            });
        Ok(CreateRemoteLayerResponse { host_handle })
    }

    fn destroy_remote_layer(&mut self, message: DestroyRemoteLayer) -> Result<(), Misbehavior> {
        self.core.destroy_remote_layer(message.remote_layer_id, self.now);
        Ok(())
    }

    fn show_context_in_remote_layer(&mut self, message: ShowContextInRemoteLayer) -> Result<(), Misbehavior> {
        self.core
            .show_context_in_remote_layer(message.remote_layer_id, message.context_id, self.now);
        Ok(())
    }

    fn set_remote_layer_fit(&mut self, message: SetRemoteLayerFit) -> Result<(), Misbehavior> {
        if message.fit_within.is_some_and(IntSize::is_empty) {
            return Err(Misbehavior("Empty size for a remote layer to fit frames within"));
        }
        self.core
            .set_remote_layer_fit(message.remote_layer_id, message.fit_within);
        Ok(())
    }

    fn begin_synchronized_resize(&mut self, message: BeginSynchronizedResize) -> Result<(), Misbehavior> {
        self.core
            .begin_synchronized_resize(message.remote_layer_id, message.token);
        Ok(())
    }

    fn await_synchronized_resize_frame(&mut self, message: AwaitSynchronizedResizeFrame) -> Result<(), Misbehavior> {
        self.core
            .await_synchronized_resize_frame(message.remote_layer_id, message.token, message.size);
        Ok(())
    }

    fn commit_synchronized_resize(&mut self, message: CommitSynchronizedResize) -> Result<(), Misbehavior> {
        self.core
            .commit_synchronized_resize(message.remote_layer_id, message.token);
        Ok(())
    }

    fn set_window_server_may_show_high_dynamic_range(
        &mut self,
        message: SetWindowServerMayShowHighDynamicRange,
    ) -> Result<(), Misbehavior> {
        self.core
            .set_window_server_may_show_high_dynamic_range(message.may_show, self.now);
        Ok(())
    }

    fn bind_presentation(&mut self, message: BindPresentation) -> Result<BindPresentationResponse, Misbehavior> {
        self.core
            .bind_presentation(message.context_id, PresentationId(message.presentation_id), self.now);
        Ok(BindPresentationResponse)
    }

    fn viewport_size_updated(
        &mut self,
        message: compositor_control_server::messages::ViewportSizeUpdated,
    ) -> Result<(), Misbehavior> {
        if !is_valid_device_pixel_ratio(message.device_pixel_ratio) {
            return Err(Misbehavior("Invalid device pixel ratio"));
        }
        self.core.viewport_size_updated(
            message.context_id,
            message.viewport_size,
            message.device_pixel_ratio,
            message.window_resize_in_progress == WindowResizingInProgress::Yes,
            self.now,
        );
        Ok(())
    }

    fn set_ui_overlays(&mut self, message: SetUiOverlays) -> Result<(), Misbehavior> {
        self.core
            .set_ui_overlays(message.context_id, message.overlays.into(), self.now);
        Ok(())
    }

    fn set_display_metadata(&mut self, message: SetDisplayMetadata) -> Result<(), Misbehavior> {
        if !message.refresh_rate.is_finite() || message.refresh_rate <= 0.0 {
            return Err(Misbehavior("Invalid display refresh rate"));
        }
        self.core
            .set_display_metadata(message.context_id, message.display_id, message.refresh_rate, self.now);
        Ok(())
    }

    fn set_context_presents_from_cpu_memory(
        &mut self,
        message: SetContextPresentsFromCpuMemory,
    ) -> Result<(), Misbehavior> {
        self.core
            .set_presents_from_cpu_memory(message.context_id, message.presents_from_cpu_memory, self.now);
        Ok(())
    }

    fn set_context_scales_frames_to_fit(&mut self, message: SetContextScalesFramesToFit) -> Result<(), Misbehavior> {
        self.core
            .set_scales_frames_to_fit(message.context_id, message.scales_frames_to_fit, self.now);
        Ok(())
    }
}
