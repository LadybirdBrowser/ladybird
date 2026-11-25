/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The 2D canvas contexts, with the attributes and operations of each of their mixins in the module of its IDL file,
//! the objects that they draw with, the images that they and WebGL read and write the pixels of, and the operations of
//! a path that Path2D and the contexts share. The state that the contexts share is in crate::canvas_2d. The message
//! ports and channels of web messaging, the broadcast channels, server-sent events, Web Storage and the cues of text
//! tracks are here as well.

pub(crate) mod canvas_path;
pub(crate) mod color_stops;
pub(crate) mod predefined_color_space;

// What follows runs on the JS runtime, which the tests of the crate do not link.
#[cfg(not(test))]
pub(crate) mod bar_prop;
#[cfg(not(test))]
pub(crate) mod before_unload_event;
#[cfg(not(test))]
pub(crate) mod broadcast_channel;
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
pub(crate) mod command_event;
#[cfg(not(test))]
pub(crate) mod data_transfer;
#[cfg(not(test))]
pub(crate) mod data_transfer_item;
#[cfg(not(test))]
pub(crate) mod data_transfer_item_list;
#[cfg(not(test))]
pub(crate) mod dom_string_list;
#[cfg(not(test))]
pub(crate) mod dom_string_map;
#[cfg(not(test))]
pub(crate) mod error_event;
#[cfg(not(test))]
pub(crate) mod event_source;
#[cfg(not(test))]
pub(crate) mod exports;
#[cfg(not(test))]
pub(crate) mod form_data_event;
#[cfg(not(test))]
pub(crate) mod global_event_handlers;
#[cfg(not(test))]
pub(crate) mod hash_change_event;
#[cfg(not(test))]
pub(crate) mod html_form_element;
#[cfg(not(test))]
pub(crate) mod image_bitmap;
#[cfg(not(test))]
pub(crate) mod image_bitmap_rendering_context;
#[cfg(not(test))]
pub(crate) mod image_data;
#[cfg(not(test))]
pub(crate) mod media_error;
#[cfg(not(test))]
pub(crate) mod message_channel;
#[cfg(not(test))]
pub(crate) mod message_event;
#[cfg(not(test))]
pub(crate) mod message_port;
#[cfg(not(test))]
pub(crate) mod mime_type;
#[cfg(not(test))]
pub(crate) mod mime_type_array;
#[cfg(not(test))]
pub(crate) mod navigable;
#[cfg(not(test))]
pub(crate) mod navigation_current_entry_change_event;
#[cfg(not(test))]
pub(crate) mod navigation_type;
#[cfg(not(test))]
pub(crate) mod navigator;
#[cfg(not(test))]
pub(crate) mod numbers;
#[cfg(not(test))]
pub(crate) mod offscreen_canvas;
#[cfg(not(test))]
pub(crate) mod offscreen_canvas_rendering_context2d;
#[cfg(not(test))]
pub(crate) mod page_swap_event;
#[cfg(not(test))]
pub(crate) mod page_transition_event;
#[cfg(not(test))]
pub(crate) mod path2d;
#[cfg(not(test))]
pub(crate) mod plugin;
#[cfg(not(test))]
pub(crate) mod plugin_array;
#[cfg(not(test))]
pub(crate) mod pop_state_event;
#[cfg(not(test))]
pub(crate) mod port_channel;
#[cfg(not(test))]
pub(crate) mod promise_rejection_event;
#[cfg(not(test))]
pub(crate) mod sanitizer;
#[cfg(not(test))]
pub(crate) mod sanitizer_configuration;
#[cfg(not(test))]
pub(crate) mod storage;
#[cfg(not(test))]
pub(crate) mod storage_event;
#[cfg(not(test))]
pub(crate) mod structured_serialize;
#[cfg(not(test))]
pub(crate) mod submit_event;
#[cfg(not(test))]
pub(crate) mod text_metrics;
#[cfg(not(test))]
pub(crate) mod text_track_cue;
#[cfg(not(test))]
pub(crate) mod toggle_event;
#[cfg(not(test))]
pub(crate) mod track_event;
#[cfg(not(test))]
pub(crate) mod video_playback_quality;
#[cfg(not(test))]
pub(crate) mod visibility_state;
#[cfg(not(test))]
pub(crate) mod visibility_state_entry;
#[cfg(not(test))]
pub(crate) mod window;
#[cfg(not(test))]
pub(crate) mod window_or_worker_global_scope;
