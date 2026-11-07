/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod audio_param_timeline;
pub mod control_message;
pub mod control_message_queue;
pub mod control_thread;
pub mod rendering;

pub const MAX_NUMBER_OF_CHANNELS: usize = 32;
