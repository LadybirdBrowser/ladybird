/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The 2D canvas contexts, with the attributes and operations of each of their mixins in the module of its IDL file,
//! the objects that they draw with, the images that they and WebGL read and write the pixels of, and the operations of
//! a path that Path2D and the contexts share. The state that the contexts share is in crate::canvas_2d.

pub(crate) mod canvas_path;
pub(crate) mod color_stops;
pub(crate) mod predefined_color_space;

// What follows runs on the JS runtime, which the tests of the crate do not link.
#[cfg(not(test))]
pub(crate) mod canvas_compositing;
#[cfg(not(test))]
pub(crate) mod canvas_draw_image;
#[cfg(not(test))]
pub(crate) mod canvas_draw_path;
#[cfg(not(test))]
pub(crate) mod canvas_fill_stroke_styles;
#[cfg(not(test))]
pub(crate) mod canvas_filters;
#[cfg(not(test))]
pub(crate) mod canvas_gradient;
#[cfg(not(test))]
pub(crate) mod canvas_image_data;
#[cfg(not(test))]
pub(crate) mod canvas_image_smoothing;
#[cfg(not(test))]
pub(crate) mod canvas_path_drawing_styles;
#[cfg(not(test))]
pub(crate) mod canvas_pattern;
#[cfg(not(test))]
pub(crate) mod canvas_rect;
#[cfg(not(test))]
pub(crate) mod canvas_rendering_context2d;
#[cfg(not(test))]
pub(crate) mod canvas_rendering_context2d_settings;
#[cfg(not(test))]
pub(crate) mod canvas_settings;
#[cfg(not(test))]
pub(crate) mod canvas_shadow_styles;
#[cfg(not(test))]
pub(crate) mod canvas_state;
#[cfg(not(test))]
pub(crate) mod canvas_text;
#[cfg(not(test))]
pub(crate) mod canvas_text_drawing_styles;
#[cfg(not(test))]
pub(crate) mod canvas_transform;
#[cfg(not(test))]
pub(crate) mod exports;
#[cfg(not(test))]
pub(crate) mod image_bitmap;
#[cfg(not(test))]
pub(crate) mod image_data;
#[cfg(not(test))]
pub(crate) mod offscreen_canvas_rendering_context2d;
#[cfg(not(test))]
pub(crate) mod path2d;
#[cfg(not(test))]
pub(crate) mod structured_serialize;
#[cfg(not(test))]
pub(crate) mod text_metrics;
