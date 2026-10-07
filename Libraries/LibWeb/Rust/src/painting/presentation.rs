/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A navigable's presenter, and the seal of the frame it presents next, which what presents a frame beside the event
//! loop owns while they are away from their navigable.
//!
//! Each box owns one C++ object, which reaches nothing of the thread that made it, and destroys it when dropped. Only
//! the owner of a navigable's presenter presents to its compositor context, so frames reach the compositor in the order
//! their presenter's owners presented them.

use crate::paint_stage::Presenting;
use crate::painting::ffi::{FfiPresentation, FfiPresentedRecording};
use crate::painting::paint_passes::ClockTickVisualContexts;
use crate::painting::record::publish::RecordingResourceSink;
use crate::painting::visual_context::VisualContextTree;
use libgfx_rust::FloatPoint;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

unsafe extern "C" {
    fn web_navigable_presenter_unref(presenter: *mut c_void);
    fn web_navigable_presenter_seal_for_clock_lane(presenter: *mut c_void, committed: *const c_void) -> *mut c_void;
    fn web_sealed_presentation_destroy(sealed: *mut c_void);
    fn web_sealed_presentation_take_visual_context_tree(
        sealed: *mut c_void,
        tree: *const c_void,
        scroll_offsets: *const FloatPoint,
        scroll_offset_count: usize,
    );
    fn web_navigable_presenter_add_font(presenter: *mut c_void, font: *const c_void);
    fn web_navigable_presenter_add_image_frame(presenter: *mut c_void, frame: *const c_void);
    fn web_navigable_presenter_add_video_sink(presenter: *mut c_void, resource_id: u64, sink_handle: u64);
    fn web_navigable_presenter_present(
        presenter: *mut c_void,
        sealed: *mut c_void,
        presented: *const FfiPresentedRecording,
        sink: *mut c_void,
    );
    fn web_navigable_presenter_present_unrecorded(presenter: *mut c_void, sealed: *mut c_void, sink: *mut c_void);
    fn web_vector_image_resources_destroy(resources: *mut c_void);
    fn web_navigable_presenter_take_vector_image_resources(
        presenter: *mut c_void,
        resources: *mut c_void,
        display_list_ids: *const u64,
        display_list_id_count: usize,
    );
}

/// A reference to a `Web::Compositor::NavigablePresenter`, which its navigable shares.
pub(crate) struct PresenterBox(NonNull<c_void>);

/// A `Web::Compositor::SealedPresentation`, owned.
pub(crate) struct SealedPresentationBox(NonNull<c_void>);

// SAFETY: Each box owns its object, which reaches nothing of the thread that made it, and nothing else reaches the object
// while the box lives.
unsafe impl Send for PresenterBox {}
unsafe impl Send for SealedPresentationBox {}

impl Drop for PresenterBox {
    fn drop(&mut self) {
        // SAFETY: The box holds a reference to the presenter.
        unsafe { web_navigable_presenter_unref(self.0.as_ptr()) };
    }
}

impl Drop for SealedPresentationBox {
    fn drop(&mut self) {
        // SAFETY: The box owns the seal.
        unsafe { web_sealed_presentation_destroy(self.0.as_ptr()) };
    }
}

impl RecordingResourceSink for PresenterBox {
    fn add_font(&mut self, font: &libgfx_rust::font::FontHandle) {
        // SAFETY: The box owns the presenter, whose storage takes a reference of its own to the live font.
        unsafe { web_navigable_presenter_add_font(self.0.as_ptr(), font.as_raw()) };
    }

    fn add_image_frame(&mut self, frame: &libgfx_rust::image_frame::ImageFrameHandle) {
        // SAFETY: As above, for the live frame.
        unsafe { web_navigable_presenter_add_image_frame(self.0.as_ptr(), frame.as_raw()) };
    }

    fn add_video_sink(&mut self, resource_id: u64, sink_handle: u64) {
        // SAFETY: As above.
        unsafe { web_navigable_presenter_add_video_sink(self.0.as_ptr(), resource_id, sink_handle) };
    }
}

/// The SVG images a frame renders, which the host rendered for it: a `Web::Compositor::VectorImageResources`, owned.
pub(crate) struct VectorImageResources(NonNull<c_void>);

// SAFETY: The box owns its object, which reaches nothing of the thread that made it.
unsafe impl Send for VectorImageResources {}

impl VectorImageResources {
    /// Takes over what `resources` names, which the host gives up.
    ///
    /// # Safety
    /// `resources` must be a `Web::Compositor::VectorImageResources` the host gives up.
    pub(crate) unsafe fn adopt(resources: NonNull<c_void>) -> Self {
        Self(resources)
    }
}

impl Drop for VectorImageResources {
    fn drop(&mut self) {
        // SAFETY: The box owns the resources.
        unsafe { web_vector_image_resources_destroy(self.0.as_ptr()) };
    }
}

impl PresenterBox {
    /// Takes over the display lists `display_list_ids` names in `resources`, with what they reference.
    pub(crate) fn take_vector_image_resources(&mut self, resources: VectorImageResources, display_list_ids: &[u64]) {
        // SAFETY: The box owns the presenter, and `resources` the storage the presenter copies from.
        unsafe {
            web_navigable_presenter_take_vector_image_resources(
                self.0.as_ptr(),
                resources.0.as_ptr(),
                display_list_ids.as_ptr(),
                display_list_ids.len(),
            );
        }
    }
}

