/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The build script writes the constants that the WebGL interfaces define from their IDL.

#![allow(
    non_upper_case_globals,
    reason = "GL has lower case letters in the names of some constants"
)]
#![allow(
    dead_code,
    reason = "these are all the constants of the interfaces, of which the implementation names some"
)]

include!(concat!(env!("OUT_DIR"), "/webgl_constants.rs"));

// The constants that no WebGL interface defines.
pub(crate) const GL_FALSE: u32 = 0;
pub(crate) const GL_TRUE: u32 = 1;
pub(crate) const GL_INFO_LOG_LENGTH: u32 = 0x8B84;
pub(crate) const GL_SHADER_SOURCE_LENGTH: u32 = 0x8B88;
pub(crate) const GL_UNIFORM_BLOCK_NAME_LENGTH: u32 = 0x8A41;
pub(crate) const GL_NUM_SAMPLE_COUNTS: u32 = 0x9380;
