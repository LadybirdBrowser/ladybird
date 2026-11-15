/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The display lists an SVG image's document was rendered as, which the document keeps for the sizes, scales, color
//! schemes and force-dark settings the pages that draw it asked for, until what it shows changes.

use crate::css::css_pixels::CssPixels;
use crate::painting::display_list_resources::NestedDisplayList;
use crate::painting::host::FfiVectorImageRenderRequest;
use crate::render_state::DocumentHost;
use std::sync::Arc;

/// What an SVG image is rendered for, as C++ names it.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiVectorImageKey {
    pub css_width: CssPixels,
    pub css_height: CssPixels,
    pub raster_scale: f32,
    pub color_scheme: u8,
    pub force_dark_enabled: bool,
    pub force_dark_foreground_threshold: i32,
    pub force_dark_background_threshold: i32,
}

/// What an SVG image is rendered for: two renderings for one key show the same.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct VectorImageKey {
    css_width_raw: i32,
    css_height_raw: i32,
    raster_scale_bits: u32,
    color_scheme: u8,
    force_dark_enabled: bool,
    force_dark_foreground_threshold: i32,
    force_dark_background_threshold: i32,
}

impl VectorImageKey {
    pub(crate) fn of_request(request: &FfiVectorImageRenderRequest) -> Self {
        Self::of(&FfiVectorImageKey {
            css_width: request.css_width,
            css_height: request.css_height,
            raster_scale: request.raster_scale,
            color_scheme: request.color_scheme,
            force_dark_enabled: request.force_dark_enabled,
            force_dark_foreground_threshold: request.force_dark_foreground_threshold,
            force_dark_background_threshold: request.force_dark_background_threshold,
        })
    }

    pub(crate) fn of(key: &FfiVectorImageKey) -> Self {
        Self {
            css_width_raw: key.css_width.raw_value(),
            css_height_raw: key.css_height.raw_value(),
            raster_scale_bits: key.raster_scale.to_bits(),
            color_scheme: key.color_scheme,
            force_dark_enabled: key.force_dark_enabled,
            force_dark_foreground_threshold: key.force_dark_foreground_threshold,
            force_dark_background_threshold: key.force_dark_background_threshold,
        }
    }
}

/// How many renderings an SVG image's document keeps: the oldest goes first.
const KEPT_RENDERINGS: usize = 10;

/// The renderings of an SVG image's document, oldest first.
#[derive(Default)]
pub(crate) struct VectorImageCache {
    renderings: Vec<(VectorImageKey, Arc<NestedDisplayList>)>,
}

impl VectorImageCache {
    pub(crate) fn get(&self, key: &VectorImageKey) -> Option<Arc<NestedDisplayList>> {
        self.renderings
            .iter()
            .find_map(|(rendered_key, display_list)| (rendered_key == key).then(|| display_list.clone()))
    }

    fn keep(&mut self, key: VectorImageKey, display_list: Arc<NestedDisplayList>) {
        self.renderings.retain(|(rendered_key, _)| *rendered_key != key);
        if self.renderings.len() >= KEPT_RENDERINGS {
            self.renderings.remove(0);
        }
        self.renderings.push((key, display_list));
    }

    pub(crate) fn clear(&mut self) {
        self.renderings.clear();
    }
}

impl DocumentHost {
    /// The display list this SVG image's document was rendered as for `key`, if it keeps one.
    pub(crate) fn vector_image(&self, key: &VectorImageKey) -> Option<Arc<NestedDisplayList>> {
        self.vector_images().get(key)
    }
}

/// Whether the SVG image document of `host` keeps a rendering for `key`.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_has_vector_image(host: &DocumentHost, key: &FfiVectorImageKey) -> bool {
    host.vector_image(&VectorImageKey::of(key)).is_some()
}

/// Has the SVG image document of `host` keep the recording it took in last as its rendering for `key`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_keep_last_recording_as_vector_image(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    key: &FfiVectorImageKey,
) {
    let display_list = crate::painting::ffi::read_arena(host, read, (), |arena, ()| {
        let recording = arena.paint_state().last_recording.clone()?;
        let visual_context_tree = recording.visual_context_tree.clone()?;
        Some(NestedDisplayList::recorded(
            recording.display_list.clone(),
            visual_context_tree,
            recording.root_group_color,
            recording.resources.clone(),
        ))
    });
    host.vector_images().keep(
        VectorImageKey::of(key),
        display_list.unwrap_or_else(NestedDisplayList::empty),
    );
}

/// Has the SVG image document of `host` forget its renderings: what it shows changed.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_forget_vector_images(host: &DocumentHost) {
    host.vector_images().clear();
}
