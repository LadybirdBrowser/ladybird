/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The check that the recorder and the replayer mean the same bytes. Both get their structs from GLFunctions.json,
//! but through two generators, and what is not generated, such as the framing, is a constant on each side. A
//! difference would not fail a build, and would make the replayer read garbage, so the two sides are compared before
//! the first context records.

use std::sync::Once;

use super::proxy::recorder::{DEFAULT_SHARED_DATA_REGION_CAPACITY, SHARED_DATA_REGION_OFFSET};
use super::proxy::vocabulary::WIRE_STRUCT_LAYOUTS;
use super::proxy::wire::{HEADER_PAYLOAD_SIZE_OFFSET, HEADER_SIZE, RECORD_ALIGNMENT};

/// RemoteWebGLTransportFFI.cpp has a struct of the same name, and reports the constants of the C++ side as one.
#[derive(Debug, Default, PartialEq, Eq)]
#[repr(C)]
struct ConstantsRestatedInRust {
    record_alignment: usize,
    header_size: usize,
    header_payload_size_offset: usize,
    shared_data_region_offset: usize,
    shared_data_region_capacity: usize,
}

unsafe extern "C" {
    fn compositing_webgl_wire_struct_layouts(out_word_count: *mut usize) -> *const u32;
    fn web_webgl_constants_restated_in_rust(out_constants: *mut ConstantsRestatedInRust);
}

pub(crate) fn verify_that_the_replayer_reads_what_the_recorder_writes() {
    static VERIFIED: Once = Once::new();
    VERIFIED.call_once(|| {
        let mut constants_of_the_cpp_side = ConstantsRestatedInRust::default();
        // SAFETY: The function fills in the struct, which has the layout of its C++ counterpart.
        unsafe { web_webgl_constants_restated_in_rust(&raw mut constants_of_the_cpp_side) };
        let constants_of_the_rust_side = ConstantsRestatedInRust {
            record_alignment: RECORD_ALIGNMENT,
            header_size: HEADER_SIZE,
            header_payload_size_offset: HEADER_PAYLOAD_SIZE_OFFSET,
            shared_data_region_offset: SHARED_DATA_REGION_OFFSET,
            shared_data_region_capacity: DEFAULT_SHARED_DATA_REGION_CAPACITY,
        };
        assert!(
            constants_of_the_cpp_side == constants_of_the_rust_side,
            "the WebGL implementation restates constants of the C++ side ({constants_of_the_cpp_side:?}) differently \
             in Rust ({constants_of_the_rust_side:?})"
        );

        let mut word_count = 0;
        // SAFETY: The function returns a table that lives as long as the process, and stores its length.
        let replayer_layouts = unsafe {
            let words = compositing_webgl_wire_struct_layouts(&raw mut word_count);
            core::slice::from_raw_parts(words, word_count)
        };
        let first_difference = replayer_layouts
            .iter()
            .zip(WIRE_STRUCT_LAYOUTS)
            .position(|(theirs, ours)| theirs != ours);
        assert!(
            first_difference.is_none() && replayer_layouts.len() == WIRE_STRUCT_LAYOUTS.len(),
            "the structs of the WebGL command stream differ between its recorder and its replayer, from word \
             {first_difference:?} of {} and {} layout words on; Libraries/LibWeb/Bindings/Rust/generate and \
             Meta/Generators/libweb_webgl.py have to derive the same structs from GLFunctions.json",
            WIRE_STRUCT_LAYOUTS.len(),
            replayer_layouts.len()
        );
    });
}
