/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::host::FfiImageContentKind;
use libgfx_rust::image_frame::ImageFrameHandle;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) enum ImageContent {
    #[default]
    None,
    Raster(Option<ImageFrameHandle>),
    Vector {
        content_identity: u64,
        has_active_view_box: bool,
    },
}

impl ImageContent {
    /// # Safety
    ///
    /// `frame` must be null or point to a live `Gfx::DecodedImageFrame`.
    pub(crate) unsafe fn from_ffi(
        kind: FfiImageContentKind,
        frame: *const std::ffi::c_void,
        vector_content_identity: u64,
        vector_has_active_view_box: bool,
    ) -> Self {
        match kind {
            FfiImageContentKind::None => Self::None,
            FfiImageContentKind::Raster => {
                Self::Raster((!frame.is_null()).then(|| unsafe { ImageFrameHandle::retain(frame) }))
            }
            FfiImageContentKind::Vector => Self::Vector {
                content_identity: vector_content_identity,
                has_active_view_box: vector_has_active_view_box,
            },
        }
    }
}
