/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Web Speech API, https://webaudio.github.io/web-speech-api/

#[cfg(not(test))]
pub(crate) mod speech_recognition_alternative;
#[cfg(not(test))]
pub(crate) mod speech_recognition_event;
#[cfg(not(test))]
pub(crate) mod speech_recognition_phrase;
#[cfg(not(test))]
pub(crate) mod speech_recognition_result;
#[cfg(not(test))]
pub(crate) mod speech_recognition_result_list;
