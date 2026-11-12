/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod hit_test;
pub mod paint;
pub mod visual_context;

pub use hit_test::*;
pub use paint::*;
pub use visual_context::*;

/// The boxes the canvas background is painted from, and whether it takes over the body's.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RootBackgroundSource {
    pub use_body_background_properties: bool,
    pub root_layout_node: crate::layout::node_data::NodeSlotId,
    pub body_layout_node: crate::layout::node_data::NodeSlotId,
}

impl Default for RootBackgroundSource {
    fn default() -> Self {
        Self {
            use_body_background_properties: false,
            root_layout_node: crate::layout::node_data::NodeSlotId::INVALID,
            body_layout_node: crate::layout::node_data::NodeSlotId::INVALID,
        }
    }
}

/// What geometry tells the document. The fields are private, so the callbacks are reached only
/// through the methods below, which take the main thread token.
///
/// NB: The callbacks are typed as safe functions because the table only comes from the host through an export
///     whose safety contract keeps them callable, and the private fields let nothing else call them.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiGeometryHostCallbacks {
    context: *mut std::ffi::c_void,
    set_scroll_offset: extern "C" fn(
        *mut std::ffi::c_void,
        &crate::render_state::BegunRead,
        crate::layout::node_data::NodeSlotId,
        crate::layout::used_values::FfiCssPixelPoint,
    ),
}

impl FfiGeometryHostCallbacks {
    /// Stores a scroll offset the overflow pass settled on, after the pass, in `read`.
    pub(crate) fn set_scroll_offset(
        &self,
        _: &crate::stage::MainThread,
        read: &crate::render_state::BegunRead,
        slot: crate::layout::node_data::NodeSlotId,
        offset: crate::layout::used_values::FfiCssPixelPoint,
    ) {
        (self.set_scroll_offset)(self.context, read, slot, offset);
    }
}

/// The viewport a document's paint and hit testing properties are prepared for, which only the document knows.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPaintPreparationViewport {
    pub visual_context_tree_inputs: FfiVisualContextTreeInputs,
    pub device_viewport_size: libgfx_rust::IntSize,
}

/// What preparing the paint and hit testing properties of a document asks of the document, and tells it. The fields
/// are private, so the callbacks are reached only through the methods below, which take the main thread token.
///
/// NB: The callbacks are typed as safe functions because the table only comes from the host through an export
///     whose safety contract keeps them callable, and the private fields let nothing else call them.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPaintPreparationHostCallbacks {
    context: *mut std::ffi::c_void,
    viewport: extern "C" fn(*mut std::ffi::c_void, &mut FfiPaintPreparationViewport),
    resolve_svg_filter: extern "C" fn(
        *mut std::ffi::c_void,
        &crate::render_state::BegunRead,
        crate::layout::node_data::NodeSlotId,
        *const std::ffi::c_void,
        *mut std::ffi::c_void,
    ) -> bool,
    resolve_svg_paint_server: extern "C" fn(
        *mut std::ffi::c_void,
        &crate::render_state::BegunRead,
        crate::layout::node_data::NodeSlotId,
        bool,
        *mut std::ffi::c_void,
    ),
    require_display_list_recording: extern "C" fn(*mut std::ffi::c_void, bool),
}

impl FfiPaintPreparationHostCallbacks {
    pub(crate) fn viewport(&self, _: &crate::stage::MainThread) -> FfiPaintPreparationViewport {
        let mut viewport = FfiPaintPreparationViewport {
            visual_context_tree_inputs: FfiVisualContextTreeInputs {
                device_pixels_per_css_pixel: 1.0,
                visual_viewport_offset_x: 0.0,
                visual_viewport_offset_y: 0.0,
                visual_viewport_scale: 1.0,
            },
            device_viewport_size: libgfx_rust::IntSize::default(),
        };
        (self.viewport)(self.context, &mut viewport);
        viewport
    }

    /// Pushes the primitives of the SVG filter that the `url()` value `url` of the row in `slot` names into `primitives`,
    /// and answers whether it names one.
    pub(crate) fn resolve_svg_filter(
        &self,
        _: &crate::stage::MainThread,
        read: &crate::render_state::BegunRead,
        slot: crate::layout::node_data::NodeSlotId,
        url: *const std::ffi::c_void,
        primitives: &mut Vec<crate::painting::svg_filter::SvgFilterPrimitive>,
    ) -> bool {
        (self.resolve_svg_filter)(self.context, read, slot, url, (&raw mut *primitives).cast())
    }

    /// Writes the paint server that the fill, or with `is_stroke` the stroke, of the row in `slot` names to `published`.
    pub(crate) fn resolve_svg_paint_server(
        &self,
        _: &crate::stage::MainThread,
        read: &crate::render_state::BegunRead,
        slot: crate::layout::node_data::NodeSlotId,
        is_stroke: bool,
        published: &mut crate::painting::svg_paint_resources::PublishedSvgPaintServer,
    ) {
        (self.resolve_svg_paint_server)(self.context, read, slot, is_stroke, (&raw mut *published).cast());
    }

    /// Has the document record its display list again, and with `request_frame` render a frame for it.
    pub(crate) fn require_display_list_recording(&self, _: &crate::stage::MainThread, request_frame: bool) {
        (self.require_display_list_recording)(self.context, request_frame);
    }
}