/// What presents one frame of a navigable beside the event loop: the navigable's presenter, and what the frame is built
/// from.
pub(crate) struct Presentation {
    pub(crate) presenter: PresenterBox,
    sealed: SealedPresentationBox,
}

impl Presentation {
    /// Takes over the presenter and the seal `ffi` names, which the host gives up, or none where it names none.
    ///
    /// # Safety
    /// `ffi` must name a presenter and a seal the host made and gives up, or neither.
    pub(crate) unsafe fn adopt(ffi: FfiPresentation) -> Option<Self> {
        Some(Self {
            presenter: PresenterBox(NonNull::new(ffi.presenter)?),
            sealed: SealedPresentationBox(NonNull::new(ffi.sealed)?),
        })
    }

    /// What the clock lane of the frame this presented presents its frames with: the presenter, and a seal of frames
    /// that start from this one, or none where the compositor shows another display list than this frame records from.
    /// On the Paint thread, once this presented.
    pub(crate) fn for_clock_lane(&self) -> Option<Self> {
        // SAFETY: The presentation owns both objects. A seal the call answers comes with a reference to the presenter.
        let sealed = NonNull::new(unsafe {
            web_navigable_presenter_seal_for_clock_lane(self.presenter.0.as_ptr(), self.sealed.0.as_ptr())
        })?;
        Some(Self {
            presenter: PresenterBox(self.presenter.0),
            sealed: SealedPresentationBox(sealed),
        })
    }

    /// Gives the presenter and the seal back to the host, which takes them over.
    pub(crate) fn into_ffi(self) -> FfiPresentation {
        let this = std::mem::ManuallyDrop::new(self);
        FfiPresentation {
            presenter: this.presenter.0.as_ptr(),
            sealed: this.sealed.0.as_ptr(),
        }
    }

    /// Has the frames presented from the seal take `visual_contexts` to the compositor, the tree with which a clock
    /// tick's update left the one the seal held.
    pub(crate) fn take_visual_context_tree(&mut self, visual_contexts: ClockTickVisualContexts) {
        let ClockTickVisualContexts {
            tree,
            restructured_scroll_offsets,
        } = visual_contexts;
        let offsets = restructured_scroll_offsets.as_deref();
        // SAFETY: The presentation owns the seal, which takes over the reference and copies the offsets.
        unsafe {
            web_sealed_presentation_take_visual_context_tree(
                self.sealed.0.as_ptr(),
                Arc::into_raw(tree).cast(),
                offsets.map_or(std::ptr::null(), <[FloatPoint]>::as_ptr),
                offsets.map_or(0, <[FloatPoint]>::len),
            );
        }
    }

    /// Presents the sealed frame without a recording: the display list the compositor has stands, with
    /// `visual_context_tree`, the render state's tree, where the seal sends one.
    pub(crate) fn present_unrecorded(
        &mut self,
        visual_context_tree: Option<Arc<VisualContextTree>>,
        presenting: &mut Presenting,
    ) {
        if let Some(tree) = visual_context_tree {
            self.take_visual_context_tree(ClockTickVisualContexts {
                tree,
                restructured_scroll_offsets: None,
            });
        }
        // SAFETY: The presentation owns both objects, and the stage holds the sink.
        unsafe {
            web_navigable_presenter_present_unrecorded(
                self.presenter.0.as_ptr(),
                self.sealed.0.as_ptr(),
                presenting.sink(),
            );
        }
    }

    /// Presents the sealed frame with the recording `presented` describes, whose resources the presenter took already.
    pub(crate) fn present(&mut self, presented: &FfiPresentedRecording, presenting: &mut Presenting) {
        // SAFETY: The presentation owns both objects, the recording's display list is live for the call, and the stage
        // holds the sink.
        unsafe {
            web_navigable_presenter_present(
                self.presenter.0.as_ptr(),
                self.sealed.0.as_ptr(),
                presented,
                presenting.sink(),
            );
        }
    }
}

#[cfg(test)]
mod ffi_test_stubs {
    use super::FfiPresentedRecording;
    use std::ffi::c_void;

    #[unsafe(no_mangle)]
    extern "C" fn web_navigable_presenter_unref(_: *mut c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_navigable_presenter_seal_for_clock_lane(_: *mut c_void, _: *const c_void) -> *mut c_void {
        std::ptr::null_mut()
    }

    #[unsafe(no_mangle)]
    extern "C" fn web_sealed_presentation_destroy(_: *mut c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_sealed_presentation_take_visual_context_tree(
        _: *mut c_void,
        _: *const c_void,
        _: *const libgfx_rust::FloatPoint,
        _: usize,
    ) {
    }

    #[unsafe(no_mangle)]
    extern "C" fn web_navigable_presenter_add_font(_: *mut c_void, _: *const c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_navigable_presenter_add_image_frame(_: *mut c_void, _: *const c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_navigable_presenter_add_video_sink(_: *mut c_void, _: u64, _: u64) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_navigable_presenter_present(
        _: *mut c_void,
        _: *mut c_void,
        _: *const FfiPresentedRecording,
        _: *mut c_void,
    ) {
    }

    #[unsafe(no_mangle)]
    extern "C" fn web_navigable_presenter_present_unrecorded(_: *mut c_void, _: *mut c_void, _: *mut c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_vector_image_resources_destroy(_: *mut c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_navigable_presenter_take_vector_image_resources(
        _: *mut c_void,
        _: *mut c_void,
        _: *const u64,
        _: usize,
    ) {
    }
}
