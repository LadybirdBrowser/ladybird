/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a display list draws besides its tape: fonts, image frames and the SVG images its commands name, which the list
//! holds for as long as anything holds the list. The compositor is sent the fonts and image frames it lacks ahead of
//! the list, and the records of the SVG images in the list's tape.

use crate::painting::display_list::builder::RecordedDisplayList;
use crate::painting::display_list::summary::{DisplayListSummary, summarize};
use crate::painting::vector_image_records::VectorImageRecords;
use libgfx_rust::font::FontHandle;
use libgfx_rust::image_frame::ImageFrameHandle;
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

/// The resources a display list draws, each under the id its commands name it by.
#[derive(Clone, Default)]
pub(crate) struct DisplayListResources {
    pub(crate) fonts: HashMap<u64, FontHandle>,
    pub(crate) image_frames: HashMap<u64, ImageFrameHandle>,
    pub(crate) vector_images: HashMap<u64, Arc<VectorImageRecords>>,
}

impl DisplayListResources {
    /// Takes what `other` holds that this does not.
    pub(crate) fn include(&mut self, other: &Self) {
        self.include_fonts_and_image_frames(other);
        for (id, vector_image) in &other.vector_images {
            self.vector_images.entry(*id).or_insert_with(|| vector_image.clone());
        }
    }

    pub(crate) fn include_fonts_and_image_frames(&mut self, other: &Self) {
        for (id, font) in &other.fonts {
            self.fonts.entry(*id).or_insert_with(|| font.clone());
        }
        for (id, frame) in &other.image_frames {
            self.image_frames.entry(*id).or_insert_with(|| frame.clone());
        }
    }

    /// These and what the records of their SVG images draw: everything the compositor needs to replay a list that
    /// draws these.
    pub(crate) fn with_vector_image_resources(&self) -> Self {
        let mut closure = self.clone();
        for vector_image in self.vector_images.values() {
            closure.include_fonts_and_image_frames(&vector_image.resources);
        }
        closure
    }

    /// Takes what `source` holds of what `summary` names and this lacks: what a recording copied from the one it read
    /// paint commands from draws.
    pub(crate) fn complete_from(&mut self, summary: &DisplayListSummary, source: &Self) {
        for id in &summary.font_ids {
            if let Some(font) = source.fonts.get(id) {
                self.fonts.entry(*id).or_insert_with(|| font.clone());
            }
        }
        for id in &summary.image_frame_ids {
            if let Some(frame) = source.image_frames.get(id) {
                self.image_frames.entry(*id).or_insert_with(|| frame.clone());
            }
        }
        for id in &summary.vector_image_ids {
            if let Some(vector_image) = source.vector_images.get(id) {
                self.vector_images.entry(*id).or_insert_with(|| vector_image.clone());
            }
        }
    }

    pub(crate) fn ids(&self) -> ResourceIds {
        ResourceIds {
            fonts: self.fonts.keys().copied().collect(),
            image_frames: self.image_frames.keys().copied().collect(),
            vector_images: self.vector_images.keys().copied().collect(),
        }
    }
}

/// The ids of resources a receiver holds, and of the SVG images it may keep rasters of.
#[derive(Clone, Default, Debug, PartialEq, Eq)]
pub(crate) struct ResourceIds {
    pub(crate) fonts: HashSet<u64>,
    pub(crate) image_frames: HashSet<u64>,
    pub(crate) vector_images: HashSet<u64>,
}

/// The ids of the resources the commands of `display_list` name.
pub(crate) fn summary_of(display_list: &RecordedDisplayList) -> DisplayListSummary {
    summarize(&display_list.bytes, &display_list.command_runs)
}
