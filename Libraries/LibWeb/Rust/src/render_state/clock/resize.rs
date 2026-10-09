/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Viewport input that reaches the render owner without passing through the host's event loop.

use super::{ClockTicks, Lane, Park};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::media_list::MediaListData;
use crate::css::parser::query_parser::{FfiMediaEnvironment, FfiMediaFeatureValue, MEDIA_FEATURES};
use crate::css::style::style_job::SealedStyleInputs;
use crate::css::style_compute::FfiLengthResolutionContext;
use crate::css::style_sheet::NativeStyleSheet;
use crate::render_state::{DocumentHost, RenderState};
use libgfx_rust::{IntRect, IntSize};
use std::sync::Arc;
use std::sync::atomic::Ordering;

#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum FfiResizeBlocker {
    None,
    #[default]
    Unpublished,
    ResizeListener,
    AnimationFrameCallback,
    ResizeObserver,
    MediaQueryListener,
    IntersectionObserver,
    ViewTransition,
    ContentVisibility,
    ScrollState,
    Animation,
    Focus,
    EmbeddedFrame,
    ViewportStyle,
    MediaChange,
    NeedsHost,
}

#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub(super) struct ViewportSize {
    pub(super) width: i32,
    pub(super) height: i32,
}

#[derive(Default)]
pub(super) struct ResizeMailbox {
    pub(super) latest: Option<ViewportSize>,
    pub(super) pending: bool,
    pub(super) presented: u64,
}

pub(super) struct ResizePlan {
    pub(super) size: ViewportSize,
    scale: f64,
    media: Vec<(Arc<MediaListData>, bool)>,
    values: Box<[FfiMediaFeatureValue]>,
    lengths: FfiLengthResolutionContext,
}

impl ResizePlan {
    /// Seals media conditions as immutable parsed queries. A breakpoint requiring host-owned stylesheet
    /// processing declines before changing the fork.
    pub(super) fn new(inputs: &SealedStyleInputs, sheets: &[*const NativeStyleSheet]) -> Self {
        let inputs = inputs.inputs();
        // SAFETY: The caller holds the sealed inputs and lends live sheets on the host's thread.
        let mut media = Vec::new();
        for &sheet in sheets {
            if let Some(sheet) = unsafe { sheet.as_ref() } {
                sheet.collect_resize_media(&mut media);
            }
        }
        let values = unsafe {
            std::slice::from_raw_parts(inputs.media_feature_values.as_pointer().cast(), inputs.media_feature_value_count)
        }.to_vec().into_boxed_slice();
        let lengths = unsafe {
            *inputs.media_length_resolution_context.as_pointer().cast::<FfiLengthResolutionContext>()
        };
        Self {
            size: ViewportSize {
                width: (inputs.viewport_width * inputs.device_pixels_per_css_pixel).round() as i32,
                height: (inputs.viewport_height * inputs.device_pixels_per_css_pixel).round() as i32,
            },
            scale: inputs.device_pixels_per_css_pixel,
            media,
            values,
            lengths,
        }
    }

    fn accepts(&self, size: ViewportSize) -> bool {
        let width = size.width as f64 / self.scale;
        let height = size.height as f64 / self.scale;
        let mut values = self.values.clone();
        for (feature, value) in MEDIA_FEATURES.iter().zip(values.iter_mut()) {
            match feature.name {
                "width" => value.value = width,
                "height" => value.value = height,
                "aspect-ratio" => { value.value = width; value.second_value = height; }
                "orientation" => value.keyword = if width > height {
                    crate::css::css_enums::keyword::LANDSCAPE
                } else {
                    crate::css::css_enums::keyword::PORTRAIT
                },
                _ => {}
            }
        }
        let lengths = FfiLengthResolutionContext { viewport_width: width, viewport_height: height, ..self.lengths };
        let environment = FfiMediaEnvironment {
            values: values.as_ptr(), value_count: values.len(),
            length_resolution_context: std::ptr::from_ref(&lengths).cast(),
        };
        // SAFETY: The local values and context outlive every query evaluation.
        let environment = unsafe { environment.borrow() };
        self.media.iter().all(|(list, matched)| {
            (list.queries.is_empty() || list.queries.iter().any(|query| query.matches_media(environment))) == *matched
        })
    }
}

impl ClockTicks {
    pub(super) fn viewport_changed(&self, size: ViewportSize) -> bool {
        if size.width <= 0 || size.height <= 0 { return false; }
        let mut mailbox = self.resize.lock().expect("resize mailbox");
        if mailbox.latest == Some(size) { return false; }
        mailbox.latest = Some(size);
        mailbox.pending = true;
        self.resize_blocker.load(Ordering::Acquire) == FfiResizeBlocker::None as u8
    }

    pub(super) fn resize_waits(&self) -> bool {
        self.resize_blocker.load(Ordering::Acquire) == FfiResizeBlocker::None as u8
            && self.resize.lock().expect("resize mailbox").pending
    }
}

impl Lane {
    pub(super) fn resize(&mut self, state: &mut RenderState, size: ViewportSize) -> Result<(), Park> {
        let plan = self.plan.resize.as_mut().ok_or(Park("no resize plan"))?;
        if !plan.accepts(size) { return Err(Park("media conditions changed")); }
        let width = CssPixels::nearest_value_for(size.width as f64 / plan.scale);
        let height = CssPixels::nearest_value_for(size.height as f64 / plan.scale);
        self.plan.round.set_viewport_size(width, height);
        let arena = state.arena.arena();
        arena.set_needs_layout_update(arena.layout_root(), true);
        plan.size = size;
        let mut recorder = self.recording.0.take(super::WaitsForTickRecording(()));
        if let Some(inputs) = &mut recorder.recorder.published_inputs {
            inputs.css_viewport_rect.width = width;
            inputs.css_viewport_rect.height = height;
            inputs.uncaptured.device_viewport_size = IntSize { width: size.width, height: size.height };
            inputs.bitmap_rect = IntRect { x: 0, y: 0, width: size.width, height: size.height };
        }
        self.recording = super::TickRecording(crate::stage_thread::Riding::landed(recorder));
        Ok(())
    }
}

/// # Safety
/// `ticks` must be a retained clock handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn clock_ticks_viewport_changed(ticks: &ClockTicks, width: i32, height: i32) -> bool {
    ticks.viewport_changed(ViewportSize { width, height })
}

/// Invalidates eligibility synchronously when script installs a rendering hook.
/// # Safety
/// `host` must be live on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_block_resize(host: &DocumentHost) {
    host.clock_ticks().resize_blocker.store(FfiResizeBlocker::Unpublished as u8, Ordering::Release);
}

/// The compositor's latest viewport remains authoritative across host transactions.
/// # Safety
/// `host` must be live and `width` and `height` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_latest_viewport(host: &DocumentHost, width: &mut i32, height: &mut i32) -> bool {
    let mailbox = host.clock_ticks().resize.lock().expect("resize mailbox");
    let Some(size) = mailbox.latest else { return false; };
    *width = size.width;
    *height = size.height;
    true
}

/// # Safety
/// `host` must be live on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_resize_frame_count(host: &DocumentHost) -> u64 {
    host.clock_ticks().resize.lock().expect("resize mailbox").presented
}

/// # Safety
/// `host` must be live on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_resize_blocker(host: &DocumentHost) -> u8 {
    host.clock_ticks().resize_blocker.load(Ordering::Acquire)
}
